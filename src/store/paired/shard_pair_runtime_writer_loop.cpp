#include "glifistore/core/fault_injection.hpp"
#include "glifistore/core/hot_path_phases.hpp"
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

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace glifistore::store::paired {

void ShardPairRuntime::run(const std::size_t shard) noexcept try {
    auto& lane = *lanes_[shard];

    struct ExitGuard final {
        ShardPairRuntime& executor;

        ~ExitGuard() {
            executor.note_writer_exit();
        }
    } exit_guard{*this};

    const auto pop_queued = [&lane]() noexcept -> std::optional<runtime_detail::AsyncMutationTask> {
        const std::lock_guard lock{lane.async.queue_consumer_mutex};
        return lane.queue.try_pop();
    };

    const auto batch_config = detail::StoreAccess::durable_writer_batch_config(store_);
    const auto maximum_batch_records =
        std::min(batch_config ? static_cast<std::size_t>(batch_config->max_records)
                              : static_cast<std::size_t>(config_.async_writer_batch_max_records),
                 lane.queue.capacity());
    const auto maximum_batch_bytes = batch_config ? static_cast<std::size_t>(batch_config->max_bytes)
                                                  : config_.async_writer_batch_max_bytes;
    std::vector<runtime_detail::AsyncMutationTask> batch;
    std::vector<std::uint64_t> queue_waits;
    std::vector<std::chrono::steady_clock::time_point> service_started;
    std::vector<bool> expired;
    std::vector<MutationOutcome> completions;
    std::vector<detail::StoreAccess::DurableMutationView> durable_views;
    std::vector<std::size_t> durable_indices;
    std::vector<ReadMutation> read_mutations;
    std::vector<std::size_t> read_mutation_indices;
    std::optional<runtime_detail::AsyncMutationTask> carried_task;
    SyncMutation* carried_sync = nullptr;
    batch.reserve(maximum_batch_records);
    queue_waits.reserve(maximum_batch_records);
    service_started.reserve(maximum_batch_records);
    expired.reserve(maximum_batch_records);
    completions.reserve(maximum_batch_records);
    durable_views.reserve(maximum_batch_records);
    durable_indices.reserve(maximum_batch_records);
    read_mutations.reserve(maximum_batch_records);
    read_mutation_indices.reserve(maximum_batch_records);
    bool merge_retry_blocked{};

    FailClosedState fail_closed{store_, healthy_, expire_remaining_};
    LanePublicationContext publication_ctx{lane.generation, lane.merge, store_, shard,
                                           ShardPairRuntime::kMaximumRetiredReadGenerations};
    // Constructed once per Writer thread; allocation failure must not escape noexcept run().
    std::function<void()> publish_fail_closed;
    std::function<void()> sticky_pair_before_durable_mark;
    std::function<void()> update_delta_stats;
    std::function<void()> reclaim_quiescent;
    std::function<void()> after_durable_drain;
    std::function<bool()> drain_durable_snapshot;
    std::function<void()> reclaim_proportional;
    std::function<void(std::size_t)> process_merge;
    std::function<void(std::size_t)> prepare_publish_retry;
    const auto start_merge_with_optional_pin = [&]() -> Result<std::unique_ptr<PairReadMerge>> {
        return start_incremental_merge(lane.generation, config_.merge_maximum_post_entries);
    };
    try {
        publish_fail_closed = [&]() noexcept {
            fail_closed.arm(fail_closed_wakes_, FailClosedScope::pair_and_store);
        };
        sticky_pair_before_durable_mark = [&]() noexcept {
            fail_closed.arm(fail_closed_wakes_, FailClosedScope::pair_only);
        };
        update_delta_stats = [&]() noexcept {
            store_generation_memory_stats(lane.generation, lane.generation.writer_generation->memory_stats());
        };
        reclaim_quiescent = [&]() noexcept {
            try {
                reclaim_quiescent_generations(lane.generation, lane.reclaim, config_.reader_epoch_lease);
            } catch (...) {
                publish_fail_closed();
            }
        };
        // Drain durable Index authority into the published generation before sticky
        // close (allow_fail_closed: durable may already be unhealthy). Sync single-op
        // and batch catch use this so committed keys are not left unpublished.
        after_durable_drain = [&]() noexcept {
            merge_retry_blocked = false;
            reclaim_quiescent();
        };
        drain_durable_snapshot = [&]() noexcept -> bool {
            return try_drain_durable_snapshot(publication_ctx, true, &after_durable_drain);
        };
        // Publication produces one retired generation per successful incremental
        // publish. Reclaiming on every single-mutation publish dominated volatile
        // PUT ack time; reclaim proportionally when debt accumulates, and always
        // before hard retire limits / merge pressure checks.
        reclaim_proportional = [&]() noexcept {
            if (lane.merge.read_merge) {
                store_merge_progress(lane.merge);
            }
            constexpr std::size_t kReclaimPublishQuantum = 8;
            if (lane.generation.retired_debt() >= kReclaimPublishQuantum ||
                lane.generation.retired_debt() + 1U >= ShardPairRuntime::kMaximumRetiredReadGenerations) {
                reclaim_quiescent();
            } else {
                lane.generation.retired_generation_count.store(lane.generation.retired_generations.size(),
                                                               std::memory_order_relaxed);
            }
        };
        process_merge = [&](const std::size_t publication_records) noexcept {
            try {
                if (!lane.merge.read_merge && !merge_retry_blocked &&
                    (lane.generation.writer_generation->delta_entries() >= config_.merge_delta_entries ||
                     lane.generation.writer_generation->delta_record_versions() >=
                         config_.merge_delta_entries)) {
                    auto started = start_merge_with_optional_pin();
                    if (!started) {
                        lane.merge.read_merge_failures.fetch_add(1U, std::memory_order_relaxed);
                        if (started.error().code == ErrorCode::resource_exhausted) {
                            merge_retry_blocked = true;
                        } else {
                            publish_fail_closed();
                        }
                        return;
                    }
                    lane.merge.read_merge = std::move(*started);
                    lane.merge.read_merge_active.store(true, std::memory_order_relaxed);
                    lane.merge.read_merge_post_entries.store(0U, std::memory_order_relaxed);
                    store_merge_progress(lane.merge);
                    lane.merge.read_merge_starts.fetch_add(1U, std::memory_order_relaxed);
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
                        if (advanced.error().code == ErrorCode::resource_exhausted) {
                            lane.merge.read_merge.reset();
                            lane.merge.read_merge_active.store(false, std::memory_order_relaxed);
                            lane.merge.read_merge_post_entries.store(0U, std::memory_order_relaxed);
                            store_merge_progress(lane.merge);
                            merge_retry_blocked = true;
                        } else {
                            publish_fail_closed();
                        }
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
                const auto outcome = finish_incremental_merge_and_publish(publication_ctx);
                if (outcome.status == DualPathPublishStatus::deferred) {
                    return;
                }
                if (outcome.status == DualPathPublishStatus::build_failed) {
                    if (outcome.build_error == ErrorCode::resource_exhausted) {
                        merge_retry_blocked = true;
                    } else {
                        publish_fail_closed();
                    }
                    return;
                }
                if (outcome.status != DualPathPublishStatus::published) {
                    publish_fail_closed();
                    return;
                }
                lane.merge.read_merge.reset();
                lane.merge.read_merge_active.store(false, std::memory_order_relaxed);
                lane.merge.read_merge_post_entries.store(0U, std::memory_order_relaxed);
                store_merge_progress(lane.merge);
                lane.merge.read_merge_completions.fetch_add(1U, std::memory_order_relaxed);
                merge_retry_blocked = false;
                reclaim_quiescent();
            } catch (const std::bad_alloc&) {
                publish_fail_closed();
            } catch (...) {
                publish_fail_closed();
            }
        };
        // Stable once: avoid per-chunk std::function allocation under process_fail_at injection.
        prepare_publish_retry = [&](const std::size_t published_count) {
            reclaim_quiescent();
            process_merge(published_count);
        };
    } catch (...) {
        fail_closed.arm(fail_closed_wakes_, FailClosedScope::pair_and_store);
        return;
    }

    const auto process_reclamation = [&]() noexcept {
        if (lane.generation.reclaim_requested.exchange(false, std::memory_order_acq_rel)) {
            reclaim_quiescent();
        }
    };
    const auto process_refresh = [&]() noexcept {
        try {
            if (!lane.generation.refresh_requested.exchange(false, std::memory_order_acq_rel)) {
                return;
            }
            reclaim_quiescent();
            if (decide_generation_admission(lane.generation.retired_debt(),
                                            ShardPairRuntime::kMaximumRetiredReadGenerations,
                                            true) != GenerationAdmissionDecision::admitted) {
                lane.metrics.read_refresh_deferrals.fetch_add(1U, std::memory_order_relaxed);
                return;
            }
            lane.metrics.read_refresh_attempts.fetch_add(1U, std::memory_order_relaxed);
            auto snapshot = detail::StoreAccess::snapshot_durable_reads(store_, shard);
            if (!snapshot) {
                lane.metrics.read_refresh_failures.fetch_add(1U, std::memory_order_relaxed);
                if (snapshot.error().code != ErrorCode::resource_exhausted &&
                    !(lane.async.stopping.load(std::memory_order_acquire) &&
                      snapshot.error().code == ErrorCode::unavailable)) {
                    publish_fail_closed();
                }
                return;
            }
            if (snapshot->catalog_revision ==
                lane.generation.published_catalog_revision.load(std::memory_order_acquire)) {
                return;
            }
            const auto outcome = replace_durable_snapshot_and_publish(publication_ctx, snapshot->records);
            if (outcome.status == DualPathPublishStatus::deferred) {
                lane.metrics.read_refresh_deferrals.fetch_add(1U, std::memory_order_relaxed);
                return;
            }
            if (outcome.status == DualPathPublishStatus::build_failed) {
                lane.metrics.read_refresh_failures.fetch_add(1U, std::memory_order_relaxed);
                if (outcome.build_error != ErrorCode::resource_exhausted) {
                    publish_fail_closed();
                }
                return;
            }
            if (outcome.status != DualPathPublishStatus::published) {
                publish_fail_closed();
                return;
            }
            note_catalog_snapshot_installed(publication_ctx, snapshot->catalog_revision);
            merge_retry_blocked = false;
            lane.metrics.read_refresh_successes.fetch_add(1U, std::memory_order_relaxed);
        } catch (const std::bad_alloc&) {
            publish_fail_closed();
        } catch (...) {
            publish_fail_closed();
        }
    };
    for (;;) {
        // ADR 0037: refresh/merge/sync share the execution token with the combiner.
        if (!try_acquire_execution_token(lane.async.execution_token)) {
            const auto observed = lane.async.signal.load(std::memory_order_acquire);
            lane.async.signal.wait(observed, std::memory_order_acquire);
            continue;
        }
        process_reclamation();
        if (!lane.async.stopping.load(std::memory_order_acquire) &&
            healthy_.load(std::memory_order_acquire)) {
            process_refresh();
        }

        // Drain one bounded synchronous turn before asynchronous lane work. Neither
        // repeated sync admissions nor one large caller batch may keep the dedicated
        // Writer here forever: after at most 32 records, an already-admitted async
        // batch gets a turn under the same execution token.
        bool drained_sync_turn = false;
        {
            WriterSyncDrainEnv sync_env{
                .lane = lane,
                .shard = shard,
                .batch_config = batch_config,
                .carried_sync = carried_sync,
                .merge_retry_blocked = merge_retry_blocked,
                .publication_ctx = publication_ctx,
                .hooks = {},
            };
            sync_env.hooks.publish_fail_closed = &publish_fail_closed;
            sync_env.hooks.sticky_pair_before_durable_mark = &sticky_pair_before_durable_mark;
            sync_env.hooks.reclaim_quiescent = &reclaim_quiescent;
            sync_env.hooks.reclaim_proportional = &reclaim_proportional;
            sync_env.hooks.drain_durable_snapshot = &drain_durable_snapshot;
            sync_env.hooks.process_merge = &process_merge;
            sync_env.hooks.update_delta_stats = &update_delta_stats;
            sync_env.hooks.prepare_publish_retry = &prepare_publish_retry;
            drained_sync_turn = run_writer_sync_drain(sync_env);
        }
        if (drained_sync_turn && (carried_task.has_value() || lane.queue.size() != 0U)) {
            lane.metrics.sync_async_fairness_turns.fetch_add(1U, std::memory_order_relaxed);
        }

        auto task = carried_task ? std::exchange(carried_task, std::nullopt) : pop_queued();
        if (!task) {
            if (lane.async.stopping.load(std::memory_order_acquire) && carried_sync == nullptr) {
                release_execution_token(lane.async.execution_token);
                return;
            }
            const auto observed = lane.async.signal.load(std::memory_order_acquire);
            task = pop_queued();
            bool sync_pending = carried_sync != nullptr;
            if (!sync_pending) {
                const std::lock_guard lock{lane.sync.sync_mutex};
                sync_pending = lane.sync_head != nullptr;
            }
            if (!task && !sync_pending && !lane.async.stopping.load(std::memory_order_acquire) &&
                healthy_.load(std::memory_order_acquire)) {
                // Foreground completions are already delivered. Run one
                // bounded maintenance turn before deciding whether this
                // Writer can sleep; a just-reached delta threshold therefore
                // cannot strand a merge until the next client mutation.
                process_merge(0U);
            }
            if (!task && !sync_pending && !lane.async.stopping.load(std::memory_order_acquire) &&
                !lane.generation.refresh_requested.load(std::memory_order_acquire) &&
                !lane.generation.reclaim_requested.load(std::memory_order_acquire) &&
                !lane.merge.read_merge) {
                // Wave 2 wakeup ladder: pause-spin → OS yield → park on signal.
                // Keeps the hot handoff short while avoiding immediate
                // atomic::wait under brief Writer contention.
                bool woke = false;
                for (unsigned spin = 0; spin < 16U; ++spin) {
                    if (lane.async.signal.load(std::memory_order_acquire) != observed) {
                        woke = true;
                        break;
                    }
#if defined(__aarch64__) || defined(_M_ARM64)
                    __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(_M_X64)
                    __builtin_ia32_pause();
#else
                    std::this_thread::yield();
#endif
                }
                for (unsigned yield_spin = 0; !woke && yield_spin < 8U; ++yield_spin) {
                    if (lane.async.signal.load(std::memory_order_acquire) != observed) {
                        woke = true;
                        break;
                    }
                    std::this_thread::yield();
                }
                if (!woke) {
                    release_execution_token(lane.async.execution_token);
                    lane.async.signal.wait(observed, std::memory_order_acquire);
                    continue;
                }
            }
            if (!task) {
                release_execution_token(lane.async.execution_token);
                continue;
            }
        }

        batch.clear();
        batch.push_back(*task);
        WriterAsyncBatchEnv async_env{
            .lane = lane,
            .shard = shard,
            .batch_config = batch_config,
            .maximum_batch_records = maximum_batch_records,
            .maximum_batch_bytes = maximum_batch_bytes,
            .batch = batch,
            .carried_task = carried_task,
            .merge_retry_blocked = merge_retry_blocked,
            .fail_closed = fail_closed,
            .publication_ctx = publication_ctx,
            .queue_waits = queue_waits,
            .service_started = service_started,
            .expired = expired,
            .completions = completions,
            .durable_views = durable_views,
            .durable_indices = durable_indices,
            .read_mutations = read_mutations,
            .read_mutation_indices = read_mutation_indices,
            .hooks = {},
        };
        async_env.hooks.publish_fail_closed = &publish_fail_closed;
        async_env.hooks.reclaim_quiescent = &reclaim_quiescent;
        async_env.hooks.process_merge = &process_merge;
        async_env.hooks.drain_durable_snapshot = &drain_durable_snapshot;
        async_env.hooks.update_delta_stats = &update_delta_stats;
        run_writer_async_batch(async_env);
    }
} catch (...) {

    FailClosedState{store_, healthy_, expire_remaining_}.arm(fail_closed_wakes_,
                                                             FailClosedScope::pair_and_store);
}

} // namespace glifistore::store::paired
