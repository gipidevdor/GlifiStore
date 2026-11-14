#include "glifistore/worker/worker.hpp"

#include "glifistore/core/fault_injection.hpp"
#include "glifistore/core/hot_path_phases.hpp"
#include "glifistore/core/key_hash.hpp"

#include <algorithm>
#include <cassert>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace glifistore {
namespace {

static_assert(std::is_nothrow_move_assignable_v<Index>);
static_assert(std::is_nothrow_swappable_v<std::vector<SegmentPtr>>);
static_assert(std::is_nothrow_swappable_v<std::unordered_map<SegmentId, Segment*>>);

[[nodiscard]] auto validate_live_record(const Segment& segment, const RecordRef& ref) -> Status {
    if (auto valid = segment.validate_ref_extent(ref); !valid) {
        return valid;
    }
    const auto stats = segment.stats();
    if (stats.live_records == 0 || stats.live_bytes < ref.size.value) {
        return fail(ErrorCode::corrupted_data, "record is not accounted as live");
    }
    return {};
}

// Append already consumed Segment bytes: Index/publication failure is indeterminate
// for wire polarity (must not map to OVERLOADED / known-not-committed).
[[nodiscard]] auto indeterminate_after_append(Error error) -> Error {
    error.code = ErrorCode::unavailable;
    return error;
}

} // namespace

Worker::Worker(const WorkerId id, GlobalSegmentManager& manager, const WorkerRoutingState routing)
    : id_(id), manager_(manager), index_(routing) {
    active_ = manager_.allocate_active(id_);
    register_owned_segment(active_);
}

void Worker::register_owned_segment(SegmentPtr segment) {
    const auto id = segment->id();
    owned_.push_back(std::move(segment));
    try {
        const auto [position, inserted] = owned_by_id_.emplace(id, owned_.back().get());
        static_cast<void>(position);
        if (!inserted) {
            throw std::logic_error{"duplicate Worker Segment registration"};
        }
    } catch (...) {
        owned_.pop_back();
        throw;
    }
}

void Worker::unregister_owned_segment(const SegmentId id) noexcept {
    owned_by_id_.erase(id);
    std::erase_if(owned_, [id](const SegmentPtr& candidate) { return candidate && candidate->id() == id; });
}

auto Worker::find_owned_segment(const SegmentId id) noexcept -> Segment* {
    const auto found = owned_by_id_.find(id);
    return found == owned_by_id_.end() ? nullptr : found->second;
}

auto Worker::find_owned_segment(const SegmentId id) const noexcept -> const Segment* {
    const auto found = owned_by_id_.find(id);
    return found == owned_by_id_.end() ? nullptr : found->second;
}

void Worker::maybe_retire(Segment& segment) {
    if (segment.state() != SegmentState::sealed || segment.stats().live_records != 0) {
        return;
    }

    const auto id = segment.id();
    if (auto retired = manager_.try_retire(id); !retired) {
        return;
    }
    unregister_owned_segment(id);
}

auto Worker::next_sequence() -> SequenceNumber {
    const auto current = next_sequence_;
    next_sequence_ = SequenceNumber{next_sequence_.value + 1};
    return current;
}

auto Worker::append_record(const RecordInput& input) -> Result<RecordRef> {
    // Litmus: pre-append rotation/catalog failure (invalid_reference) must stay
    // known-not-committed at the Writer — not wire INTERNAL_ERROR / reconcile.
    if (glifistore::fault::consume_fail(glifistore::fault::Site::rotate)) {
        return fail(ErrorCode::invalid_reference, "injected volatile rotation failure before append");
    }
    auto ref = active_->append(input);
    if (!ref && ref.error().code == ErrorCode::segment_full) {
        auto sealed = active_;
        auto replacement = manager_.prepare_rotation(sealed, id_);
        if (!replacement) {
            return unexpected(replacement.error());
        }
        register_owned_segment(*replacement);
        try {
            if (auto committed = manager_.commit_rotation(sealed, *replacement); !committed) {
                unregister_owned_segment((*replacement)->id());
                return unexpected(committed.error());
            }
        } catch (...) {
            unregister_owned_segment((*replacement)->id());
            throw;
        }
        active_ = *replacement;
        maybe_retire(*sealed);
        ref = active_->append(input);
    }
    return ref;
}

