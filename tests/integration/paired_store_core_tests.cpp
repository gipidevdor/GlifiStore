#include "glifistore/core/fault_injection.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/store/paired/publication_coordinator.hpp"
#include "glifistore/store/store.hpp"
#include "paired_reader_quiescence.hpp"
#include "store/store_internal.hpp"
#include "test.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

auto bytes(const std::string_view value) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

} // namespace

GLIFI_TEST("paired Store read-after-write and close drain") {
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 2}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(store.put("alpha", bytes("one")).has_value());
    const auto first = store.get("alpha");
    GLIFI_REQUIRE(first.has_value());
    GLIFI_REQUIRE(std::string_view(reinterpret_cast<const char*>(first->bytes.data()), first->bytes.size()) ==
                  "one");
    GLIFI_REQUIRE(store.put("alpha", bytes("two")).has_value());
    const auto second = store.get("alpha");
    GLIFI_REQUIRE(second.has_value());
    GLIFI_REQUIRE(
        std::string_view(reinterpret_cast<const char*>(second->bytes.data()), second->bytes.size()) == "two");
    GLIFI_REQUIRE(store.erase("alpha").has_value());
    GLIFI_REQUIRE(!store.get("alpha").has_value());
    GLIFI_REQUIRE(store.close().has_value());
    GLIFI_REQUIRE(!store.put("alpha", bytes("late")).has_value());
}

GLIFI_TEST("paired Store concurrent GET and PUT on one key stay linearized") {
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(store.put("shared", bytes("0")).has_value());

    std::atomic_bool stop{false};
    glifistore::test::PairedReaderQuiescence quiescence;
    std::atomic_int reader_failure{};
    std::atomic_int writer_failure{};
    std::atomic_uint64_t writes{0};
    std::thread writer([&] {
        for (std::uint64_t value = 1; value <= 200; ++value) {
            const auto encoded = std::to_string(value);
            auto put = store.put("shared", bytes(encoded));
            if (!put && put.error().code == glifistore::ErrorCode::resource_exhausted) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
                if (!quiescence.request_until(deadline)) {
                    writer_failure.store(-1, std::memory_order_relaxed);
                    break;
                }
                put = store.put("shared", bytes(encoded));
                quiescence.release();
            }
            if (!put) {
                writer_failure.store(static_cast<int>(put.error().code) + 1, std::memory_order_relaxed);
                break;
            }
            writes.fetch_add(1, std::memory_order_relaxed);
        }
        stop.store(true, std::memory_order_release);
    });
    std::thread reader([&] {
        while (!stop.load(std::memory_order_acquire)) {
            quiescence.reader_checkpoint(stop);
            auto value = store.get("shared");
            if (!value.has_value()) {
                reader_failure.store(static_cast<int>(value.error().code) + 1, std::memory_order_relaxed);
                return;
            }
            std::this_thread::yield();
        }
    });
    writer.join();
    reader.join();
    GLIFI_REQUIRE(reader_failure.load(std::memory_order_relaxed) == 0);
    GLIFI_REQUIRE(writer_failure.load(std::memory_order_relaxed) == 0);
    GLIFI_REQUIRE(writes.load() == 200);
    const auto final_value = store.get("shared");
    GLIFI_REQUIRE(final_value.has_value());
    GLIFI_REQUIRE(std::string_view(reinterpret_cast<const char*>(final_value->bytes.data()),
                                   final_value->bytes.size()) == "200");
    GLIFI_REQUIRE(store.close().has_value());
}

