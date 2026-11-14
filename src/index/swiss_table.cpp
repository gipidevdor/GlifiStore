#include "glifistore/index/swiss_table.hpp"

#include "glifistore/core/error.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/index/index_hash_seed.hpp"
#include "glifistore/index/swiss_control_group.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <span>

namespace glifistore {
namespace {

inline constexpr std::uint64_t kSwissMixConstant = 0x9E3779B97F4A7C15ULL;
inline constexpr std::size_t kHeapArenaCompactDeadThreshold = 65'536;
inline constexpr std::size_t kSwissMinimumDeletedRebuild = kSwissGroupSize;
} // namespace

SwissTableIndex::SwissTableIndex(const std::uint64_t seed) : SwissTableIndex(get_worker_routing(), seed) {}

SwissTableIndex::SwissTableIndex(const WorkerRoutingState routing, const std::uint64_t seed)
    : routing_(routing), seed_(seed) {
    capacity_ = kSwissGroupSize;
    control_.assign(capacity_, kSwissEmpty);
    slots_.resize(capacity_);
}

auto SwissTableIndex::normalize_capacity(const std::size_t minimum_slots) -> Result<std::size_t> {
    std::size_t capacity = kSwissGroupSize;
    while (capacity < minimum_slots) {
        if (capacity > std::numeric_limits<std::size_t>::max() / 2U) {
            return fail(ErrorCode::arithmetic_overflow, "swiss table capacity overflow");
        }
        capacity <<= 1U;
    }
    return capacity;
}

auto SwissTableIndex::mix_hash(const std::uint64_t key_hash) const noexcept -> std::uint64_t {
    std::uint64_t hash = key_hash;
    hash ^= seed_;
    hash *= kSwissMixConstant;
    hash ^= hash >> 33U;
    hash *= kSwissMixConstant;
    hash ^= hash >> 29U;
    return hash;
}

auto SwissTableIndex::h2(const std::uint64_t hash) noexcept -> std::uint8_t {
    const auto fingerprint = static_cast<std::uint8_t>(hash & 0x7FU);
    return fingerprint == 0 ? static_cast<std::uint8_t>(1) : fingerprint;
}

auto SwissTableIndex::probe_start(const std::uint64_t hash) const noexcept -> std::size_t {
    const auto group_index = (hash >> 7U) & ((capacity_ / kSwissGroupSize) - 1U);
    return group_index * kSwissGroupSize;
}

auto SwissTableIndex::next_group(const std::size_t group_start, const std::size_t capacity) noexcept
    -> std::size_t {
    const auto next = group_start + kSwissGroupSize;
    return next >= capacity ? next - capacity : next;
}

auto SwissTableIndex::effective_occupancy() const noexcept -> std::size_t {
    return size_ + deleted_count_;
}

auto SwissTableIndex::maximum_occupancy() const noexcept -> std::size_t {
    return capacity_ - capacity_ / 8U;
}

auto SwissTableIndex::tombstone_rebuild_beneficial() const noexcept -> bool {
    const auto threshold = std::max(kSwissMinimumDeletedRebuild, capacity_ / 4U);
    return deleted_count_ >= threshold && deleted_count_ > size_;
}

void SwissTableIndex::observe_probe(const std::size_t groups) const noexcept {
    if (groups > maximum_probe_groups_) {
        maximum_probe_groups_ = groups;
    }
}

auto SwissTableIndex::slot_key(const Slot& slot) const noexcept -> std::string_view {
    const auto key_size = slot.key_size();
    if (key_size == 0) {
        return {};
    }
    if (slot.key_is_inline()) {
        return {reinterpret_cast<const char*>(slot.key.inline_key), key_size};
    }
    return {reinterpret_cast<const char*>(heap_keys_.data(slot.key.heap_key_offset, key_size).data()),
            key_size};
}

auto SwissTableIndex::key_equals(const Slot& slot, const std::string_view key,
                                 const std::uint64_t key_hash) const noexcept -> bool {
    const auto key_size = slot.key_size();
    if (key_size != key.size() || slot.key_hash_tag != static_cast<std::uint32_t>(key_hash)) {
        return false;
    }
    if (key.empty()) {
        return true;
    }
    if (slot.key_is_inline()) {
        return std::memcmp(slot.key.inline_key, key.data(), key.size()) == 0;
    }
    const auto heap_key = heap_keys_.data(slot.key.heap_key_offset, key_size);
    if (heap_key.size() != key.size()) {
        return false;
    }
    return std::memcmp(heap_key.data(), key.data(), key.size()) == 0;
}

auto SwissTableIndex::find_slot_index(const std::string_view key, const std::uint64_t key_hash) const
    -> std::optional<std::size_t> {
    if (capacity_ == 0) {
        return std::nullopt;
    }
    const auto hash = mix_hash(key_hash);
    const auto fingerprint = h2(hash);
    auto group_start = probe_start(hash);
    for (std::size_t probe = 0; probe < capacity_; probe += kSwissGroupSize) {
        const auto probe_groups = probe / kSwissGroupSize + 1U;
        const auto control_word = detail::load_control_group64(&control_[group_start]);
        const auto fingerprint_mask = detail::equal_byte_mask(&control_[group_start], fingerprint);
        for (std::size_t offset = 0; offset < kSwissGroupSize; ++offset) {
            const auto control = detail::control_byte_at(control_word, offset);
            if (control == kSwissEmpty) {
                observe_probe(probe_groups);
                return std::nullopt;
            }
            if ((fingerprint_mask & (1ULL << offset)) != 0 &&
                key_equals(slots_[group_start + offset], key, key_hash)) {
                observe_probe(probe_groups);
                return group_start + offset;
            }
        }
        group_start = next_group(group_start, capacity_);
    }
    observe_probe(capacity_ / kSwissGroupSize);
    return std::nullopt;
}

auto SwissTableIndex::find_insert_index(const std::string_view key, const std::uint64_t key_hash,
                                        const std::uint64_t mixed_hash) -> Result<std::size_t> {
    if (auto grown = grow_if_needed(); !grown) {
        return unexpected(grown.error());
    }
    const auto fingerprint = h2(mixed_hash);
    auto group_start = probe_start(mixed_hash);
    std::optional<std::size_t> first_deleted;

    for (std::size_t probe = 0; probe < capacity_; probe += kSwissGroupSize) {
        const auto probe_groups = probe / kSwissGroupSize + 1U;
        const auto control_word = detail::load_control_group64(&control_[group_start]);
        const auto fingerprint_mask = detail::equal_byte_mask(&control_[group_start], fingerprint);
        for (std::size_t offset = 0; offset < kSwissGroupSize; ++offset) {
            const auto index = group_start + offset;
            const auto control = detail::control_byte_at(control_word, offset);
            if (control == kSwissEmpty) {
                observe_probe(probe_groups);
                return first_deleted.value_or(index);
            }
            if (control == kSwissDeleted && !first_deleted.has_value()) {
                first_deleted = index;
            }
            if ((fingerprint_mask & (1ULL << offset)) != 0 && key_equals(slots_[index], key, key_hash)) {
                observe_probe(probe_groups);
                return index;
            }
        }
        group_start = next_group(group_start, capacity_);
    }
    observe_probe(capacity_ / kSwissGroupSize);
    return fail(ErrorCode::corrupted_data, "swiss table probe sequence exhausted without insert slot");
}

auto SwissTableIndex::set_key(Slot& slot, const std::string_view key, const std::uint64_t key_hash)
    -> Status {
    if (key.size() >= Slot::kInlineKeyMask) {
        return fail(ErrorCode::invalid_argument, "index key exceeds packed metadata capacity");
    }
    const auto key_size = static_cast<std::uint32_t>(key.size());
    slot.key_hash_tag = static_cast<std::uint32_t>(key_hash);
    if (key.size() <= kSwissInlineKeyBytes) {
        slot.set_key_metadata(key_size, true);
        std::memset(slot.key.inline_key, 0, kSwissInlineKeyBytes);
        if (!key.empty()) {
            std::memcpy(slot.key.inline_key, key.data(), key.size());
        }
        return {};
    }
    slot.set_key_metadata(key_size, false);
    auto offset = heap_keys_.allocate(key.size());
    if (!offset) {
        slot = Slot{};
        return unexpected(offset.error());
    }
    slot.key.heap_key_offset = *offset;
    const auto destination = heap_keys_.data(slot.key.heap_key_offset, key.size());
    if (destination.size() != key.size()) {
        slot = Slot{};
        return fail(ErrorCode::corrupted_data, "index key arena allocation is out of range");
    }
    std::memcpy(const_cast<std::byte*>(destination.data()), key.data(), key.size());
    heap_live_bytes_ += key.size();
    return {};
}

auto SwissTableIndex::compact_heap_keys(const std::span<Slot> slots,
                                        const std::span<const std::uint8_t> control) -> Status {
    KeyArena compacted;
    std::vector<std::uint32_t> new_offsets(slots.size());
    std::size_t heap_bytes = 0;
    for (std::size_t index = 0; index < slots.size(); ++index) {
        const auto slot_control = control[index];
        if (slot_control == kSwissEmpty || slot_control == kSwissDeleted) {
            continue;
        }
        const auto& slot = slots[index];
        if (!slot.key_is_inline() && slot.key_size() > 0) {
            heap_bytes += slot.key_size();
        }
    }
    if (auto reserved = compacted.reserve(heap_bytes); !reserved) {
        return reserved;
    }
    for (std::size_t index = 0; index < slots.size(); ++index) {
        auto& slot = slots[index];
        const auto slot_control = control[index];
        const auto key_size = slot.key_size();
        if (slot_control == kSwissEmpty || slot_control == kSwissDeleted || slot.key_is_inline() ||
            key_size == 0) {
            continue;
        }
        const auto source = heap_keys_.data(slot.key.heap_key_offset, key_size);
        if (source.size() != key_size) {
            return fail(ErrorCode::corrupted_data, "heap key compaction source is out of range");
        }
        const auto offset = compacted.allocate(key_size);
        if (!offset) {
            return unexpected(offset.error());
        }
        const auto destination = compacted.data(*offset, key_size);
        if (destination.size() != key_size) {
            return fail(ErrorCode::corrupted_data, "heap key compaction destination is out of range");
        }
        std::memcpy(const_cast<std::byte*>(destination.data()), source.data(), key_size);
        new_offsets[index] = *offset;
    }
    for (std::size_t index = 0; index < slots.size(); ++index) {
        if (control[index] != kSwissEmpty && control[index] != kSwissDeleted &&
            !slots[index].key_is_inline() && slots[index].key_size() > 0) {
            slots[index].key.heap_key_offset = new_offsets[index];
        }
    }
    heap_keys_ = std::move(compacted);
    heap_dead_bytes_ = 0;
    heap_live_bytes_ = heap_keys_.allocated_bytes();
    return {};
}

auto SwissTableIndex::maybe_compact_heap_keys() -> Status {
    if (heap_dead_bytes_ == 0) {
        return {};
    }
    if (heap_live_bytes_ == 0) {
        heap_keys_.clear();
        heap_dead_bytes_ = 0;
        return {};
    }
    // Require both a meaningful absolute saving and at least 50% arena
    // fragmentation. Compacting at every fixed-size amount of dead storage
    // repeatedly recopies almost the entire live arena during an erase sweep.
    // The geometric trigger bounds total copied key bytes amortized over the
    // sweep while keeping dead storage below max(live bytes, threshold).
    if (heap_dead_bytes_ < kHeapArenaCompactDeadThreshold || heap_dead_bytes_ < heap_live_bytes_) {
        return {};
    }
    return compact_heap_keys(slots_, control_);
}

void SwissTableIndex::clear_slot(const std::size_t index) {
    const auto key_size = slots_[index].key_size();
    if (!slots_[index].key_is_inline() && key_size > 0) {
        if (heap_live_bytes_ >= key_size) {
            heap_live_bytes_ -= key_size;
        } else {
            heap_live_bytes_ = 0;
        }
        heap_dead_bytes_ += key_size;
    }
    control_[index] = kSwissDeleted;
    slots_[index] = Slot{};
    ++deleted_count_;
}

auto SwissTableIndex::grow_if_needed() -> Status {
    if (capacity_ == 0) {
        return {};
    }
    if (size_ > capacity_ || deleted_count_ > capacity_ - size_) {
        return fail(ErrorCode::corrupted_data, "swiss table occupancy counters exceed capacity");
    }
    if (tombstone_rebuild_beneficial()) {
        return rehash(capacity_, true);
    }
    if (effective_occupancy() < maximum_occupancy()) {
        return {};
    }
    if (size_ < maximum_occupancy() && deleted_count_ != 0) {
        return rehash(capacity_, true);
    }
    if (capacity_ > std::numeric_limits<std::size_t>::max() / 2U) {
        return fail(ErrorCode::arithmetic_overflow, "swiss table growth overflow");
    }
    return rehash(capacity_ << 1U);
}

auto SwissTableIndex::rehash(const std::size_t new_capacity, const bool force_same_capacity) -> Status {
    auto normalized = normalize_capacity(new_capacity);
    if (!normalized) {
        return unexpected(normalized.error());
    }
    if (*normalized == capacity_ && !force_same_capacity) {
        return {};
    }
    if (size_ > *normalized - *normalized / 8U) {
        return fail(ErrorCode::invalid_argument, "swiss table rehash capacity cannot hold live entries");
    }

    // Build complete independent state so any vector or key-arena allocation
    // failure leaves this table byte-for-byte authoritative.
    SwissTableIndex rebuilt;
    rebuilt.routing_ = routing_;
    rebuilt.seed_ = seed_;
    rebuilt.capacity_ = *normalized;
    rebuilt.size_ = 0;
    rebuilt.deleted_count_ = 0;
    rebuilt.control_.assign(*normalized, kSwissEmpty);
    rebuilt.slots_.clear();
    rebuilt.slots_.resize(*normalized);
    if (auto reserved = rebuilt.heap_keys_.reserve(heap_live_bytes_); !reserved) {
        return reserved;
    }
    for (std::size_t index = 0; index < capacity_; ++index) {
        if (control_[index] == kSwissEmpty || control_[index] == kSwissDeleted) {
            continue;
        }
        const auto key = slot_key(slots_[index]);
        auto inserted =
            rebuilt.insert_or_assign(HashedKey{key, hash_key_routing(key, routing_)}, slots_[index].ref);
        if (!inserted) {
            return unexpected(inserted.error());
        }
        if (!inserted->inserted) {
            return fail(ErrorCode::corrupted_data, "swiss table rebuild encountered a duplicate key");
        }
    }
    const auto same_capacity = *normalized == capacity_;
    capacity_ = *normalized;
    size_ = rebuilt.size_;
    deleted_count_ = 0;
    heap_live_bytes_ = rebuilt.heap_live_bytes_;
    heap_dead_bytes_ = 0;
    control_ = std::move(rebuilt.control_);
    slots_ = std::move(rebuilt.slots_);
    heap_keys_ = std::move(rebuilt.heap_keys_);
    maximum_probe_groups_ = std::max(maximum_probe_groups_, rebuilt.maximum_probe_groups_);
    ++rehash_count_;
    if (same_capacity) {
        ++tombstone_rebuild_count_;
    }
    return {};
}

auto SwissTableIndex::find(const std::string_view key) const -> std::optional<RecordRef> {
    return find(HashedKey{key, hash_key_routing(key, routing_)});
}

auto SwissTableIndex::find(const HashedKey& key) const -> std::optional<RecordRef> {
    const auto index = find_slot_index(key.key, key.hash);
    if (!index) {
        return std::nullopt;
    }
    return slots_[*index].ref;
}

auto SwissTableIndex::insert_or_assign(const std::string_view key, RecordRef ref)
    -> Result<IndexMutationResult> {
    return insert_or_assign(HashedKey{key, hash_key_routing(key, routing_)}, ref);
}

auto SwissTableIndex::insert_or_assign(const HashedKey& key, RecordRef ref) -> Result<IndexMutationResult> {
    if (key.key.size() >= Slot::kInlineKeyMask) {
        return fail(ErrorCode::invalid_argument, "index key exceeds supported size");
    }
    const auto mixed_hash = mix_hash(key.hash);
    auto slot_index = find_insert_index(key.key, key.hash, mixed_hash);
    if (!slot_index) {
        return unexpected(slot_index.error());
    }
    const auto index = *slot_index;
    const auto fingerprint = h2(mixed_hash);
    if (control_[index] == kSwissEmpty || control_[index] == kSwissDeleted) {
        const auto previous_control = control_[index];
        if (auto key_set = set_key(slots_[index], key.key, key.hash); !key_set) {
            return unexpected(key_set.error());
        }
        slots_[index].ref = ref;
        control_[index] = fingerprint;
        if (previous_control == kSwissDeleted) {
            --deleted_count_;
        }
        ++size_;
        return IndexMutationResult{.inserted = true, .previous = std::nullopt};
    }
    const auto previous = slots_[index].ref;
    slots_[index].ref = ref;
    return IndexMutationResult{.inserted = false, .previous = previous};
}

auto SwissTableIndex::replace_if_current(const HashedKey& key, const RecordRef& expected,
                                         const RecordRef replacement) -> bool {
    const auto index = find_slot_index(key.key, key.hash);
    if (!index || slots_[*index].ref != expected) {
        return false;
    }
    slots_[*index].ref = replacement;
    return true;
}

auto SwissTableIndex::erase(const std::string_view key) -> IndexMutationResult {
    return erase(HashedKey{key, hash_key_routing(key, routing_)});
}

auto SwissTableIndex::erase(const HashedKey& key) -> IndexMutationResult {
    auto result = erase_no_compact(key);
    if (result.previous) {
        (void)maybe_compact_heap_keys();
    }
    return result;
}

auto SwissTableIndex::erase_no_compact(const HashedKey& key) -> IndexMutationResult {
    const auto index = find_slot_index(key.key, key.hash);
    if (!index) {
        return {};
    }
    const auto previous = slots_[*index].ref;
    clear_slot(*index);
    --size_;
    return {.inserted = false, .previous = previous};
}

auto SwissTableIndex::prepare_insert(const HashedKey& key) -> Status {
    if (key.key.size() >= Slot::kInlineKeyMask) {
        return fail(ErrorCode::invalid_argument, "index key exceeds supported size");
    }
    if (find_slot_index(key.key, key.hash)) {
        return {};
    }
    if (size_ == std::numeric_limits<std::size_t>::max()) {
        return fail(ErrorCode::arithmetic_overflow, "swiss table size overflow");
    }
    if (auto prepared = grow_if_needed(); !prepared) {
        return prepared;
    }
    if (key.key.size() > kSwissInlineKeyBytes) {
        return heap_keys_.prepare_allocate(key.key.size());
    }
    return {};
}

auto SwissTableIndex::prepare_batch_insert(const std::size_t additional_entries,
                                           const std::size_t additional_heap_key_bytes) -> Status {
    if (additional_entries > std::numeric_limits<std::size_t>::max() - size_) {
        return fail(ErrorCode::arithmetic_overflow, "swiss table batch reserve overflow");
    }
    if (auto prepared = reserve(size_ + additional_entries); !prepared) {
        return prepared;
    }
    return heap_keys_.prepare_allocate(additional_heap_key_bytes);
}

auto SwissTableIndex::reserve(const std::size_t count) -> Status {
    if (size_ > capacity_ || deleted_count_ > capacity_ - size_) {
        return fail(ErrorCode::corrupted_data, "swiss table occupancy counters exceed capacity");
    }
    const auto extra = count / 7U + (count % 7U == 0 ? 0U : 1U);
    if (extra > std::numeric_limits<std::size_t>::max() - count) {
        return fail(ErrorCode::arithmetic_overflow, "swiss table reserve overflow");
    }
    const auto minimum = count + extra;
    if (capacity_ == 0) {
        auto normalized = normalize_capacity(std::max(minimum, kSwissGroupSize));
        if (!normalized) {
            return unexpected(normalized.error());
        }
        capacity_ = *normalized;
        control_.assign(capacity_, kSwissEmpty);
        slots_.resize(capacity_);
        return {};
    }
    auto needed = normalize_capacity(std::max(minimum, kSwissGroupSize));
    if (!needed) {
        return unexpected(needed.error());
    }
    if (*needed > capacity_) {
        return rehash(*needed);
    }
    if (count > size_) {
        const auto additional = count - size_;
        const auto occupancy = effective_occupancy();
        const bool may_exhaust_empty =
            occupancy > maximum_occupancy() || additional > maximum_occupancy() - occupancy;
        if (may_exhaust_empty || tombstone_rebuild_beneficial()) {
            return rehash(capacity_, true);
        }
    }
    return {};
}

auto SwissTableIndex::entries() const -> std::vector<IndexEntry> {
    std::vector<IndexEntry> result;
    result.reserve(size_);
    for (std::size_t index = 0; index < capacity_; ++index) {
        const auto control = control_[index];
        if (control == kSwissEmpty || control == kSwissDeleted) {
            continue;
        }
        result.push_back({std::string{slot_key(slots_[index])}, slots_[index].ref});
    }
    return result;
}

auto SwissTableIndex::stats() const noexcept -> IndexStats {
    return {size_,
            capacity_,
            deleted_count_,
            capacity_ == 0 ? 0.0F : static_cast<float>(size_) / static_cast<float>(capacity_),
            capacity_ == 0 ? 0.0F : static_cast<float>(effective_occupancy()) / static_cast<float>(capacity_),
            heap_keys_.allocated_bytes(),
            heap_live_bytes_,
            sizeof(Slot),
            slots_.capacity() * sizeof(Slot) + control_.capacity() * sizeof(std::uint8_t),
            maximum_probe_groups_,
            rehash_count_,
            tombstone_rebuild_count_};
}

auto SwissTableIndex::clone_empty() const -> SwissTableIndex {
    return SwissTableIndex{routing_, seed_};
}

} // namespace glifistore