auto Worker::read_ref(const RecordRef& ref) const -> Result<RecordView> {
    const auto* segment = find_owned_segment(ref.segment_id);
    if (!segment) {
        return fail(ErrorCode::invalid_reference,
                    "record reference targets a segment not owned by this worker");
    }
    // Store get must always verify CRC: segment bytes are reachable via Store::segments() and
    // Segment::mutable_base(), so skipping checksum would allow in-memory tampering.
    return segment->read(ref);
}

auto Worker::publish(const HashedKey& key, const RecordRef& ref, Segment& segment) -> Status {
    GS_PHASE_PUT(index_publish);
    if (segment.id() != ref.segment_id || segment.owner() != id_) {
        return fail(ErrorCode::invalid_reference,
                    "new record reference targets a segment not owned by this worker");
    }

    auto mutation = index_.insert_or_assign(key, ref);
    if (!mutation) {
        return unexpected(mutation.error());
    }
    const auto previous = mutation->previous;
    const auto rollback_index = [&]() -> Status {
        if (previous) {
            auto restored = index_.insert_or_assign(key, previous.value());
            if (!restored || restored->previous != ref) {
                return fail(ErrorCode::corrupted_data, "failed to restore replaced index entry");
            }
            return {};
        }
        const auto removed = index_.erase(key);
        if (removed.previous != ref) {
            return fail(ErrorCode::corrupted_data, "failed to remove unpublished index entry");
        }
        return {};
    };

    Segment* previous_segment = nullptr;
    if (previous) {
        previous_segment = find_owned_segment(previous.value().segment_id);
        if (!previous_segment) {
            if (auto rolled_back = rollback_index(); !rolled_back) {
                return rolled_back;
            }
            return fail(ErrorCode::invalid_reference,
                        "previous record reference targets a segment not owned by this worker");
        }
        if (auto valid = validate_live_record(*previous_segment, previous.value()); !valid) {
            if (auto rolled_back = rollback_index(); !rolled_back) {
                return rolled_back;
            }
            return valid;
        }
    }

    if (auto live = segment.mark_live(ref); !live) {
        if (auto rolled_back = rollback_index(); !rolled_back) {
            return rolled_back;
        }
        return live;
    }
    if (previous) {
        if (auto dead = previous_segment->mark_dead(previous.value()); !dead) {
            static_cast<void>(segment.mark_dead(ref));
            if (auto rolled_back = rollback_index(); !rolled_back) {
                return rolled_back;
            }
            return dead;
        }
    }
    if (previous) {
        maybe_retire(*previous_segment);
    }
    return {};
}

auto Worker::get(const HashedKey& key, const std::uint64_t now_ns) -> Result<RecordView> {
    const std::lock_guard lock{mutex_};
    return get_locked(key, now_ns);
}

auto Worker::put(const HashedKey& key, const std::span<const std::byte> value,
                 const std::uint64_t expire_at_ns) -> Status {
    const std::lock_guard lock{mutex_};
    return put_locked(key, value, expire_at_ns);
}

auto Worker::erase(const HashedKey& key) -> Status {
    const std::lock_guard lock{mutex_};
    return erase_locked(key);
}

