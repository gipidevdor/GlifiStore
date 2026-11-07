#include "glifistore/core/fault_injection.hpp"
#include "glifistore/core/integer_math.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/index/swiss_table.hpp"
#include "glifistore/persistence/compaction.hpp"
#include "glifistore/persistence/durable_flush_coordinator.hpp"
#include "glifistore/persistence/resource_limits.hpp"
#include "glifistore/persistence/runtime_catalog.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "glifistore/segment/record.hpp"
#include "persistence/adaptive_batch_sizer.hpp"
#include "persistence/hot_record_table.hpp"
#include "persistence/runtime_catalog_detail.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>

namespace glifistore {
using namespace glifistore::runtime_catalog_detail;

auto DurableRuntimeCatalog::compact_worker(const std::size_t worker_index, const std::uint64_t now_ns,
                                           const std::uint64_t max_copy_bytes,
                                           const std::uint64_t max_copy_bytes_per_second)
    -> DurableCompactionResult {
    bool recovery_required{};
    DurableCompactionCopyStats stats{};
    const auto notify_fail_closed = [&] {
        healthy_.store(false, std::memory_order_release);
        for (const auto& worker : workers_) {
            worker->batch_closed.notify_all();
        }
    };
    const auto failure = [&](Error error) {
        if (recovery_required) {
            notify_fail_closed();
        }
        return DurableCompactionResult{
            .outcome = recovery_required ? DurableCompactionOutcome::recovery_required
                                         : DurableCompactionOutcome::not_compacted,
            .stats = stats,
            .error = std::move(error),
        };
    };

    try {
        if (worker_index >= workers_.size()) {
            return failure(Error{ErrorCode::invalid_argument, "Worker index is outside the durable runtime"});
        }
        if (!healthy()) {
            return failure(Error{ErrorCode::unavailable, "durable runtime is fail-closed"});
        }

        auto& worker = *workers_[worker_index];
        Manifest snapshot;
        SequenceNumber snapshot_next_sequence{};
        std::vector<IndexEntry> snapshot_entries;
        std::vector<std::shared_ptr<const RuntimeSegmentGeneration>> source_pins;

        // Phase A: capture only owning state. No file operation is allowed in
        // this scope. The complete Index enumeration is currently necessary
        // because the replacement Index must retain active-Segment references.
        //
        // Exclusive durable_sync elides worker.mutex on mutate, so the mutex alone
        // does not own the Index. ExclusiveIndexQuiesce arms the gate and drains
        // hot_path_depth before enumerating; clear before Phase B so ordinary
        // mutations continue during the unlocked build (Phase C re-arms for the
        // Index swap).
        {
            const bool elide_worker_mutex = options_.exclusive_writer && flusher_ == nullptr;
            ExclusiveIndexQuiesce snapshot_gate{worker, elide_worker_mutex};

            std::unique_lock worker_lock{worker.mutex};
            if (worker.mutation_io_active) {
                return failure(Error{ErrorCode::sequence_conflict,
                                     "durable compaction conflicts with active mutation I/O"});
            }
            if (dedicated_commit_executor_) {
                worker.batch_closed.wait(worker_lock, [&] { return !worker.batch_closing || !healthy(); });
            }
            const std::shared_lock catalog_lock{catalog_mutex_};
            if (!healthy()) {
                return failure(Error{ErrorCode::unavailable, "durable runtime is fail-closed"});
            }
            if (!worker.pending_group_mutations.empty() || worker.batch_closing) {
                return failure(Error{ErrorCode::sequence_conflict,
                                     "durable compaction cannot snapshot a pending group publication"});
            }
            const auto sealed_live_record_bytes =
                worker.sealed_live_record_bytes.load(std::memory_order_relaxed);
            if (max_copy_bytes > 0 && sealed_live_record_bytes > max_copy_bytes) {
                return failure(Error{ErrorCode::sequence_conflict,
                                     "durable compaction candidate exceeds its maintenance copy budget"});
            }
            snapshot = manifest_;
            snapshot_next_sequence = worker.next_sequence;
            const auto index_size = worker.index.stats().size;
            snapshot_entries = worker.index.entries();
            if (snapshot_entries.size() != index_size || generation_pins_.size() != segments_.size() ||
                segments_.size() != snapshot.segments.size()) {
                return failure(
                    Error{ErrorCode::corrupted_data, "durable compaction snapshot is not catalog-aligned"});
            }
            for (std::size_t index = 0; index < snapshot.segments.size(); ++index) {
                const auto& entry = snapshot.segments[index];
                if (entry.owner_worker != worker.worker_id || entry.role != ManifestSegmentRole::sealed) {
                    continue;
                }
                const auto& pin = generation_pins_[index];
                if (!pin || pin->identity.segment_id != entry.segment_id ||
                    pin->identity.generation != entry.generation ||
                    pin->identity.owner_worker != entry.owner_worker) {
                    return failure(Error{ErrorCode::corrupted_data,
                                         "durable compaction source has no matching generation pin"});
                }
                source_pins.push_back(pin);
            }
            // Drop the snapshot gate before Phase B so mutates proceed during build.
            snapshot_gate.clear();
        }

        if (source_pins.empty()) {
            return failure(Error{ErrorCode::not_found, "durable Worker has no sealed Segments to compact"});
        }

        // Phase B performs the exact scan, replacement Index construction and
        // the complete staged output copy/seal/verification without a global
        // publication lease. The builder invokes this gate immediately before
        // the durable intent; from that boundary through transaction cleanup,
        // recovery v1 requires the exact old/next authority pair to remain
        // exclusive.
        bool publication_lease_active{};
        std::optional<std::chrono::steady_clock::time_point> publication_lease_started;
        const auto release_publication_lease = [&]() noexcept {
            if (!publication_lease_active) {
                return;
            }
            {
                const std::lock_guard lock{manifest_publication_mutex_};
                compaction_publication_active_ = false;
                publication_lease_active = false;
            }
            if (publication_lease_started) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - *publication_lease_started)
                                         .count();
                stats.publication_lease_duration_ns = elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
            }
            manifest_publication_changed_.notify_all();
        };
        ScopeExit publication_lease{release_publication_lease};

