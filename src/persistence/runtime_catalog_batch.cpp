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

auto DurableRuntimeCatalog::should_flush_batch(RuntimeWorker& worker) const noexcept -> bool {
    if (!options_.batch || !worker.cached_file || !worker.cached_file->has_pending_commit()) {
        return false;
    }
    if (worker.batch_closing) {
        return true;
    }
    const auto& config = *options_.batch;
    const auto pending_records = worker.cached_file->pending_record_count();
    if (pending_records >= config.max_records) {
        atomic_saturating_add(worker.batch_metrics.record_limit_closes, 1U);
        return true;
    }
    if (worker.cached_file->pending_bytes() >= config.max_bytes) {
        atomic_saturating_add(worker.batch_metrics.byte_limit_closes, 1U);
        return true;
    }
    const auto record_target = worker.batch_sizer.target();
    if (options_.strict_ack && pending_records >= record_target) {
        // If more producers are already admitted than fit in this batch, grow
        // the next target up to the observed burst. A relaxed sample is enough:
        // this is a tuning hint and never affects acknowledgement correctness.
        const auto admissions = worker.active_group_mutations.load(std::memory_order_relaxed);
        worker.batch_sizer.observe_target_reached(pending_records, admissions);
        worker.batch_metrics.current_record_target.store(worker.batch_sizer.target(),
                                                         std::memory_order_relaxed);
        atomic_saturating_add(worker.batch_metrics.adaptive_target_closes, 1U);
        return true;
    }
    if (worker.batch_started != std::chrono::steady_clock::time_point{}) {
        const auto elapsed = std::chrono::steady_clock::now() - worker.batch_started;
        if (elapsed >= std::chrono::milliseconds{config.max_wait_ms}) {
            // A deadline means the current target exceeded available
            // concurrency. Contract the next batch to the occupancy actually
            // achieved, within the caller's explicit bounds.
            if (options_.strict_ack) {
                worker.batch_sizer.observe_deadline(pending_records);
                worker.batch_metrics.current_record_target.store(worker.batch_sizer.target(),
                                                                 std::memory_order_relaxed);
            }
            atomic_saturating_add(worker.batch_metrics.deadline_closes, 1U);
            return true;
        }
    }
    return false;
}

void DurableRuntimeCatalog::abandon_pending_batches() noexcept {
    healthy_.store(false, std::memory_order_release);
    for (auto& worker : workers_) {
        try {
            const std::lock_guard lock{worker->mutex};
            worker->pending_group_mutations.clear();
            worker->pending_group_insertions = 0;
            worker->pending_group_heap_key_bytes = 0;
            worker->batch_started = {};
            worker->batch_closing = false;
            worker->batch_closed.notify_all();
        } catch (...) {
            worker->batch_closed.notify_all();
        }
    }
}

