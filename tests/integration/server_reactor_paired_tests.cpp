#include "experimental/generation_slot_pool.hpp"
#include "glifistore/core/fault_injection.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "glifistore/persistence/store_backup.hpp"
#include "glifistore/server/daemon_log.hpp"
#include "glifistore/server/protocol.hpp"
#include "glifistore/server/server.hpp"
#include "glifistore/store/store.hpp"
#include "server/reactor_detail.hpp"
#include "server_reactor_test_support.hpp"
#include "store/store_internal.hpp"
#include "test.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <mutex>
#include <netinet/in.h>
#include <span>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <vector>

using namespace glifistore::test::server_reactor_support;

GLIFI_TEST("durable daemon retries only a proven non-committed first-attempt sequence conflict") {
    using glifistore::DurableMutationOutcome;
    using glifistore::DurableMutationResult;
    using glifistore::Error;
    using glifistore::ErrorCode;
    const auto should_retry = [](const DurableMutationResult& result, const unsigned attempt) {
        return glifistore::detail::StoreAccess::should_retry_durable_mutation(result, attempt);
    };

    const DurableMutationResult retryable{
        .outcome = DurableMutationOutcome::not_committed,
        .error = Error{ErrorCode::sequence_conflict, "stale rotation snapshot"},
    };
    GLIFI_REQUIRE(should_retry(retryable, 0));
    GLIFI_REQUIRE(!should_retry(retryable, 1));

    const DurableMutationResult committed{
        .outcome = DurableMutationOutcome::committed,
        .error = Error{ErrorCode::sequence_conflict, "post-commit diagnostic"},
    };
    const DurableMutationResult indeterminate{
        .outcome = DurableMutationOutcome::indeterminate,
        .error = Error{ErrorCode::sequence_conflict, "authority uncertain"},
    };
    const DurableMutationResult other_error{
        .outcome = DurableMutationOutcome::not_committed,
        .error = Error{ErrorCode::io_error, "pre-commit I/O failure"},
    };
    const DurableMutationResult missing_error{
        .outcome = DurableMutationOutcome::not_committed,
    };
    GLIFI_REQUIRE(!should_retry(committed, 0));
    GLIFI_REQUIRE(!should_retry(indeterminate, 0));
    GLIFI_REQUIRE(!should_retry(other_error, 0));
    GLIFI_REQUIRE(!should_retry(missing_error, 0));
}

#if defined(GLIFISTORE_FAULT_INJECTION)
GLIFI_TEST("dedicated paired Writer gives admitted async work a turn within one large sync batch") {
    auto opened = open_paired_store_for_writer(1, 8);
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    auto executor = glifistore::server::PairWriterPool::create(store, 1, 8, kTestMutationArenaBytes,
                                                               std::chrono::milliseconds{0}, {});
    GLIFI_REQUIRE(executor.has_value());
    GLIFI_REQUIRE((*executor)->start().has_value());
    glifistore::server::BoundedSpscQueue<glifistore::server::MutationCompletion> completions{8};
    auto wakeup = glifistore::server::Wakeup::create();
    GLIFI_REQUIRE(wakeup.has_value());

    glifistore::fault::reset();
    glifistore::fault::arm_block(glifistore::fault::Site::sync_lane_snapshot);
    std::vector<std::string> sync_keys;
    sync_keys.reserve(64);
    for (std::size_t index = 0; index < 64U; ++index) {
        sync_keys.push_back("large-sync-batch-" + std::to_string(index));
    }
    std::vector<glifistore::Store::PutItem> sync_items;
    sync_items.reserve(sync_keys.size());
    for (const auto& key : sync_keys) {
        sync_items.push_back({.key = key, .value = bytes("sync")});
    }
    std::atomic_bool sync_batch_ok{};
    std::thread sync_batch{[&] {
        const auto statuses = store.put_batch(sync_items);
        sync_batch_ok.store(
            std::ranges::all_of(statuses, [](const auto& status) { return status.has_value(); }));
    }};
    const auto first_snapshot_blocked =
        glifistore::fault::wait_until_blocked(glifistore::fault::Site::sync_lane_snapshot);

    const auto async_admitted = (*executor)
                                    ->try_submit({
                                        .connection = {.slot = 1, .generation = 1},
                                        .request_id = 700,
                                        .worker_index = 0,
                                        .kind = glifistore::server::MutationKind::put,
                                        .key = bytes("async-between-sync-snapshots"),
                                        .key_hash = glifistore::hash_key("async-between-sync-snapshots"),
                                        .value = bytes("async"),
                                        .completions = &completions,
                                        .wakeup = &*wakeup,
                                    })
                                    .has_value();

    glifistore::fault::release_block(glifistore::fault::Site::sync_lane_snapshot);
    std::optional<glifistore::server::MutationCompletion> async_completion;
    const auto completion_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!async_completion && std::chrono::steady_clock::now() < completion_deadline) {
        async_completion = completions.try_pop();
        if (!async_completion) {
            std::this_thread::yield();
        }
    }
    sync_batch.join();
    glifistore::fault::reset();

    GLIFI_REQUIRE(first_snapshot_blocked);
    GLIFI_REQUIRE(async_admitted);
    GLIFI_REQUIRE(sync_batch_ok.load());
    GLIFI_REQUIRE(async_completion.has_value());
    GLIFI_REQUIRE(!async_completion->error.has_value());
    GLIFI_REQUIRE((*executor)->release_payload(0, async_completion->payload_slot));
    const auto stats = (*executor)->stats();
    GLIFI_REQUIRE(stats.size() == 1);
    GLIFI_REQUIRE(stats[0].sync_drain_turns >= 2U);
    GLIFI_REQUIRE(stats[0].sync_turn_splits >= 1U);
    GLIFI_REQUIRE(stats[0].sync_async_fairness_turns >= 1U);
    GLIFI_REQUIRE((*executor)->stop_and_drain().has_value());
    GLIFI_REQUIRE(store.close().has_value());
}
#endif