        struct IntentGateContext final {
            DurableRuntimeCatalog& runtime;
            const Manifest& snapshot;
            bool& active;
            std::optional<std::chrono::steady_clock::time_point>& started;
        } intent_gate_context{.runtime = *this,
                              .snapshot = snapshot,
                              .active = publication_lease_active,
                              .started = publication_lease_started};

        const DurableCompactionIntentGate intent_gate{
            .context = &intent_gate_context,
            .acquire = [](void* opaque) -> Status {
                auto& gate = *static_cast<IntentGateContext*>(opaque);
                std::lock_guard publication_lock{gate.runtime.manifest_publication_mutex_};
                if (gate.runtime.compaction_publication_active_ ||
                    gate.runtime.rotation_publication_active_) {
                    return fail(ErrorCode::sequence_conflict,
                                "another durable manifest publication is active");
                }
                const std::shared_lock catalog_lock{gate.runtime.catalog_mutex_};
                if (!gate.runtime.healthy() || gate.runtime.manifest_ != gate.snapshot) {
                    return fail(ErrorCode::sequence_conflict,
                                "manifest changed before compaction intent publication");
                }
                gate.runtime.compaction_publication_active_ = true;
                gate.active = true;
                gate.started = std::chrono::steady_clock::now();
                return {};
            },
        };
        auto built = build_durable_worker_compaction(
            directory_, snapshot, worker.worker_id, std::move(snapshot_entries), now_ns, options_.limits,
            intent_gate, {.bytes_per_second = max_copy_bytes_per_second});
        if (!built.succeeded()) {
            stats = built.stats;
            if (built.outcome == DurableCompactionBuildOutcome::not_beneficial) {
                return {.outcome = DurableCompactionOutcome::not_beneficial,
                        .stats = built.stats,
                        .error = std::move(built.error)};
            }
            recovery_required = built.outcome == DurableCompactionBuildOutcome::recovery_required;
            return failure(built.error.value_or(
                Error{ErrorCode::io_error, "durable compaction replacement build failed"}));
        }
        if (!built.prepared) {
            return failure(
                Error{ErrorCode::internal_error, "successful compaction build has no prepared state"});
        }
        recovery_required = true;
        auto prepared = std::move(*built.prepared);
        stats = prepared.stats;
        auto sources = std::move(prepared.plan.sources);
        const auto abort_prepared = [&](Error reason) -> DurableCompactionResult {
            auto rolled_back = rollback_prepared_compaction(directory_, snapshot, prepared.plan.replacements,
                                                            options_.limits);
            if (!rolled_back) {
                return failure(rolled_back.error());
            }
            {
                const std::unique_lock catalog_lock{catalog_mutex_};
                namespace_audit_ = std::move(*rolled_back);
            }
            recovery_required = false;
            return failure(std::move(reason));
        };
        if (prepared.replacement_commits.size() != prepared.plan.replacements.size()) {
            return abort_prepared(
                Error{ErrorCode::internal_error, "compaction replacement commit catalog is incomplete"});
        }
        std::vector<RecoveredSegmentState> next_segments;
        next_segments.reserve(prepared.plan.next_manifest.segments.size());
        std::vector<std::shared_ptr<const RuntimeSegmentGeneration>> replacement_pins;
        replacement_pins.reserve(prepared.plan.replacements.size());
        for (std::size_t index = 0; index < prepared.plan.replacements.size(); ++index) {
            const auto& entry = prepared.plan.replacements[index];
            const SegmentHeaderIdentity identity{.store_id = snapshot.store_id,
                                                 .segment_id = entry.segment_id,
                                                 .generation = entry.generation,
                                                 .owner_worker = entry.owner_worker};
            auto opened = DurableSegmentFile::open(directory_, identity, SegmentFileOpenMode::read_only);
            if (!opened) {
                return abort_prepared(opened.error());
            }
            if (opened->selected_commit() != prepared.replacement_commits[index]) {
                return abort_prepared(
                    Error{ErrorCode::corrupted_data,
                          "compaction replacement changed before generation pin publication"});
            }
            replacement_pins.push_back(
                std::make_shared<const RuntimeSegmentGeneration>(RuntimeSegmentGeneration{
                    .identity = identity,
                    .selected = prepared.replacement_commits[index],
                    .file = std::move(*opened),
                }));
        }
        std::vector<std::shared_ptr<const RuntimeSegmentGeneration>> next_generation_pins;
        next_generation_pins.reserve(prepared.plan.next_manifest.segments.size());
        std::vector<std::size_t> retained_old_indices;
        retained_old_indices.reserve(prepared.plan.next_manifest.segments.size());
        auto installed_manifest = prepared.plan.next_manifest;
        auto next_pin_slots = prepare_pin_slot_index(prepared.plan.next_manifest);
        if (!next_pin_slots) {
            return abort_prepared(next_pin_slots.error());
        }