auto DurableRuntimeCatalog::flush_worker_batch(RuntimeWorker& worker,
                                               std::unique_lock<std::mutex>& worker_lock,
                                               std::shared_lock<std::shared_mutex>& catalog_lock,
                                               const SegmentCommitSync sync) -> Status {
    if (!worker.cached_file || !worker.cached_writable || !worker.cached_file->has_pending_commit()) {
        return {};
    }
    if (!worker_lock.owns_lock() || !catalog_lock.owns_lock() || worker.mutation_io_active) {
        return fail(ErrorCode::internal_error, "batch flush requires an unreserved Worker and catalog lock");
    }
    const auto identity = worker.cached_file->identity();
    const auto expected_selected = worker.cached_file->selected_commit();
    const auto pin_index = catalog_index_for_segment(identity.segment_id);
    if (!pin_index || !generation_pins_[*pin_index] || generation_pins_[*pin_index]->identity != identity) {
        return fail(ErrorCode::corrupted_data, "batched Segment has no exact generation pin");
    }
    const auto generation_pin = generation_pins_[*pin_index];
    const auto pending_records = worker.cached_file->pending_record_count();
    const auto pending_bytes = worker.cached_file->pending_bytes();
    atomic_saturating_add(worker.batch_metrics.flush_attempts, 1U);
    auto io_file = std::move(worker.cached_file);
    worker.cached_file.reset();
    worker.cached_writable = false;
    worker.mutation_io_active = true;
    catalog_lock.unlock();
    worker_lock.unlock();

    const auto commit_started = std::chrono::steady_clock::now();
    SegmentCommitResult flushed;
    try {
        flushed = io_file->flush_pending_commit(sync);
    } catch (const std::bad_alloc&) {
        flushed = {.outcome = SegmentCommitOutcome::indeterminate,
                   .error = Error{ErrorCode::resource_exhausted, {}}};
    } catch (...) {
        flushed = {.outcome = SegmentCommitOutcome::indeterminate,
                   .error = Error{ErrorCode::internal_error, {}}};
    }
    const auto commit_duration_ns = steady_elapsed_ns(commit_started);
    atomic_saturating_add(worker.batch_metrics.total_commit_duration_ns, commit_duration_ns);
    atomic_observe_max(worker.batch_metrics.maximum_commit_duration_ns, commit_duration_ns);

    worker_lock.lock();
    catalog_lock.lock();
    const auto current_position =
        std::lower_bound(manifest_.segments.begin(), manifest_.segments.end(), identity.segment_id,
                         [](const ManifestSegmentEntry& entry, const SegmentId id) {
                             return entry.segment_id.value < id.value;
                         });
    const auto current_pin_index = catalog_index_for_segment(identity.segment_id);
    const bool reservation_valid =
        current_position != manifest_.segments.end() && current_position->segment_id == identity.segment_id &&
        current_position->generation == identity.generation &&
        current_position->owner_worker == identity.owner_worker &&
        current_position->role == ManifestSegmentRole::active &&
        worker.active_segment == identity.segment_id && worker.mutation_io_active &&
        current_pin_index.has_value() && generation_pins_[*current_pin_index] == generation_pin &&
        io_file->identity() == identity &&
        expected_selected ==
            segments_[static_cast<std::size_t>(current_position - manifest_.segments.begin())].selected;
    worker.cached_file.emplace(std::move(*io_file));
    worker.cached_writable = true;
    worker.mutation_io_active = false;
    worker.mutation_io_finished.notify_all();
    if (!reservation_valid) {
        healthy_.store(false, std::memory_order_release);
        return fail(ErrorCode::corrupted_data, "batch flush I/O reservation failed relinearization");
    }
    if (!flushed.committed()) {
        atomic_saturating_add(worker.batch_metrics.failed_batches, 1U);
        healthy_.store(false, std::memory_order_release);
        worker.pending_group_mutations.clear();
        worker.pending_group_insertions = 0;
        worker.pending_group_heap_key_bytes = 0;
        worker.batch_closing = false;
        worker.batch_closed.notify_all();
        return unexpected(flushed.error.value_or(Error{ErrorCode::io_error, "batch flush failed"}));
    }
    atomic_saturating_add(worker.batch_metrics.committed_batches, 1U);
    atomic_saturating_add(worker.batch_metrics.committed_records, pending_records);
    atomic_saturating_add(worker.batch_metrics.committed_bytes, pending_bytes);
    atomic_observe_max(worker.batch_metrics.maximum_batch_records, static_cast<std::size_t>(pending_records));
    atomic_observe_max(worker.batch_metrics.maximum_batch_bytes, pending_bytes);
    worker.batch_metrics.pending_records.store(0, std::memory_order_relaxed);
    worker.batch_metrics.pending_bytes.store(0, std::memory_order_relaxed);
    const auto position =
        std::lower_bound(manifest_.segments.begin(), manifest_.segments.end(), identity.segment_id,
                         [](const ManifestSegmentEntry& entry, const SegmentId id) {
                             return entry.segment_id.value < id.value;
                         });
    if (position == manifest_.segments.end() || position->segment_id != identity.segment_id ||
        position->generation != identity.generation || position->owner_worker != identity.owner_worker) {
        return fail(ErrorCode::corrupted_data,
                    "batched Segment cache identity is absent from the runtime catalog");
    }
    const auto catalog_index = static_cast<std::size_t>(position - manifest_.segments.begin());
    segments_[catalog_index].selected = worker.cached_file->selected_commit();
    const auto publication_failed = [&](Error error) -> Status {
        healthy_.store(false, std::memory_order_release);
        worker.pending_group_mutations.clear();
        worker.pending_group_insertions = 0;
        worker.pending_group_heap_key_bytes = 0;
        worker.batch_started = {};
        worker.batch_closing = false;
        worker.batch_closed.notify_all();
        return unexpected(std::move(error));
    };
    try {
        for (auto& mutation : worker.pending_group_mutations) {
            const HashedKey hashed{.key = mutation.key, .hash = mutation.key_hash};
            if (mutation.opcode == Opcode::put) {
                const auto published = worker.index.insert_or_assign(hashed, mutation.reference);
                if (!published) {
                    return publication_failed(published.error());
                }
                // Index authority applied: advance durable_through before secondary work so
                // Writer finalize keeps success ACK if count/hot fails after Index publish.
                worker.record_compaction_change(hashed, mutation.reference);
                worker.durable_through = mutation.reference.sequence;
                if (glifistore::fault::consume_fail(glifistore::fault::Site::index_account)) {
                    return publication_failed(
                        Error{ErrorCode::resource_exhausted, "injected Index accounting failure"});
                }
                if (auto counted = worker.update_live_record_bytes(published->previous, mutation.reference);
                    !counted) {
                    return publication_failed(counted.error());
                }
                if (mutation.hot_record.empty()) {
                    worker.erase_hot_record(mutation.key, mutation.key_hash);
                } else if (auto hot_published =
                               worker.publish_hot_record(mutation.hot_record, mutation.reference);
                           !hot_published) {
                    return publication_failed(hot_published.error());
                }
            } else {
                const auto erased = worker.index.erase_no_compact(hashed);
                worker.record_compaction_change(hashed, std::nullopt);
                worker.durable_through = mutation.reference.sequence;
                if (glifistore::fault::consume_fail(glifistore::fault::Site::index_account)) {
                    return publication_failed(
                        Error{ErrorCode::resource_exhausted, "injected Index accounting failure"});
                }
                if (auto counted = worker.update_live_record_bytes(erased.previous, std::nullopt); !counted) {
                    return publication_failed(counted.error());
                }
                worker.erase_hot_record(mutation.key, mutation.key_hash);
            }
        }
    } catch (const std::bad_alloc&) {
        // Index/hot allocation must not escape after durable_through advanced: pending
        // would remain and healthy could stay true while Index is partially published.
        return publication_failed(
            Error{ErrorCode::resource_exhausted, "Index publication allocation failed"});
    } catch (...) {
        return publication_failed(Error{ErrorCode::internal_error, "Index publication failed"});
    }
    worker.pending_group_mutations.clear();
    worker.pending_group_insertions = 0;
    worker.pending_group_heap_key_bytes = 0;
    worker.batch_started = {};
    worker.batch_closing = false;
    worker.batch_closed.notify_all();
    return {};
}

