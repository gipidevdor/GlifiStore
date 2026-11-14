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
    std::uint64_t snapshot_duration_ns{};
    std::optional<std::chrono::steady_clock::time_point> commit_gate_started;
    bool completed{};
    ScopeExit publish_metrics{[&] {
        atomic_saturating_add(compaction_attempts_, 1);
        atomic_saturating_add(compactions_completed_, completed ? 1 : 0);
        atomic_saturating_add(compaction_manifest_conflicts_, stats.manifest_conflict_aborts);
        atomic_saturating_add(compaction_abandoned_copy_bytes_, stats.abandoned_copy_bytes);
        if (completed) {
            atomic_saturating_add(compaction_remapped_records_, stats.remapped_records);
            atomic_saturating_add(compaction_superseded_copies_, stats.superseded_copies);
            atomic_saturating_add(compaction_manifest_rebases_, stats.manifest_rebases);
        }
        atomic_observe_max(maximum_compaction_snapshot_ns_, snapshot_duration_ns);
        atomic_observe_max(maximum_compaction_commit_gate_ns_, stats.commit_gate_duration_ns);
    }};
    const auto notify_fail_closed = [&] {
        healthy_.store(false, std::memory_order_release);
        for (const auto& worker : workers_) {
            worker->batch_closed.notify_all();
        }
    };
    const auto failure = [&](Error error) {
        stats.snapshot_duration_ns = snapshot_duration_ns;
        if (commit_gate_started && stats.commit_gate_duration_ns == 0) {
            stats.commit_gate_duration_ns = steady_elapsed_ns(*commit_gate_started);
        }
        if (!recovery_required) {
            stats.abandoned_copy_bytes = stats.bytes_copied;
        }
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
        const bool elide_worker_mutex = options_.exclusive_writer && flusher_ == nullptr;
        Manifest snapshot;
        std::vector<IndexEntry> snapshot_entries;
        // Exact generation pins of the Worker's sealed Segments, in Manifest order.
        std::vector<std::shared_ptr<const RuntimeSegmentGeneration>> source_pins;
        // Image bytes of each source pin, captured under the catalog lock (ADR 0046 credit).
        std::vector<std::uint64_t> source_image_bytes;
        bool ownership_claimed{};
        ScopeExit release_ownership{[&] {
            if (ownership_claimed) {
                const std::lock_guard lock{worker.mutex};
                worker.compaction_active = false;
            }
        }};
        const auto snapshot_started = std::chrono::steady_clock::now();

        // Phase A: capture only owning state. No file operation is allowed in
        // this scope. Exclusive durable_sync elides worker.mutex on mutate, so the
        // mutex alone does not own the Index: ExclusiveIndexQuiesce arms the gate
        // and drains its Writer depth before enumerating, and disarms before
        // Phase B so mutations continue during the unlocked build.
        {
            ExclusiveIndexQuiesce snapshot_gate{worker, elide_worker_mutex};

            std::unique_lock worker_lock{worker.mutex};
            if (worker.compaction_active) {
                return failure(Error{ErrorCode::sequence_conflict, "another compaction owns the Worker"});
            }
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
            // A pending group has appended Records whose Index publication is
            // still to come. They live in the active Segment, never in a sealed
            // prefix, but a snapshot taken now would not see them as live.
            if (!worker.pending_group_mutations.empty() || worker.batch_closing) {
                return failure(Error{ErrorCode::sequence_conflict,
                                     "durable compaction cannot snapshot a pending group publication"});
            }
            snapshot = manifest_;
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
                source_image_bytes.push_back(pin->image ? pin->image->charged_bytes() : 0U);
            }
            worker.compaction_active = true;
            ownership_claimed = true;
            snapshot_gate.clear_locked();
        }
        snapshot_duration_ns = steady_elapsed_ns(snapshot_started);

        // ADR 0044: choose the oldest sealed prefix that reclaims the most
        // Segments within the copy budget, then keep only its references.
        const auto choice =
            select_durable_compaction_prefix(snapshot, worker.worker_id, snapshot_entries, max_copy_bytes);
        if (!choice) {
            return failure(choice.error());
        }
        if (choice->source_count == 0) {
            const bool has_sealed =
                std::ranges::any_of(snapshot.segments, [&](const ManifestSegmentEntry& entry) {
                    return entry.owner_worker == worker.worker_id &&
                           entry.role == ManifestSegmentRole::sealed;
                });
            if (!has_sealed) {
                return failure(
                    Error{ErrorCode::not_found, "durable Worker has no sealed Segments to compact"});
            }
            stats.snapshot_duration_ns = snapshot_duration_ns;
            return {.outcome = DurableCompactionOutcome::not_beneficial,
                    .stats = stats,
                    .error = Error{ErrorCode::not_found,
                                   "the oldest sealed Segment alone exceeds the maintenance copy budget"}};
        }
        snapshot_entries = durable_compaction_prefix_entries(snapshot, worker.worker_id, choice->source_count,
                                                             std::move(snapshot_entries));

        if (source_pins.size() < choice->source_count) {
            return failure(
                Error{ErrorCode::internal_error, "durable compaction prefix pin set is incomplete"});
        }
        source_pins.resize(choice->source_count);
        source_image_bytes.resize(choice->source_count);
        std::uint64_t source_image_credit_bytes{};
        for (const auto bytes : source_image_bytes) {
            source_image_credit_bytes += bytes;
        }

        // Phase B performs the exact scan and the complete staged output
        // copy/seal/verification without a global publication lease. The
        // builder invokes this gate immediately before the durable intent; from
        // that boundary through transaction cleanup, recovery v1 requires the
        // exact old/next authority pair to remain exclusive.
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
                stats.publication_lease_duration_ns = steady_elapsed_ns(*publication_lease_started);
            }
            manifest_publication_changed_.notify_all();
        };
        ScopeExit publication_lease{release_publication_lease};

        struct IntentGateContext final {
            DurableRuntimeCatalog& runtime;
            bool& active;
            std::optional<std::chrono::steady_clock::time_point>& started;
        } intent_gate_context{
            .runtime = *this, .active = publication_lease_active, .started = publication_lease_started};

        const DurableCompactionIntentGate intent_gate{
            .context = &intent_gate_context,
            // Returns the current authority; the builder rebases the identical
            // prefix plan onto it when unrelated rotations advanced it.
            .acquire = [](void* opaque) -> Result<Manifest> {
                auto& gate = *static_cast<IntentGateContext*>(opaque);
                std::lock_guard publication_lock{gate.runtime.manifest_publication_mutex_};
                if (gate.runtime.compaction_publication_active_ ||
                    gate.runtime.rotation_publication_active_) {
                    return fail(ErrorCode::sequence_conflict,
                                "another durable manifest publication is active");
                }
                const std::shared_lock catalog_lock{gate.runtime.catalog_mutex_};
                if (!gate.runtime.healthy()) {
                    return fail(ErrorCode::sequence_conflict,
                                "durable runtime closed before compaction intent publication");
                }
                auto authority = gate.runtime.manifest_;
                gate.runtime.compaction_publication_active_ = true;
                gate.active = true;
                gate.started = std::chrono::steady_clock::now();
                return authority;
            },
        };
        auto built = build_durable_worker_compaction(
            directory_, snapshot, worker.worker_id, std::move(snapshot_entries), options_.limits,
            {.source_count = choice->source_count,
             .now_ns = now_ns,
             .intent_gate = intent_gate,
             .pacing = {.bytes_per_second = max_copy_bytes_per_second},
             .image_reserve = options_.limits.resident_reads_enabled ? memory_reserve_ : nullptr,
             .image_credit_bytes = source_image_credit_bytes});
        if (!built.succeeded()) {
            stats = built.stats;
            stats.snapshot_duration_ns = snapshot_duration_ns;
            if (built.error && built.error->code == ErrorCode::sequence_conflict) {
                stats.manifest_conflict_aborts = 1;
            }
            if (built.outcome == DurableCompactionBuildOutcome::not_beneficial) {
                return {.outcome = DurableCompactionOutcome::not_beneficial,
                        .stats = stats,
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
        stats.snapshot_duration_ns = snapshot_duration_ns;
        const auto& old_authority = prepared.old_manifest;
        const auto& sources = prepared.plan.sources;
        const auto abort_prepared = [&](Error reason) -> DurableCompactionResult {
            auto rolled_back = rollback_prepared_compaction(directory_, old_authority,
                                                            prepared.plan.replacements, options_.limits);
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
        std::vector<std::shared_ptr<const RuntimeSegmentGeneration>> replacement_pins;
        replacement_pins.reserve(prepared.plan.replacements.size());
        for (std::size_t index = 0; index < prepared.plan.replacements.size(); ++index) {
            const auto& entry = prepared.plan.replacements[index];
            const SegmentHeaderIdentity identity{.store_id = old_authority.store_id,
                                                 .segment_id = entry.segment_id,
                                                 .generation = entry.generation,
                                                 .owner_worker = entry.owner_worker,
                                                 .format_version = old_authority.format_version};
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
                    .file = std::make_shared<const DurableSegmentFile>(std::move(*opened)),
                    .image = index < prepared.replacement_images.size() ? prepared.replacement_images[index]
                                                                        : nullptr,
                }));
        }
        auto installed_manifest = prepared.plan.next_manifest;
        auto next_pin_slots = prepare_pin_slot_index(prepared.plan.next_manifest);
        if (!next_pin_slots) {
            return abort_prepared(next_pin_slots.error());
        }

        // The rebased old authority must still be the runtime authority and
        // every source must still be pinned by the exact generation captured in
        // Phase A. The publication lease forbids any further Manifest change.
        const auto runtime_matches_authority = [&]() -> bool {
            if (!healthy() || manifest_ != old_authority ||
                segments_.size() != old_authority.segments.size() || source_pins.size() != sources.size()) {
                return false;
            }
            for (std::size_t source_index = 0; source_index < sources.size(); ++source_index) {
                const auto catalog_index = catalog_index_for_segment(sources[source_index].segment_id);
                if (!catalog_index || manifest_.segments[*catalog_index] != sources[source_index] ||
                    *catalog_index >= generation_pins_.size() || !generation_pins_[*catalog_index] ||
                    generation_pins_[*catalog_index]->file != source_pins[source_index]->file) {
                    return false;
                }
            }
            return true;
        };

        // Phase C validates while locked, then releases every physical mutex
        // before quiesce waits and the durable Manifest write.
        std::unique_lock worker_lock{worker.mutex, std::try_to_lock};
        if (!worker_lock.owns_lock()) {
            return abort_prepared(
                Error{ErrorCode::sequence_conflict, "Worker changed while compaction was prepared"});
        }
        std::unique_lock catalog_lock{catalog_mutex_};
        if (!runtime_matches_authority()) {
            catalog_lock.unlock();
            worker_lock.unlock();
            return abort_prepared(
                Error{ErrorCode::sequence_conflict, "runtime state changed during durable compaction"});
        }

        // Next catalog vectors are fully prepared (allocated) before the
        // Manifest becomes durable; afterwards only moves and refreshes remain.
        std::vector<RecoveredSegmentState> next_segments;
        std::vector<std::shared_ptr<const RuntimeSegmentGeneration>> next_generation_pins;
        std::vector<std::size_t> retained_old_indices;
        next_segments.reserve(prepared.plan.next_manifest.segments.size());
        next_generation_pins.reserve(prepared.plan.next_manifest.segments.size());
        retained_old_indices.reserve(prepared.plan.next_manifest.segments.size());
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
                const auto old_index = catalog_index_for_segment(entry.segment_id);
                if (!old_index || manifest_.segments[*old_index] != entry || !generation_pins_[*old_index]) {
                    return failure(Error{ErrorCode::corrupted_data,
                                         "compaction retained entry is absent from the old catalog"});
                }
                next_segments.push_back(segments_[*old_index]);
                next_generation_pins.push_back(generation_pins_[*old_index]);
                retained_old_indices.push_back(*old_index);
            }
            if (replacement_index != prepared.plan.replacements.size()) {
                return failure(
                    Error{ErrorCode::internal_error, "compaction runtime catalog preparation is incomplete"});
            }
        }

        struct WorkerCommitGate final {
            RuntimeWorker& worker;
            DurableCompactionCopyStats& stats;
            std::chrono::steady_clock::time_point started{std::chrono::steady_clock::now()};
            bool active{true};

            WorkerCommitGate(RuntimeWorker& owner, DurableCompactionCopyStats& metrics) noexcept
                : worker(owner), stats(metrics) {
                worker.arm_index_quiesce_gate();
            }

            ~WorkerCommitGate() {
                if (!active) {
                    return;
                }
                const std::lock_guard lock{worker.mutex};
                clear_locked();
            }

            void clear_locked() noexcept {
                worker.disarm_index_quiesce_gate_locked();
                stats.commit_gate_duration_ns = steady_elapsed_ns(started);
                active = false;
            }

            WorkerCommitGate(const WorkerCommitGate&) = delete;
            auto operator=(const WorkerCommitGate&) -> WorkerCommitGate& = delete;
        } commit_gate{worker, stats};

        commit_gate_started = commit_gate.started;
        // Holding catalog/worker across the Writer drain would deadlock an
        // exclusive mutate that already entered the hot path and needs the shared
        // catalog lock to publish (durable-compaction.md).
        catalog_lock.unlock();
        worker_lock.unlock();
        if (elide_worker_mutex) {
            worker.index_gate.wait_for_writers();
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
        if (worker.mutation_io_active || !runtime_matches_authority()) {
            catalog_lock.unlock();
            commit_gate.clear_locked();
            worker_lock.unlock();
            return abort_prepared(
                Error{ErrorCode::sequence_conflict, "runtime state changed during durable compaction"});
        }
        catalog_lock.unlock();
        worker_lock.unlock();

        // The gate stays armed: the Index cannot change until the remap below.
        const auto published =
            directory_.publish_manifest(prepared.plan.next_manifest, options_.limits.max_manifest_bytes);
        if (!published.durable()) {
            return failure(published.error.value_or(
                Error{ErrorCode::io_error, "compaction manifest publication failed"}));
        }

        try {
            worker_lock.lock();
            // ADR 0044 conditional remap: allocation-free, failure-free and
            // O(selected Records). Every Index reference into the sources is
            // either retargeted to its copy or erased (expired); entries already
            // superseded by a foreground mutation keep their newer reference.
            const auto remap_started = std::chrono::steady_clock::now();
            for (const auto& remap : prepared.remaps) {
                const HashedKey key{.key = remap.key, .hash = remap.key_hash};
                if (remap.output) {
                    if (worker.index.replace_if_current(key, remap.source, *remap.output)) {
                        ++stats.remapped_records;
                        if (auto* hot = worker.hot_records.find(key);
                            hot != nullptr && hot->reference == remap.source) {
                            hot->reference = *remap.output;
                        }
                    } else {
                        ++stats.superseded_copies;
                        stats.superseded_copy_bytes =
                            saturating_add(stats.superseded_copy_bytes, remap.source.size.value);
                    }
                    continue;
                }
                if (worker.index.find(key) == remap.source) {
                    const auto erased = worker.index.erase_no_compact(key);
                    if (auto counted = worker.update_live_record_bytes(erased.previous, std::nullopt);
                        !counted) {
                        throw std::logic_error{"durable compaction live byte accounting underflow"};
                    }
                    worker.erase_hot_record(key);
                    ++stats.expired_index_erasures;
                }
            }
            stats.remap_duration_ns = steady_elapsed_ns(remap_started);

            catalog_lock.lock();
            for (std::size_t next_index = 0; next_index < retained_old_indices.size(); ++next_index) {
                const auto old_index = retained_old_indices[next_index];
                if (old_index == std::numeric_limits<std::size_t>::max()) {
                    continue;
                }
                next_segments[next_index] = segments_[old_index];
                next_generation_pins[next_index] = generation_pins_[old_index];
            }
            // Retired sources stop counting against the reserve now; Readers still holding
            // their generations keep the pages until epoch reclamation drops them (ADR 0046).
            // Outputs borrowed at most these bytes, so no eviction is needed here.
            for (const auto& source_pin : source_pins) {
                if (source_pin->image) {
                    source_pin->image->release_charge();
                }
            }
            manifest_ = std::move(prepared.plan.next_manifest);
            segments_ = std::move(next_segments);
            generation_pins_ = std::move(next_generation_pins);
            pin_slot_by_segment_id_ = std::move(*next_pin_slots);
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

        const auto retired = directory_.retire_compaction_segments(old_authority.store_id, sources);
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
        completed = true;
        return {.outcome = DurableCompactionOutcome::compacted, .stats = stats, .error = std::nullopt};
    } catch (const std::bad_alloc&) {
        return failure(Error{ErrorCode::resource_exhausted, {}});
    } catch (...) {
        return failure(Error{ErrorCode::internal_error, {}});
    }
}

auto DurableRuntimeCatalog::compaction_stats() const noexcept -> DurableCompactionMetrics {
    return {.attempts = compaction_attempts_.load(std::memory_order_relaxed),
            .completed = compactions_completed_.load(std::memory_order_relaxed),
            .manifest_conflict_aborts = compaction_manifest_conflicts_.load(std::memory_order_relaxed),
            .abandoned_copy_bytes = compaction_abandoned_copy_bytes_.load(std::memory_order_relaxed),
            .remapped_records = compaction_remapped_records_.load(std::memory_order_relaxed),
            .superseded_copies = compaction_superseded_copies_.load(std::memory_order_relaxed),
            .manifest_rebases = compaction_manifest_rebases_.load(std::memory_order_relaxed),
            .maximum_snapshot_duration_ns = maximum_compaction_snapshot_ns_.load(std::memory_order_relaxed),
            .maximum_commit_gate_duration_ns =
                maximum_compaction_commit_gate_ns_.load(std::memory_order_relaxed)};
}

} // namespace glifistore
