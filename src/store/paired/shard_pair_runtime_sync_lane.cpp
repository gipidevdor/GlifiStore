#include "glifistore/core/fault_injection.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/store/paired/completion_policy.hpp"
#include "glifistore/store/paired/fail_closed_state.hpp"
#include "glifistore/store/paired/lane_publication.hpp"
#include "glifistore/store/paired/mutation_batch.hpp"
#include "glifistore/store/paired/mutation_execution.hpp"
#include "glifistore/store/paired/mutation_recovery.hpp"
#include "glifistore/store/paired/mutation_state.hpp"
#include "glifistore/store/paired/publication_coordinator.hpp"
#include "glifistore/store/paired/shard_combining_executor.hpp"
#include "glifistore/store/paired/shard_pair_runtime.hpp"
#include "glifistore/store/paired/volatile_sync_chunk.hpp"
#include "store/paired/shard_pair_runtime_impl.hpp"
#include "store/store_internal.hpp"

#include <array>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace glifistore::store::paired {

void ShardPairRuntime::combiner_housekeeping(const std::size_t shard,
                                             const std::size_t publication_records) noexcept {
    try {
        auto& lane = *lanes_[shard];
        const auto reclaim_quiescent = [&]() noexcept {
            try {
                reclaim_quiescent_generations(lane.generation, lane.reclaim, config_.reader_epoch_lease);
            } catch (...) {
                FailClosedState{store_, healthy_, expire_remaining_}.arm(fail_closed_wakes_,
                                                                         FailClosedScope::pair_and_store);
            }
        };
        if (lane.generation.reclaim_requested.exchange(false, std::memory_order_acq_rel)) {
            reclaim_quiescent();
        }
        if (!healthy_.load(std::memory_order_acquire)) {
            return;
        }
        if (!lane.merge.read_merge &&
            (lane.generation.writer_generation->delta_entries() >= config_.merge_delta_entries ||
             lane.generation.writer_generation->delta_record_versions() >= config_.merge_delta_entries)) {
            Result<std::unique_ptr<PairReadMerge>> started{fail(ErrorCode::internal_error, "merge start")};
            started = start_incremental_merge(lane.generation, config_.merge_maximum_post_entries);
            if (started) {
                lane.merge.read_merge = std::move(*started);
                lane.merge.read_merge_active.store(true, std::memory_order_relaxed);
                lane.merge.read_merge_post_entries.store(0U, std::memory_order_relaxed);
                store_merge_progress(lane.merge);
                lane.merge.read_merge_starts.fetch_add(1U, std::memory_order_relaxed);
            } else {
                lane.merge.read_merge_failures.fetch_add(1U, std::memory_order_relaxed);
            }
        }
        if (!lane.merge.read_merge) {
            return;
        }
        if (!PairReadGeneration::merge_ready(*lane.merge.read_merge)) {
            const auto advance_budget = PairReadGeneration::merge_advance_budget(
                *lane.merge.read_merge, publication_records,
                runtime_detail::merge_minimum_advance_slots(config_.merge_quantum_slots,
                                                            publication_records));
            auto advanced =
                PairReadGeneration::advance_incremental_merge(*lane.merge.read_merge, advance_budget);
            if (!advanced) {
                lane.merge.read_merge_failures.fetch_add(1U, std::memory_order_relaxed);
                lane.merge.read_merge.reset();
                lane.merge.read_merge_active.store(false, std::memory_order_relaxed);
                lane.merge.read_merge_post_entries.store(0U, std::memory_order_relaxed);
                store_merge_progress(lane.merge);
                return;
            }
            runtime_detail::note_merge_advance(lane.merge, *advanced);
        }
        if (!PairReadGeneration::merge_ready(*lane.merge.read_merge)) {
            return;
        }
        reclaim_quiescent();
        if (decide_generation_admission(lane.generation.retired_debt(),
                                        ShardPairRuntime::kMaximumRetiredReadGenerations,
                                        true) != GenerationAdmissionDecision::admitted) {
            return;
        }
        LanePublicationContext publication_ctx{lane.generation, lane.merge, store_, shard,
                                               ShardPairRuntime::kMaximumRetiredReadGenerations};
        const auto outcome = finish_incremental_merge_and_publish(publication_ctx);
        if (outcome.status != DualPathPublishStatus::published) {
            return;
        }
        lane.merge.read_merge.reset();
        lane.merge.read_merge_active.store(false, std::memory_order_relaxed);
        lane.merge.read_merge_post_entries.store(0U, std::memory_order_relaxed);
        store_merge_progress(lane.merge);
        lane.merge.read_merge_completions.fetch_add(1U, std::memory_order_relaxed);
        reclaim_quiescent();
    } catch (const std::bad_alloc&) {
        FailClosedState{store_, healthy_, expire_remaining_}.arm(fail_closed_wakes_,
                                                                 FailClosedScope::pair_and_store);
    } catch (...) {
        FailClosedState{store_, healthy_, expire_remaining_}.arm(fail_closed_wakes_,
                                                                 FailClosedScope::pair_and_store);
    }
}