auto Worker::compact(const std::uint64_t now_ns, const VacuumPolicy policy)
    -> Result<std::optional<VacuumStats>> {
    // Exclusive Writer: mutex alone does not own the Index — arm the quiesce gate and
    // drain hot_path_depth before taking mutex_ (do not hold mutex across the wait).
    ExclusiveIndexQuiesce index_quiesce{*this, exclusive_writer_};
    // Litmus: hold the gate after depth drain so a sibling exclusive put observes
    // sequence_conflict (mutex-only compact raced unlocked Index publish).
    GS_FAULT_BLOCK(compact);
    const std::lock_guard lock{mutex_};
    const VacuumPlanner planner{policy};
    const auto candidates = planner.candidates(owned_);
    if (candidates.empty()) {
        return std::optional<VacuumStats>{};
    }

    VacuumBuilder builder;
    auto vacuumed =
        builder.rebuild(index_, owned_, candidates, [this] { return manager_.prepare_segment(id_); }, now_ns);
    if (!vacuumed) {
        return unexpected(vacuumed.error());
    }
    if (vacuumed->segments.size() >= candidates.size()) {
        return std::optional<VacuumStats>{};
    }

    const auto is_candidate = [&](const SegmentId id) {
        return std::ranges::find(candidates, id) != candidates.end();
    };
    std::vector<SegmentPtr> next_owned;
    next_owned.reserve(owned_.size() - candidates.size() + vacuumed->segments.size());
    for (const auto& segment : owned_) {
        if (!is_candidate(segment->id())) {
            next_owned.push_back(segment);
        }
    }
    for (const auto& segment : vacuumed->segments) {
        next_owned.push_back(segment);
    }

    std::unordered_map<SegmentId, Segment*> next_owned_by_id;
    next_owned_by_id.reserve(next_owned.size());
    for (const auto& segment : next_owned) {
        if (!next_owned_by_id.emplace(segment->id(), segment.get()).second) {
            return fail(ErrorCode::corrupted_data,
                        "volatile vacuum produced a duplicate owned Segment identity");
        }
    }
    if (!next_owned_by_id.contains(active_->id())) {
        return fail(ErrorCode::corrupted_data, "volatile vacuum attempted to replace the active Segment");
    }

    const auto entries = index_.entries();
    std::size_t marked_dead{};
    const auto restore_liveness = [&] {
        std::size_t restored{};
        for (const auto& entry : entries) {
            if (!is_candidate(entry.record.segment_id)) {
                continue;
            }
            if (restored == marked_dead) {
                break;
            }
            auto* segment = find_owned_segment(entry.record.segment_id);
            if (segment != nullptr) {
                static_cast<void>(segment->mark_live(entry.record));
            }
            ++restored;
        }
    };
    for (const auto& entry : entries) {
        if (!is_candidate(entry.record.segment_id)) {
            continue;
        }
        auto* segment = find_owned_segment(entry.record.segment_id);
        if (segment == nullptr) {
            restore_liveness();
            return fail(ErrorCode::invalid_reference,
                        "volatile vacuum source disappeared from Worker ownership");
        }
        if (auto dead = segment->mark_dead(entry.record); !dead) {
            restore_liveness();
            return unexpected(dead.error());
        }
        ++marked_dead;
    }

    try {
        if (auto published = manager_.replace_sealed(candidates, vacuumed->segments); !published) {
            restore_liveness();
            return unexpected(published.error());
        }
    } catch (...) {
        restore_liveness();
        throw;
    }

    index_ = std::move(vacuumed->index);
    owned_.swap(next_owned);
    owned_by_id_.swap(next_owned_by_id);
    return std::optional<VacuumStats>{vacuumed->stats};
}

auto Worker::get_locked(const HashedKey& key, const std::uint64_t now_ns) -> Result<RecordView> {
    const auto ref = index_.find(key);
    if (!ref) {
        return fail(ErrorCode::not_found, "key is not present");
    }
    auto record = read_ref(*ref);
    if (!record) {
        return unexpected(record.error());
    }
    if (record->opcode == Opcode::erase) {
        return fail(ErrorCode::not_found, "key is not present");
    }
    if (record->expired(now_ns)) {
        if (auto erased = erase_locked(key); !erased) {
            return unexpected(erased.error());
        }
        return fail(ErrorCode::not_found, "key has expired");
    }
    return record;
}

auto Worker::put_locked(const HashedKey& key, const std::span<const std::byte> value,
                        const std::uint64_t expire_at_ns) -> Status {
    auto published = put_locked_published(key, value, expire_at_ns);
    if (!published) {
        return unexpected(published.error());
    }
    return {};
}