GLIFI_TEST("paired Store concurrent read-after-write keeps adopted generations alive") {
    constexpr std::size_t kThreadCount = 4;
    constexpr std::size_t kWritesPerThread = 1'024;
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 4}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;

    std::atomic_bool failed{false};
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);
    for (std::size_t thread = 0; thread < kThreadCount; ++thread) {
        threads.emplace_back([&, thread] {
            for (std::size_t write = 0; write < kWritesPerThread; ++write) {
                const auto key = "lease-" + std::to_string(thread) + '-' + std::to_string(write);
                const auto value = "value-" + std::to_string(write);
                if (!store.put(key, bytes(value)).has_value() || !store.get(key).has_value()) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    GLIFI_REQUIRE(!failed.load(std::memory_order_relaxed));
    GLIFI_REQUIRE(store.close().has_value());
}

GLIFI_TEST("paired durable group threshold requires final commit-slot synchronization") {
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-paired-group-sync-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    GLIFI_REQUIRE(::mkdtemp(writable.data()) != nullptr);
    const std::filesystem::path root{writable.data()};

    struct FailFirstCommitSlotSync final {
        bool fired{};

        static auto before(void* context, const glifistore::FilesystemOperation operation)
            -> glifistore::Status {
            auto& self = *static_cast<FailFirstCommitSlotSync*>(context);
            if (operation == glifistore::FilesystemOperation::sync_commit_slot && !self.fired) {
                self.fired = true;
                return glifistore::fail(glifistore::ErrorCode::io_error,
                                        "injected strict group commit-slot sync failure");
            }
            return {};
        }
    } failure;

    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 1},
         .concurrency = glifistore::StoreConcurrencyMode::paired,
         .paired = {.async_lane_capacity = 8,
                    .async_lane_payload_bytes = 1U * 1024U * 1024U,
                    .reader_epoch_lease = true},
         .storage_mode = glifistore::StorageMode::durable_group,
         .data_directory = root / "store",
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .durable_group = {.max_records = 2, .max_bytes = 65'536, .max_wait_ms = 60'000, .min_records = 2},
         .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
         .filesystem_hooks = {.context = &failure, .before = &FailFirstCommitSlotSync::before}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;

    const std::array items{
        glifistore::Store::PutItem{.key = "strict-a", .value = bytes("alpha")},
        glifistore::Store::PutItem{.key = "strict-b", .value = bytes("beta")},
    };
    const auto statuses = store.put_batch(items);
    GLIFI_REQUIRE(failure.fired);
    GLIFI_REQUIRE(statuses.size() == items.size());
    GLIFI_REQUIRE(!statuses[0].has_value());
    GLIFI_REQUIRE(!statuses[1].has_value());
    GLIFI_REQUIRE(statuses[0].error().code == glifistore::ErrorCode::unavailable);
    GLIFI_REQUIRE(statuses[1].error().code == glifistore::ErrorCode::unavailable);

    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    GLIFI_REQUIRE(runtime != nullptr);
    GLIFI_REQUIRE(!runtime->healthy());
    static_cast<void>(store.close());

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

GLIFI_TEST("ADR 0036 V5 production shutdown finalization rejects a live Reader lease") {
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    GLIFI_REQUIRE(runtime != nullptr);
    GLIFI_REQUIRE(!runtime->finalize_reader_shutdown().has_value());

    {
        glifistore::store::paired::ShardPairRuntime::ReadLease lease{*runtime, 0};
        GLIFI_REQUIRE(static_cast<bool>(lease));
        GLIFI_REQUIRE(store.put("shutdown-final-generation", bytes("value")).has_value());
        GLIFI_REQUIRE(runtime->stats()[0].retired_generation_count >= 1);
        GLIFI_REQUIRE(runtime->stop_and_drain().has_value());

        const auto blocked = runtime->finalize_reader_shutdown();
        GLIFI_REQUIRE(!blocked.has_value());
        GLIFI_REQUIRE(blocked.error().code == glifistore::ErrorCode::unavailable);
        GLIFI_REQUIRE(runtime->adopt_read_generation(0) != nullptr);
        GLIFI_REQUIRE(!runtime->stats()[0].reader_shutdown_finalized);
    }

    std::atomic_bool first_finalized{};
    std::atomic_bool second_finalized{};
    std::thread first_finalizer{[&] {
        first_finalized.store(runtime->finalize_reader_shutdown().has_value(), std::memory_order_release);
    }};
    std::thread second_finalizer{[&] {
        second_finalized.store(runtime->finalize_reader_shutdown().has_value(), std::memory_order_release);
    }};
    first_finalizer.join();
    second_finalizer.join();
    GLIFI_REQUIRE(first_finalized.load(std::memory_order_acquire));
    GLIFI_REQUIRE(second_finalized.load(std::memory_order_acquire));
    GLIFI_REQUIRE(runtime->finalize_reader_shutdown().has_value());
    GLIFI_REQUIRE(runtime->adopt_read_generation(0) == nullptr);
    const auto finalized = runtime->stats()[0];
    GLIFI_REQUIRE(finalized.reader_shutdown_finalized);
    GLIFI_REQUIRE(finalized.retired_generation_count == 0);
    GLIFI_REQUIRE(finalized.shutdown_generations_reclaimed >= 2);
    GLIFI_REQUIRE(finalized.reader_safe_epoch > finalized.writer_epoch);
    GLIFI_REQUIRE(store.close().has_value());
}

GLIFI_TEST("ADR 0036 V5 Store close owns terminal Reader generation finalization") {
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    GLIFI_REQUIRE(runtime != nullptr);
    GLIFI_REQUIRE(store.put("close-finalization", bytes("value")).has_value());
    GLIFI_REQUIRE(runtime->adopt_read_generation(0) != nullptr);

    GLIFI_REQUIRE(store.close().has_value());
    GLIFI_REQUIRE(runtime->adopt_read_generation(0) == nullptr);
    const auto stats = runtime->stats()[0];
    GLIFI_REQUIRE(stats.reader_shutdown_finalized);
    GLIFI_REQUIRE(stats.retired_generation_count == 0);
    GLIFI_REQUIRE(stats.shutdown_generations_reclaimed >= 1);
    GLIFI_REQUIRE(stats.reader_safe_epoch > stats.writer_epoch);
}

GLIFI_TEST("paired generation admission rejects embedded sync before Store at retire bound") {
    using glifistore::store::paired::decide_generation_admission;
    using glifistore::store::paired::GenerationAdmissionDecision;
    using glifistore::store::paired::ShardPairRuntime;

    GLIFI_REQUIRE(decide_generation_admission(ShardPairRuntime::kMaximumRetiredReadGenerations - 1U,
                                              ShardPairRuntime::kMaximumRetiredReadGenerations,
                                              true) == GenerationAdmissionDecision::admitted);
    GLIFI_REQUIRE(decide_generation_admission(ShardPairRuntime::kMaximumRetiredReadGenerations,
                                              ShardPairRuntime::kMaximumRetiredReadGenerations, true) ==
                  GenerationAdmissionDecision::reader_quiescence_required);
    GLIFI_REQUIRE(decide_generation_admission(0U, ShardPairRuntime::kMaximumRetiredReadGenerations, false) ==
                  GenerationAdmissionDecision::incremental_merge_required);

    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    GLIFI_REQUIRE(runtime != nullptr);

    {
        ShardPairRuntime::ReadLease pinned_reader{*runtime, 0};
        GLIFI_REQUIRE(static_cast<bool>(pinned_reader));
        for (std::size_t publication = 0; publication < ShardPairRuntime::kMaximumRetiredReadGenerations;
             ++publication) {
            const auto value = "generation-" + std::to_string(publication);
            GLIFI_REQUIRE(store.put("retire-pressure", bytes(value)).has_value());
        }
        GLIFI_REQUIRE(runtime->stats()[0].retired_generation_count ==
                      ShardPairRuntime::kMaximumRetiredReadGenerations);

        const auto blocked = store.put("must-not-enter-store", bytes("blocked"));
        GLIFI_REQUIRE(!blocked.has_value());
        GLIFI_REQUIRE(blocked.error().code == glifistore::ErrorCode::resource_exhausted);
        GLIFI_REQUIRE(blocked.error().message == "mutation rejected until paired Reader reaches quiescence");
        GLIFI_REQUIRE(runtime->stats()[0].retired_generation_count ==
                      ShardPairRuntime::kMaximumRetiredReadGenerations);
        GLIFI_REQUIRE(runtime->stats()[0].generation_admission_backpressure_total == 1U);
        const auto absent = store.get("must-not-enter-store");
        GLIFI_REQUIRE(!absent.has_value());
        GLIFI_REQUIRE(absent.error().code == glifistore::ErrorCode::not_found);
    }

    GLIFI_REQUIRE(store.put("must-not-enter-store", bytes("after-quiescence")).has_value());
    GLIFI_REQUIRE(store.get("must-not-enter-store").has_value());
    GLIFI_REQUIRE(runtime->stats()[0].retired_generation_count <
                  ShardPairRuntime::kMaximumRetiredReadGenerations);
    GLIFI_REQUIRE(store.close().has_value());
}

GLIFI_TEST("paired generation admission rejects dedicated Writer sync before Store at retire bound") {
    using glifistore::store::paired::ShardPairRuntime;

    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 1},
         .paired = {.async_lane_capacity = 8, .async_lane_payload_bytes = 64U * 1024U}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    GLIFI_REQUIRE(runtime != nullptr);

    {
        ShardPairRuntime::ReadLease pinned_reader{*runtime, 0};
        GLIFI_REQUIRE(static_cast<bool>(pinned_reader));
        for (std::size_t publication = 0; publication < ShardPairRuntime::kMaximumRetiredReadGenerations;
             ++publication) {
            const auto value = "dedicated-generation-" + std::to_string(publication);
            GLIFI_REQUIRE(store.put("dedicated-retire-pressure", bytes(value)).has_value());
        }
        const auto blocked = store.put("dedicated-must-not-enter", bytes("blocked"));
        GLIFI_REQUIRE(!blocked.has_value());
        GLIFI_REQUIRE(blocked.error().code == glifistore::ErrorCode::resource_exhausted);
        GLIFI_REQUIRE(runtime->stats()[0].retired_generation_count ==
                      ShardPairRuntime::kMaximumRetiredReadGenerations);
        GLIFI_REQUIRE(runtime->stats()[0].generation_admission_backpressure_total == 1U);
        GLIFI_REQUIRE(!store.get("dedicated-must-not-enter").has_value());
    }

    GLIFI_REQUIRE(store.put("dedicated-must-not-enter", bytes("after-quiescence")).has_value());
    GLIFI_REQUIRE(store.get("dedicated-must-not-enter").has_value());
    GLIFI_REQUIRE(store.close().has_value());
}

GLIFI_TEST("paired generation admission performs no durable write at retire bound") {
    using glifistore::store::paired::ShardPairRuntime;

    auto pattern =
        (std::filesystem::temp_directory_path() / "glifistore-generation-admission-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    GLIFI_REQUIRE(::mkdtemp(writable.data()) != nullptr);
    const std::filesystem::path root{writable.data()};

    struct WriteCounter final {
        std::atomic_uint64_t records{};

        static auto before(void* context, const glifistore::FilesystemOperation operation)
            -> glifistore::Status {
            auto& self = *static_cast<WriteCounter*>(context);
            if (operation == glifistore::FilesystemOperation::write_record) {
                self.records.fetch_add(1U, std::memory_order_relaxed);
            }
            return {};
        }
    } writes;

    auto opened =
        glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                 .storage_mode = glifistore::StorageMode::durable_sync,
                                 .data_directory = root / "store",
                                 .durable_open_mode = glifistore::DurableOpenMode::create_new,
                                 .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
                                 .filesystem_hooks = {.context = &writes, .before = &WriteCounter::before}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    GLIFI_REQUIRE(runtime != nullptr);

    {
        ShardPairRuntime::ReadLease pinned_reader{*runtime, 0};
        GLIFI_REQUIRE(static_cast<bool>(pinned_reader));
        for (std::size_t publication = 0; publication < ShardPairRuntime::kMaximumRetiredReadGenerations;
             ++publication) {
            const auto value = "durable-generation-" + std::to_string(publication);
            GLIFI_REQUIRE(store.put("durable-retire-pressure", bytes(value)).has_value());
        }
        const auto writes_before_rejection = writes.records.load(std::memory_order_relaxed);
        const auto blocked = store.put("durable-must-not-enter", bytes("blocked"));
        GLIFI_REQUIRE(!blocked.has_value());
        GLIFI_REQUIRE(blocked.error().code == glifistore::ErrorCode::resource_exhausted);
        GLIFI_REQUIRE(writes.records.load(std::memory_order_relaxed) == writes_before_rejection);
        GLIFI_REQUIRE(!store.get("durable-must-not-enter").has_value());
        GLIFI_REQUIRE(runtime->stats()[0].generation_admission_backpressure_total == 1U);
    }

    const auto writes_before_resume = writes.records.load(std::memory_order_relaxed);
    GLIFI_REQUIRE(store.put("durable-must-not-enter", bytes("after-quiescence")).has_value());
    GLIFI_REQUIRE(writes.records.load(std::memory_order_relaxed) > writes_before_resume);
    GLIFI_REQUIRE(store.get("durable-must-not-enter").has_value());
    GLIFI_REQUIRE(store.close().has_value());

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

GLIFI_TEST("paired generation admission rejects every durable group batch item before Store") {
    using glifistore::store::paired::ShardPairRuntime;

    auto pattern =
        (std::filesystem::temp_directory_path() / "glifistore-generation-group-admission-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    GLIFI_REQUIRE(::mkdtemp(writable.data()) != nullptr);
    const std::filesystem::path root{writable.data()};

    struct WriteCounter final {
        std::atomic_uint64_t records{};

        static auto before(void* context, const glifistore::FilesystemOperation operation)
            -> glifistore::Status {
            auto& self = *static_cast<WriteCounter*>(context);
            if (operation == glifistore::FilesystemOperation::write_record) {
                self.records.fetch_add(1U, std::memory_order_relaxed);
            }
            return {};
        }
    } writes;

    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 1},
         .storage_mode = glifistore::StorageMode::durable_group,
         .data_directory = root / "store",
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .durable_group = {.max_records = 32, .max_bytes = 64U * 1024U, .max_wait_ms = 1, .min_records = 1},
         .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
         .filesystem_hooks = {.context = &writes, .before = &WriteCounter::before}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    GLIFI_REQUIRE(runtime != nullptr);

    const std::string first_key{"group-must-not-enter-a"};
    const std::string second_key{"group-must-not-enter-b"};
    const std::array<glifistore::Store::PutItem, 2> rejected_items{
        glifistore::Store::PutItem{.key = first_key, .value = bytes("first")},
        glifistore::Store::PutItem{.key = second_key, .value = bytes("second")},
    };
    {
        ShardPairRuntime::ReadLease pinned_reader{*runtime, 0};
        GLIFI_REQUIRE(static_cast<bool>(pinned_reader));
        for (std::size_t publication = 0; publication < ShardPairRuntime::kMaximumRetiredReadGenerations;
             ++publication) {
            const auto value = "group-generation-" + std::to_string(publication);
            GLIFI_REQUIRE(store.put("group-retire-pressure", bytes(value)).has_value());
        }

        const auto writes_before_rejection = writes.records.load(std::memory_order_relaxed);
        const auto statuses = store.put_batch(rejected_items);
        GLIFI_REQUIRE(statuses.size() == rejected_items.size());
        for (const auto& status : statuses) {
            GLIFI_REQUIRE(!status.has_value());
            GLIFI_REQUIRE(status.error().code == glifistore::ErrorCode::resource_exhausted);
        }
        GLIFI_REQUIRE(writes.records.load(std::memory_order_relaxed) == writes_before_rejection);
        GLIFI_REQUIRE(runtime->stats()[0].generation_admission_backpressure_total == rejected_items.size());
        GLIFI_REQUIRE(!store.get(first_key).has_value());
        GLIFI_REQUIRE(!store.get(second_key).has_value());
    }

    const auto writes_before_resume = writes.records.load(std::memory_order_relaxed);
    const auto resumed = store.put_batch(rejected_items);
    GLIFI_REQUIRE(resumed.size() == rejected_items.size());
    GLIFI_REQUIRE(resumed[0].has_value());
    GLIFI_REQUIRE(resumed[1].has_value());
    GLIFI_REQUIRE(writes.records.load(std::memory_order_relaxed) > writes_before_resume);
    GLIFI_REQUIRE(store.get(first_key).has_value());
    GLIFI_REQUIRE(store.get(second_key).has_value());
    GLIFI_REQUIRE(store.close().has_value());

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}
