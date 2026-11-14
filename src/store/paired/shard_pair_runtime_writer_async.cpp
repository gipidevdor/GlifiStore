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
#include <chrono>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace glifistore::store::paired {

void ShardPairRuntime::run_writer_async_batch(WriterAsyncBatchEnv& env) noexcept {
    auto& lane = env.lane;
    const std::size_t shard = env.shard;
    const auto publish_fail_closed = [&]() noexcept {
        if (env.hooks.publish_fail_closed != nullptr) {
            (*env.hooks.publish_fail_closed)();
        }
    };
    const auto reclaim_quiescent = [&]() noexcept {
        if (env.hooks.reclaim_quiescent != nullptr) {
            (*env.hooks.reclaim_quiescent)();
        }
    };
    const auto process_merge = [&](const std::size_t records) noexcept {
        if (env.hooks.process_merge != nullptr) {
            (*env.hooks.process_merge)(records);
        }
    };
    const auto drain_durable_snapshot = [&]() noexcept -> bool {
        return env.hooks.drain_durable_snapshot != nullptr && (*env.hooks.drain_durable_snapshot)();
    };
    const auto payload_for = [&lane](const runtime_detail::AsyncMutationTask& task) noexcept {
        auto payload = lane.payloads.view(task.payload_slot);
        if (!payload) {
            std::terminate();
        }
        return *payload;
    };
    const auto pop_queued = [&lane]() noexcept -> std::optional<runtime_detail::AsyncMutationTask> {
        const std::lock_guard lock{lane.async.queue_consumer_mutex};
        return lane.queue.try_pop();
    };
    auto batch_bytes = env.batch.front().admission_bytes;
    const auto coalescing_started = std::chrono::steady_clock::now();
    const auto minimum_batch_deadline =
        env.batch_config ? coalescing_started + std::chrono::milliseconds{env.batch_config->max_wait_ms}
                         : std::chrono::steady_clock::time_point{};
    const auto burst_deadline =
        env.batch_config
            ? coalescing_started + std::min(std::chrono::duration_cast<std::chrono::microseconds>(
                                                std::chrono::milliseconds{env.batch_config->max_wait_ms}),
                                            std::chrono::microseconds{250})
            : std::chrono::steady_clock::time_point{};
    const auto queue_deadline = maximum_queue_wait_.count() == 0
                                    ? std::chrono::steady_clock::time_point::max()
                                    : env.batch.front().admitted_at + maximum_queue_wait_;
    bool durability_deadline_closed = false;
    bool queue_deadline_closed = false;
    std::size_t empty_polls{};
    GS_PHASE_PUT_NAMED(batch_collect_phase, batch_collect);
    while (env.batch.size() < env.maximum_batch_records) {
        auto next = pop_queued();
        if (!next) {
            // Drain deadline may arm expire while we wait for min_records /
            // burst coalescing — do not hold pre-Store work past abandon.
            if (expire_remaining_.load(std::memory_order_acquire)) {
                break;
            }
            const auto now = std::chrono::steady_clock::now();
            const auto durable_deadline = env.batch_config && env.batch.size() < env.batch_config->min_records
                                              ? minimum_batch_deadline
                                              : burst_deadline;
            const auto wait_deadline = std::min(durable_deadline, queue_deadline);
            if (env.batch_config && now < wait_deadline) {
                if (empty_polls++ < 64U) {
                    std::this_thread::yield();
                } else {
                    const auto remaining = wait_deadline - now;
                    std::this_thread::sleep_for(std::min(
                        remaining, std::chrono::steady_clock::duration{std::chrono::microseconds{50}}));
                }
                continue;
            }
            if (env.batch_config && env.batch_config->max_wait_ms != 0U) {
                if (queue_deadline <= durable_deadline && now >= queue_deadline) {
                    queue_deadline_closed = true;
                } else if (now >= durable_deadline) {
                    durability_deadline_closed = true;
                }
            }
            break;
        }
        empty_polls = 0;
        if (batch_bytes >= env.maximum_batch_bytes ||
            next->admission_bytes > env.maximum_batch_bytes - batch_bytes) {
            env.carried_task = *next;
            break;
        }
        batch_bytes += next->admission_bytes;
        env.batch.push_back(*next);
    }
    GS_PHASE_FINISH(batch_collect_phase);
    const auto writer_batch_wait_ns =
        runtime_detail::elapsed_ns(coalescing_started, std::chrono::steady_clock::now());
    env.lane.metrics.writer_batches.fetch_add(1U, std::memory_order_relaxed);
    env.lane.metrics.writer_batch_records.fetch_add(env.batch.size(), std::memory_order_relaxed);
    runtime_detail::atomic_max(env.lane.metrics.maximum_writer_batch_records, env.batch.size());
    runtime_detail::atomic_saturating_add(env.lane.metrics.total_writer_batch_wait_ns, writer_batch_wait_ns);
    runtime_detail::atomic_max(env.lane.metrics.maximum_writer_batch_wait_ns, writer_batch_wait_ns);
    if (durability_deadline_closed) {
        env.lane.metrics.writer_batch_durability_deadline_closes.fetch_add(1U, std::memory_order_relaxed);
    }
    if (queue_deadline_closed) {
        env.lane.metrics.writer_batch_queue_deadline_closes.fetch_add(1U, std::memory_order_relaxed);
    }

    env.queue_waits.clear();
    env.service_started.clear();
    env.expired.clear();
    env.completions.clear();
    env.durable_views.clear();
    env.durable_indices.clear();
    env.read_mutations.clear();
    env.read_mutation_indices.clear();
    bool post_commit_publication_failure{};
    bool durable_commit_observed = false;
    // Clean durable commits (no mutation error) eligible for ACK-after-drain when
    // capture/incremental publication fails but Index authority is snapshotted.
    std::vector<std::size_t> clean_durable_commit_indices;
    clean_durable_commit_indices.reserve(env.maximum_batch_records);
    // Sticky committed+error / Index-visible indeterminate eligible for ACK-after-visibility.
    std::vector<std::size_t> sticky_durable_commit_indices;
    sticky_durable_commit_indices.reserve(env.maximum_batch_records);
    const auto stage_durable_publication = [&](const std::size_t batch_index,
                                               const DurableMutationResult& result) {
        auto& queued = env.batch[batch_index];
        auto& completion = env.completions[batch_index];
        if (!result.sequence) {
            completion.error.emplace(ErrorCode::unavailable,
                                     "committed durable mutation has no publication sequence");
            post_commit_publication_failure = true;
            return;
        }
        const auto payload = payload_for(queued);
        const HashedKey key{.key = payload.key, .hash = queued.key_hash};
        ReadMutation publication{
            .key = key,
            .record = RecordRef{.sequence = *result.sequence},
            .opcode = queued.kind == MutationKind::put ? Opcode::put : Opcode::erase,
        };
        if (queued.kind == MutationKind::put) {
            auto captured = detail::StoreAccess::capture_durable_read(store_, shard, key);
            if (!captured) {
                // Keep sticky; ACK upgraded after successful drain-snapshot (mirror sync).
                completion.error.emplace(std::move(captured.error()));
                completion.error->code = ErrorCode::unavailable;
                post_commit_publication_failure = true;
                return;
            }
            publication.record = captured->reference();
            publication.durable.emplace(std::move(*captured));
        }
        env.read_mutations.push_back(std::move(publication));
        env.read_mutation_indices.push_back(batch_index);
    };
    const auto ack_clean_durable_after_drain = [&]() noexcept {
        for (const auto index : clean_durable_commit_indices) {
            env.completions[index].error.reset();
        }
        // Same DualPath loader as Reader adopt (ADR 0036 token + mirrored pointer).
        const auto* published = load_published_generation(env.lane.generation);
        if (published == nullptr) {
            return;
        }
        for (const auto index : sticky_durable_commit_indices) {
            auto& completion = env.completions[index];
            if (!completion.error) {
                continue;
            }
            const auto& queued = env.batch[index];
            const HashedKey key{.key = payload_for(queued).key, .hash = queued.key_hash};
            const auto view = published->prepare_durable(key);
            if (queued.kind == MutationKind::put) {
                if (view.has_value()) {
                    completion.error.reset();
                }
            } else if (!view.has_value() && view.error().code == ErrorCode::not_found) {
                completion.error.reset();
            }
        }
    };
    const auto ack_staged_after_publish = [&]() noexcept {
        for (const auto index : env.read_mutation_indices) {
            env.completions[index].error.reset();
        }
    };
    const auto finish_published_generation = [&]() {
        // Authority is already visible; preserve staged success ACKs across reclaim /
        // injected post-publish faults (mirror sync volatile/durable catch).
        ack_staged_after_publish();
        if (glifistore::fault::consume_fail(glifistore::fault::Site::publish)) {
            throw std::bad_alloc{};
        }
        if (env.lane.merge.read_merge) {
            env.lane.merge.read_merge_post_entries.store(
                PairReadGeneration::merge_post_entries(*env.lane.merge.read_merge),
                std::memory_order_relaxed);
        }
        store_merge_progress(env.lane.merge);
        env.merge_retry_blocked = false;
        reclaim_quiescent();
    };
    reclaim_quiescent();
    process_merge(env.batch.size());
    auto generation_admission = decide_generation_admission(
        env.lane.generation.retired_debt(), ShardPairRuntime::kMaximumRetiredReadGenerations,
        PairReadGeneration::can_publish_incremental(*env.lane.generation.writer_generation,
                                                    env.lane.merge.read_merge.get(), env.batch.size()));
    std::optional<GenerationSlotPool::Reservation> async_slot_reservation;
    if (generation_admission == GenerationAdmissionDecision::admitted &&
        env.lane.generation.uses_slot_pool()) {
        async_slot_reservation = try_reserve_publication_slot(env.lane.generation);
        if (!async_slot_reservation) {
            generation_admission = GenerationAdmissionDecision::reader_quiescence_required;
        }
    }
    const bool generation_pressure = generation_admission != GenerationAdmissionDecision::admitted;
    const bool force_expire = expire_remaining_.load(std::memory_order_acquire);
    GS_PHASE_PUT_NAMED(store_apply_phase, store_apply);
    for (std::size_t index = 0; index < env.batch.size(); ++index) {
        auto& queued = env.batch[index];
        env.lane.async.queued_bytes.fetch_sub(queued.admission_bytes, std::memory_order_relaxed);
        const auto queue_wait_ns =
            runtime_detail::elapsed_ns(queued.admitted_at, std::chrono::steady_clock::now());
        env.queue_waits.push_back(queue_wait_ns);
        runtime_detail::atomic_saturating_add(env.lane.metrics.total_queue_wait_ns, queue_wait_ns);
        runtime_detail::atomic_max(env.lane.metrics.maximum_queue_wait_ns, queue_wait_ns);
        env.lane.metrics.queue_wait_histogram.observe(queue_wait_ns);
        const bool task_expired =
            force_expire || generation_pressure ||
            (maximum_queue_wait_.count() != 0 &&
             std::cmp_greater_equal(
                 queue_wait_ns,
                 std::chrono::duration_cast<std::chrono::nanoseconds>(maximum_queue_wait_).count()));
        env.expired.push_back(task_expired);
        env.service_started.push_back(std::chrono::steady_clock::now());
        env.completions.push_back({.context = queued.context,
                                   .request_id = queued.request_id,
                                   .admission_bytes = queued.admission_bytes,
                                   .payload_slot = queued.payload_slot});
        if (task_expired) {
            if (generation_pressure && !force_expire) {
                env.lane.generation.generation_admission_backpressure_total.fetch_add(
                    1U, std::memory_order_relaxed);
            }
            if (generation_admission == GenerationAdmissionDecision::incremental_merge_required &&
                !force_expire) {
                env.lane.merge.read_merge_backpressure.fetch_add(1U, std::memory_order_relaxed);
            }
            env.completions.back().error.emplace(
                ErrorCode::resource_exhausted,
                force_expire ? "mutation abandoned after shutdown drain deadline"
                             : (generation_pressure ? generation_admission_message(generation_admission)
                                                    : "mutation env.expired before Store execution"));
        } else if (env.batch_config) {
            const auto payload = payload_for(queued);
            env.durable_views.push_back({.operation = queued.kind == MutationKind::put
                                                          ? detail::StoreAccess::MutationOperation::put
                                                          : detail::StoreAccess::MutationOperation::erase,
                                         .key = {.key = payload.key, .hash = queued.key_hash},
                                         .value = payload.value,
                                         .expire_at_ns = queued.expire_at_ns});
            env.durable_indices.push_back(index);
        }
    }

    if (env.batch_config && !env.durable_views.empty()) {
        if (expire_remaining_.load(std::memory_order_acquire)) {
            for (const auto batch_index : env.durable_indices) {
                if (!env.completions[batch_index].error) {
                    env.expired[batch_index] = true;
                    env.completions[batch_index].error.emplace(
                        ErrorCode::resource_exhausted, "mutation abandoned after shutdown drain deadline");
                }
            }
            env.durable_views.clear();
            env.durable_indices.clear();
        }
    }
    if (env.batch_config && !env.durable_views.empty()) {
        bool durable_mutate_entered = false;
        std::size_t first_unprocessed = 0;
        std::size_t inflight_begin = 0;
        std::size_t inflight_end = 0;
        bool mutate_inflight = false;
        const auto upgrade_unprocessed = [&](const std::size_t view_index,
                                             const ErrorCode before_mutate) -> ErrorCode {
            if (mutate_inflight && view_index >= inflight_begin && view_index < inflight_end) {
                return ErrorCode::unavailable;
            }
            if (mutate_inflight && view_index >= inflight_end) {
                return ErrorCode::resource_exhausted;
            }
            if (view_index >= first_unprocessed) {
                return durable_mutate_entered || durable_commit_observed || post_commit_publication_failure
                           ? ErrorCode::resource_exhausted
                           : before_mutate;
            }
            return ErrorCode::unavailable;
        };
        try {
            std::size_t begin = 0;
            while (begin < env.durable_views.size()) {
                if (expire_remaining_.load(std::memory_order_acquire)) {
                    for (std::size_t index = begin; index < env.durable_views.size(); ++index) {
                        const auto batch_index = env.durable_indices[index];
                        env.expired[batch_index] = true;
                        if (!env.completions[batch_index].error) {
                            env.completions[batch_index].error.emplace(
                                ErrorCode::resource_exhausted,
                                "mutation abandoned after shutdown drain deadline");
                        }
                    }
                    break;
                }
                if (post_commit_publication_failure || !healthy_.load(std::memory_order_acquire) ||
                    !detail::StoreAccess::operational(store_)) {
                    post_commit_publication_failure = true;
                    for (std::size_t index = begin; index < env.durable_views.size(); ++index) {
                        auto& completion = env.completions[env.durable_indices[index]];
                        if (!completion.error) {
                            // Remaining siblings never entered mutate_durable_batch.
                            completion.error.emplace(ErrorCode::resource_exhausted,
                                                     "paired runtime is fail-closed");
                        }
                    }
                    break;
                }
                const std::size_t end = durable_subbatch_end(
                    begin, env.durable_views.size(), [&](const std::size_t index) -> const HashedKey& {
                        return env.durable_views[index].key;
                    });
                GS_FAULT_SITE(mutate);
                inflight_begin = begin;
                inflight_end = end;
                mutate_inflight = true;
                durable_mutate_entered = true;
                auto results = detail::StoreAccess::mutate_durable_batch(
                    store_, shard, std::span{env.durable_views}.subspan(begin, end - begin));
                // Keep mutate_inflight through classification so post-return throws
                // cannot stamp Store-entered siblings as never-started OVERLOADED.
                if (glifistore::fault::consume_fail(glifistore::fault::Site::post_mutate)) {
                    throw std::bad_alloc{};
                }
                if (results.size() != end - begin) {
                    std::terminate();
                }
                std::vector<std::size_t> pending_stage_offsets;
                pending_stage_offsets.reserve(results.size());
                for (std::size_t offset = 0; offset < results.size(); ++offset) {
                    auto& result = results[offset];
                    const auto batch_index = env.durable_indices[begin + offset];
                    auto& completion = env.completions[batch_index];
                    if (result.conflict_retried) {
                        env.lane.metrics.conflict_retries.fetch_add(1U, std::memory_order_relaxed);
                        if (result.mutation.committed() && !result.mutation.error) {
                            env.lane.metrics.conflict_retry_commits.fetch_add(1U, std::memory_order_relaxed);
                        }
                    }
                    if (result.mutation.committed()) {
                        durable_commit_observed = true;
                    }
                    if (!result.mutation.committed() || result.mutation.error) {
                        auto error =
                            result.mutation.error
                                ? std::move(*result.mutation.error)
                                : Error{ErrorCode::io_error, "durable mutation failed without an error"};
                        if (result.mutation.committed() ||
                            result.mutation.outcome == DurableMutationOutcome::indeterminate) {
                            // Committed-without-publication or indeterminate (write boundary may
                            // have been crossed) must sticky-fail before later sub-batches.
                            error.code = ErrorCode::unavailable;
                            post_commit_publication_failure = true;
                            if (result.mutation.committed()) {
                                sticky_durable_commit_indices.push_back(batch_index);
                            }
                        } else {
                            rewrite_known_not_committed_wire_error(error);
                        }
                        completion.error.emplace(std::move(error));
                    } else {
                        clean_durable_commit_indices.push_back(batch_index);
                        pending_stage_offsets.push_back(offset);
                    }
                }
                first_unprocessed = end;
                mutate_inflight = false;
                for (const auto offset : pending_stage_offsets) {
                    stage_durable_publication(env.durable_indices[begin + offset], results[offset].mutation);
                }
                begin = end;
            }
        } catch (const std::bad_alloc&) {
            // Exception after entering durable mutate must not ACK via a later
            // publish_incremental, and must not leave the pair healthy with possible
            // unpublished Store state (partial commit is indistinguishable here).
            if (durable_mutate_entered || durable_commit_observed || !env.read_mutations.empty() ||
                post_commit_publication_failure) {
                post_commit_publication_failure = true;
            }
            for (std::size_t view_index = 0; view_index < env.durable_indices.size(); ++view_index) {
                const auto index = env.durable_indices[view_index];
                // Preserve staged sibling successes; drain-snapshot publishes them.
                // Overwriting clean ACKs here yields visible generation + error ACK.
                if (!env.completions[index].error &&
                    std::ranges::contains(env.read_mutation_indices, index)) {
                    continue;
                }
                if (!env.completions[index].error) {
                    env.completions[index].error.emplace(
                        upgrade_unprocessed(view_index, ErrorCode::resource_exhausted),
                        "paired mutation allocation failed");
                }
            }
        } catch (...) {
            if (durable_mutate_entered || durable_commit_observed || !env.read_mutations.empty() ||
                post_commit_publication_failure) {
                post_commit_publication_failure = true;
            }
            for (std::size_t view_index = 0; view_index < env.durable_indices.size(); ++view_index) {
                const auto index = env.durable_indices[view_index];
                if (!env.completions[index].error &&
                    std::ranges::contains(env.read_mutation_indices, index)) {
                    continue;
                }
                if (!env.completions[index].error) {
                    env.completions[index].error.emplace(
                        upgrade_unprocessed(view_index, ErrorCode::resource_exhausted),
                        "paired Writer failure");
                }
            }
        }
        if (durable_mutate_entered && !detail::StoreAccess::operational(store_)) {
            post_commit_publication_failure = true;
        }
    } else {
        for (std::size_t index = 0; index < env.batch.size(); ++index) {
            if (env.expired[index]) {
                continue;
            }
            auto& queued = env.batch[index];
            auto& completion = env.completions[index];
            // Same class as sync multi-chunk: after sticky post-commit failure, do not
            // Store-mutate later items in this coalesced async env.batch.
            if (post_commit_publication_failure || !healthy_.load(std::memory_order_acquire) ||
                (detail::StoreAccess::is_durable(store_) && !detail::StoreAccess::operational(store_))) {
                post_commit_publication_failure = true;
                if (!completion.error) {
                    // Never Store-entered after sticky — known not newly committed.
                    completion.error.emplace(ErrorCode::resource_exhausted, "paired runtime is fail-closed");
                }
                continue;
            }
            // Drain deadline may arm expire while this env.batch already left the queue;
            // re-check before Store so known-not-committed work never enters mutate.
            if (expire_remaining_.load(std::memory_order_acquire)) {
                env.expired[index] = true;
                completion.error.emplace(ErrorCode::resource_exhausted,
                                         "mutation abandoned after shutdown drain deadline");
                continue;
            }
            bool current_committed = false;
            bool durable_mutate_entered = false;
            try {
                const auto payload = payload_for(queued);
                const HashedKey key{.key = payload.key, .hash = queued.key_hash};
                if (detail::StoreAccess::is_durable(store_)) {
                    DurableMutationResult result;
                    bool conflict_retried{};
                    durable_mutate_entered = true;
                    for (unsigned attempt = 0; attempt < 2; ++attempt) {
                        result = queued.kind == MutationKind::put
                                     ? detail::StoreAccess::put_durable(store_, shard, key, payload.value,
                                                                        queued.expire_at_ns)
                                     : detail::StoreAccess::erase_durable(store_, shard, key);
                        if (!detail::StoreAccess::should_retry_durable_mutation(result, attempt)) {
                            break;
                        }
                        conflict_retried = true;
                    }
                    if (conflict_retried) {
                        env.lane.metrics.conflict_retries.fetch_add(1U, std::memory_order_relaxed);
                        if (result.committed() && !result.error) {
                            env.lane.metrics.conflict_retry_commits.fetch_add(1U, std::memory_order_relaxed);
                        }
                    }
                    if (result.committed()) {
                        current_committed = true;
                        durable_commit_observed = true;
                    }
                    if (!result.committed() || result.error) {
                        auto error = result.error ? std::move(*result.error)
                                                  : Error{ErrorCode::io_error,
                                                          "durable mutation failed without an error"};
                        if (result.committed() || result.outcome == DurableMutationOutcome::indeterminate) {
                            error.code = ErrorCode::unavailable;
                            post_commit_publication_failure = true;
                            if (result.committed()) {
                                sticky_durable_commit_indices.push_back(index);
                            }
                        } else {
                            rewrite_known_not_committed_wire_error(error);
                        }
                        completion.error.emplace(std::move(error));
                    } else {
                        clean_durable_commit_indices.push_back(index);
                        stage_durable_publication(index, result);
                    }
                } else {
                    GS_FAULT_SITE(mutate);
                    auto published = queued.kind == MutationKind::put
                                         ? detail::StoreAccess::put_volatile_published(
                                               store_, shard, key, payload.value, queued.expire_at_ns)
                                         : detail::StoreAccess::erase_volatile_published(store_, shard, key);
                    if (!published) {
                        bool sticky = false;
                        auto error = classify_volatile_mutation_error(published.error(), sticky);
                        if (sticky) {
                            // Append crossed; mirror durable indeterminate sticky.
                            post_commit_publication_failure = true;
                        }
                        completion.error.emplace(std::move(error));
                    } else {
                        current_committed = true;
                        env.read_mutations.push_back({.key = key,
                                                      .record = published->record,
                                                      .segment = std::move(published->segment),
                                                      .opcode = published->opcode});
                        env.read_mutation_indices.push_back(index);
                    }
                }
            } catch (const std::bad_alloc&) {
                // If already staged into env.read_mutations, do not error-ACK here —
                // sticky/happy publish must success-ACK once authority is visible.
                const bool already_staged = std::ranges::contains(env.read_mutation_indices, index);
                if (current_committed || durable_mutate_entered || post_commit_publication_failure) {
                    post_commit_publication_failure = true;
                }
                if (!already_staged) {
                    completion.error.emplace(post_commit_publication_failure ? ErrorCode::unavailable
                                                                             : ErrorCode::resource_exhausted,
                                             "paired mutation allocation failed");
                }
            } catch (...) {
                const bool already_staged = std::ranges::contains(env.read_mutation_indices, index);
                if (current_committed || durable_mutate_entered || post_commit_publication_failure) {
                    post_commit_publication_failure = true;
                }
                if (!already_staged) {
                    completion.error.emplace(post_commit_publication_failure ? ErrorCode::unavailable
                                                                             : ErrorCode::resource_exhausted,
                                             "paired Writer failure");
                }
            }
            if (durable_mutate_entered && !detail::StoreAccess::operational(store_)) {
                post_commit_publication_failure = true;
            }
        }
    }
    GS_PHASE_FINISH(store_apply_phase);

    // Linearization order for paired mutations:
    // Store append/index publication -> immutable generation publication
    // (release) -> completion queue -> client ACK. A publication failure is
    // sticky and fail-closed because the mutable Store has already changed.
    //
    // When a later item fails publication after earlier commits, still publish
    // durable authority (snapshot) or already-staged volatile mutations so
    // successful siblings are not left unpublished with an error ACK.
    // Snapshot/incremental MUST run before publish_fail_closed(): marking the
    // durable catalog fail-closed first makes snapshot_published_reads reject.
    // After a successful durable drain, clear errors on clean commits (ACK-after-
    // publish) — capture-failed items must not keep error ACK while GET-visible.
    // After a successful incremental publish, clear errors on staged indices so a
    // catch that raced staging cannot invert RAW (visible + error ACK).
    bool generation_published = false;
    GS_PHASE_PUT_NAMED(generation_build_phase, generation_build);
    try {
        if (post_commit_publication_failure && detail::StoreAccess::is_durable(store_) &&
            (durable_commit_observed || !env.read_mutations.empty())) {
            if (drain_durable_snapshot()) {
                generation_published = true;
                ack_clean_durable_after_drain();
                if (glifistore::fault::consume_fail(glifistore::fault::Site::publish)) {
                    throw std::bad_alloc{};
                }
            } else {
                for (const auto index : env.read_mutation_indices) {
                    if (!env.completions[index].error) {
                        env.completions[index].error.emplace(
                            ErrorCode::unavailable, "read publication failed after a committed mutation; "
                                                    "paired runtime is fail-closed");
                    }
                }
            }
            publish_fail_closed();
        } else if (post_commit_publication_failure && !env.read_mutations.empty()) {
            const bool published_ok = publish_incremental_read_mutations(
                env.publication_ctx, env.read_mutations, async_slot_reservation);
            if (!published_ok) {
                for (const auto index : env.read_mutation_indices) {
                    if (!env.completions[index].error) {
                        env.completions[index].error.emplace(
                            ErrorCode::unavailable, "read publication failed after a committed mutation; "
                                                    "paired runtime is fail-closed");
                    }
                }
            } else {
                generation_published = true;
                finish_published_generation();
            }
            publish_fail_closed();
        } else if (post_commit_publication_failure) {
            if (async_slot_reservation) {
                async_slot_reservation->reset();
            }
            publish_fail_closed();
        } else if (!env.read_mutations.empty()) {
            bool published_ok = publish_incremental_read_mutations(env.publication_ctx, env.read_mutations,
                                                                   async_slot_reservation);
            if (!published_ok) {
                // Durable happy-path incremental fail: drain Index before sticky close
                // (mirror sync single-op). Volatile has nothing to drain.
                if (detail::StoreAccess::is_durable(store_) && durable_commit_observed &&
                    drain_durable_snapshot()) {
                    generation_published = true;
                    ack_clean_durable_after_drain();
                    if (glifistore::fault::consume_fail(glifistore::fault::Site::publish)) {
                        throw std::bad_alloc{};
                    }
                } else {
                    for (const auto index : env.read_mutation_indices) {
                        env.completions[index].error.emplace(
                            ErrorCode::unavailable, "read publication failed after mutation linearization; "
                                                    "paired runtime is fail-closed");
                    }
                }
                publish_fail_closed();
            } else {
                generation_published = true;
                finish_published_generation();
            }
        } else if (async_slot_reservation) {
            async_slot_reservation->reset();
        }
    } catch (const std::bad_alloc&) {
        if (generation_published) {
            ack_staged_after_publish();
            if (detail::StoreAccess::is_durable(store_)) {
                ack_clean_durable_after_drain();
            }
        } else if (!env.read_mutations.empty()) {
            for (const auto index : env.read_mutation_indices) {
                if (!env.completions[index].error) {
                    env.completions[index].error.emplace(ErrorCode::unavailable,
                                                         "paired mutation allocation failed");
                }
            }
        }
        publish_fail_closed();
    } catch (...) {
        if (generation_published) {
            ack_staged_after_publish();
            if (detail::StoreAccess::is_durable(store_)) {
                ack_clean_durable_after_drain();
            }
        } else if (!env.read_mutations.empty()) {
            for (const auto index : env.read_mutation_indices) {
                if (!env.completions[index].error) {
                    env.completions[index].error.emplace(ErrorCode::unavailable, "paired Writer failure");
                }
            }
        }
        publish_fail_closed();
    }
    GS_PHASE_FINISH(generation_build_phase);

    if (generation_published && !env.read_mutation_indices.empty()) {
        env.lane.metrics.publications.fetch_add(1U, std::memory_order_relaxed);
        env.lane.metrics.publication_records.fetch_add(env.read_mutation_indices.size(),
                                                       std::memory_order_relaxed);
    }

    const auto completed_at = std::chrono::steady_clock::now();
    MutationSink pending_notification{};
    bool has_pending_notification{};
    std::uint64_t notification_count{};
    for (std::size_t index = 0; index < env.batch.size(); ++index) {
        const auto service_ns =
            env.expired[index] ? 0U : runtime_detail::elapsed_ns(env.service_started[index], completed_at);
        if (!env.expired[index]) {
            const auto foreground_ns =
                service_ns > std::numeric_limits<std::uint64_t>::max() - env.queue_waits[index]
                    ? std::numeric_limits<std::uint64_t>::max()
                    : service_ns + env.queue_waits[index];
            detail::StoreAccess::report_foreground_latency(store_, foreground_ns);
            env.lane.metrics.service_histogram.observe(service_ns);
        } else {
            env.lane.metrics.expired_before_store.fetch_add(1U, std::memory_order_relaxed);
        }
        env.lane.metrics.completed.fetch_add(1U, std::memory_order_relaxed);
        runtime_detail::atomic_saturating_add(env.lane.metrics.total_service_ns, service_ns);
        runtime_detail::atomic_max(env.lane.metrics.maximum_service_ns, service_ns);
        env.completions[index].writer_epoch =
            env.lane.generation.writer_epoch.load(std::memory_order_relaxed);
        bool delivered = false;
        {
            GS_PHASE_PUT(completion_delivery);
            delivered =
                runtime_detail::deliver_outcome(env.batch[index].sink, std::move(env.completions[index]));
        }
        if (!delivered) {
            std::terminate();
        }

        const auto& sink = env.batch[index].sink;
        if (runtime_detail::has_notification_target(sink)) {
            if (has_pending_notification &&
                !runtime_detail::same_notification_target(pending_notification, sink)) {
                GS_PHASE_PUT(completion_notify);
                runtime_detail::notify_sink(pending_notification);
                ++notification_count;
            }
            pending_notification = sink;
            has_pending_notification = true;
        }
    }

    // Publication and every FIFO delivery happen-before the wakeup. A Reader drains its
    // completion queue after one signal, so the official single-Reader pair needs one
    // notification per Writer env.batch. A transition to a defensive non-standard target closes
    // the preceding target group. No allocation or quadratic target scan occurs here.
    if (has_pending_notification) {
        GS_PHASE_PUT(completion_notify);
        runtime_detail::notify_sink(pending_notification);
        ++notification_count;
    }
    runtime_detail::atomic_saturating_add(env.lane.metrics.completion_notifications, notification_count);
    release_execution_token(env.lane.async.execution_token);
}

} // namespace glifistore::store::paired