void DurableRuntimeCatalog::wait_for_batch_close(RuntimeWorker& worker, const SequenceNumber sequence,
                                                 std::unique_lock<std::mutex>& lock) {
    worker.batch_closed.wait(lock,
                             [&] { return worker.durable_through.value >= sequence.value || !healthy(); });
}

auto DurableRuntimeCatalog::writer_batch_config() const noexcept -> std::optional<DurableGroupConfig> {
    if (!options_.strict_ack) {
        return std::nullopt;
    }
    return options_.batch;
}

auto DurableRuntimeCatalog::writer_durable_through(const std::size_t worker_index) const noexcept
    -> SequenceNumber {
    if (worker_index >= workers_.size()) {
        return {};
    }
    // Synchronize with flush_worker_batch / mutate writers: durable_through is
    // non-atomic and advanced under worker.mutex. Unlocked reads after
    // commit_writer_batch would lack happens-before and could under-read Index
    // coverage → rewrite visible successes to indeterminate (inverted RAW).
    auto& worker = *workers_[worker_index];
    const std::lock_guard lock{worker.mutex};
    return worker.durable_through;
}

auto DurableRuntimeCatalog::commit_writer_batch(const std::size_t worker_index) -> Status {
    if (worker_index >= workers_.size()) {
        return fail(ErrorCode::invalid_argument, "Writer batch targets an invalid Worker");
    }
    auto& worker = *workers_[worker_index];
    std::unique_lock worker_lock{worker.mutex};
    worker.mutation_io_finished.wait(worker_lock, [&] { return !worker.mutation_io_active || !healthy(); });
    while (dedicated_commit_executor_ && worker.batch_closing && healthy()) {
        worker.batch_closed.wait(worker_lock);
    }
    if (!healthy()) {
        // Strict-ack mutates may already have flushed earlier items in this batch.
        // An empty pending-group with no orphaned Segment pending means there is
        // nothing left to commit; returning fail would incorrectly rewrite those
        // successes to indeterminate upstream. Orphaned file pending (group cleared
        // after a later-item failure) must not look like a clean no-op.
        if (worker.pending_group_mutations.empty()) {
            if (worker.cached_file && worker.cached_writable && worker.cached_file->has_pending_commit()) {
                return fail(ErrorCode::unavailable,
                            "durable runtime is fail-closed with orphaned pending Segment commit");
            }
            return {};
        }
        return fail(ErrorCode::unavailable, "durable runtime is fail-closed");
    }
    if (worker.pending_group_mutations.empty()) {
        return {};
    }
    std::shared_lock catalog_lock{catalog_mutex_};
    return flush_worker_batch(worker, worker_lock, catalog_lock, SegmentCommitSync::immediate);
}