        // Phase C first validates and prepares a non-allocating publication
        // while locked. The durable manifest write happens only after the
        // target Worker is logically quiesced and every physical mutex has
        // been released.
        std::unique_lock worker_lock{worker.mutex, std::try_to_lock};
        if (!worker_lock.owns_lock()) {
            return abort_prepared(
                Error{ErrorCode::sequence_conflict, "Worker changed while compaction was prepared"});
        }
        std::unique_lock catalog_lock{catalog_mutex_};
        bool sources_still_pinned = source_pins.size() == sources.size();
        for (std::size_t source_index = 0; sources_still_pinned && source_index < sources.size();
             ++source_index) {
            const auto found = std::lower_bound(manifest_.segments.begin(), manifest_.segments.end(),
                                                sources[source_index].segment_id,
                                                [](const ManifestSegmentEntry& entry, const SegmentId id) {
                                                    return entry.segment_id.value < id.value;
                                                });
            if (found == manifest_.segments.end() || *found != sources[source_index]) {
                sources_still_pinned = false;
                break;
            }
            const auto catalog_index = static_cast<std::size_t>(found - manifest_.segments.begin());
            sources_still_pinned = catalog_index < generation_pins_.size() &&
                                   generation_pins_[catalog_index] == source_pins[source_index];
        }
        if (!healthy() || manifest_ != snapshot || segments_.size() != snapshot.segments.size() ||
            worker.next_sequence != snapshot_next_sequence || !worker.pending_group_mutations.empty() ||
            worker.batch_closing || !sources_still_pinned) {
            catalog_lock.unlock();
            worker_lock.unlock();
            return abort_prepared(
                Error{ErrorCode::sequence_conflict, "runtime state changed during durable compaction"});
        }

