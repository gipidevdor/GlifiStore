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

auto DurableRuntimeCatalog::put(const std::span<const std::byte> key, const std::span<const std::byte> value,
                                const std::uint64_t expire_at_ns, const ValueType type,
                                const std::uint32_t flags) -> DurableMutationResult {
    return mutate(key, value, Opcode::put, hash_key_routing(key, worker_routing_), expire_at_ns, type, flags);
}

auto DurableRuntimeCatalog::put(const HashedKey& key, const std::span<const std::byte> value,
                                const std::uint64_t expire_at_ns, const ValueType type,
                                const std::uint32_t flags) -> DurableMutationResult {
    return mutate(
        std::span<const std::byte>{reinterpret_cast<const std::byte*>(key.key.data()), key.key.size()}, value,
        Opcode::put, key.hash, expire_at_ns, type, flags);
}

auto DurableRuntimeCatalog::erase(const std::span<const std::byte> key) -> DurableMutationResult {
    return mutate(key, {}, Opcode::erase, hash_key_routing(key, worker_routing_), 0, ValueType::bytes, 0);
}

auto DurableRuntimeCatalog::erase(const HashedKey& key) -> DurableMutationResult {
    return mutate(
        std::span<const std::byte>{reinterpret_cast<const std::byte*>(key.key.data()), key.key.size()}, {},
        Opcode::erase, key.hash, 0, ValueType::bytes, 0);
}