auto Worker::put_locked_published(const HashedKey& key, const std::span<const std::byte> value,
                                  const std::uint64_t expire_at_ns) -> Result<WorkerMutationPublication> {
    ExclusiveHotPathGuard hot_path{exclusive_writer_ ? this : nullptr};
#ifndef NDEBUG
    if (exclusive_writer_) {
        // Paired exclusive Writer must not hold mutex_ on the hot path. Compaction
        // and verify still take it after Index quiesce; if they race, wait then
        // release before mutating.
        std::unique_lock lock{mutex_, std::defer_lock};
        if (!lock.try_lock()) {
            lock.lock();
        }
        lock.unlock();
        assert(!lock.owns_lock());
    }
#endif
    if (exclusive_writer_ && index_quiesce_active_.load(std::memory_order_acquire)) {
        return fail(ErrorCode::sequence_conflict,
                    "volatile mutation conflicts with Index quiesce (compact/verify)");
    }
    const RecordInput input{
        .sequence = next_sequence(),
        .opcode = Opcode::put,
        .key_hash = key.hash,
        .expire_at_ns = expire_at_ns,
        .key = {reinterpret_cast<const std::byte*>(key.key.data()), key.key.size()},
        .value = value,
    };
    auto ref = append_record(input);
    if (!ref) {
        return unexpected(ref.error());
    }
    const auto segment = active_;
    // Litmus / post-append: Index publication crossed the append boundary.
    if (glifistore::fault::consume_fail(glifistore::fault::Site::index_account)) {
        return unexpected(
            Error{ErrorCode::unavailable, "injected volatile post-append index publication failure"});
    }
    if (auto visible = publish(key, *ref, *segment); !visible) {
        return unexpected(indeterminate_after_append(visible.error()));
    }
    return WorkerMutationPublication{.record = *ref, .segment = segment, .opcode = Opcode::put};
}

auto Worker::erase_locked(const HashedKey& key) -> Status {
    auto published = erase_locked_published(key);
    if (!published) {
        return unexpected(published.error());
    }
    return {};
}

auto Worker::erase_locked_published(const HashedKey& key) -> Result<WorkerMutationPublication> {
    ExclusiveHotPathGuard hot_path{exclusive_writer_ ? this : nullptr};
#ifndef NDEBUG
    if (exclusive_writer_) {
        std::unique_lock lock{mutex_, std::defer_lock};
        if (!lock.try_lock()) {
            lock.lock();
        }
        lock.unlock();
        assert(!lock.owns_lock());
    }
#endif
    if (exclusive_writer_ && index_quiesce_active_.load(std::memory_order_acquire)) {
        return fail(ErrorCode::sequence_conflict,
                    "volatile mutation conflicts with Index quiesce (compact/verify)");
    }
    const auto existing = index_.find(key);
    if (!existing) {
        return fail(ErrorCode::not_found, "key is not present");
    }
    auto* previous_segment = find_owned_segment(existing->segment_id);
    if (!previous_segment) {
        return fail(ErrorCode::invalid_reference,
                    "existing record targets a segment not owned by this worker");
    }
    if (auto valid = validate_live_record(*previous_segment, *existing); !valid) {
        return unexpected(valid.error());
    }
    const RecordInput input{
        .sequence = next_sequence(),
        .opcode = Opcode::erase,
        .key_hash = key.hash,
        .key = {reinterpret_cast<const std::byte*>(key.key.data()), key.key.size()},
    };
    auto ref = append_record(input);
    if (!ref) {
        return unexpected(ref.error());
    }
    if (auto dead = previous_segment->mark_dead(*existing); !dead) {
        return unexpected(indeterminate_after_append(dead.error()));
    }
    const auto mutation = index_.erase(key);
    if (mutation.previous != existing) {
        static_cast<void>(previous_segment->mark_live(*existing));
        return fail(ErrorCode::unavailable, "index changed during record removal");
    }
    const auto segment = active_;
    maybe_retire(*previous_segment);
    return WorkerMutationPublication{.record = *ref, .segment = segment, .opcode = Opcode::erase};
}

} // namespace glifistore