void ShardPairRuntime::process_sync_lane(const std::size_t shard) noexcept {
    auto& lane = *lanes_[shard];
    // durable_group coalesce stays on the dedicated Writer loop in run().
    if (detail::StoreAccess::is_durable(store_) &&
        detail::StoreAccess::durable_writer_batch_config(store_).has_value()) {
        return;
    }
    FailClosedState fail_closed{store_, healthy_, expire_remaining_};
    LanePublicationContext publication_ctx{lane.generation, lane.merge, store_, shard,
                                           ShardPairRuntime::kMaximumRetiredReadGenerations};
    // Stable functors for apply_volatile (pointers; no per-chunk std::function temps).
    // Construction itself may allocate — catch before the rest of this noexcept drain.
    std::function<void()> publish_fail_closed;
    std::function<void()> reclaim_proportional;
    try {
        publish_fail_closed = [&]() noexcept {
            fail_closed.arm(fail_closed_wakes_, FailClosedScope::pair_and_store);
        };
        reclaim_proportional = [&]() noexcept {
            if (lane.merge.read_merge) {
                store_merge_progress(lane.merge);
            }
            constexpr std::size_t kReclaimPublishQuantum = 8;
            if (lane.generation.retired_debt() >= kReclaimPublishQuantum ||
                lane.generation.retired_debt() + 1U >= ShardPairRuntime::kMaximumRetiredReadGenerations) {
                lane.generation.reclaim_requested.store(true, std::memory_order_release);
                combiner_housekeeping(shard);
            } else {
                lane.generation.retired_generation_count.store(lane.generation.retired_debt(),
                                                               std::memory_order_relaxed);
            }
        };
    } catch (...) {
        fail_closed.arm(fail_closed_wakes_, FailClosedScope::pair_and_store);
        return;
    }
    const auto drain_durable_snapshot = [&]() noexcept -> bool {
        return try_drain_durable_snapshot(publication_ctx, true, &reclaim_proportional);
    };

    for (;;) {
        SyncMutation* sync_batch = nullptr;
        {
            const std::lock_guard lock{lane.sync.sync_mutex};
            sync_batch = lane.sync_head;
            lane.sync_head = nullptr;
        }
        if (sync_batch == nullptr) {
            break;
        }
        SyncMutation* rev = nullptr;
        while (sync_batch != nullptr) {
            auto* next = sync_batch->next;
            sync_batch->next = rev;
            rev = sync_batch;
            sync_batch = next;
        }
        if (!healthy_.load(std::memory_order_acquire)) {
            for (auto* node = rev; node != nullptr;) {
                auto* const next = node->next;
                node->status = Status{fail(ErrorCode::resource_exhausted, "paired runtime is fail-closed")};
                node->done.store(true, std::memory_order_release);
                node->done.notify_one();
                node = next;
            }
            continue;
        }

        if (!detail::StoreAccess::is_durable(store_)) {
            while (rev != nullptr) {
                if (!healthy_.load(std::memory_order_acquire)) {
                    while (rev != nullptr) {
                        auto* const next = rev->next;
                        rev->status =
                            Status{fail(ErrorCode::resource_exhausted, "paired runtime is fail-closed")};
                        rev->done.store(true, std::memory_order_release);
                        rev->done.notify_one();
                        rev = next;
                    }
                    break;
                }
                std::array<SyncMutation*, kMaximumPublicationBatch> chunk{};
                std::size_t chunk_size = 0;
                while (rev != nullptr && chunk_size < kMaximumPublicationBatch) {
                    chunk[chunk_size++] = rev;
                    rev = rev->next;
                }
                std::optional<GenerationSlotPool::Reservation> slot_reservation;
                // process_sync_lane is noexcept: catch pre-apply allocation faults.
                try {
                    lane.generation.reclaim_requested.store(true, std::memory_order_release);
                    combiner_housekeeping(shard, chunk_size);
                    if (!healthy_.load(std::memory_order_acquire)) {
                        for (std::size_t index = 0; index < chunk_size; ++index) {
                            chunk[index]->status =
                                Status{fail(ErrorCode::resource_exhausted, "paired runtime is fail-closed")};
                            chunk[index]->done.store(true, std::memory_order_release);
                            chunk[index]->done.notify_one();
                        }
                        while (rev != nullptr) {
                            auto* const next = rev->next;
                            rev->status =
                                Status{fail(ErrorCode::resource_exhausted, "paired runtime is fail-closed")};
                            rev->done.store(true, std::memory_order_release);
                            rev->done.notify_one();
                            rev = next;
                        }
                        break;
                    }
                    auto generation_admission = decide_generation_admission(
                        lane.generation.retired_debt(), ShardPairRuntime::kMaximumRetiredReadGenerations,
                        PairReadGeneration::can_publish_incremental(*lane.generation.writer_generation,
                                                                    lane.merge.read_merge.get(), chunk_size));
                    generation_admission = runtime_detail::wait_generation_admission(
                        generation_admission, [&] { combiner_housekeeping(shard, chunk_size); },
                        [&] {
                            return decide_generation_admission(
                                lane.generation.retired_debt(),
                                ShardPairRuntime::kMaximumRetiredReadGenerations,
                                PairReadGeneration::can_publish_incremental(
                                    *lane.generation.writer_generation, lane.merge.read_merge.get(),
                                    chunk_size));
                        });
                    if (generation_admission != GenerationAdmissionDecision::admitted) {
                        lane.generation.generation_admission_backpressure_total.fetch_add(
                            chunk_size, std::memory_order_relaxed);
                        if (generation_admission == GenerationAdmissionDecision::incremental_merge_required) {
                            lane.merge.read_merge_backpressure.fetch_add(1U, std::memory_order_relaxed);
                        }
                        for (std::size_t index = 0; index < chunk_size; ++index) {
                            chunk[index]->status =
                                Status{fail(ErrorCode::resource_exhausted,
                                            generation_admission_message(generation_admission))};
                            chunk[index]->done.store(true, std::memory_order_release);
                            chunk[index]->done.notify_one();
                        }
                        continue;
                    }
                    if (lane.generation.uses_slot_pool()) {
                        slot_reservation = try_reserve_publication_slot(lane.generation);
                        if (!slot_reservation) {
                            lane.generation.generation_admission_backpressure_total.fetch_add(
                                chunk_size, std::memory_order_relaxed);
                            for (std::size_t index = 0; index < chunk_size; ++index) {
                                chunk[index]->status = Status{
                                    fail(ErrorCode::resource_exhausted,
                                         generation_admission_message(
                                             GenerationAdmissionDecision::reader_quiescence_required))};
                                chunk[index]->done.store(true, std::memory_order_release);
                                chunk[index]->done.notify_one();
                            }
                            continue;
                        }
                    }
                } catch (const std::bad_alloc&) {
                    publish_fail_closed();
                    for (std::size_t index = 0; index < chunk_size; ++index) {
                        chunk[index]->status =
                            Status{fail(ErrorCode::resource_exhausted, "paired mutation allocation failed")};
                        chunk[index]->done.store(true, std::memory_order_release);
                        chunk[index]->done.notify_one();
                    }
                    while (rev != nullptr) {
                        auto* const next = rev->next;
                        rev->status =
                            Status{fail(ErrorCode::resource_exhausted, "paired runtime is fail-closed")};
                        rev->done.store(true, std::memory_order_release);
                        rev->done.notify_one();
                        rev = next;
                    }
                    break;
                } catch (...) {
                    publish_fail_closed();
                    for (std::size_t index = 0; index < chunk_size; ++index) {
                        chunk[index]->status =
                            Status{fail(ErrorCode::resource_exhausted, "paired Writer failure")};
                        chunk[index]->done.store(true, std::memory_order_release);
                        chunk[index]->done.notify_one();
                    }
                    while (rev != nullptr) {
                        auto* const next = rev->next;
                        rev->status =
                            Status{fail(ErrorCode::resource_exhausted, "paired runtime is fail-closed")};
                        rev->done.store(true, std::memory_order_release);
                        rev->done.notify_one();
                        rev = next;
                    }
                    break;
                }
                std::array<VolatileSyncMutationView, kMaximumPublicationBatch> view_storage{};
                for (std::size_t index = 0; index < chunk_size; ++index) {
                    const auto* node = chunk[index];
                    view_storage[index] = VolatileSyncMutationView{
                        .kind = node->kind == MutationKind::put ? VolatileSyncMutationView::Kind::put
                                                                : VolatileSyncMutationView::Kind::erase,
                        .key = node->key,
                        .value = node->value,
                        .expire_at_ns = node->expire_at_ns,
                        .status = node->status,
                    };
                }
                apply_volatile_sync_publication_chunk(
                    store_, shard, publication_ctx, std::span{view_storage.data(), chunk_size},
                    slot_reservation, VolatileSyncChunkMode::combiner, healthy_, &publish_fail_closed,
                    &reclaim_proportional, nullptr);
                for (std::size_t index = 0; index < chunk_size; ++index) {
                    chunk[index]->status = view_storage[index].status;
                }
                for (std::size_t index = 0; index < chunk_size; ++index) {
                    chunk[index]->done.store(true, std::memory_order_release);
                    chunk[index]->done.notify_one();
                }
                if (!healthy_.load(std::memory_order_acquire)) {
                    while (rev != nullptr) {
                        auto* const next = rev->next;
                        rev->status =
                            Status{fail(ErrorCode::resource_exhausted, "paired runtime is fail-closed")};
                        rev->done.store(true, std::memory_order_release);
                        rev->done.notify_one();
                        rev = next;
                    }
                    break;
                }
            }
            continue;
        }

        // durable_sync under the combiner token (ACK after visibility).
        while (rev != nullptr) {
            if (!healthy_.load(std::memory_order_acquire)) {
                for (auto* node = rev; node != nullptr;) {
                    auto* const next = node->next;
                    node->status =
                        Status{fail(ErrorCode::resource_exhausted, "paired runtime is fail-closed")};
                    node->done.store(true, std::memory_order_release);
                    node->done.notify_one();
                    node = next;
                }
                break;
            }
            auto* node = rev;
            rev = rev->next;
            lane.generation.reclaim_requested.store(true, std::memory_order_release);
            combiner_housekeeping(shard, 1U);
            auto generation_admission = decide_generation_admission(
                lane.generation.retired_debt(), ShardPairRuntime::kMaximumRetiredReadGenerations,
                PairReadGeneration::can_publish_incremental(*lane.generation.writer_generation,
                                                            lane.merge.read_merge.get(), 1U));
            generation_admission = runtime_detail::wait_generation_admission(
                generation_admission, [&] { combiner_housekeeping(shard, 1U); },
                [&] {
                    return decide_generation_admission(
                        lane.generation.retired_debt(), ShardPairRuntime::kMaximumRetiredReadGenerations,
                        PairReadGeneration::can_publish_incremental(*lane.generation.writer_generation,
                                                                    lane.merge.read_merge.get(), 1U));
                });
            if (generation_admission != GenerationAdmissionDecision::admitted) {
                lane.generation.generation_admission_backpressure_total.fetch_add(1U,
                                                                                  std::memory_order_relaxed);
                if (generation_admission == GenerationAdmissionDecision::incremental_merge_required) {
                    lane.merge.read_merge_backpressure.fetch_add(1U, std::memory_order_relaxed);
                }
                node->status = Status{
                    fail(ErrorCode::resource_exhausted, generation_admission_message(generation_admission))};
                node->done.store(true, std::memory_order_release);
                node->done.notify_one();
                continue;
            }
            Status status{};
            bool durable_committed = false;
            bool durable_mutate_entered = false;
            bool generation_published = false;
            bool status_resolved = false;
            MutationLifecycle life{};
            static_cast<void>(life.admit());
            static_cast<void>(life.stage_for_writer());
            const auto shadow_mark_published = [&](const bool published) noexcept {
                if (published) {
                    static_cast<void>(life.mark_published());
                    return;
                }
                if (life.publication().state == PublicationState::required ||
                    life.publication().state == PublicationState::staged) {
                    static_cast<void>(life.mark_publication_failed());
                }
            };
            const auto shadow_resolve_status = [&]() noexcept {
                status_resolved = true;
                if (life.stage() != MutationStage::completion_decided &&
                    life.stage() != MutationStage::completed) {
                    auto decided = decide_completion(life.durable(), life.publication());
                    if (status) {
                        decided.kind = CompletionDecision::Kind::success;
                    } else if (status.error().code == ErrorCode::unavailable) {
                        decided.kind = CompletionDecision::Kind::indeterminate;
                    } else {
                        decided.kind = CompletionDecision::Kind::known_not_committed;
                    }
                    static_cast<void>(life.decide(decided));
                }
                static_cast<void>(life.mark_completed());
            };
            try {
                const auto& key = *node->key;
                durable_mutate_entered = true;
                static_cast<void>(life.mark_durable_started());
                auto result = execute_durable_single(store_, shard, node->kind, key, node->value,
                                                     node->expire_at_ns, node->type);
                if (result.committed()) {
                    durable_committed = true;
                }
                static_cast<void>(life.apply_durable_result(result));
                const auto ack_after_published_visibility = [&]() -> Status {
                    // Same DualPath loader as Reader adopt (ADR 0036 token + mirrored pointer).
                    const auto* published = load_published_generation(lane.generation);
                    if (published == nullptr) {
                        return Status{
                            fail(ErrorCode::unavailable, "paired read generation missing after drain")};
                    }
                    const auto view = published->prepare_durable(key);
                    if (node->kind == MutationKind::put) {
                        return view.has_value()
                                   ? Status{}
                                   : Status{fail(ErrorCode::unavailable,
                                                 "committed put missing from published generation")};
                    }
                    return (!view.has_value() && view.error().code == ErrorCode::not_found)
                               ? Status{}
                               : Status{fail(ErrorCode::unavailable,
                                             "committed erase still visible after drain")};
                };
                if (!result.committed() || result.error || !result.sequence) {
                    auto error = result.error         ? *result.error
                                 : result.committed() ? Error{ErrorCode::internal_error,
                                                              "committed durable mutation has no sequence"}
                                                      : Error{ErrorCode::io_error, "durable mutation failed"};
                    if (result.committed() || result.outcome == DurableMutationOutcome::indeterminate) {
                        error.code = ErrorCode::unavailable;
                        generation_published = drain_durable_snapshot();
                        shadow_mark_published(generation_published);
                        publish_fail_closed();
                        if (generation_published && result.committed()) {
                            status = ack_after_published_visibility();
                        } else {
                            status = Status{unexpected(std::move(error))};
                        }
                    } else {
                        rewrite_known_not_committed_wire_error(error);
                        status = Status{unexpected(std::move(error))};
                    }
                    shadow_resolve_status();
                } else {
                    ReadMutation publication{.key = key,
                                             .record = RecordRef{.sequence = result.sequence.value()},
                                             .opcode = node->kind == MutationKind::put ? Opcode::put
                                                                                       : Opcode::erase};
                    if (node->kind == MutationKind::put) {
                        auto captured = detail::StoreAccess::capture_durable_read(store_, shard, key);
                        if (!captured) {
                            generation_published = drain_durable_snapshot();
                            shadow_mark_published(generation_published);
                            publish_fail_closed();
                            status =
                                generation_published
                                    ? ack_after_published_visibility()
                                    : Status{fail(ErrorCode::unavailable, "durable read capture failed")};
                            shadow_resolve_status();
                        } else {
                            publication.record = captured->reference();
                            publication.durable.emplace(std::move(*captured));
                            std::optional<GenerationSlotPool::Reservation> slot_reservation;
                            if (lane.generation.uses_slot_pool()) {
                                slot_reservation = try_reserve_publication_slot(lane.generation);
                                if (!slot_reservation) {
                                    status = Status{
                                        fail(ErrorCode::resource_exhausted,
                                             generation_admission_message(
                                                 GenerationAdmissionDecision::reader_quiescence_required))};
                                    shadow_resolve_status();
                                }
                            }
                            if (!status_resolved) {
                                static_cast<void>(life.mark_publication_staged());
                                if (!publish_incremental_read_mutations(publication_ctx,
                                                                        std::span{&publication, 1},
                                                                        slot_reservation, nullptr)) {
                                    generation_published = drain_durable_snapshot();
                                    shadow_mark_published(generation_published);
                                    publish_fail_closed();
                                    status =
                                        generation_published
                                            ? ack_after_published_visibility()
                                            : Status{fail(ErrorCode::unavailable, "read publication failed")};
                                    shadow_resolve_status();
                                } else {
                                    store_generation_memory_stats(
                                        lane.generation, lane.generation.writer_generation->memory_stats());
                                    generation_published = true;
                                    shadow_mark_published(true);
                                    if (glifistore::fault::consume_fail(glifistore::fault::Site::publish)) {
                                        throw std::bad_alloc{};
                                    }
                                    reclaim_proportional();
                                    status = Status{};
                                    shadow_resolve_status();
                                }
                            }
                        }
                    } else {
                        std::optional<GenerationSlotPool::Reservation> slot_reservation;
                        if (lane.generation.uses_slot_pool()) {
                            slot_reservation = try_reserve_publication_slot(lane.generation);
                            if (!slot_reservation) {
                                status = Status{
                                    fail(ErrorCode::resource_exhausted,
                                         generation_admission_message(
                                             GenerationAdmissionDecision::reader_quiescence_required))};
                                shadow_resolve_status();
                            }
                        }
                        if (!status_resolved) {
                            static_cast<void>(life.mark_publication_staged());
                            if (!publish_incremental_read_mutations(
                                    publication_ctx, std::span{&publication, 1}, slot_reservation, nullptr)) {
                                generation_published = drain_durable_snapshot();
                                shadow_mark_published(generation_published);
                                publish_fail_closed();
                                status =
                                    generation_published
                                        ? ack_after_published_visibility()
                                        : Status{fail(ErrorCode::unavailable, "read publication failed")};
                                shadow_resolve_status();
                            } else {
                                store_generation_memory_stats(
                                    lane.generation, lane.generation.writer_generation->memory_stats());
                                generation_published = true;
                                shadow_mark_published(true);
                                if (glifistore::fault::consume_fail(glifistore::fault::Site::publish)) {
                                    throw std::bad_alloc{};
                                }
                                reclaim_proportional();
                                status = Status{};
                                shadow_resolve_status();
                            }
                        }
                    }
                }
            } catch (const std::bad_alloc&) {
                const SyncDurableExceptionContext recovery_ctx{
                    .durable_committed = durable_committed,
                    .durable_mutate_entered = durable_mutate_entered,
                    .generation_published = generation_published,
                    .status_resolved = status_resolved,
                };
                const auto recovery = plan_sync_durable_exception_recovery(recovery_ctx);
                if (recovery.mark_exception_lifecycle) {
                    static_cast<void>(life.mark_exception_after_durable_start());
                }
                if (recovery.drain_if_unpublished) {
                    generation_published = drain_durable_snapshot() || generation_published;
                    shadow_mark_published(generation_published);
                }
                if (recovery.fail_closed) {
                    publish_fail_closed();
                }
                const auto status_plan =
                    plan_sync_durable_exception_status({.durable_committed = durable_committed,
                                                        .durable_mutate_entered = durable_mutate_entered,
                                                        .generation_published = generation_published,
                                                        .status_resolved = status_resolved});
                switch (status_plan.kind) {
                case SyncDurableExceptionStatusKind::keep_resolved:
                    break;
                case SyncDurableExceptionStatusKind::success_after_visibility:
                    status = Status{};
                    shadow_resolve_status();
                    break;
                case SyncDurableExceptionStatusKind::unavailable_store_entered:
                    status = Status{fail(ErrorCode::unavailable, "paired mutation allocation failed")};
                    shadow_resolve_status();
                    break;
                case SyncDurableExceptionStatusKind::resource_exhausted_never_entered:
                    status = Status{fail(ErrorCode::resource_exhausted, "paired mutation allocation failed")};
                    shadow_resolve_status();
                    break;
                }
            } catch (...) {
                const SyncDurableExceptionContext recovery_ctx{
                    .durable_committed = durable_committed,
                    .durable_mutate_entered = durable_mutate_entered,
                    .generation_published = generation_published,
                    .status_resolved = status_resolved,
                };
                const auto recovery = plan_sync_durable_exception_recovery(recovery_ctx);
                if (recovery.mark_exception_lifecycle) {
                    static_cast<void>(life.mark_exception_after_durable_start());
                }
                if (recovery.drain_if_unpublished) {
                    generation_published = drain_durable_snapshot() || generation_published;
                    shadow_mark_published(generation_published);
                }
                if (recovery.fail_closed) {
                    publish_fail_closed();
                }
                const auto status_plan =
                    plan_sync_durable_exception_status({.durable_committed = durable_committed,
                                                        .durable_mutate_entered = durable_mutate_entered,
                                                        .generation_published = generation_published,
                                                        .status_resolved = status_resolved});
                switch (status_plan.kind) {
                case SyncDurableExceptionStatusKind::keep_resolved:
                    break;
                case SyncDurableExceptionStatusKind::success_after_visibility:
                    status = Status{};
                    shadow_resolve_status();
                    break;
                case SyncDurableExceptionStatusKind::unavailable_store_entered:
                    status = Status{fail(ErrorCode::unavailable, "paired Writer failure")};
                    shadow_resolve_status();
                    break;
                case SyncDurableExceptionStatusKind::resource_exhausted_never_entered:
                    status = Status{fail(ErrorCode::resource_exhausted, "paired Writer failure")};
                    shadow_resolve_status();
                    break;
                }
            }
            if (durable_mutate_entered && !detail::StoreAccess::operational(store_)) {
                if (!generation_published) {
                    generation_published = drain_durable_snapshot();
                    shadow_mark_published(generation_published);
                }
                publish_fail_closed();
                if (!generation_published) {
                    if (!status_resolved) {
                        status = Status{fail(ErrorCode::unavailable, "paired runtime is fail-closed")};
                        shadow_resolve_status();
                    } else if (status) {
                        status = Status{fail(ErrorCode::unavailable, "paired runtime is fail-closed")};
                    }
                }
            }
            node->status = status;
            node->done.store(true, std::memory_order_release);
            node->done.notify_one();
        }
    }
}

void ShardPairRuntime::combine_sync_lane(const std::size_t shard) noexcept {
    auto& lane = *lanes_[shard];
    // durable_group (!async): leave work for the dedicated Writer.
    if (dedicated_writer_required()) {
        return;
    }
    while (try_acquire_execution_token(lane.async.execution_token)) {
        process_sync_lane(shard);
        bool pending = false;
        {
            const std::lock_guard lock{lane.sync.sync_mutex};
            pending = lane.sync_head != nullptr;
        }
        release_execution_token(lane.async.execution_token);
        if (!try_reacquire_execution_token_if_pending(lane.async.execution_token, pending)) {
            return;
        }
        process_sync_lane(shard);
        {
            const std::lock_guard lock{lane.sync.sync_mutex};
            pending = lane.sync_head != nullptr;
        }
        release_execution_token(lane.async.execution_token);
        if (!pending) {
            return;
        }
    }
}
} // namespace glifistore::store::paired
