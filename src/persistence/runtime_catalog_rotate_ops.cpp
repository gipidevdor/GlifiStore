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
#include <stdexcept>
#include <string>
#include <utility>

namespace glifistore {
using namespace glifistore::runtime_catalog_detail;

auto DurableRuntimeCatalog::rotate_active(RuntimeWorker& worker, std::unique_lock<std::mutex>& worker_lock)
    -> DurableMutationResult {
    // Linearization protocol (runtime contract):
    //  1. caller owns worker.mutex (legacy / flusher paths) or is the paired
    //     exclusive Writer with the mutex elided; publication + catalog locks
    //     capture one manifest generation, its immutable generation pin, and
    //     the writable Segment handle;
    //  2. mutation_io_active serializes writers while worker/catalog mutexes are
    //     released. GET remains lock-independent through the captured pin;
    //  3. after durable manifest publication, worker then catalog mutex are
    //     reacquired (when held) and the prepared state is installed without
    //     allocation.
    // No RecordRef, file, or Segment generation crosses phase 1 without the
    // exact shared generation pin captured below.
    // Mutex elision matches mutate / ExclusiveHotPathPause: only exclusive
    // durable_sync (no background flusher) skips worker.mutex. Paired
    // durable-group/periodic share Worker state with the flusher and must
    // re-lock before touching mutation_io_active / cached_file / active_segment.
    const bool elide_worker_mutex = options_.exclusive_writer && flusher_ == nullptr;
    GS_FAULT_SITE(rotate);
    if (!worker_lock.owns_lock() && !elide_worker_mutex) {
        return mutation_failure(
            DurableMutationOutcome::indeterminate,
            Error{ErrorCode::internal_error, "rotation requires the owning Worker mutex"});
    }
    const auto rotation_started = std::chrono::steady_clock::now();
    rotation_attempts_.fetch_add(1U, std::memory_order_relaxed);
    const auto observed_active_segment = worker.active_segment;
    const auto observed_next_sequence = worker.next_sequence;
    bool counted_compaction_wait{};
    for (;;) {
        std::unique_lock publication_lock{manifest_publication_mutex_};
        if (!compaction_publication_active_ && !rotation_publication_active_) {
            rotation_publication_active_ = true;
            publication_lock.unlock();
            break;
        }
        if (compaction_publication_active_ && !counted_compaction_wait) {
            rotation_compaction_waits_.fetch_add(1U, std::memory_order_relaxed);
            counted_compaction_wait = true;
        }
        // A manifest publisher may perform durable I/O. Do not turn its
        // logical lease into equivalent Worker head-of-line blocking.
        publication_lock.unlock();
        if (worker_lock.owns_lock()) {
            worker_lock.unlock();
        }

        // Exclusive Writer (mutex-elided durable_sync only): drop hot_path_depth while
        // blocked on another publisher (compaction). Compaction waits for depth==0
        // before the Index swap; keeping depth here deadlocks against that wait.
        // Paired durable-group/periodic keep the Worker mutex (flusher shares state)
        // and never increment depth — pausing here would underflow to UINT32_MAX and
        // deadlock compaction's depth wait against this publication wait.
        struct ExclusiveHotPathPause final {
            RuntimeWorker* worker{};
            bool paused{};

            explicit ExclusiveHotPathPause(RuntimeWorker* owner, const bool enable) noexcept
                : worker(enable ? owner : nullptr) {
                if (worker == nullptr) {
                    return;
                }
                if (worker->hot_path_depth.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
                    worker->hot_path_depth.notify_all();
                }
                paused = true;
            }

            ~ExclusiveHotPathPause() {
                if (paused && worker != nullptr) {
                    worker->hot_path_depth.fetch_add(1U, std::memory_order_acq_rel);
                }
            }

            ExclusiveHotPathPause(const ExclusiveHotPathPause&) = delete;
            auto operator=(const ExclusiveHotPathPause&) -> ExclusiveHotPathPause& = delete;
        } hot_path_pause{&worker, elide_worker_mutex};

        {
            std::unique_lock wait_lock{manifest_publication_mutex_};
            manifest_publication_changed_.wait(wait_lock, [&] {
                return (!compaction_publication_active_ && !rotation_publication_active_) || !healthy();
            });
        }
        if (!elide_worker_mutex) {
            if (!worker_lock.owns_lock()) {
                worker_lock.lock();
            }
        }
        if (!healthy()) {
            return mutation_failure(DurableMutationOutcome::not_committed,
                                    Error{ErrorCode::unavailable, "durable runtime is fail-closed"});
        }
        if (worker.next_sequence != observed_next_sequence) {
            return mutation_failure(DurableMutationOutcome::not_committed,
                                    Error{ErrorCode::sequence_conflict,
                                          "Worker advanced while mutation waited for manifest publication"});
        }
        if (worker.active_segment != observed_active_segment) {
            // Another same-Worker writer completed the required rotation while
            // this caller slept. The caller can retry its final append against
            // the newly linearized active generation.
            return {.outcome = DurableMutationOutcome::committed,
                    .sequence = std::nullopt,
                    .error = std::nullopt};
        }
    }
    ScopeExit release_publication_reservation{[this]() noexcept {
        {
            const std::lock_guard lock{manifest_publication_mutex_};
            rotation_publication_active_ = false;
        }
        manifest_publication_changed_.notify_all();
    }};
    const auto execution_started = std::chrono::steady_clock::now();
    bool rotation_committed{};
    std::uint64_t seal_ns{};
    std::uint64_t create_ns{};
    std::uint64_t manifest_publication_ns{};
    ScopeExit telemetry{[&, this]() noexcept {
        const auto finished = std::chrono::steady_clock::now();
        const auto publication_wait_ns = steady_duration_ns(rotation_started, execution_started);
        const auto execution_ns = steady_duration_ns(execution_started, finished);
        const auto total_ns = steady_duration_ns(rotation_started, finished);
        const auto previous_version = begin_atomic_stats_publication(rotation_stats_version_);
        if (rotation_committed) {
            rotations_committed_.fetch_add(1U, std::memory_order_relaxed);
        }
        last_rotation_publication_wait_ns_.store(publication_wait_ns, std::memory_order_relaxed);
        atomic_saturating_add(total_rotation_publication_wait_ns_, publication_wait_ns);
        atomic_observe_max(maximum_rotation_publication_wait_ns_, publication_wait_ns);
        last_rotation_seal_ns_.store(seal_ns, std::memory_order_relaxed);
        atomic_saturating_add(total_rotation_seal_ns_, seal_ns);
        atomic_observe_max(maximum_rotation_seal_ns_, seal_ns);
        last_rotation_create_ns_.store(create_ns, std::memory_order_relaxed);
        atomic_saturating_add(total_rotation_create_ns_, create_ns);
        atomic_observe_max(maximum_rotation_create_ns_, create_ns);
        last_rotation_manifest_publication_ns_.store(manifest_publication_ns, std::memory_order_relaxed);
        atomic_saturating_add(total_rotation_manifest_publication_ns_, manifest_publication_ns);
        atomic_observe_max(maximum_rotation_manifest_publication_ns_, manifest_publication_ns);
        last_rotation_execution_ns_.store(execution_ns, std::memory_order_relaxed);
        atomic_saturating_add(total_rotation_execution_ns_, execution_ns);
        atomic_observe_max(maximum_rotation_execution_ns_, execution_ns);
        last_rotation_total_ns_.store(total_ns, std::memory_order_relaxed);
        atomic_saturating_add(total_rotation_ns_, total_ns);
        atomic_observe_max(maximum_rotation_total_ns_, total_ns);
        end_atomic_stats_publication(rotation_stats_version_, previous_version);
    }};
    std::unique_lock catalog_lock{catalog_mutex_};
    const auto old_position =
        std::lower_bound(manifest_.segments.begin(), manifest_.segments.end(), worker.active_segment,
                         [](const ManifestSegmentEntry& entry, const SegmentId id) {
                             return entry.segment_id.value < id.value;
                         });
    if (old_position == manifest_.segments.end() || old_position->segment_id != worker.active_segment ||
        old_position->owner_worker != worker.worker_id || old_position->role != ManifestSegmentRole::active) {
        abandon_pending_batches();
        return mutation_failure(
            DurableMutationOutcome::indeterminate,
            Error{ErrorCode::corrupted_data, "runtime active Segment disagrees with the manifest"});
    }
    const auto old_index = static_cast<std::size_t>(old_position - manifest_.segments.begin());
    const auto old_entry = *old_position;
    auto next_manifest = rotation_manifest(manifest_, old_entry, options_.limits);
    if (!next_manifest) {
        return mutation_failure(DurableMutationOutcome::not_committed, next_manifest.error());
    }
    const auto next_manifest_bytes = durable_manifest_bytes(next_manifest->segments.size());
    if (!next_manifest_bytes) {
        return mutation_failure(DurableMutationOutcome::not_committed, next_manifest_bytes.error());
    }
    if (*next_manifest_bytes > std::numeric_limits<std::uint64_t>::max() - kSegmentSizeBytes) {
        return mutation_failure(
            DurableMutationOutcome::not_committed,
            Error{ErrorCode::arithmetic_overflow, "rotation free-space requirement overflow"});
    }
    auto next_pin_slots = prepare_pin_slot_index(*next_manifest);
    if (!next_pin_slots) {
        return mutation_failure(DurableMutationOutcome::not_committed, next_pin_slots.error());
    }
    segments_.reserve(segments_.size() + 1U);
    generation_pins_.reserve(generation_pins_.size() + 1U);

    const SegmentHeaderIdentity old_identity{.store_id = manifest_.store_id,
                                             .segment_id = old_entry.segment_id,
                                             .generation = old_entry.generation,
                                             .owner_worker = old_entry.owner_worker,
                                             .format_version = manifest_.format_version};
    if (old_index >= generation_pins_.size() || !generation_pins_[old_index] ||
        generation_pins_[old_index]->identity != old_identity) {
        abandon_pending_batches();
        return mutation_failure(
            DurableMutationOutcome::indeterminate,
            Error{ErrorCode::corrupted_data, "active Segment has no exact generation pin"});
    }
    const auto old_pin = generation_pins_[old_index];
    const auto old_selected = segments_[old_index].selected;
    std::optional<DurableSegmentFile> active_file;
    if (worker.cached_file && worker.cached_file->identity() == old_identity && worker.cached_writable) {
        active_file.emplace(std::move(*worker.cached_file));
    }
    worker.cached_file.reset();
    worker.cached_writable = false;
    const auto& replacement_entry = next_manifest->segments.back();
    const SegmentHeaderIdentity replacement_identity{.store_id = manifest_.store_id,
                                                     .segment_id = replacement_entry.segment_id,
                                                     .generation = replacement_entry.generation,
                                                     .owner_worker = replacement_entry.owner_worker,
                                                     .format_version = manifest_.format_version};
    worker.mutation_io_active = true;
    catalog_lock.unlock();
    if (worker_lock.owns_lock()) {
        worker_lock.unlock();
    }
    SegmentFileCreationResult created;
    SelectedSegmentCommit sealed_selected{};
    SelectedSegmentCommit replacement_selected{};
    std::shared_ptr<const RuntimeSegmentGeneration> sealed_generation;
    std::shared_ptr<const RuntimeSegmentGeneration> replacement_generation;
    DurableMutationResult io_result{
        .outcome = DurableMutationOutcome::not_committed, .sequence = std::nullopt, .error = std::nullopt};
    // Caller entered rotate only after this mutation's append returned not_committed
    // (segment_full / segment_sealed). Keep catch polarity not_committed until rotation
    // itself crosses a durable write boundary (committed seal / indeterminate create or
    // publish). Matches mutate() fail-closed and pre-append open polarity.
    auto exception_outcome = DurableMutationOutcome::not_committed;
    try {
        if (glifistore::fault::consume_fail(glifistore::fault::Site::rotate)) {
            throw std::runtime_error{"injected pre-write rotation failure"};
        }
        if (auto available = require_durable_available_space(
                directory_, kSegmentSizeBytes + *next_manifest_bytes, options_.limits);
            !available) {
            io_result = mutation_failure(DurableMutationOutcome::not_committed, available.error());
        } else {
            if (!active_file) {
                auto opened =
                    DurableSegmentFile::open(directory_, old_identity, SegmentFileOpenMode::read_write);
                if (!opened || opened->selected_commit() != old_selected) {
                    io_result = mutation_failure(
                        DurableMutationOutcome::not_committed,
                        opened ? Error{ErrorCode::corrupted_data, "active Segment changed after reservation"}
                               : opened.error());
                } else {
                    active_file.emplace(std::move(*opened));
                }
            }
            if (!io_result.error) {
                if (!active_file) {
                    io_result = mutation_failure(
                        DurableMutationOutcome::indeterminate,
                        Error{ErrorCode::internal_error, "rotation has no active Segment handle"});
                    exception_outcome = DurableMutationOutcome::indeterminate;
                } else {
                    auto& active = *active_file;
                    const auto seal_started = std::chrono::steady_clock::now();
                    ScopeExit observe_seal{[&]() noexcept { seal_ns = steady_elapsed_ns(seal_started); }};
                    if (active.selected_commit().commit.state != PersistedSegmentState::sealed) {
                        const auto sealed = active.seal();
                        if (!sealed.committed()) {
                            io_result = mutation_failure(
                                sealed.outcome == SegmentCommitOutcome::indeterminate
                                    ? DurableMutationOutcome::indeterminate
                                    : DurableMutationOutcome::not_committed,
                                sealed.error.value_or(Error{ErrorCode::io_error, "Segment seal failed"}));
                            if (sealed.outcome == SegmentCommitOutcome::indeterminate) {
                                exception_outcome = DurableMutationOutcome::indeterminate;
                            }
                        } else {
                            exception_outcome = DurableMutationOutcome::indeterminate;
                        }
                    } else {
                        // Already sealed (e.g. partial prior rotation): later create/publish
                        // failures must not demote to known-not-committed / OVERLOADED.
                        exception_outcome = DurableMutationOutcome::indeterminate;
                    }
                    sealed_selected = active.selected_commit();
                }
            }
            if (!io_result.error) {
                auto sealed_reader =
                    DurableSegmentFile::open(directory_, old_identity, SegmentFileOpenMode::read_only);
                if (!sealed_reader || sealed_reader->selected_commit() != sealed_selected) {
                    // After a committed seal this attempt, reader-open miss is sticky
                    // indeterminate — not known-not-committed / OVERLOADED.
                    io_result = mutation_failure(
                        exception_outcome,
                        sealed_reader ? Error{ErrorCode::corrupted_data,
                                              "sealed Segment changed before generation-pin preparation"}
                                      : sealed_reader.error());
                } else {
                    sealed_generation =
                        std::make_shared<const RuntimeSegmentGeneration>(RuntimeSegmentGeneration{
                            .identity = old_identity,
                            .selected = sealed_selected,
                            .file = std::move(*sealed_reader),
                        });
                }
            }
            if (!io_result.error) {
                const auto create_started = std::chrono::steady_clock::now();
                ScopeExit observe_create{[&]() noexcept { create_ns = steady_elapsed_ns(create_started); }};
                created = DurableSegmentFile::create(directory_, replacement_identity);
                if (!created.durable()) {
                    if (created.outcome == SegmentFileCreationOutcome::indeterminate) {
                        exception_outcome = DurableMutationOutcome::indeterminate;
                    }
                    // Preserve post-seal exception_outcome — do not demote not_published
                    // create rejects to known-not-committed after a durable seal.
                    io_result = mutation_failure(
                        exception_outcome, created.error.value_or(Error{
                                               ErrorCode::io_error, "replacement Segment creation failed"}));
                } else {
                    exception_outcome = DurableMutationOutcome::indeterminate;
                }
            }
            if (!io_result.error) {
                if (!created.file) {
                    io_result = mutation_failure(
                        DurableMutationOutcome::indeterminate,
                        Error{ErrorCode::internal_error, "durable replacement creation has no file handle"});
                } else {
                    replacement_selected = created.file->selected_commit();
                    auto replacement_reader = DurableSegmentFile::open(directory_, replacement_identity,
                                                                       SegmentFileOpenMode::read_only);
                    if (!replacement_reader ||
                        replacement_reader->selected_commit() != replacement_selected) {
                        // Durable create already succeeded — reader-open miss is sticky.
                        io_result = mutation_failure(
                            exception_outcome,
                            replacement_reader
                                ? Error{ErrorCode::corrupted_data,
                                        "new active Segment changed before generation-pin preparation"}
                                : replacement_reader.error());
                    } else {
                        replacement_generation =
                            std::make_shared<const RuntimeSegmentGeneration>(RuntimeSegmentGeneration{
                                .identity = replacement_identity,
                                .selected = replacement_selected,
                                .file = std::move(*replacement_reader),
                            });
                    }
                }
            }
            if (!io_result.error) {
                const auto manifest_started = std::chrono::steady_clock::now();
                ScopeExit observe_manifest{
                    [&]() noexcept { manifest_publication_ns = steady_elapsed_ns(manifest_started); }};
                const auto published =
                    directory_.publish_manifest(*next_manifest, options_.limits.max_manifest_bytes);
                if (!published.durable()) {
                    if (published.outcome == ManifestPublicationOutcome::indeterminate) {
                        exception_outcome = DurableMutationOutcome::indeterminate;
                    }
                    // Preserve post-seal / post-create exception_outcome for not_published.
                    io_result = mutation_failure(
                        exception_outcome, published.error.value_or(Error{
                                               ErrorCode::io_error, "rotation manifest publication failed"}));
                } else {
                    io_result = {.outcome = DurableMutationOutcome::committed,
                                 .sequence = std::nullopt,
                                 .error = std::nullopt};
                }
            }
        }
    } catch (const std::bad_alloc&) {
        io_result = mutation_failure(exception_outcome, Error{ErrorCode::resource_exhausted, {}});
    } catch (...) {
        io_result = mutation_failure(exception_outcome, Error{ErrorCode::internal_error, {}});
    }

    // Same predicate as mutate post-append: exclusive+flusher must re-take the
    // Worker mutex before clearing mutation_io_active or installing active_segment /
    // cached_file — otherwise the flusher (or a sibling mutate waiting on
    // mutation_io_finished) races non-atomic Worker fields.
    if (!elide_worker_mutex) {
        if (!worker_lock.owns_lock()) {
            worker_lock.lock();
        }
    }
    catalog_lock.lock();
    const auto clear_reservation = [&]() noexcept {
        worker.mutation_io_active = false;
        worker.mutation_io_finished.notify_all();
    };
    if (!io_result.committed()) {
        if (active_file && !worker.cached_file) {
            worker.cached_file.emplace(std::move(*active_file));
            worker.cached_writable = true;
        }
        if (io_result.outcome == DurableMutationOutcome::indeterminate || !directory_.healthy()) {
            healthy_.store(false, std::memory_order_release);
        }
        clear_reservation();
        return io_result;
    }

    const auto current_position =
        std::lower_bound(manifest_.segments.begin(), manifest_.segments.end(), old_entry.segment_id,
                         [](const ManifestSegmentEntry& entry, const SegmentId id) {
                             return entry.segment_id.value < id.value;
                         });
    if (!healthy() || current_position == manifest_.segments.end() || *current_position != old_entry ||
        worker.active_segment != old_entry.segment_id || generation_pins_[old_index] != old_pin ||
        worker.cached_file || !worker.mutation_io_active || !created.file) {
        healthy_.store(false, std::memory_order_release);
        clear_reservation();
        return mutation_failure(
            DurableMutationOutcome::indeterminate,
            Error{ErrorCode::corrupted_data, "rotation reservation failed final relinearization"});
    }
    const auto active_live_record_bytes = worker.active_live_record_bytes.load(std::memory_order_relaxed);
    const auto sealed_live_record_bytes = worker.sealed_live_record_bytes.load(std::memory_order_relaxed);
    if (active_live_record_bytes > std::numeric_limits<std::uint64_t>::max() - sealed_live_record_bytes) {
        healthy_.store(false, std::memory_order_release);
        clear_reservation();
        return mutation_failure(DurableMutationOutcome::indeterminate,
                                Error{ErrorCode::arithmetic_overflow,
                                      "durable rotation live Record byte count overflows after publication"});
    }
    const auto next_sealed_live_record_bytes = sealed_live_record_bytes + active_live_record_bytes;

    const auto replacement_segment_id = replacement_entry.segment_id;
    manifest_ = std::move(*next_manifest);
    segments_[old_index].selected = sealed_selected;
    segments_.push_back({.selected = replacement_selected});
    generation_pins_[old_index] = std::move(sealed_generation);
    generation_pins_.push_back(std::move(replacement_generation));
    pin_slot_by_segment_id_ = std::move(*next_pin_slots);
    worker.active_segment = replacement_segment_id;
    worker.active_live_record_bytes.store(0, std::memory_order_release);
    worker.sealed_live_record_bytes.store(next_sealed_live_record_bytes, std::memory_order_release);
    worker.cached_file.emplace(std::move(*created.file));
    worker.cached_writable = true;
    worker.hot_records.erase_if([&](const std::string& key, std::uint64_t, HotRecordEntry& entry) {
        if (entry.reference.segment_id != old_entry.segment_id) {
            return false;
        }
        subtract_hot_record_accounting(worker.hot_record_resident_bytes, key, entry);
        worker.get_path_metrics.hot_evictions.fetch_add(1U, std::memory_order_relaxed);
        return true;
    });
    if (!advance_read_catalog_revision(worker)) {
        healthy_.store(false, std::memory_order_release);
        clear_reservation();
        return mutation_failure(
            DurableMutationOutcome::indeterminate,
            Error{ErrorCode::arithmetic_overflow, "durable read catalog revision exhausted"});
    }
    clear_reservation();
    rotation_committed = true;
    return {.outcome = DurableMutationOutcome::committed, .sequence = std::nullopt, .error = std::nullopt};
}

} // namespace glifistore