GLIFI_TEST("paired Writer completes incremental read merge in bounded quanta") {
    const glifistore::server::PairReadMergeConfig merge_config{
        .delta_entries = 4,
        .maximum_post_entries = 8,
        .quantum_slots = 4'096,
    };
    auto opened =
        open_paired_store_for_writer(1, 8, kTestMutationArenaBytes,
                                     {.async_writer_batch_max_records = 2,
                                      .merge_delta_entries = merge_config.delta_entries,
                                      .merge_maximum_post_entries = merge_config.maximum_post_entries,
                                      .merge_quantum_slots = merge_config.quantum_slots});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    glifistore::server::BoundedSpscQueue<glifistore::server::MutationCompletion> completions{8};
    auto wakeup = glifistore::server::Wakeup::create();
    GLIFI_REQUIRE(wakeup.has_value());
    auto executor = glifistore::server::PairWriterPool::create(store, 1, 8, kTestMutationArenaBytes,
                                                               std::chrono::milliseconds{0}, merge_config);
    GLIFI_REQUIRE(executor.has_value());
    GLIFI_REQUIRE((*executor)->start().has_value());

    std::array<std::string, 4> keys{"merge-a", "merge-b", "merge-c", "merge-d"};
    for (std::size_t index = 0; index < keys.size(); ++index) {
        GLIFI_REQUIRE((*executor)
                          ->try_submit({
                              .connection = {.slot = static_cast<std::uint32_t>(index + 1U), .generation = 1},
                              .request_id = 800U + index,
                              .worker_index = 0,
                              .kind = glifistore::server::MutationKind::put,
                              .key = bytes(keys[index]),
                              .key_hash = glifistore::hash_key(keys[index]),
                              .value = bytes("value"),
                              .completions = &completions,
                              .wakeup = &*wakeup,
                          })
                          .has_value());
    }

    std::size_t completed{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (completed != keys.size() && std::chrono::steady_clock::now() < deadline) {
        if (auto completion = completions.try_pop()) {
            GLIFI_REQUIRE(!completion->error.has_value());
            GLIFI_REQUIRE((*executor)->release_payload(0, completion->payload_slot));
            ++completed;
        } else {
            static_cast<void>((*executor)->adopt_read_generation(0));
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
    GLIFI_REQUIRE(completed == keys.size());
    const auto completion_stats = (*executor)->stats()[0];
    GLIFI_REQUIRE(completion_stats.writer_batch_records == keys.size());
    GLIFI_REQUIRE(completion_stats.writer_batches >= 1);
    GLIFI_REQUIRE(completion_stats.writer_batches <= keys.size());
    GLIFI_REQUIRE(completion_stats.maximum_writer_batch_records <= 2);
    GLIFI_REQUIRE(completion_stats.publications == completion_stats.writer_batches);
    GLIFI_REQUIRE(completion_stats.publication_records == keys.size());
    // One Reader owns this lane and drains every delivered completion after a wakeup. The
    // Writer therefore emits one notification per completed Writer batch, not per mutation.
    GLIFI_REQUIRE(completion_stats.completion_notifications == completion_stats.writer_batches);

    glifistore::server::PairWriterStats stats;
    const auto merge_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < merge_deadline) {
        static_cast<void>((*executor)->adopt_read_generation(0));
        stats = (*executor)->stats()[0];
        if (stats.read_merge_completions != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    GLIFI_REQUIRE(stats.read_merge_starts == 1);
    GLIFI_REQUIRE(stats.read_merge_completions == 1);
    GLIFI_REQUIRE(stats.read_merge_failures == 0);
    GLIFI_REQUIRE(stats.read_merge_backpressure == 0);
    GLIFI_REQUIRE(stats.read_merge_slots_processed > 0);
    GLIFI_REQUIRE(stats.read_merge_remaining_slots == 0);
    GLIFI_REQUIRE(stats.read_merge_post_capacity_remaining == 0);
    GLIFI_REQUIRE(stats.maximum_read_merge_quantum_slots <= merge_config.quantum_slots);
    GLIFI_REQUIRE(!stats.read_merge_active);
    GLIFI_REQUIRE(stats.read_merge_post_entries == 0);
    GLIFI_REQUIRE(stats.read_generation_memory.base_entries == keys.size());
    GLIFI_REQUIRE(stats.read_generation_memory.base_record_storage_bytes == keys.size() * 64U);
    GLIFI_REQUIRE(stats.read_generation_memory.base_record_mapped_storage_bytes == 0);
    GLIFI_REQUIRE(stats.read_generation_memory.base_lookup_storage_bytes ==
                  stats.read_generation_memory.base_capacity * 5U);
    GLIFI_REQUIRE(stats.read_generation_memory.current_allocated_lower_bound_bytes > 0);

    const auto* generation = (*executor)->adopt_read_generation(0);
    GLIFI_REQUIRE(generation != nullptr);
    GLIFI_REQUIRE(generation->base_entries() == keys.size());
    GLIFI_REQUIRE(generation->delta_entries() == 0);
    for (const auto& key : keys) {
        auto value = generation->get({.key = key, .hash = glifistore::hash_key(key)}, 0);
        GLIFI_REQUIRE(value.has_value());
        GLIFI_REQUIRE(text(value->view()) == "value");
    }

    // Four versions of one existing key must start a second merge even though
    // the logical Delta contains only one entry. This bounds overwrite churn
    // in the append-only record arena.
    for (std::size_t index = 0; index < 4; ++index) {
        GLIFI_REQUIRE((*executor)
                          ->try_submit({
                              .connection = {.slot = static_cast<std::uint32_t>(index + 9U), .generation = 1},
                              .request_id = 900U + index,
                              .worker_index = 0,
                              .kind = glifistore::server::MutationKind::put,
                              .key = bytes(keys[0]),
                              .key_hash = glifistore::hash_key(keys[0]),
                              .value = bytes("new-value"),
                              .completions = &completions,
                              .wakeup = &*wakeup,
                          })
                          .has_value());
    }
    completed = 0;
    const auto overwrite_completion_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (completed != 4 && std::chrono::steady_clock::now() < overwrite_completion_deadline) {
        if (auto completion = completions.try_pop()) {
            GLIFI_REQUIRE(!completion->error.has_value());
            GLIFI_REQUIRE((*executor)->release_payload(0, completion->payload_slot));
            ++completed;
        } else {
            static_cast<void>((*executor)->adopt_read_generation(0));
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
    GLIFI_REQUIRE(completed == 4);
    const auto overwrite_merge_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < overwrite_merge_deadline) {
        static_cast<void>((*executor)->adopt_read_generation(0));
        stats = (*executor)->stats()[0];
        if (stats.read_merge_completions == 2) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    GLIFI_REQUIRE(stats.read_merge_starts == 2);
    GLIFI_REQUIRE(stats.read_merge_completions == 2);
    GLIFI_REQUIRE(stats.delta_entries == 0);
    GLIFI_REQUIRE(stats.delta_record_versions == 0);
    GLIFI_REQUIRE(stats.delta_arena_record_bytes == 0);
    generation = (*executor)->adopt_read_generation(0);
    GLIFI_REQUIRE(generation != nullptr);
    const auto overwritten = generation->get({.key = keys[0], .hash = glifistore::hash_key(keys[0])}, 0);
    GLIFI_REQUIRE(overwritten.has_value());
    GLIFI_REQUIRE(text(overwritten->bytes) == "new-value");

    GLIFI_REQUIRE((*executor)->stop_and_drain().has_value());
    GLIFI_REQUIRE(store.close().has_value());
}

GLIFI_TEST("paired Writer validates merge bounds and aligns payload credits with ring capacity") {
    GLIFI_REQUIRE(
        !open_paired_store_for_writer(1, 8, kTestMutationArenaBytes, {.async_writer_batch_max_records = 0})
             .has_value());
    GLIFI_REQUIRE(
        !open_paired_store_for_writer(1, 8, kTestMutationArenaBytes, {.async_writer_batch_max_bytes = 0})
             .has_value());

    auto opened = open_paired_store_for_writer(1, 3);
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;

    GLIFI_REQUIRE(!glifistore::server::PairWriterPool::create(
                       store, 1, 8, kTestMutationArenaBytes, std::chrono::milliseconds{0},
                       {.delta_entries = 0, .maximum_post_entries = 1, .quantum_slots = 1})
                       .has_value());
    GLIFI_REQUIRE(!glifistore::server::PairWriterPool::create(
                       store, 1, 8, kTestMutationArenaBytes, std::chrono::milliseconds{0},
                       {.delta_entries = 1, .maximum_post_entries = 1, .quantum_slots = 0})
                       .has_value());
    GLIFI_REQUIRE(!glifistore::server::PairWriterPool::create(
                       store, 1, 8, kTestMutationArenaBytes, std::chrono::milliseconds{0},
                       {.delta_entries = 1, .maximum_post_entries = 0, .quantum_slots = 1})
                       .has_value());
    GLIFI_REQUIRE(
        !glifistore::server::PairWriterPool::create(
             store, 1, 8, kTestMutationArenaBytes, std::chrono::milliseconds{0},
             {.delta_entries = glifistore::server::PairReadGeneration::kMaximumIncrementalDeltaEntries,
              .maximum_post_entries = 1,
              .quantum_slots = 1})
             .has_value());

    auto rounded = glifistore::server::PairWriterPool::create(store, 1, 3, kTestMutationArenaBytes,
                                                              std::chrono::milliseconds{0});
    GLIFI_REQUIRE(rounded.has_value());
    const auto rounded_stats = (*rounded)->stats();
    GLIFI_REQUIRE(rounded_stats.size() == 1);
    GLIFI_REQUIRE(rounded_stats[0].payload_slot_capacity == 4);
    rounded->reset();
    GLIFI_REQUIRE(store.close().has_value());
}

GLIFI_TEST("paired async Writer rejects retire pressure through completion before Store") {
    using glifistore::store::paired::ShardPairRuntime;

    auto opened = open_paired_store_for_writer(1, 2, 64U * 1024U);
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    glifistore::server::BoundedSpscQueue<glifistore::server::MutationCompletion> completions{2};
    auto wakeup = glifistore::server::Wakeup::create();
    GLIFI_REQUIRE(wakeup.has_value());
    auto executor =
        glifistore::server::PairWriterPool::create(store, 1, 2, 64U * 1024U, std::chrono::milliseconds{0});
    GLIFI_REQUIRE(executor.has_value());
    GLIFI_REQUIRE((*executor)->start().has_value());

    const auto* pinned_generation = (*executor)->adopt_read_generation(0);
    GLIFI_REQUIRE(pinned_generation != nullptr);
    const auto pinned_epoch = pinned_generation->epoch();
    const auto submit_and_wait = [&](const std::uint64_t request_id, const std::string_view key,
                                     const std::string_view value) -> glifistore::server::MutationCompletion {
        GLIFI_REQUIRE((*executor)
                          ->try_submit({.connection = {.slot = 1, .generation = 1},
                                        .request_id = request_id,
                                        .worker_index = 0,
                                        .kind = glifistore::server::MutationKind::put,
                                        .key = bytes(key),
                                        .key_hash = glifistore::hash_key(key),
                                        .value = bytes(value),
                                        .completions = &completions,
                                        .wakeup = &*wakeup})
                          .has_value());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        std::optional<glifistore::server::MutationCompletion> completion;
        while (!completion && std::chrono::steady_clock::now() < deadline) {
            completion = completions.try_pop();
            if (!completion) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }
        GLIFI_REQUIRE(completion.has_value());
        GLIFI_REQUIRE((*executor)->release_payload(0, completion->payload_slot));
        return std::move(*completion);
    };

    for (std::size_t publication = 0; publication < ShardPairRuntime::kMaximumRetiredReadGenerations;
         ++publication) {
        const auto value = "async-generation-" + std::to_string(publication);
        const auto completion = submit_and_wait(10'000U + publication, "async-retire-pressure", value);
        GLIFI_REQUIRE(!completion.error.has_value());
    }
    auto stats = (*executor)->stats()[0];
    GLIFI_REQUIRE(stats.reader_safe_epoch == pinned_epoch);
    GLIFI_REQUIRE(stats.retired_generation_count == ShardPairRuntime::kMaximumRetiredReadGenerations);

    const auto writer_epoch_before_rejection = stats.writer_epoch;
    const auto blocked = submit_and_wait(20'000U, "async-must-not-enter", "blocked");
    GLIFI_REQUIRE(blocked.error.has_value());
    GLIFI_REQUIRE(blocked.error->code == glifistore::ErrorCode::resource_exhausted);
    GLIFI_REQUIRE(blocked.error->message == "mutation rejected until paired Reader reaches quiescence");
    stats = (*executor)->stats()[0];
    GLIFI_REQUIRE(stats.writer_epoch == writer_epoch_before_rejection);
    GLIFI_REQUIRE(stats.retired_generation_count == ShardPairRuntime::kMaximumRetiredReadGenerations);
    GLIFI_REQUIRE(stats.generation_admission_backpressure_total == 1U);
    GLIFI_REQUIRE(stats.expired_before_store == 1U);
    GLIFI_REQUIRE(!store.get("async-must-not-enter").has_value());

    const auto* resumed_generation = (*executor)->adopt_read_generation(0);
    GLIFI_REQUIRE(resumed_generation != nullptr);
    GLIFI_REQUIRE(resumed_generation->epoch() > pinned_epoch);
    const auto resumed = submit_and_wait(30'000U, "async-must-not-enter", "after-quiescence");
    GLIFI_REQUIRE(!resumed.error.has_value());
    stats = (*executor)->stats()[0];
    GLIFI_REQUIRE(stats.retired_generation_count < ShardPairRuntime::kMaximumRetiredReadGenerations);
    GLIFI_REQUIRE(store.get("async-must-not-enter").has_value());
    GLIFI_REQUIRE((*executor)->stop_and_drain().has_value());
    GLIFI_REQUIRE(store.close().has_value());
}

GLIFI_TEST("paired Writer feeds one bounded maintenance latency window") {
    ServerTemporaryDirectory temporary;
    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 1},
         .concurrency = glifistore::StoreConcurrencyMode::paired,
         .paired = {.async_lane_capacity = 2,
                    .async_lane_payload_bytes = kTestMutationArenaBytes,
                    .reader_epoch_lease = true},
         .storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = temporary.store_path(),
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .maintenance = {.mode = glifistore::MaintenanceMode::background,
                         .min_eval_interval_ms = 60'000,
                         .max_eval_interval_ms = 60'000,
                         .suspend_on_p99_latency_ms = std::numeric_limits<std::uint32_t>::max()}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    auto* maintenance = glifistore::detail::StoreAccess::maintenance_controller(store);
    GLIFI_REQUIRE(maintenance != nullptr);
    const auto wait_for_idle_cycle_after = [&](const std::uint64_t previous_cycles) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        auto snapshot = store.maintenance_snapshot();
        while ((snapshot.evaluation_cycles <= previous_cycles ||
                snapshot.state != glifistore::MaintenanceState::idle) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
            snapshot = store.maintenance_snapshot();
        }
        return snapshot;
    };
    const auto initial_snapshot = wait_for_idle_cycle_after(0);
    GLIFI_REQUIRE(initial_snapshot.evaluation_cycles > 0);
    GLIFI_REQUIRE(initial_snapshot.state == glifistore::MaintenanceState::idle);

    glifistore::server::BoundedSpscQueue<glifistore::server::MutationCompletion> completions{2};
    auto wakeup = glifistore::server::Wakeup::create();
    GLIFI_REQUIRE(wakeup.has_value());
    auto executor = glifistore::server::PairWriterPool::create(store, 1, 2, kTestMutationArenaBytes,
                                                               std::chrono::milliseconds{0});
    GLIFI_REQUIRE(executor.has_value());
    GLIFI_REQUIRE((*executor)->start().has_value());
    const std::string key{"latency-feedback"};
    GLIFI_REQUIRE((*executor)
                      ->try_submit({
                          .connection = {.slot = 1, .generation = 1},
                          .request_id = 601,
                          .worker_index = 0,
                          .kind = glifistore::server::MutationKind::put,
                          .key = bytes(key),
                          .key_hash = glifistore::hash_key(key),
                          .value = bytes("value"),
                          .completions = &completions,
                          .wakeup = &*wakeup,
                      })
                      .has_value());
    const auto completion_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    std::optional<glifistore::server::MutationCompletion> completion;
    while (!completion && std::chrono::steady_clock::now() < completion_deadline) {
        completion = completions.try_pop();
        if (!completion) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
    GLIFI_REQUIRE(completion.has_value());
    GLIFI_REQUIRE((*executor)->release_payload(0, completion->payload_slot));
    GLIFI_REQUIRE(!completion->error.has_value());

    const auto cycle_before_request = store.maintenance_snapshot().evaluation_cycles;
    maintenance->request_evaluate();
    const auto snapshot = wait_for_idle_cycle_after(cycle_before_request);
    GLIFI_REQUIRE(snapshot.evaluation_cycles > cycle_before_request);
    GLIFI_REQUIRE(snapshot.state == glifistore::MaintenanceState::idle);
    GLIFI_REQUIRE(snapshot.foreground_latency_samples == 1);
    GLIFI_REQUIRE(snapshot.last_foreground_p99_ns >= 1'000'000ULL);

    GLIFI_REQUIRE((*executor)->stop_and_drain().has_value());
    GLIFI_REQUIRE(store.close().has_value());
}

GLIFI_TEST("paired Writer preserves same-shard FIFO while compaction publication is active") {
    ServerTemporaryDirectory temporary;
    BlockingCompactionIntent blocker;
    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 1},
         .concurrency = glifistore::StoreConcurrencyMode::paired,
         .paired = {.async_lane_capacity = 8,
                    .async_lane_payload_bytes = kTestMutationArenaBytes,
                    .reader_epoch_lease = true},
         .storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = temporary.store_path(),
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .filesystem_hooks = {.context = &blocker, .before = &BlockingCompactionIntent::before}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;

    GLIFI_REQUIRE(store.put("retry-seed", bytes("v1")).has_value());
    blocker.force_next_record_write_full();
    GLIFI_REQUIRE(store.put("retry-seed", bytes("v2")).has_value());
    blocker.force_next_record_write_full();
    GLIFI_REQUIRE(store.put("active-seed", bytes("active")).has_value());

    std::optional<glifistore::Result<glifistore::CompactionResult>> compacted;
    std::thread compactor{[&] { compacted = store.compact(); }};
    GLIFI_REQUIRE(blocker.wait_until_blocked());

    glifistore::server::BoundedSpscQueue<glifistore::server::MutationCompletion> completions{8};
    auto wakeup = glifistore::server::Wakeup::create();
    GLIFI_REQUIRE(wakeup.has_value());
    auto executor = glifistore::server::PairWriterPool::create(store, 1, 8, kTestMutationArenaBytes,
                                                               std::chrono::milliseconds{0});
    GLIFI_REQUIRE(executor.has_value());
    GLIFI_REQUIRE((*executor)->start().has_value());

    const auto submit = [&](const std::uint64_t request_id, std::string key, std::string_view value) {
        const auto hash = glifistore::hash_key(key);
        return (*executor)
            ->try_submit({
                .connection = {.slot = static_cast<std::uint32_t>(request_id), .generation = 1},
                .request_id = request_id,
                .worker_index = 0,
                .kind = glifistore::server::MutationKind::put,
                .key = bytes(key),
                .key_hash = hash,
                .value = bytes(value),
                .completions = &completions,
                .wakeup = &*wakeup,
            })
            .has_value();
    };

    const auto baseline_rotations = store.maintenance_snapshot().rotation.attempts;
    blocker.force_next_record_write_full();
    GLIFI_REQUIRE(submit(501, "retry-after-lease", "first"));
    const auto rotation_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (store.maintenance_snapshot().rotation.attempts == baseline_rotations &&
           std::chrono::steady_clock::now() < rotation_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    const bool rotation_waiting = store.maintenance_snapshot().rotation.attempts > baseline_rotations;
    GLIFI_REQUIRE(submit(502, "progress-during-lease", "second"));

    std::vector<glifistore::server::MutationCompletion> observed;
    const auto fifo_probe_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{50};
    while (std::chrono::steady_clock::now() < fifo_probe_deadline) {
        if (auto completion = completions.try_pop()) {
            GLIFI_REQUIRE((*executor)->release_payload(0, completion->payload_slot));
            observed.push_back(std::move(*completion));
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    const bool no_completion_before_release = observed.empty();

    blocker.release();
    compactor.join();
    const auto retry_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (observed.size() != 2 && std::chrono::steady_clock::now() < retry_deadline) {
        if (auto completion = completions.try_pop()) {
            GLIFI_REQUIRE((*executor)->release_payload(0, completion->payload_slot));
            observed.push_back(std::move(*completion));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
    GLIFI_REQUIRE(rotation_waiting);
    GLIFI_REQUIRE(no_completion_before_release);
    GLIFI_REQUIRE(observed.size() == 2);
    GLIFI_REQUIRE(observed[0].request_id == 501);
    GLIFI_REQUIRE(observed[1].request_id == 502);
    GLIFI_REQUIRE(!observed[0].error.has_value());
    GLIFI_REQUIRE(!observed[1].error.has_value());
    const auto stats = (*executor)->stats();
    GLIFI_REQUIRE(stats.size() == 1);
    GLIFI_REQUIRE(compacted.has_value());
    GLIFI_REQUIRE(compacted->has_value());
    GLIFI_REQUIRE(text(store.get("retry-after-lease")->view()) == "first");
    GLIFI_REQUIRE(text(store.get("progress-during-lease")->view()) == "second");

    GLIFI_REQUIRE((*executor)->stop_and_drain().has_value());
    GLIFI_REQUIRE(store.close().has_value());
}

GLIFI_TEST("paired Reader refreshes compacted durable pins and retires the old generation") {
    ServerTemporaryDirectory temporary;
    BlockingCompactionIntent blocker;
    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 1},
         .concurrency = glifistore::StoreConcurrencyMode::paired,
         .paired = {.async_lane_capacity = 8,
                    .async_lane_payload_bytes = kTestMutationArenaBytes,
                    .reader_epoch_lease = true},
         .storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = temporary.store_path(),
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .filesystem_hooks = {.context = &blocker, .before = &BlockingCompactionIntent::before}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;

    // A live anchor in the oldest Segment makes ADR 0044 select both sealed
    // Segments, so the refreshed Record moves to a different Segment.
    GLIFI_REQUIRE(store.put("refresh-key-anchor", bytes("anchor")).has_value());
    GLIFI_REQUIRE(store.put("refresh-key", bytes("old")).has_value());
    blocker.force_next_record_write_full();
    GLIFI_REQUIRE(store.put("refresh-key", bytes("current")).has_value());
    blocker.force_next_record_write_full();
    GLIFI_REQUIRE(store.put("active-key", bytes("active")).has_value());

    auto executor = glifistore::server::PairWriterPool::create(store, 1, 8, kTestMutationArenaBytes,
                                                               std::chrono::milliseconds{0});
    GLIFI_REQUIRE(executor.has_value());
    GLIFI_REQUIRE((*executor)->start().has_value());
    const auto* initial_generation = (*executor)->adopt_read_generation(0);
    GLIFI_REQUIRE(initial_generation != nullptr);
    const std::string key{"refresh-key"};
    const glifistore::HashedKey hashed{key, glifistore::hash_key(key)};
    glifistore::RecordRef initial_reference;
    std::optional<glifistore::detail::StoreAccess::PreparedGet> pending_read;
    {
        auto initial_record = initial_generation->prepare_durable(hashed);
        GLIFI_REQUIRE(initial_record.has_value());
        initial_reference = initial_record->reference();
        auto prepared =
            glifistore::detail::StoreAccess::prepare_published_durable_get(store, 0, *initial_record, 0);
        GLIFI_REQUIRE(prepared.has_value());
        GLIFI_REQUIRE(!prepared->value.has_value());
        GLIFI_REQUIRE(prepared->cold.has_value());
        pending_read.emplace(std::move(*prepared));
    }
    const auto initial_epoch = initial_generation->epoch();
    const auto initial_revision = glifistore::detail::StoreAccess::durable_read_catalog_revision(store, 0);

    std::optional<glifistore::Result<glifistore::CompactionResult>> compacted;
    std::thread compactor{[&] { compacted = store.compact(); }};
    GLIFI_REQUIRE(blocker.wait_until_blocked());
    blocker.release();
    compactor.join();
    GLIFI_REQUIRE(compacted.has_value());
    GLIFI_REQUIRE(compacted->has_value());
    GLIFI_REQUIRE((*compacted)->compacted);
    GLIFI_REQUIRE(glifistore::detail::StoreAccess::durable_read_catalog_revision(store, 0) >
                  initial_revision);

    const auto refresh_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while ((*executor)->stats()[0].read_refresh_successes == 0 &&
           std::chrono::steady_clock::now() < refresh_deadline) {
        (*executor)->request_read_refresh(0);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    auto stats = (*executor)->stats()[0];
    GLIFI_REQUIRE(stats.read_refresh_attempts >= 1);
    GLIFI_REQUIRE(stats.read_refresh_successes == 1);
    GLIFI_REQUIRE(stats.read_refresh_failures == 0);
    GLIFI_REQUIRE(stats.read_catalog_revision > initial_revision);

    // Simulate one asynchronous cold read that still borrows the initial
    // generation. Reader may adopt the new pointer, but Writer must keep the
    // old generation until the explicit minimum lease epoch advances.
    const auto* refreshed_generation = (*executor)->adopt_read_generation(0, initial_epoch);
    GLIFI_REQUIRE(refreshed_generation != nullptr);
    GLIFI_REQUIRE(refreshed_generation->epoch() > initial_epoch);
    auto refreshed_record = refreshed_generation->prepare_durable(hashed);
    GLIFI_REQUIRE(refreshed_record.has_value());
    GLIFI_REQUIRE(refreshed_record->reference().sequence == initial_reference.sequence);
    GLIFI_REQUIRE(refreshed_record->reference().segment_id != initial_reference.segment_id);

    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    stats = (*executor)->stats()[0];
    GLIFI_REQUIRE(stats.reader_safe_epoch == initial_epoch);
    GLIFI_REQUIRE(stats.writer_epoch == refreshed_generation->epoch());
    GLIFI_REQUIRE(stats.retired_generation_count == 1);

    // The cold task borrows both the key and the Segment generation from the
    // retired read generation. Completing it here proves that the advertised
    // safe epoch, rather than a per-request shared_ptr, pins the complete read
    // state across compaction publication and source retirement.
    GLIFI_REQUIRE(pending_read.has_value());
    GLIFI_REQUIRE(pending_read->cold.has_value());
    auto borrowed_value =
        glifistore::detail::StoreAccess::complete_get_owned(store, 0, std::move(*pending_read->cold));
    GLIFI_REQUIRE(borrowed_value.has_value());
    GLIFI_REQUIRE(text(borrowed_value->view()) == "current");

    const auto* released_generation = (*executor)->adopt_read_generation(0);
    GLIFI_REQUIRE(released_generation == refreshed_generation);

    const auto reclaim_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    do {
        stats = (*executor)->stats()[0];
        if (stats.retired_generation_count == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    } while (std::chrono::steady_clock::now() < reclaim_deadline);
    GLIFI_REQUIRE(stats.generations_retired >= 1);
    GLIFI_REQUIRE(stats.retired_generation_count == 0);

    GLIFI_REQUIRE((*executor)->stop_and_drain().has_value());
    GLIFI_REQUIRE(store.close().has_value());
}

GLIFI_TEST("ADR 0036 V8 candidate preserves durable cold pin across compacted slot refresh") {
    using Generation = glifistore::server::PairReadGeneration;
    using Pool = glifistore::experimental::GenerationSlotPool<Generation, 4>;

    ServerTemporaryDirectory temporary;
    BlockingCompactionIntent blocker;
    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 1},
         .concurrency = glifistore::StoreConcurrencyMode::paired,
         .paired = {.async_lane_capacity = 8,
                    .async_lane_payload_bytes = kTestMutationArenaBytes,
                    .reader_epoch_lease = true},
         .storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = temporary.store_path(),
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .filesystem_hooks = {.context = &blocker, .before = &BlockingCompactionIntent::before}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;

    // A live anchor in the oldest Segment makes ADR 0044 select both sealed
    // Segments, so the refreshed Record moves to a different Segment.
    GLIFI_REQUIRE(store.put("slot-refresh-anchor", bytes("anchor")).has_value());
    GLIFI_REQUIRE(store.put("slot-refresh", bytes("old")).has_value());
    blocker.force_next_record_write_full();
    GLIFI_REQUIRE(store.put("slot-refresh", bytes("current")).has_value());
    blocker.force_next_record_write_full();
    GLIFI_REQUIRE(store.put("slot-active", bytes("active")).has_value());

    auto initial_snapshot = glifistore::detail::StoreAccess::snapshot_durable_reads(store, 0);
    GLIFI_REQUIRE(initial_snapshot.has_value());
    auto initial_result = Generation::from_durable_snapshot(
        glifistore::detail::StoreAccess::worker_routing(store), initial_snapshot->records);
    GLIFI_REQUIRE(initial_result.has_value());
    auto initial = std::move(*initial_result);
    auto replacement_parent = initial;
    std::weak_ptr<const Generation> initial_lifetime = initial;
    auto pool = Pool::create(std::move(initial));
    GLIFI_REQUIRE(pool.has_value());

    const auto* adopted_initial = (*pool)->adopt();
    GLIFI_REQUIRE(adopted_initial != nullptr);
    const auto initial_epoch = adopted_initial->epoch();
    const std::string key{"slot-refresh"};
    const glifistore::HashedKey hashed{key, glifistore::hash_key(key)};
    auto initial_record = adopted_initial->prepare_durable(hashed);
    GLIFI_REQUIRE(initial_record.has_value());
    const auto initial_reference = initial_record->reference();
    auto pending =
        glifistore::detail::StoreAccess::prepare_published_durable_get(store, 0, *initial_record, 0);
    GLIFI_REQUIRE(pending.has_value());
    GLIFI_REQUIRE(!pending->value.has_value());
    GLIFI_REQUIRE(pending->cold.has_value());

    std::optional<glifistore::Result<glifistore::CompactionResult>> compacted;
    std::thread compactor{[&] { compacted = store.compact(); }};
    GLIFI_REQUIRE(blocker.wait_until_blocked());
    blocker.release();
    compactor.join();
    GLIFI_REQUIRE(compacted.has_value());
    GLIFI_REQUIRE(compacted->has_value());
    GLIFI_REQUIRE((*compacted)->compacted);

    auto refreshed_snapshot = glifistore::detail::StoreAccess::snapshot_durable_reads(store, 0);
    GLIFI_REQUIRE(refreshed_snapshot.has_value());
    auto refreshed_result =
        Generation::replace_durable_snapshot(replacement_parent, refreshed_snapshot->records);
    GLIFI_REQUIRE(refreshed_result.has_value());
    replacement_parent.reset();
    initial_snapshot->records.clear();
    GLIFI_REQUIRE((*pool)->try_publish(std::move(*refreshed_result)) ==
                  glifistore::experimental::GenerationSlotPublishStatus::published);

    const auto* refreshed = (*pool)->adopt(initial_epoch);
    GLIFI_REQUIRE(refreshed != nullptr);
    GLIFI_REQUIRE(refreshed->epoch() > initial_epoch);
    auto refreshed_record = refreshed->prepare_durable(hashed);
    GLIFI_REQUIRE(refreshed_record.has_value());
    GLIFI_REQUIRE(refreshed_record->reference().sequence == initial_reference.sequence);
    GLIFI_REQUIRE(refreshed_record->reference().segment_id != initial_reference.segment_id);
    (*pool)->reclaim();
    GLIFI_REQUIRE(!initial_lifetime.expired());
    GLIFI_REQUIRE((*pool)->stats().reader_safe_epoch == initial_epoch);

    auto value = glifistore::detail::StoreAccess::complete_get_owned(store, 0, std::move(*pending->cold));
    GLIFI_REQUIRE(value.has_value());
    GLIFI_REQUIRE(text(value->view()) == "current");
    GLIFI_REQUIRE((*pool)->adopt() == refreshed);
    (*pool)->reclaim();
    GLIFI_REQUIRE(initial_lifetime.expired());
    GLIFI_REQUIRE((*pool)->stats().live_slots == 1);

    GLIFI_REQUIRE(store.close().has_value());
}

GLIFI_TEST("ADR 0036 V8 candidate publishes a Writer-owned rotation as one slot generation") {
    using Generation = glifistore::server::PairReadGeneration;
    using Pool = glifistore::experimental::GenerationSlotPool<Generation, 4>;

    ServerTemporaryDirectory temporary;
    BlockingCompactionIntent blocker;
    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 1},
         .concurrency = glifistore::StoreConcurrencyMode::paired,
         .paired = {.async_lane_capacity = 4,
                    .async_lane_payload_bytes = kTestMutationArenaBytes,
                    .reader_epoch_lease = true},
         .storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = temporary.store_path(),
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .filesystem_hooks = {.context = &blocker, .before = &BlockingCompactionIntent::before}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(store.put("slot-rotation-base", bytes("base")).has_value());

    auto initial_snapshot = glifistore::detail::StoreAccess::snapshot_durable_reads(store, 0);
    GLIFI_REQUIRE(initial_snapshot.has_value());
    auto initial_result = Generation::from_durable_snapshot(
        glifistore::detail::StoreAccess::worker_routing(store), initial_snapshot->records);
    GLIFI_REQUIRE(initial_result.has_value());
    auto initial = std::move(*initial_result);
    auto replacement_parent = initial;
    std::weak_ptr<const Generation> initial_lifetime = initial;
    auto pool = Pool::create(std::move(initial));
    GLIFI_REQUIRE(pool.has_value());
    const auto* initial_read = (*pool)->adopt();
    GLIFI_REQUIRE(initial_read != nullptr);
    const auto initial_epoch = initial_read->epoch();

    blocker.force_next_record_write_full();
    GLIFI_REQUIRE(store.put("slot-rotation-new", bytes("rotated")).has_value());
    auto rotated_snapshot = glifistore::detail::StoreAccess::snapshot_durable_reads(store, 0);
    GLIFI_REQUIRE(rotated_snapshot.has_value());
    auto rotated_result = Generation::replace_durable_snapshot(replacement_parent, rotated_snapshot->records);
    GLIFI_REQUIRE(rotated_result.has_value());
    replacement_parent.reset();
    initial_snapshot->records.clear();
    GLIFI_REQUIRE((*pool)->try_publish(std::move(*rotated_result)) ==
                  glifistore::experimental::GenerationSlotPublishStatus::published);

    const auto* rotated = (*pool)->adopt(initial_epoch);
    GLIFI_REQUIRE(rotated != nullptr);
    GLIFI_REQUIRE(rotated->epoch() == initial_epoch + 1U);
    GLIFI_REQUIRE(rotated->delta_entries() == 0);
    GLIFI_REQUIRE(rotated->base_entries() == 2);
    const glifistore::HashedKey base{"slot-rotation-base", glifistore::hash_key("slot-rotation-base")};
    const glifistore::HashedKey added{"slot-rotation-new", glifistore::hash_key("slot-rotation-new")};
    GLIFI_REQUIRE(rotated->prepare_durable(base).has_value());
    GLIFI_REQUIRE(rotated->prepare_durable(added).has_value());
    (*pool)->reclaim();
    GLIFI_REQUIRE(!initial_lifetime.expired());

    GLIFI_REQUIRE((*pool)->adopt() == rotated);
    (*pool)->reclaim();
    GLIFI_REQUIRE(initial_lifetime.expired());
    GLIFI_REQUIRE((*pool)->stats().live_slots == 1);
    GLIFI_REQUIRE(store.close().has_value());
}

GLIFI_TEST("ADR 0036 V5 candidate shutdown retires a real durable generation after Reader drain") {
    using Generation = glifistore::server::PairReadGeneration;
    using Pool = glifistore::experimental::GenerationSlotPool<Generation, 4>;

    ServerTemporaryDirectory temporary;
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                           .concurrency = glifistore::StoreConcurrencyMode::paired,
                                           .paired = {.async_lane_capacity = 4,
                                                      .async_lane_payload_bytes = kTestMutationArenaBytes,
                                                      .reader_epoch_lease = true},
                                           .storage_mode = glifistore::StorageMode::durable_sync,
                                           .data_directory = temporary.store_path(),
                                           .durable_open_mode = glifistore::DurableOpenMode::create_new});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(store.put("slot-v5-initial", bytes("initial")).has_value());

    auto initial_snapshot = glifistore::detail::StoreAccess::snapshot_durable_reads(store, 0);
    GLIFI_REQUIRE(initial_snapshot.has_value());
    auto initial_result = Generation::from_durable_snapshot(
        glifistore::detail::StoreAccess::worker_routing(store), initial_snapshot->records);
    GLIFI_REQUIRE(initial_result.has_value());
    auto initial = std::move(*initial_result);
    auto replacement_parent = initial;
    std::weak_ptr<const Generation> initial_lifetime = initial;
    auto pool = Pool::create(std::move(initial));
    GLIFI_REQUIRE(pool.has_value());
    const auto* adopted_initial = (*pool)->adopt();
    GLIFI_REQUIRE(adopted_initial != nullptr);
    const auto borrowed_epoch = adopted_initial->epoch();

    GLIFI_REQUIRE(store.put("slot-v5-final", bytes("final")).has_value());
    auto final_snapshot = glifistore::detail::StoreAccess::snapshot_durable_reads(store, 0);
    GLIFI_REQUIRE(final_snapshot.has_value());
    auto final_result = Generation::replace_durable_snapshot(replacement_parent, final_snapshot->records);
    GLIFI_REQUIRE(final_result.has_value());
    replacement_parent.reset();
    initial_snapshot->records.clear();
    GLIFI_REQUIRE((*pool)->try_publish(std::move(*final_result)) ==
                  glifistore::experimental::GenerationSlotPublishStatus::published);
    GLIFI_REQUIRE((*pool)->adopt(borrowed_epoch) != nullptr);

    (*pool)->stop_admission();
    (*pool)->reclaim();
    GLIFI_REQUIRE(!initial_lifetime.expired());
    GLIFI_REQUIRE(!(*pool)->try_finish_shutdown());

    // The owner completes all output/cold borrows before this terminal edge.
    GLIFI_REQUIRE((*pool)->mark_reader_quiescent());
    GLIFI_REQUIRE((*pool)->try_finish_shutdown());
    GLIFI_REQUIRE(initial_lifetime.expired());
    GLIFI_REQUIRE((*pool)->stats().live_slots == 1);
    GLIFI_REQUIRE(store.close().has_value());
}

GLIFI_TEST("ADR 0036 V6 candidate fail-closes a committed mutation then snapshot-drains authority") {
    using Generation = glifistore::server::PairReadGeneration;
    using Pool = glifistore::experimental::GenerationSlotPool<Generation, 4>;

    ServerTemporaryDirectory temporary;
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                           .concurrency = glifistore::StoreConcurrencyMode::paired,
                                           .paired = {.async_lane_capacity = 4,
                                                      .async_lane_payload_bytes = kTestMutationArenaBytes,
                                                      .reader_epoch_lease = true},
                                           .storage_mode = glifistore::StorageMode::durable_sync,
                                           .data_directory = temporary.store_path(),
                                           .durable_open_mode = glifistore::DurableOpenMode::create_new});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(store.put("slot-v6-seed", bytes("seed")).has_value());

    auto initial_snapshot = glifistore::detail::StoreAccess::snapshot_durable_reads(store, 0);
    GLIFI_REQUIRE(initial_snapshot.has_value());
    auto initial_result = Generation::from_durable_snapshot(
        glifistore::detail::StoreAccess::worker_routing(store), initial_snapshot->records);
    GLIFI_REQUIRE(initial_result.has_value());
    auto initial = std::move(*initial_result);
    auto replacement_parent = initial;
    auto pool =
        Pool::create(std::move(initial), {.context = &store, .fail_closed = [](void* context) noexcept {
                                              glifistore::detail::StoreAccess::mark_fail_closed(
                                                  *static_cast<glifistore::Store*>(context));
                                          }});
    GLIFI_REQUIRE(pool.has_value());
    GLIFI_REQUIRE((*pool)->adopt() != nullptr);

    {
        auto reservation = (*pool)->try_reserve();
        GLIFI_REQUIRE(reservation.has_value());
        // This is the candidate ordering: capacity first, then Store entry.
        GLIFI_REQUIRE(store.put("slot-v6-committed", bytes("authority")).has_value());
        reservation->mark_store_linearized();
        // Deterministically model generation construction/publication failure.
        GLIFI_REQUIRE((*pool)->commit(*reservation, {}) ==
                      glifistore::experimental::GenerationSlotPublishStatus::invalid_generation);
    }
    GLIFI_REQUIRE(!glifistore::detail::StoreAccess::operational(store));
    GLIFI_REQUIRE((*pool)->stats().unpublished_linearizations == 1);
    GLIFI_REQUIRE((*pool)->stats().reserved_slots == 0);

    // Same recovery authority used by the production fail-closed epilogue:
    // snapshot is explicitly allowed after the durable catalog becomes sticky.
    auto drain = glifistore::detail::StoreAccess::snapshot_durable_reads(store, 0, true);
    GLIFI_REQUIRE(drain.has_value());
    auto drained_generation = Generation::replace_durable_snapshot(replacement_parent, drain->records);
    GLIFI_REQUIRE(drained_generation.has_value());
    replacement_parent.reset();
    initial_snapshot->records.clear();
    GLIFI_REQUIRE((*pool)->try_publish(std::move(*drained_generation)) ==
                  glifistore::experimental::GenerationSlotPublishStatus::published);
    const auto* adopted = (*pool)->adopt();
    GLIFI_REQUIRE(adopted != nullptr);
    GLIFI_REQUIRE(
        adopted->prepare_durable({.key = "slot-v6-seed", .hash = glifistore::hash_key("slot-v6-seed")})
            .has_value());
    GLIFI_REQUIRE(
        adopted
            ->prepare_durable({.key = "slot-v6-committed", .hash = glifistore::hash_key("slot-v6-committed")})
            .has_value());
    GLIFI_REQUIRE(adopted->base_entries() == 2);
    GLIFI_REQUIRE((*pool)->stats().publications == 1);

    GLIFI_REQUIRE(store.close().has_value());
}

GLIFI_TEST("paired Reader refreshes durable pins after a Writer-owned rotation") {
    ServerTemporaryDirectory temporary;
    BlockingCompactionIntent blocker;
    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 1},
         .concurrency = glifistore::StoreConcurrencyMode::paired,
         .paired = {.async_lane_capacity = 4,
                    .async_lane_payload_bytes = kTestMutationArenaBytes,
                    .reader_epoch_lease = true},
         .storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = temporary.store_path(),
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .filesystem_hooks = {.context = &blocker, .before = &BlockingCompactionIntent::before}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(store.put("rotation-base", bytes("base")).has_value());

    glifistore::server::BoundedSpscQueue<glifistore::server::MutationCompletion> completions{4};
    auto wakeup = glifistore::server::Wakeup::create();
    GLIFI_REQUIRE(wakeup.has_value());
    auto executor = glifistore::server::PairWriterPool::create(store, 1, 4, kTestMutationArenaBytes,
                                                               std::chrono::milliseconds{0});
    GLIFI_REQUIRE(executor.has_value());
    GLIFI_REQUIRE((*executor)->start().has_value());
    const auto* initial_generation = (*executor)->adopt_read_generation(0);
    GLIFI_REQUIRE(initial_generation != nullptr);
    const auto initial_epoch = initial_generation->epoch();
    const auto initial_revision = glifistore::detail::StoreAccess::durable_read_catalog_revision(store, 0);

    blocker.force_next_record_write_full();
    const std::string key{"rotation-published"};
    GLIFI_REQUIRE((*executor)
                      ->try_submit({
                          .connection = {.slot = 1, .generation = 1},
                          .request_id = 701,
                          .worker_index = 0,
                          .kind = glifistore::server::MutationKind::put,
                          .key = bytes(key),
                          .key_hash = glifistore::hash_key(key),
                          .value = bytes("rotated"),
                          .completions = &completions,
                          .wakeup = &*wakeup,
                      })
                      .has_value());
    const auto completion_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    std::optional<glifistore::server::MutationCompletion> completion;
    while (!completion && std::chrono::steady_clock::now() < completion_deadline) {
        completion = completions.try_pop();
        if (!completion) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
    GLIFI_REQUIRE(completion.has_value());
    GLIFI_REQUIRE((*executor)->release_payload(0, completion->payload_slot));
    GLIFI_REQUIRE(!completion->error.has_value());
    GLIFI_REQUIRE(glifistore::detail::StoreAccess::durable_read_catalog_revision(store, 0) >
                  initial_revision);

    const auto refresh_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while ((*executor)->stats()[0].read_refresh_successes == 0 &&
           std::chrono::steady_clock::now() < refresh_deadline) {
        (*executor)->request_read_refresh(0);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    const auto* refreshed_generation = (*executor)->adopt_read_generation(0);
    GLIFI_REQUIRE(refreshed_generation != nullptr);
    GLIFI_REQUIRE(refreshed_generation->epoch() >= initial_epoch + 2U);
    auto published = refreshed_generation->prepare_durable({.key = key, .hash = glifistore::hash_key(key)});
    GLIFI_REQUIRE(published.has_value());
    auto sealed = refreshed_generation->prepare_durable(
        {.key = "rotation-base", .hash = glifistore::hash_key("rotation-base")});
    GLIFI_REQUIRE(sealed.has_value());
    GLIFI_REQUIRE(refreshed_generation->delta_entries() == 0);
    GLIFI_REQUIRE(refreshed_generation->base_entries() == 2);
    const auto stats = (*executor)->stats()[0];
    GLIFI_REQUIRE(stats.read_refresh_successes == 1);
    GLIFI_REQUIRE(stats.read_refresh_failures == 0);

    GLIFI_REQUIRE((*executor)->stop_and_drain().has_value());
    GLIFI_REQUIRE(store.close().has_value());
}

GLIFI_TEST("durable read catalog refresh is isolated to the compacted shard pair") {
    ServerTemporaryDirectory temporary;
    BlockingCompactionIntent blocker;
    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 2},
         .concurrency = glifistore::StoreConcurrencyMode::paired,
         .paired = {.async_lane_capacity = 8,
                    .async_lane_payload_bytes = kTestMutationArenaBytes,
                    .reader_epoch_lease = true},
         .storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = temporary.store_path(),
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .filesystem_hooks = {.context = &blocker, .before = &BlockingCompactionIntent::before}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    const auto key_for = [](const std::string_view prefix, const std::size_t worker) {
        for (std::size_t suffix = 0; suffix < 10'000; ++suffix) {
            auto key = std::string{prefix} + std::to_string(suffix);
            if (glifistore::route_worker(key, 2) == worker) {
                return key;
            }
        }
        return std::string{};
    };
    const auto compacted_key = key_for("isolated-compact-", 0);
    const auto active_key = key_for("isolated-active-", 0);
    const auto other_key = key_for("isolated-other-", 1);
    GLIFI_REQUIRE(!compacted_key.empty());
    GLIFI_REQUIRE(!active_key.empty());
    GLIFI_REQUIRE(!other_key.empty());

    GLIFI_REQUIRE(store.put(compacted_key, bytes("v1")).has_value());
    blocker.force_next_record_write_full();
    GLIFI_REQUIRE(store.put(compacted_key, bytes("v2")).has_value());
    blocker.force_next_record_write_full();
    GLIFI_REQUIRE(store.put(active_key, bytes("active")).has_value());
    GLIFI_REQUIRE(store.put(other_key, bytes("other")).has_value());

    auto executor = glifistore::server::PairWriterPool::create(store, 2, 8, kTestMutationArenaBytes,
                                                               std::chrono::milliseconds{0});
    GLIFI_REQUIRE(executor.has_value());
    GLIFI_REQUIRE((*executor)->start().has_value());
    const auto worker_zero_revision =
        glifistore::detail::StoreAccess::durable_read_catalog_revision(store, 0);
    const auto worker_one_revision = glifistore::detail::StoreAccess::durable_read_catalog_revision(store, 1);

    std::optional<glifistore::Result<glifistore::CompactionResult>> compacted;
    std::thread compactor{[&] { compacted = store.compact(); }};
    GLIFI_REQUIRE(blocker.wait_until_blocked());
    blocker.release();
    compactor.join();
    GLIFI_REQUIRE(compacted.has_value());
    GLIFI_REQUIRE(compacted->has_value());
    GLIFI_REQUIRE((*compacted)->compacted);
    GLIFI_REQUIRE((*compacted)->worker_index == 0);
    GLIFI_REQUIRE(glifistore::detail::StoreAccess::durable_read_catalog_revision(store, 0) >
                  worker_zero_revision);
    GLIFI_REQUIRE(glifistore::detail::StoreAccess::durable_read_catalog_revision(store, 1) ==
                  worker_one_revision);

    const auto refresh_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    std::vector<glifistore::server::PairWriterStats> stats;
    do {
        (*executor)->request_read_refresh(0);
        (*executor)->request_read_refresh(1);
        stats = (*executor)->stats();
        if (stats[0].read_refresh_successes != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    } while (std::chrono::steady_clock::now() < refresh_deadline);
    GLIFI_REQUIRE(stats.size() == 2);
    GLIFI_REQUIRE(stats[0].read_refresh_successes == 1);
    GLIFI_REQUIRE(stats[1].read_refresh_attempts == 0);
    GLIFI_REQUIRE(stats[1].read_refresh_successes == 0);

    GLIFI_REQUIRE((*executor)->stop_and_drain().has_value());
    GLIFI_REQUIRE(store.close().has_value());
}