auto DurableRuntimeCatalog::flush_pending_batches(const SegmentCommitSync sync) -> Status {
    for (auto& worker : workers_) {
        std::unique_lock lock{worker->mutex};
        // Use healthy_ (not healthy()): close() sets closed_ before the final flush,
        // and healthy() is false while closed_ — that must not skip the close flush.
        worker->mutation_io_finished.wait(
            lock, [&] { return !worker->mutation_io_active || !healthy_.load(std::memory_order_acquire); });
        worker->compaction_commit_finished.wait(
            lock, [&] { return !worker->compaction_commit_active.load(std::memory_order_relaxed); });
        // Sticky abandon only: do not flush orphaned Segment pending after fail-closed.
        if (!healthy_.load(std::memory_order_acquire)) {
            return {};
        }
        std::shared_lock catalog_lock{catalog_mutex_};
        if (auto flushed = flush_worker_batch(*worker, lock, catalog_lock, sync); !flushed) {
            return flushed;
        }
    }
    return {};
}

auto DurableRuntimeCatalog::flush_due_batches(const SegmentCommitSync sync) -> Status {
    for (auto& worker : workers_) {
        std::unique_lock lock{worker->mutex};
        worker->mutation_io_finished.wait(
            lock, [&] { return !worker->mutation_io_active || !healthy_.load(std::memory_order_acquire); });
        worker->compaction_commit_finished.wait(
            lock, [&] { return !worker->compaction_commit_active.load(std::memory_order_relaxed); });
        if (!healthy_.load(std::memory_order_acquire)) {
            return {};
        }
        if (!should_flush_batch(*worker)) {
            continue;
        }
        std::shared_lock catalog_lock{catalog_mutex_};
        if (auto flushed = flush_worker_batch(*worker, lock, catalog_lock, sync); !flushed) {
            return flushed;
        }
    }
    return {};
}

} // namespace glifistore
