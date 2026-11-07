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
#include "glifistore/store/paired/shard_pair_runtime.hpp"
#include "glifistore/store/paired/volatile_sync_chunk.hpp"
#include "store/paired/shard_pair_runtime_impl.hpp"
#include "store/store_internal.hpp"

#include <cassert>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace glifistore::store::paired {

auto ShardPairRuntime::run_writer_sync_drain(WriterSyncDrainEnv& env) noexcept -> bool {
    auto& lane = env.lane;
    const std::size_t shard = env.shard;
    const auto& batch_config = env.batch_config;
    auto& carried_sync = env.carried_sync;
    auto& merge_retry_blocked = env.merge_retry_blocked;
    auto& publication_ctx = env.publication_ctx;
    const auto invoke_void = [](const std::function<void()>* function) noexcept {
        if (function != nullptr) {
            (*function)();
        }
    };
    const auto publish_fail_closed = [&]() noexcept { invoke_void(env.hooks.publish_fail_closed); };
    const auto sticky_pair_before_durable_mark = [&]() noexcept {
        invoke_void(env.hooks.sticky_pair_before_durable_mark);
    };
    const auto reclaim_quiescent = [&]() noexcept { invoke_void(env.hooks.reclaim_quiescent); };
    const auto reclaim_proportional = [&]() noexcept { invoke_void(env.hooks.reclaim_proportional); };
    const auto drain_durable_snapshot = [&]() noexcept -> bool {
        return env.hooks.drain_durable_snapshot != nullptr && (*env.hooks.drain_durable_snapshot)();
    };
    const auto process_merge = [&](const std::size_t records) noexcept {
        if (env.hooks.process_merge != nullptr) {
            (*env.hooks.process_merge)(records);
        }
    };
    const auto update_delta_stats = [&]() noexcept { invoke_void(env.hooks.update_delta_stats); };
    bool drained_sync_turn = false;
    for (;;) {
        SyncMutation* rev = nullptr;
        if (carried_sync != nullptr) {
            // Writer-local continuation is already FIFO and is older than
            // every new LIFO admission still attached to sync_head.
            rev = std::exchange(carried_sync, nullptr);
        } else {
            SyncMutation* sync_batch = nullptr;
            {
                const std::lock_guard lock{lane.sync.sync_mutex};
                sync_batch = lane.sync_head;
                lane.sync_head = nullptr;
            }
            if (sync_batch == nullptr) {
                break;
            }
            // Reverse to preserve FIFO order from LIFO admission.
            while (sync_batch != nullptr) {
                auto* next = sync_batch->next;
                sync_batch->next = rev;
                rev = sync_batch;
                sync_batch = next;
            }
        }

        // Bound one dedicated-Writer sync turn. The continuation stays
        // Writer-local, keeps caller-owned nodes alive (callers still wait),
        // and cannot be overtaken by later sync_head admissions.
        constexpr std::size_t kMaximumSyncTurnRecords = 32U;
        auto* turn_tail = rev;
        for (std::size_t records = 1U; records < kMaximumSyncTurnRecords && turn_tail->next != nullptr;
             ++records) {
            turn_tail = turn_tail->next;
        }
        if (turn_tail->next != nullptr) {
            carried_sync = turn_tail->next;
            turn_tail->next = nullptr;
            lane.metrics.sync_turn_splits.fetch_add(1U, std::memory_order_relaxed);
        }
        drained_sync_turn = true;
        lane.metrics.sync_drain_turns.fetch_add(1U, std::memory_order_relaxed);
        GS_FAULT_BLOCK(sync_lane_snapshot);
        if (!healthy_.load(std::memory_order_acquire)) {
            for (auto* node = rev; node != nullptr;) {
                auto* const next = node->next;
                // Never Store-entered — known not newly committed (not sticky reconcile).
                node->status = Status{fail(ErrorCode::resource_exhausted, "paired runtime is fail-closed")};
                node->done.store(true, std::memory_order_release);
                node->done.notify_one();
                node = next;
            }
            break;
        }
        // Every sync drain below publishes at least one replacement generation
        // when it Store-commits. Reserve that bounded ownership capacity before
        // entering volatile or durable Store authority.
        reclaim_quiescent();
        auto turn_admission = decide_generation_admission(
            lane.generation.retired_debt(), ShardPairRuntime::kMaximumRetiredReadGenerations, true);
        turn_admission = runtime_detail::wait_generation_admission(
            turn_admission, [&] { reclaim_quiescent(); },
            [&] {
                return decide_generation_admission(lane.generation.retired_debt(),
                                                   ShardPairRuntime::kMaximumRetiredReadGenerations, true);
            });
        if (turn_admission != GenerationAdmissionDecision::admitted) {
            for (auto* node = rev; node != nullptr;) {
                auto* const next = node->next;
                lane.generation.generation_admission_backpressure_total.fetch_add(1U,
                                                                                  std::memory_order_relaxed);
                node->status =
                    Status{fail(ErrorCode::resource_exhausted, generation_admission_message(turn_admission))};
                node->done.store(true, std::memory_order_release);
                node->done.notify_one();
                node = next;
            }
            break;
        }
        if (batch_config && detail::StoreAccess::is_durable(store_)) {
            std::vector<SyncMutation*> nodes;
            std::vector<detail::StoreAccess::DurableMutationView> views;
            bool publication_required = false;
            bool durable_mutate_entered = false;
            bool sticky_publication_failure = false;
            bool sibling_snapshot_published = false;
            std::size_t first_unprocessed = 0;
            std::size_t inflight_begin = 0;
            std::size_t inflight_end = 0;
            bool mutate_inflight = false;
            // Only Index-applied committed+error items may ACK-after-visibility.
            // Put-hit alone must not upgrade a later same-key not_committed failure.
            std::vector<SyncMutation*> sticky_committed_nodes;
            sticky_committed_nodes.reserve(8);
            const auto ack_sticky_after_visibility = [&]() noexcept {
                // Same DualPath loader as Reader adopt (ADR 0036 token + mirrored pointer).
                const auto* published = load_published_generation(lane.generation);
                if (published == nullptr) {
                    return;
                }
                for (auto* node : sticky_committed_nodes) {
                    if (node->status) {
                        continue;
                    }
                    const auto view = published->prepare_durable(*node->key);
                    if (node->kind == MutationKind::put) {
                        if (view.has_value()) {
                            node->status = Status{};
                        }
                    } else if (!view.has_value() && view.error().code == ErrorCode::not_found) {
                        node->status = Status{};
                    }
                }
            };
            // sticky_pair_before_durable_mark (outer): stop later sub-batches without
            // Store mark_fail_closed before sibling snapshot.
            const auto upgrade_placeholder = [&](const std::size_t index,
                                                 const ErrorCode before_mutate) -> ErrorCode {
                // In-flight sub-batch may have crossed a write boundary → unavailable.
                // Later never-started placeholders stay known-not-committed.
                if (mutate_inflight && index >= inflight_begin && index < inflight_end) {
                    return ErrorCode::unavailable;
                }
                if (mutate_inflight && index >= inflight_end) {
                    return ErrorCode::resource_exhausted;
                }
                if (index >= first_unprocessed) {
                    return durable_mutate_entered || publication_required ? ErrorCode::resource_exhausted
                                                                          : before_mutate;
                }
                return ErrorCode::unavailable;
            };
            try {
                for (auto* node = rev; node != nullptr; node = node->next) {
                    nodes.push_back(node);
                }
                // Default Status{} is success — must not survive catch/drain for items
                // never Store-mutated (would success-ACK invisible keys).
                for (auto* node : nodes) {
                    node->status = Status{
                        fail(ErrorCode::resource_exhausted, "paired durable batch item not processed")};
                }
                views.reserve(nodes.size());
                std::size_t begin = 0;
                while (begin < nodes.size()) {
                    if (!healthy_.load(std::memory_order_acquire) ||
                        !detail::StoreAccess::operational(store_)) {
                        sticky_publication_failure = true;
                        sticky_pair_before_durable_mark();
                        for (std::size_t index = begin; index < nodes.size(); ++index) {
                            // Remaining siblings never entered mutate_durable_batch.
                            nodes[index]->status =
                                Status{fail(ErrorCode::resource_exhausted, "paired runtime is fail-closed")};
                        }
                        break;
                    }
                    const std::size_t end = durable_subbatch_end(
                        begin, nodes.size(),
                        [&](const std::size_t index) -> const HashedKey& { return *nodes[index]->key; });
                    views.clear();
                    for (std::size_t index = begin; index < end; ++index) {
                        auto* node = nodes[index];
                        views.push_back({.operation = node->kind == MutationKind::put
                                                          ? detail::StoreAccess::MutationOperation::put
                                                          : detail::StoreAccess::MutationOperation::erase,
                                         .key = *node->key,
                                         .value = node->value,
                                         .expire_at_ns = node->expire_at_ns,
                                         .type = node->type});
                    }
                    GS_FAULT_SITE(mutate);
                    inflight_begin = begin;
                    inflight_end = end;
                    mutate_inflight = true;
                    durable_mutate_entered = true;
                    auto results = detail::StoreAccess::mutate_durable_batch(store_, shard, views);
                    // Keep mutate_inflight through classification: a throw here must not
                    // stamp Store-entered siblings as never-started resource_exhausted.
                    if (glifistore::fault::consume_fail(glifistore::fault::Site::post_mutate)) {
                        throw std::bad_alloc{};
                    }
                    if (results.size() != views.size()) {
                        std::terminate();
                    }
                    for (std::size_t offset = 0; offset < results.size(); ++offset) {
                        auto& result = results[offset].mutation;
                        publication_required = publication_required || result.committed();
                        if (!result.committed() || result.error) {
                            auto error = result.error ? std::move(*result.error)
                                                      : Error{ErrorCode::io_error,
                                                              "durable mutation failed without an error"};
                            if (result.committed() ||
                                result.outcome == DurableMutationOutcome::indeterminate) {
                                // Defer durable mark_fail_closed until after sibling snapshot.
                                error.code = ErrorCode::unavailable;
                                sticky_publication_failure = true;
                                sticky_pair_before_durable_mark();
                                if (result.committed()) {
                                    sticky_committed_nodes.push_back(nodes[begin + offset]);
                                }
                            } else {
                                rewrite_known_not_committed_wire_error(error);
                            }
                            nodes[begin + offset]->status = Status{unexpected(std::move(error))};
                        } else {
                            nodes[begin + offset]->status = Status{};
                        }
                    }
                    first_unprocessed = end;
                    mutate_inflight = false;
                    begin = end;
                }
                if (publication_required) {
                    // allow_fail_closed: failing mutate may already have marked durable
                    // unhealthy while earlier siblings remain indexed.
                    auto snapshot = detail::StoreAccess::snapshot_durable_reads(store_, shard, true);
                    if (!snapshot) {
                        publish_fail_closed();
                        for (auto* node : nodes) {
                            if (node->status) {
                                node->status =
                                    Status{fail(ErrorCode::unavailable,
                                                "read publication failed after durable Writer batch")};
                            }
                        }
                    } else {
                        const auto outcome =
                            replace_durable_snapshot_and_publish(publication_ctx, snapshot->records);
                        if (outcome.status != DualPathPublishStatus::published) {
                            publish_fail_closed();
                            for (auto* node : nodes) {
                                if (node->status) {
                                    node->status =
                                        Status{fail(ErrorCode::unavailable,
                                                    "read publication failed after durable Writer batch")};
                                }
                            }
                        } else {
                            note_catalog_snapshot_installed(publication_ctx, snapshot->catalog_revision);
                            merge_retry_blocked = false;
                            reclaim_quiescent();
                            sibling_snapshot_published = true;
                            // ACK-after-visibility only for sticky committed+error items.
                            ack_sticky_after_visibility();
                            if (glifistore::fault::consume_fail(glifistore::fault::Site::publish)) {
                                throw std::bad_alloc{};
                            }
                            if (sticky_publication_failure) {
                                publish_fail_closed();
                            }
                        }
                    }
                } else if (sticky_publication_failure) {
                    publish_fail_closed();
                }
            } catch (const std::bad_alloc&) {
                if (publication_required || durable_mutate_entered) {
                    sibling_snapshot_published = drain_durable_snapshot();
                }
                if (publication_required || durable_mutate_entered) {
                    publish_fail_closed();
                }
                if (sibling_snapshot_published) {
                    // Only sticky Index-committed items — never presence-upgrade
                    // unprocessed attempted puts (pre-existing key would false-ACK).
                    ack_sticky_after_visibility();
                }
                for (std::size_t index = 0; index < nodes.size(); ++index) {
                    auto* node = nodes[index];
                    if (sibling_snapshot_published && node->status) {
                        continue;
                    }
                    if (!node->status) {
                        // Keep mid-chunk fail-closed / rewritten not-committed / sticky
                        // unavailable. Only the pre-mutate placeholder may be upgraded.
                        if (node->status.error().message.find("paired durable batch item not processed") ==
                            std::string::npos) {
                            continue;
                        }
                    }
                    node->status = Status{fail(upgrade_placeholder(index, ErrorCode::resource_exhausted),
                                               "paired mutation allocation failed")};
                }
            } catch (...) {
                if (publication_required || durable_mutate_entered) {
                    sibling_snapshot_published = drain_durable_snapshot();
                }
                if (publication_required || durable_mutate_entered) {
                    publish_fail_closed();
                }
                if (sibling_snapshot_published) {
                    ack_sticky_after_visibility();
                }
                for (std::size_t index = 0; index < nodes.size(); ++index) {
                    auto* node = nodes[index];
                    if (sibling_snapshot_published && node->status) {
                        continue;
                    }
                    if (!node->status) {
                        if (node->status.error().message.find("paired durable batch item not processed") ==
                            std::string::npos) {
                            continue;
                        }
                    }
                    node->status = Status{fail(upgrade_placeholder(index, ErrorCode::resource_exhausted),
                                               "paired Writer failure")};
                }
            }
            // Durable may mark itself fail-closed while swallowing exceptions into
            // Status errors. Couple the pair sticky bit so sync mutate rejects next.
            // Do not rewrite successful sibling ACKs after a successful drain snapshot.
            if (durable_mutate_entered && !detail::StoreAccess::operational(store_)) {
                publish_fail_closed();
                if (!sibling_snapshot_published) {
                    for (auto* node = rev; node != nullptr; node = node->next) {
                        if (node->status) {
                            node->status =
                                Status{fail(ErrorCode::unavailable, "paired runtime is fail-closed")};
                        }
                    }
                }
            }
            for (auto* node = rev; node != nullptr;) {
                // Snapshot next before notify: SyncMutation lives on the caller's stack and
                // may be destroyed as soon as done is observed.
                auto* const next = node->next;
                node->done.store(true, std::memory_order_release);
                node->done.notify_one();
                node = next;
            }
            break;
        }
        if (!detail::StoreAccess::is_durable(store_)) {
            // Volatile sync: apply FIFO then one publish_incremental per ≤32
            // mutations (mirrors async). ACK only after publication. Stack
            // chunking keeps the N=1 path allocation-free.
            const auto reject_remaining_fail_closed = [&](SyncMutation*& head) noexcept {
                while (head != nullptr) {
                    auto* const next = head->next;
                    // Later chunks never mutated after an earlier sticky close.
                    head->status =
                        Status{fail(ErrorCode::resource_exhausted, "paired runtime is fail-closed")};
                    head->done.store(true, std::memory_order_release);
                    head->done.notify_one();
                    head = next;
                }
            };
            while (rev != nullptr) {
                // After publish_fail_closed in an earlier chunk, do not mutate or
                // publish later chunks from the same sync drain (put_batch >32).
                if (!healthy_.load(std::memory_order_acquire)) {
                    reject_remaining_fail_closed(rev);
                    break;
                }
                std::array<SyncMutation*, kMaximumPublicationBatch> chunk{};
                std::size_t chunk_size = 0;
                const auto chunk_cap = sync_publication_chunk_cap(kMaximumPublicationBatch);
                while (rev != nullptr && chunk_size < chunk_cap) {
                    chunk[chunk_size++] = rev;
                    rev = rev->next;
                }
                std::optional<GenerationSlotPool::Reservation> slot_reservation;
                // run_writer_sync_drain is noexcept: allocation faults from merge /
                // reclaim / admission must not escape past apply_volatile's own try.
                try {
                    reclaim_quiescent();
                    process_merge(chunk_size);
                    if (!healthy_.load(std::memory_order_acquire)) {
                        for (std::size_t index = 0; index < chunk_size; ++index) {
                            chunk[index]->status =
                                Status{fail(ErrorCode::resource_exhausted, "paired runtime is fail-closed")};
                            chunk[index]->done.store(true, std::memory_order_release);
                            chunk[index]->done.notify_one();
                        }
                        reject_remaining_fail_closed(rev);
                        break;
                    }
                    auto generation_admission = decide_generation_admission(
                        lane.generation.retired_debt(), ShardPairRuntime::kMaximumRetiredReadGenerations,
                        PairReadGeneration::can_publish_incremental(*lane.generation.writer_generation,
                                                                    lane.merge.read_merge.get(), chunk_size));
                    generation_admission = runtime_detail::wait_generation_admission(
                        generation_admission,
                        [&] {
                            reclaim_quiescent();
                            process_merge(chunk_size);
                        },
                        [&] {
                            return decide_generation_admission(
                                lane.generation.retired_debt(),
                                ShardPairRuntime::kMaximumRetiredReadGenerations,
                                PairReadGeneration::can_publish_incremental(
                                    *lane.generation.writer_generation, lane.merge.read_merge.get(),
                                    chunk_size));
                        });
                    if (!healthy_.load(std::memory_order_acquire)) {
                        for (std::size_t index = 0; index < chunk_size; ++index) {
                            chunk[index]->status =
                                Status{fail(ErrorCode::resource_exhausted, "paired runtime is fail-closed")};
                            chunk[index]->done.store(true, std::memory_order_release);
                            chunk[index]->done.notify_one();
                        }
                        reject_remaining_fail_closed(rev);
                        break;
                    }
                    if (generation_admission != GenerationAdmissionDecision::admitted) {
                        lane.generation.generation_admission_backpressure_total.fetch_add(
                            chunk_size, std::memory_order_relaxed);
                        if (generation_admission == GenerationAdmissionDecision::incremental_merge_required) {
                            lane.merge.read_merge_backpressure.fetch_add(1U, std::memory_order_relaxed);
                        }
                        for (std::size_t index = 0; index < chunk_size; ++index) {
                            // Never Store-entered — same polarity as async generation pressure.
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
                    // Use the Writer-stable prepare_publish_retry (no per-chunk std::function).
                    apply_volatile_sync_publication_chunk(
                        store_, shard, publication_ctx, std::span{view_storage.data(), chunk_size},
                        slot_reservation, VolatileSyncChunkMode::dedicated_writer, healthy_,
                        env.hooks.publish_fail_closed, env.hooks.reclaim_proportional,
                        env.hooks.prepare_publish_retry);
                    bool chunk_failed = false;
                    bool chunk_succeeded = false;
                    for (std::size_t index = 0; index < chunk_size; ++index) {
                        chunk[index]->status = view_storage[index].status;
                        if (!chunk[index]->status) {
                            chunk_failed = true;
                        } else {
                            chunk_succeeded = true;
                        }
                    }
                    bool all_failed_indeterminate = false;
                    if (chunk_failed && !chunk_succeeded) {
                        all_failed_indeterminate = !healthy_.load(std::memory_order_acquire);
                        if (!all_failed_indeterminate) {
                            for (std::size_t index = 0; index < chunk_size; ++index) {
                                if (!chunk[index]->status &&
                                    chunk[index]->status.error().code == ErrorCode::unavailable) {
                                    all_failed_indeterminate = true;
                                    break;
                                }
                            }
                        }
                    }
                    for (std::size_t index = 0; index < chunk_size; ++index) {
                        // Completion releases the caller: do not dereference its stack-backed node
                        // after this store. Derive every chunk decision above the notification.
                        chunk[index]->done.store(true, std::memory_order_release);
                        chunk[index]->done.notify_one();
                    }
                    // Sticky only for indeterminate (unavailable) all-failed chunks or an
                    // already-armed fail-closed. Pure known-not-committed all-failed
                    // (rotation, compact gate, admission) must leave the pair healthy so
                    // callers can retry. Mixed success/failure stays non-sticky.
                    if (all_failed_indeterminate) {
                        publish_fail_closed();
                        reject_remaining_fail_closed(rev);
                        break;
                    }
                    if (!healthy_.load(std::memory_order_acquire)) {
                        reject_remaining_fail_closed(rev);
                        break;
                    }
                } catch (const std::bad_alloc&) {
                    publish_fail_closed();
                    for (std::size_t index = 0; index < chunk_size; ++index) {
                        if (chunk[index]->done.load(std::memory_order_acquire)) {
                            continue;
                        }
                        chunk[index]->status =
                            Status{fail(ErrorCode::resource_exhausted, "paired mutation allocation failed")};
                        chunk[index]->done.store(true, std::memory_order_release);
                        chunk[index]->done.notify_one();
                    }
                    reject_remaining_fail_closed(rev);
                    break;
                } catch (...) {
                    publish_fail_closed();
                    for (std::size_t index = 0; index < chunk_size; ++index) {
                        if (chunk[index]->done.load(std::memory_order_acquire)) {
                            continue;
                        }
                        chunk[index]->status =
                            Status{fail(ErrorCode::resource_exhausted, "paired Writer failure")};
                        chunk[index]->done.store(true, std::memory_order_release);
                        chunk[index]->done.notify_one();
                    }
                    reject_remaining_fail_closed(rev);
                    break;
                }
            }
            break;
        }
        while (rev != nullptr) {
            if (!healthy_.load(std::memory_order_acquire)) {
                for (auto* node = rev; node != nullptr;) {
                    auto* const next = node->next;
                    // Never Store-entered — known not newly committed.
                    node->status =
                        Status{fail(ErrorCode::resource_exhausted, "paired runtime is fail-closed")};
                    node->done.store(true, std::memory_order_release);
                    node->done.notify_one();
                    node = next;
                }
                rev = nullptr;
                break;
            }
            auto* node = rev;
            rev = rev->next;
            reclaim_quiescent();
            process_merge(1U);
            auto generation_admission = decide_generation_admission(
                lane.generation.retired_debt(), ShardPairRuntime::kMaximumRetiredReadGenerations,
                PairReadGeneration::can_publish_incremental(*lane.generation.writer_generation,
                                                            lane.merge.read_merge.get(), 1U));
            generation_admission = runtime_detail::wait_generation_admission(
                generation_admission,
                [&] {
                    reclaim_quiescent();
                    process_merge(1U);
                },
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
            // Behavior-neutral shadow lifecycle (docs/spec/mutation-lifecycle.md).
            // Existing bools remain authoritative for control flow.
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
                if (detail::StoreAccess::is_durable(store_)) {
                    durable_mutate_entered = true;
                    static_cast<void>(life.mark_durable_started());
                    auto result = execute_durable_single(store_, shard, node->kind, key, node->value,
                                                         node->expire_at_ns, node->type);
                    if (result.committed()) {
                        durable_committed = true;
                    }
                    static_cast<void>(life.apply_durable_result(result));
                    // After drain/publish: success ACK iff published generation matches the
                    // mutation (put hit / erase miss). Index-insert-fail stays error+miss.
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
                        auto error = result.error ? *result.error
                                     : result.committed()
                                         ? Error{ErrorCode::internal_error,
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
                                        status = Status{fail(
                                            ErrorCode::resource_exhausted,
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
                                        status = generation_published
                                                     ? ack_after_published_visibility()
                                                     : Status{fail(ErrorCode::unavailable,
                                                                   "read publication failed")};
                                        shadow_resolve_status();
                                    } else {
                                        update_delta_stats();
                                        // Mark published before reclaim so catch cannot invert RAW.
                                        generation_published = true;
                                        shadow_mark_published(true);
                                        if (glifistore::fault::consume_fail(
                                                glifistore::fault::Site::publish)) {
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
                                    update_delta_stats();
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
                } else {
                    status = Status{fail(ErrorCode::internal_error, "volatile sync path misrouted")};
                    shadow_resolve_status();
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
                    // Keep definitive polarity — including visibility-failed errors after
                    // drain. Do not promote those to success just because a generation
                    // was published (inverted RAW).
                    break;
                case SyncDurableExceptionStatusKind::success_after_visibility:
                    // Happy-path: authority published before status assignment
                    // (e.g. Site::publish after publish_read_generation).
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
                // Match catch / durable_group sync: keep definitive polarity. A
                // known-not-committed rewrite (resource_exhausted → wire OVERLOADED)
                // must not be overwritten to unavailable (wire INTERNAL_ERROR /
                // reconcile_first) just because the catalog went fail-closed.
                // Only fill when unresolved; only demote unpublished success.
                if (!generation_published) {
                    if (!status_resolved) {
                        status = Status{fail(ErrorCode::unavailable, "paired runtime is fail-closed")};
                        shadow_resolve_status();
                    } else if (status) {
                        status = Status{fail(ErrorCode::unavailable, "paired runtime is fail-closed")};
                    }
                }
            }
#ifndef NDEBUG
            if (durable_mutate_entered) {
                assert(life.durable().mutate_entered);
                if (durable_committed) {
                    assert(life.durable().committed());
                }
                if (generation_published) {
                    assert(life.publication().published() || life.stage() == MutationStage::completed ||
                           life.stage() == MutationStage::completion_decided);
                }
                if (status_resolved) {
                    assert(life.stage() == MutationStage::completed ||
                           life.stage() == MutationStage::completion_decided);
                }
            }
#endif
            node->status = status;
            node->done.store(true, std::memory_order_release);
            node->done.notify_one();
        }
        break;
    }

    return drained_sync_turn;
}

} // namespace glifistore::store::paired