        {
            std::size_t replacement_index{};
            for (const auto& entry : prepared.plan.next_manifest.segments) {
                if (replacement_index < prepared.plan.replacements.size() &&
                    entry == prepared.plan.replacements[replacement_index]) {
                    next_segments.push_back({.selected = prepared.replacement_commits[replacement_index]});
                    next_generation_pins.push_back(replacement_pins[replacement_index]);
                    retained_old_indices.push_back(std::numeric_limits<std::size_t>::max());
                    ++replacement_index;
                    continue;
                }
                const auto found =
                    std::lower_bound(snapshot.segments.begin(), snapshot.segments.end(), entry.segment_id,
                                     [](const ManifestSegmentEntry& candidate, const SegmentId id) {
                                         return candidate.segment_id.value < id.value;
                                     });
                if (found == snapshot.segments.end() || *found != entry) {
                    return failure(Error{ErrorCode::corrupted_data,
                                         "compaction retained entry is absent from the old catalog"});
                }
                const auto old_index = static_cast<std::size_t>(found - snapshot.segments.begin());
                next_segments.push_back(segments_[old_index]);
                if (old_index >= generation_pins_.size() || !generation_pins_[old_index]) {
                    return failure(Error{ErrorCode::corrupted_data,
                                         "retained compaction Segment has no generation pin"});
                }
                next_generation_pins.push_back(generation_pins_[old_index]);
                retained_old_indices.push_back(old_index);
            }
            if (replacement_index != prepared.plan.replacements.size() ||
                next_segments.size() != prepared.plan.next_manifest.segments.size() ||
                next_generation_pins.size() != prepared.plan.next_manifest.segments.size() ||
                retained_old_indices.size() != prepared.plan.next_manifest.segments.size()) {
                return failure(
                    Error{ErrorCode::internal_error, "compaction runtime catalog preparation is incomplete"});
            }
        }

        struct WorkerCommitGate final {
            RuntimeWorker& worker;
            bool active{true};

            explicit WorkerCommitGate(RuntimeWorker& owner) noexcept : worker(owner) {
                worker.arm_index_quiesce_gate();
            }

            ~WorkerCommitGate() {
                if (!active) {
                    return;
                }
                const std::lock_guard lock{worker.mutex};
                worker.disarm_index_quiesce_gate();
            }

            void clear_locked() noexcept {
                worker.disarm_index_quiesce_gate();
                active = false;
            }

            WorkerCommitGate(const WorkerCommitGate&) = delete;
            auto operator=(const WorkerCommitGate&) -> WorkerCommitGate& = delete;
        } commit_gate{worker};

        // Release every physical mutex before quiesce waits and the durable
        // manifest write (durable-compaction.md). Holding catalog/worker across
        // hot_path_depth wait deadlocks the exclusive Writer: mutate already
        // entered the hot path, dropped catalog for append I/O, and needs shared
        // catalog again to publish Index / drop depth.
        catalog_lock.unlock();
        worker_lock.unlock();

        // Depth wait matches ExclusiveHotPathGuard: only the mutex-elided exclusive
        // Writer path increments hot_path_depth. Flusher paths keep the Worker mutex.
        if (options_.exclusive_writer && flusher_ == nullptr) {
            for (;;) {
                const auto depth = worker.hot_path_depth.load(std::memory_order_acquire);
                if (depth == 0) {
                    break;
                }
                worker.hot_path_depth.wait(depth, std::memory_order_acquire);
            }
        }