auto DurableRuntimeCatalog::mutate(const std::span<const std::byte> key,
                                   const std::span<const std::byte> value, const Opcode opcode,
                                   const std::uint64_t key_hash, const std::uint64_t expire_at_ns,
                                   const ValueType type, const std::uint32_t flags, const bool writer_batch)
    -> DurableMutationResult {
    auto exception_outcome = DurableMutationOutcome::not_committed;
    std::optional<std::chrono::steady_clock::time_point> final_record_commit_started;
    bool final_record_committed{};
    ScopeExit final_record_telemetry{[&, this]() noexcept {
        if (final_record_commit_started) {
            record_rotation_final_commit(steady_elapsed_ns(*final_record_commit_started),
                                         final_record_committed);
        }
    }};
    try {
        if (!healthy()) {
            return mutation_failure(DurableMutationOutcome::not_committed,
                                    Error{ErrorCode::unavailable, "durable runtime is fail-closed"});
        }
        const auto worker_index = route_worker(key_hash, workers_.size());
        auto& worker = *workers_[worker_index];
        const bool strict_batch = options_.batch.has_value() && options_.strict_ack;

        struct GroupAdmission final {
            std::atomic_size_t* active{};

            explicit GroupAdmission(std::atomic_size_t* counter) noexcept : active(counter) {
                if (active) {
                    active->fetch_add(1U, std::memory_order_relaxed);
                }
            }

            ~GroupAdmission() {
                if (active) {
                    active->fetch_sub(1U, std::memory_order_relaxed);
                }
            }

            GroupAdmission(const GroupAdmission&) = delete;
            auto operator=(const GroupAdmission&) -> GroupAdmission& = delete;
        } admission{strict_batch ? &worker.active_group_mutations : nullptr};

        // Paired exclusive Writer (no background flusher): skip Worker mutex on the
        // ordinary mutate hot path. Compaction observes hot_path_depth instead.
        // Group/periodic paths keep the mutex because the flush coordinator shares
        // pending-batch and file state with mutate.
        const bool elide_worker_mutex = options_.exclusive_writer && flusher_ == nullptr;
        std::unique_lock worker_lock{worker.mutex, std::defer_lock};

        struct ExclusiveHotPathGuard final {
            RuntimeWorker* worker{};

            explicit ExclusiveHotPathGuard(RuntimeWorker* owner) noexcept : worker(owner) {
                if (worker != nullptr) {
                    worker->hot_path_depth.fetch_add(1U, std::memory_order_acq_rel);
                }
            }

            ~ExclusiveHotPathGuard() {
                if (worker == nullptr) {
                    return;
                }
                if (worker->hot_path_depth.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
                    worker->hot_path_depth.notify_all();
                }
            }

            ExclusiveHotPathGuard(const ExclusiveHotPathGuard&) = delete;
            auto operator=(const ExclusiveHotPathGuard&) -> ExclusiveHotPathGuard& = delete;
        } hot_path{elide_worker_mutex ? &worker : nullptr};

        if (elide_worker_mutex) {
#ifndef NDEBUG
            // Debug: the exclusive Writer must not find the mutex already held by
            // compaction/verify/backup. If it is, wait then release — those paths
            // retain the lock by design.
            if (!worker_lock.try_lock()) {
                worker_lock.lock();
            }
            worker_lock.unlock();
#endif
            if (worker.compaction_commit_active.load(std::memory_order_acquire)) {
                return mutation_failure(
                    DurableMutationOutcome::not_committed,
                    Error{ErrorCode::sequence_conflict,
                          "durable mutation conflicts with compaction manifest publication"});
            }
        } else {
            worker_lock.lock();
            // Both waits release the Worker mutex. Recheck both gates after every
            // wake so a producer cannot enter a closing batch while another
            // producer has just returned the writable handle (or vice versa).
            while (healthy() &&
                   (worker.mutation_io_active || (dedicated_commit_executor_ && worker.batch_closing))) {
                if (dedicated_commit_executor_ && worker.batch_closing) {
                    worker.batch_closed.wait(worker_lock);
                } else {
                    worker.mutation_io_finished.wait(worker_lock);
                }
            }
        }
        if (!healthy()) {
            return mutation_failure(DurableMutationOutcome::not_committed,
                                    Error{ErrorCode::unavailable, "durable runtime is fail-closed"});
        }
        if (auto drained = worker.drain_deferred_ttl(worker.deferred_ttl_reclaims.size()); !drained) {
            // Mirror GET: sticky. Never call abandon_pending_batches() while this
            // Worker mutex is held (non-recursive); clear under lock, then unlock.
            healthy_.store(false, std::memory_order_release);
            worker.pending_group_mutations.clear();
            worker.pending_group_insertions = 0;
            worker.pending_group_heap_key_bytes = 0;
            worker.batch_started = {};
            worker.batch_closing = false;
            worker.batch_closed.notify_all();
            if (worker_lock.owns_lock()) {
                worker_lock.unlock();
            }
            abandon_pending_batches();
            return mutation_failure(DurableMutationOutcome::indeterminate, drained.error());
        }
        if (worker.compaction_commit_active.load(std::memory_order_acquire)) {
            return mutation_failure(DurableMutationOutcome::not_committed,
                                    Error{ErrorCode::sequence_conflict,
                                          "durable mutation conflicts with compaction manifest publication"});
        }
        if (worker.next_sequence.value == 0 ||
            worker.next_sequence.value == std::numeric_limits<std::uint64_t>::max()) {
            return mutation_failure(
                DurableMutationOutcome::not_committed,
                Error{ErrorCode::arithmetic_overflow, "Worker sequence space is exhausted"});
        }

        const HashedKey hashed{.key = as_string_view(key), .hash = key_hash};
        bool key_present = worker.index.find(hashed).has_value();
        if (strict_batch) {
            const auto pending = std::find_if(
                worker.pending_group_mutations.rbegin(), worker.pending_group_mutations.rend(),
                [&](const PendingGroupMutation& mutation) { return mutation.key == hashed.key; });
            if (pending != worker.pending_group_mutations.rend()) {
                key_present = pending->opcode == Opcode::put;
            }
        }
        if (opcode == Opcode::erase && !key_present) {
            return mutation_failure(DurableMutationOutcome::not_committed,
                                    Error{ErrorCode::not_found, "key is not present"});
        }
        std::size_t prospective_group_insertions = worker.pending_group_insertions;
        std::size_t prospective_group_heap_key_bytes = worker.pending_group_heap_key_bytes;
        if (opcode == Opcode::put) {
            if (!key_present) {
                const auto live_key_limit = durable_worker_live_key_limit(worker_index, workers_.size(),
                                                                          options_.limits.max_live_keys);
                const auto current_size = worker.index.stats().size;
                if (current_size >= live_key_limit ||
                    prospective_group_insertions >= live_key_limit - current_size) {
                    return mutation_failure(
                        DurableMutationOutcome::not_committed,
                        Error{ErrorCode::resource_exhausted, "durable Worker live-key budget is exhausted"});
                }
            }
            if (auto prepared = worker.index.prepare_insert(hashed); !prepared) {
                return mutation_failure(DurableMutationOutcome::not_committed, prepared.error());
            }
            if (strict_batch && !key_present) {
                if (prospective_group_insertions == std::numeric_limits<std::size_t>::max() ||
                    (key.size() > kSwissInlineKeyBytes &&
                     key.size() >
                         std::numeric_limits<std::size_t>::max() - prospective_group_heap_key_bytes)) {
                    return mutation_failure(
                        DurableMutationOutcome::not_committed,
                        Error{ErrorCode::arithmetic_overflow, "group publication capacity overflow"});
                }
                ++prospective_group_insertions;
                if (key.size() > kSwissInlineKeyBytes) {
                    prospective_group_heap_key_bytes += key.size();
                }
                if (auto prepared = worker.index.prepare_batch_insert(prospective_group_insertions,
                                                                      prospective_group_heap_key_bytes);
                    !prepared) {
                    return mutation_failure(DurableMutationOutcome::not_committed, prepared.error());
                }
            }
        }
        const RecordInput input{.sequence = worker.next_sequence,
                                .opcode = opcode,
                                .type = type,
                                .flags = flags,
                                .key_hash = key_hash,
                                .expire_at_ns = expire_at_ns,
                                .key = key,
                                .value = value,
                                .format_version = manifest_.format_version};
        const auto committed_sequence = worker.next_sequence;
        const auto encoded_size = encoded_record_size(input);
        if (!encoded_size) {
            return mutation_failure(DurableMutationOutcome::not_committed, encoded_size.error());
        }
        worker.encode_scratch.resize(*encoded_size);
        if (auto encoded = encode_record(worker.encode_scratch, input, *encoded_size); !encoded) {
            return mutation_failure(DurableMutationOutcome::not_committed, encoded.error());
        }
        // The mutation, not the Worker, owns encoded bytes across every
        // unlocked I/O or publication wait. Move preserves scratch capacity on
        // the uncontended path and prevents a later same-Worker producer from
        // overwriting a sleeping mutation's final Record.
        std::vector<std::byte> encoded_record{std::move(worker.encode_scratch)};
        ScopeExit restore_encode_scratch{[&]() noexcept {
            if (worker_lock.owns_lock() || elide_worker_mutex) {
                worker.encode_scratch = std::move(encoded_record);
            }
        }};
        PendingGroupMutation group_mutation;
        PreparedHotRecord prepared_hot_record;
        if (opcode == Opcode::put) {
            constexpr auto group_publication_fixed_bytes =
                static_cast<std::uint64_t>(sizeof(PendingGroupMutation));
            const auto publication_staging_bytes =
                strict_batch
                    ? (key.size() > std::numeric_limits<std::uint64_t>::max() - group_publication_fixed_bytes
                           ? std::numeric_limits<std::uint64_t>::max()
                           : group_publication_fixed_bytes + static_cast<std::uint64_t>(key.size()))
                    : 0U;
            auto prepared =
                worker.prepare_hot_record(worker_index, workers_.size(), options_.limits, hashed.key,
                                          hashed.hash, value, expire_at_ns, publication_staging_bytes);
            if (!prepared) {
                return mutation_failure(DurableMutationOutcome::not_committed, prepared.error());
            }
            if (strict_batch) {
                group_mutation.hot_record = std::move(*prepared);
            } else {
                prepared_hot_record = std::move(*prepared);
            }
        }
        if (strict_batch) {
            group_mutation.key.assign(hashed.key);
            group_mutation.opcode = opcode;
            group_mutation.key_hash = key_hash;
            group_mutation.expire_at_ns = expire_at_ns;
            worker.pending_group_mutations.reserve(worker.pending_group_mutations.size() + 1U);
        }

        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            std::shared_lock catalog_lock{catalog_mutex_};
            const auto position =
                std::lower_bound(manifest_.segments.begin(), manifest_.segments.end(), worker.active_segment,
                                 [](const ManifestSegmentEntry& entry, const SegmentId id) {
                                     return entry.segment_id.value < id.value;
                                 });
            if (position == manifest_.segments.end() || position->segment_id != worker.active_segment) {
                healthy_.store(false, std::memory_order_release);
                return mutation_failure(
                    DurableMutationOutcome::indeterminate,
                    Error{ErrorCode::corrupted_data, "runtime active Segment is absent from the manifest"});
            }
            const auto catalog_index = static_cast<std::size_t>(position - manifest_.segments.begin());
            const SegmentHeaderIdentity identity{.store_id = manifest_.store_id,
                                                 .segment_id = position->segment_id,
                                                 .generation = position->generation,
                                                 .owner_worker = position->owner_worker,
                                                 .format_version = manifest_.format_version};
            if (catalog_index >= generation_pins_.size() || !generation_pins_[catalog_index] ||
                generation_pins_[catalog_index]->identity != identity) {
                healthy_.store(false, std::memory_order_release);
                return mutation_failure(
                    DurableMutationOutcome::indeterminate,
                    Error{ErrorCode::corrupted_data, "active Segment has no exact generation pin"});
            }
            const auto active_pin = generation_pins_[catalog_index];
            const auto expected_selected = segments_[catalog_index].selected;
            const bool batch_enabled = options_.batch.has_value();
            const bool deferred_commit = options_.commit_sync == SegmentCommitSync::deferred;
            SegmentCommitResult appended{};
            std::optional<DurableSegmentFile> io_file;
            if (worker.cached_file && worker.cached_file->identity() == identity && worker.cached_writable) {
                io_file.emplace(std::move(*worker.cached_file));
            }
            worker.cached_file.reset();
            worker.cached_writable = false;
            worker.mutation_io_active = true;
            catalog_lock.unlock();
            if (worker_lock.owns_lock()) {
                worker_lock.unlock();
            }

            std::uint32_t offset{};
            // Keep exception_outcome as not_committed through open + append. before-hooks and
            // Record writes that never start are known not committed; append's own outcome
            // (or a later Index/publication boundary) upgrades to indeterminate when needed.
            // Segment open / selected-commit mismatch before append does not write a Record —
            // same polarity as sealed-reader / replacement open (not_committed, not sticky).
            try {
                if (!io_file) {
                    auto opened =
                        DurableSegmentFile::open(directory_, identity, SegmentFileOpenMode::read_write);
                    if (!opened || opened->selected_commit() != expected_selected) {
                        appended = {
                            .outcome = SegmentCommitOutcome::not_committed,
                            .error = opened ? Error{ErrorCode::corrupted_data,
                                                    "active Segment changed after I/O reservation"}
                                            : opened.error(),
                        };
                    } else {
                        io_file.emplace(std::move(*opened));
                    }
                }
                if (!appended.error) {
                    offset = io_file->selected_commit().commit.committed_end;
                    // V2 blob history must rotate before file quotas can strand obsolete
                    // descriptors in a mostly empty active Segment. V1 is unchanged.
                    if (identity.format_version == 2 && type == ValueType::blob_ref &&
                        io_file->selected_commit().commit.record_count >= 32 && attempt == 0) {
                        appended = {.outcome = SegmentCommitOutcome::not_committed,
                                    .error =
                                        Error{ErrorCode::segment_full, "blob descriptor rotation quantum"}};
                    } else {
                        appended = batch_enabled || deferred_commit
                                       ? io_file->append_record(encoded_record)
                                       : io_file->append(encoded_record, options_.commit_sync);
                    }
                    if (appended.outcome == SegmentCommitOutcome::indeterminate) {
                        exception_outcome = DurableMutationOutcome::indeterminate;
                    }
                }
            } catch (const std::bad_alloc&) {
                appended = {.outcome = exception_outcome == DurableMutationOutcome::indeterminate
                                           ? SegmentCommitOutcome::indeterminate
                                           : SegmentCommitOutcome::not_committed,
                            .error = Error{ErrorCode::resource_exhausted, {}}};
            } catch (...) {
                appended = {.outcome = exception_outcome == DurableMutationOutcome::indeterminate
                                           ? SegmentCommitOutcome::indeterminate
                                           : SegmentCommitOutcome::not_committed,
                            .error = Error{ErrorCode::internal_error, {}}};
            }

            if (!elide_worker_mutex) {
                worker_lock.lock();
            }
            catalog_lock.lock();
            const auto current_position =
                std::lower_bound(manifest_.segments.begin(), manifest_.segments.end(), identity.segment_id,
                                 [](const ManifestSegmentEntry& entry, const SegmentId id) {
                                     return entry.segment_id.value < id.value;
                                 });
            const auto current_pin_index = catalog_index_for_segment(identity.segment_id);
            const bool reservation_valid = current_position != manifest_.segments.end() &&
                                           current_position->segment_id == identity.segment_id &&
                                           current_position->generation == identity.generation &&
                                           current_position->owner_worker == identity.owner_worker &&
                                           current_position->role == ManifestSegmentRole::active &&
                                           worker.active_segment == identity.segment_id &&
                                           worker.mutation_io_active && current_pin_index.has_value() &&
                                           generation_pins_[*current_pin_index] == active_pin;
            if (io_file) {
                worker.cached_file.emplace(std::move(*io_file));
                worker.cached_writable = true;
            }
            worker.mutation_io_active = false;
            worker.mutation_io_finished.notify_all();
            if (!reservation_valid) {
                healthy_.store(false, std::memory_order_release);
                return mutation_failure(
                    DurableMutationOutcome::indeterminate,
                    Error{ErrorCode::corrupted_data, "mutation I/O reservation failed relinearization"});
            }
            const auto current_catalog_index =
                static_cast<std::size_t>(current_position - manifest_.segments.begin());
            if (!appended.committed()) {
                if (appended.error &&
                    (appended.error->code == ErrorCode::segment_full ||
                     appended.error->code == ErrorCode::segment_sealed) &&
                    attempt == 0) {
                    if (strict_batch && !worker.pending_group_mutations.empty()) {
                        if (dedicated_commit_executor_) {
                            worker.batch_closing = true;
                            catalog_lock.unlock();
                            flusher_->request_flush();
                            worker.batch_closed.wait(worker_lock, [&] {
                                return worker.pending_group_mutations.empty() || !healthy();
                            });
                            if (!healthy()) {
                                return mutation_failure(
                                    DurableMutationOutcome::indeterminate,
                                    Error{ErrorCode::unavailable, "durable runtime is fail-closed"});
                            }
                        } else if (auto flushed = flush_worker_batch(worker, worker_lock, catalog_lock,
                                                                     SegmentCommitSync::immediate);
                                   !flushed) {
                            return mutation_failure(DurableMutationOutcome::indeterminate, flushed.error());
                        }
                        prospective_group_insertions = opcode == Opcode::put && !key_present ? 1U : 0U;
                        prospective_group_heap_key_bytes =
                            prospective_group_insertions != 0 && key.size() > kSwissInlineKeyBytes
                                ? key.size()
                                : 0U;
                    }
                    if (catalog_lock.owns_lock()) {
                        catalog_lock.unlock();
                    }
                    const auto rotated = rotate_active(worker, worker_lock);
                    if (!rotated.committed()) {
                        return rotated;
                    }
                    final_record_commit_started = std::chrono::steady_clock::now();
                    continue;
                }
                if (appended.outcome == SegmentCommitOutcome::indeterminate || !directory_.healthy()) {
                    healthy_.store(false, std::memory_order_release);
                    worker.pending_group_mutations.clear();
                    worker.pending_group_insertions = 0;
                    worker.pending_group_heap_key_bytes = 0;
                    worker.batch_closing = false;
                    worker.batch_closed.notify_all();
                }
                return mutation_failure(
                    appended.outcome == SegmentCommitOutcome::indeterminate
                        ? DurableMutationOutcome::indeterminate
                        : DurableMutationOutcome::not_committed,
                    appended.error.value_or(Error{ErrorCode::io_error, "Record append failed"}));
            }
            if (!worker.cached_file) {
                healthy_.store(false, std::memory_order_release);
                return mutation_failure(
                    DurableMutationOutcome::indeterminate,
                    Error{ErrorCode::corrupted_data, "committed mutation lost its writable Segment handle"});
            }

            segments_[current_catalog_index].selected = worker.cached_file->selected_commit();
            ++worker.next_sequence.value;

            const RecordRef reference{.segment_id = identity.segment_id,
                                      .offset = RecordOffset{offset},
                                      .size = RecordSize{static_cast<std::uint32_t>(encoded_record.size())},
                                      .sequence = committed_sequence,
                                      .generation = identity.generation};
            if (strict_batch) {
                group_mutation.reference = reference;
                worker.pending_group_mutations.push_back(std::move(group_mutation));
                worker.pending_group_insertions = prospective_group_insertions;
                worker.pending_group_heap_key_bytes = prospective_group_heap_key_bytes;
            }

            if (batch_enabled) {
                worker.batch_metrics.pending_records.store(
                    static_cast<std::size_t>(worker.cached_file->pending_record_count()),
                    std::memory_order_relaxed);
                worker.batch_metrics.pending_bytes.store(worker.cached_file->pending_bytes(),
                                                         std::memory_order_relaxed);
                if (worker.batch_started == std::chrono::steady_clock::time_point{}) {
                    worker.batch_started = std::chrono::steady_clock::now();
                    if (dedicated_commit_executor_ && flusher_) {
                        flusher_->request_flush_at(worker.batch_started +
                                                   std::chrono::milliseconds{options_.batch->max_wait_ms});
                    }
                }
                if (options_.strict_ack && !writer_batch) {
                    if (should_flush_batch(worker)) {
                        if (dedicated_commit_executor_) {
                            worker.batch_closing = true;
                            catalog_lock.unlock();
                            flusher_->request_flush();
                            wait_for_batch_close(worker, committed_sequence, worker_lock);
                            if (worker.durable_through.value >= committed_sequence.value) {
                                // Index already published this sequence; sticky close is not
                                // indeterminate for RAW (ACK + drain-snapshot).
                                final_record_committed = true;
                                return {.outcome = DurableMutationOutcome::committed,
                                        .sequence = committed_sequence,
                                        .error = std::nullopt};
                            }
                            return mutation_failure(
                                DurableMutationOutcome::indeterminate,
                                Error{ErrorCode::unavailable, "durable runtime is fail-closed"});
                        } else if (auto flushed = flush_worker_batch(worker, worker_lock, catalog_lock,
                                                                     SegmentCommitSync::immediate);
                                   !flushed) {
                            if (worker.durable_through.value >= committed_sequence.value) {
                                final_record_committed = true;
                                return {.outcome = DurableMutationOutcome::committed,
                                        .sequence = committed_sequence,
                                        .error = std::nullopt};
                            }
                            return mutation_failure(DurableMutationOutcome::indeterminate, flushed.error());
                        }
                    } else {
                        catalog_lock.unlock();
                        wait_for_batch_close(worker, committed_sequence, worker_lock);
                        if (worker.durable_through.value >= committed_sequence.value) {
                            final_record_committed = true;
                            return {.outcome = DurableMutationOutcome::committed,
                                    .sequence = committed_sequence,
                                    .error = std::nullopt};
                        }
                        return mutation_failure(
                            DurableMutationOutcome::indeterminate,
                            Error{ErrorCode::unavailable, "durable runtime is fail-closed"});
                    }
                } else if (should_flush_batch(worker)) {
                    // A Writer-owned input may contain multiple physical groups, and one
                    // admitted record may itself exceed max_bytes. Every strict threshold
                    // therefore closes here with an immediate slot sync; the explicit
                    // commit_writer_batch() after the append loop closes only the residual.
                    // Using deferred here could clear the pending group and turn that final
                    // strict commit into a no-op, permitting an ACK without slot durability.
                    const auto threshold_sync = options_.strict_ack && writer_batch
                                                    ? SegmentCommitSync::immediate
                                                    : SegmentCommitSync::deferred;
                    if (auto flushed = flush_worker_batch(worker, worker_lock, catalog_lock, threshold_sync);
                        !flushed) {
                        if (worker.durable_through.value >= committed_sequence.value) {
                            // Threshold publication advanced durable_through before secondary
                            // accounting failed.
                            final_record_committed = true;
                            return {.outcome = DurableMutationOutcome::committed,
                                    .sequence = committed_sequence,
                                    .error = std::nullopt};
                        }
                        return mutation_failure(DurableMutationOutcome::indeterminate, flushed.error());
                    }
                } else if (flusher_) {
                    flusher_->notify_batch_activity();
                }
            } else if (deferred_commit && flusher_) {
                flusher_->notify_batch_activity();
            }

            if (strict_batch) {
                final_record_committed = true;
                return {.outcome = DurableMutationOutcome::committed,
                        .sequence = committed_sequence,
                        .error = std::nullopt};
            }
            if (opcode == Opcode::put) {
                const auto published = worker.index.insert_or_assign(hashed, reference);
                if (!published) {
                    healthy_.store(false, std::memory_order_release);
                    return {.outcome = DurableMutationOutcome::committed,
                            .sequence = committed_sequence,
                            .error = published.error()};
                }
                // Index authority applied: advance durable_through before secondary work.
                worker.record_compaction_change(hashed, reference);
                worker.durable_through = committed_sequence;
                try {
                    if (glifistore::fault::consume_fail(glifistore::fault::Site::index_account)) {
                        healthy_.store(false, std::memory_order_release);
                        return {.outcome = DurableMutationOutcome::committed,
                                .sequence = committed_sequence,
                                .error = Error{ErrorCode::resource_exhausted,
                                               "injected Index accounting failure"}};
                    }
                    if (auto counted = worker.update_live_record_bytes(published->previous, reference);
                        !counted) {
                        healthy_.store(false, std::memory_order_release);
                        return {.outcome = DurableMutationOutcome::committed,
                                .sequence = committed_sequence,
                                .error = counted.error()};
                    }
                    if (prepared_hot_record.empty()) {
                        worker.erase_hot_record(hashed);
                    } else if (auto hot_published = worker.publish_hot_record(prepared_hot_record, reference);
                               !hot_published) {
                        healthy_.store(false, std::memory_order_release);
                        return {.outcome = DurableMutationOutcome::committed,
                                .sequence = committed_sequence,
                                .error = hot_published.error()};
                    }
                } catch (const std::bad_alloc&) {
                    healthy_.store(false, std::memory_order_release);
                    return {.outcome = DurableMutationOutcome::committed,
                            .sequence = committed_sequence,
                            .error =
                                Error{ErrorCode::resource_exhausted, "Index accounting allocation failed"}};
                } catch (...) {
                    healthy_.store(false, std::memory_order_release);
                    return {.outcome = DurableMutationOutcome::committed,
                            .sequence = committed_sequence,
                            .error = Error{ErrorCode::internal_error, "Index accounting failed"}};
                }
            } else {
                const auto erased = worker.index.erase_no_compact(hashed);
                worker.record_compaction_change(hashed, std::nullopt);
                worker.durable_through = committed_sequence;
                try {
                    if (glifistore::fault::consume_fail(glifistore::fault::Site::index_account)) {
                        healthy_.store(false, std::memory_order_release);
                        return {.outcome = DurableMutationOutcome::committed,
                                .sequence = committed_sequence,
                                .error = Error{ErrorCode::resource_exhausted,
                                               "injected Index accounting failure"}};
                    }
                    if (auto counted = worker.update_live_record_bytes(erased.previous, std::nullopt);
                        !counted) {
                        healthy_.store(false, std::memory_order_release);
                        return {.outcome = DurableMutationOutcome::committed,
                                .sequence = committed_sequence,
                                .error = counted.error()};
                    }
                    worker.erase_hot_record(hashed);
                } catch (const std::bad_alloc&) {
                    healthy_.store(false, std::memory_order_release);
                    return {.outcome = DurableMutationOutcome::committed,
                            .sequence = committed_sequence,
                            .error =
                                Error{ErrorCode::resource_exhausted, "Index accounting allocation failed"}};
                } catch (...) {
                    healthy_.store(false, std::memory_order_release);
                    return {.outcome = DurableMutationOutcome::committed,
                            .sequence = committed_sequence,
                            .error = Error{ErrorCode::internal_error, "Index accounting failed"}};
                }
            }
            final_record_committed = true;
            return {.outcome = DurableMutationOutcome::committed,
                    .sequence = committed_sequence,
                    .error = std::nullopt};
        }
        return mutation_failure(
            DurableMutationOutcome::not_committed,
            Error{ErrorCode::segment_full, "Record does not fit after one durable rotation"});
    } catch (const std::bad_alloc&) {
        if (exception_outcome != DurableMutationOutcome::not_committed) {
            abandon_pending_batches();
        }
        return mutation_failure(exception_outcome, Error{ErrorCode::resource_exhausted, {}});
    } catch (...) {
        if (exception_outcome != DurableMutationOutcome::not_committed) {
            abandon_pending_batches();
        }
        return mutation_failure(exception_outcome, Error{ErrorCode::internal_error, {}});
    }
}

} // namespace glifistore