        worker_lock.lock();
        worker.mutation_io_finished.wait(worker_lock,
                                         [&] { return !worker.mutation_io_active || !healthy(); });
        if (!healthy()) {
            commit_gate.clear_locked();
            worker_lock.unlock();
            return abort_prepared(Error{ErrorCode::unavailable, "durable runtime is fail-closed"});
        }
        catalog_lock.lock();
        sources_still_pinned = source_pins.size() == sources.size();
        for (std::size_t source_index = 0; sources_still_pinned && source_index < sources.size();
             ++source_index) {
            const auto found = std::lower_bound(manifest_.segments.begin(), manifest_.segments.end(),
                                                sources[source_index].segment_id,
                                                [](const ManifestSegmentEntry& entry, const SegmentId id) {
                                                    return entry.segment_id.value < id.value;
                                                });
            if (found == manifest_.segments.end() || *found != sources[source_index]) {
                sources_still_pinned = false;
                break;
            }
            const auto catalog_index = static_cast<std::size_t>(found - manifest_.segments.begin());
            sources_still_pinned = catalog_index < generation_pins_.size() &&
                                   generation_pins_[catalog_index] == source_pins[source_index];
        }
        if (!healthy() || worker.mutation_io_active || manifest_ != snapshot ||
            segments_.size() != snapshot.segments.size() || worker.next_sequence != snapshot_next_sequence ||
            !worker.pending_group_mutations.empty() || worker.batch_closing || !sources_still_pinned) {
            catalog_lock.unlock();
            commit_gate.clear_locked();
            worker_lock.unlock();
            return abort_prepared(
                Error{ErrorCode::sequence_conflict, "runtime state changed during durable compaction"});
        }
        catalog_lock.unlock();
        worker_lock.unlock();

        const auto published =
            directory_.publish_manifest(prepared.plan.next_manifest, options_.limits.max_manifest_bytes);
        if (!published.durable()) {
            return failure(published.error.value_or(
                Error{ErrorCode::io_error, "compaction manifest publication failed"}));
        }

        try {
            worker_lock.lock();
            catalog_lock.lock();
            for (std::size_t next_index = 0; next_index < retained_old_indices.size(); ++next_index) {
                const auto old_index = retained_old_indices[next_index];
                if (old_index == std::numeric_limits<std::size_t>::max()) {
                    continue;
                }
                next_segments[next_index] = segments_[old_index];
                next_generation_pins[next_index] = generation_pins_[old_index];
            }
            manifest_ = std::move(prepared.plan.next_manifest);
            segments_ = std::move(next_segments);
            generation_pins_ = std::move(next_generation_pins);
            pin_slot_by_segment_id_ = std::move(*next_pin_slots);
            worker.index = std::move(prepared.index);
            worker.active_live_record_bytes.store(prepared.active_live_record_bytes,
                                                  std::memory_order_release);
            worker.sealed_live_record_bytes.store(stats.bytes_copied, std::memory_order_release);
            if (!advance_read_catalog_revision(worker)) {
                throw std::overflow_error{"durable read catalog revision exhausted"};
            }
            commit_gate.clear_locked();
        } catch (...) {
            if (worker_lock.owns_lock()) {
                commit_gate.clear_locked();
            }
            if (catalog_lock.owns_lock()) {
                catalog_lock.unlock();
            }
            if (worker_lock.owns_lock()) {
                worker_lock.unlock();
            }
            throw;
        }
        catalog_lock.unlock();
        worker_lock.unlock();

        const auto retired = directory_.retire_compaction_segments(snapshot.store_id, sources);
        if (!retired.durable()) {
            return failure(
                retired.error.value_or(Error{ErrorCode::io_error, "compaction source retirement failed"}));
        }
        const auto removed = directory_.remove_compaction_intent();
        if (!removed.durable()) {
            return failure(
                removed.error.value_or(Error{ErrorCode::io_error, "compaction intent removal failed"}));
        }
        auto audit = audit_data_directory(directory_, installed_manifest);
        if (!audit) {
            return failure(audit.error());
        }
        if (auto safe = validate_namespace_for_recovery(*audit); !safe) {
            return failure(safe.error());
        }
        {
            const std::unique_lock audit_lock{catalog_mutex_};
            namespace_audit_ = std::move(*audit);
        }
        recovery_required = false;
        release_publication_lease();
        return {.outcome = DurableCompactionOutcome::compacted, .stats = stats, .error = std::nullopt};
    } catch (const std::bad_alloc&) {
        return failure(Error{ErrorCode::resource_exhausted, {}});
    } catch (...) {
        return failure(Error{ErrorCode::internal_error, {}});
    }
}

} // namespace glifistore
