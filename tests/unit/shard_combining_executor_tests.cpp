#include "glifistore/core/key_hash.hpp"
#include "glifistore/store/paired/shard_combining_executor.hpp"
#include "glifistore/store/store.hpp"
#include "store/store_internal.hpp"
#include "test.hpp"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

using glifistore::store::paired::execution_token_executing;
using glifistore::store::paired::execution_token_idle;
using glifistore::store::paired::release_execution_token;
using glifistore::store::paired::try_acquire_execution_token;
using glifistore::store::paired::try_reacquire_execution_token_if_pending;

namespace {

auto bytes(const std::string_view value) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

} // namespace

GLIFI_TEST("execution token CAS grants sole ownership") {
    std::atomic<std::uint32_t> token{execution_token_idle()};
    GLIFI_REQUIRE(try_acquire_execution_token(token));
    GLIFI_REQUIRE(token.load() == execution_token_executing());
    GLIFI_REQUIRE(!try_acquire_execution_token(token));
    release_execution_token(token);
    GLIFI_REQUIRE(token.load() == execution_token_idle());
    GLIFI_REQUIRE(try_acquire_execution_token(token));
    release_execution_token(token);
}

GLIFI_TEST("execution token lost-wakeup reacquire when pending") {
    std::atomic<std::uint32_t> token{execution_token_idle()};
    GLIFI_REQUIRE(try_acquire_execution_token(token));
    release_execution_token(token);
    GLIFI_REQUIRE(!try_reacquire_execution_token_if_pending(token, false));
    GLIFI_REQUIRE(try_reacquire_execution_token_if_pending(token, true));
    release_execution_token(token);
}

GLIFI_TEST("execution token serializes concurrent acquirers") {
    std::atomic<std::uint32_t> token{execution_token_idle()};
    std::atomic_int holders{0};
    std::atomic_int max_holders{0};
    constexpr int kThreads = 8;
    constexpr int kIters = 200;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kIters; ++i) {
                while (!try_acquire_execution_token(token)) {
                }
                const int now = holders.fetch_add(1) + 1;
                int observed = max_holders.load();
                while (now > observed && !max_holders.compare_exchange_weak(observed, now)) {
                }
                holders.fetch_sub(1);
                release_execution_token(token);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    GLIFI_REQUIRE(max_holders.load() == 1);
}

GLIFI_TEST("embedded volatile combining omits dedicated Writer threads") {
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1}});
    GLIFI_REQUIRE(opened.has_value());
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(**opened);
    GLIFI_REQUIRE(runtime != nullptr);
    GLIFI_REQUIRE(runtime->combining_enabled());
    GLIFI_REQUIRE(!runtime->dedicated_writer_required());
    GLIFI_REQUIRE((**opened).put("k", bytes("v")).has_value());
    const auto got = (**opened).get("k");
    GLIFI_REQUIRE(got.has_value());
    GLIFI_REQUIRE(std::string_view(reinterpret_cast<const char*>(got->bytes.data()), got->bytes.size()) ==
                  "v");
}

GLIFI_TEST("embedded volatile combining preserves same-key FIFO under contention") {
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    constexpr int kThreads = 4;
    constexpr int kIters = 50;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    std::atomic_bool failed{false};
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kIters; ++i) {
                const auto value = std::to_string(t * kIters + i);
                if (!store.put("shared", bytes(value)).has_value()) {
                    failed.store(true);
                    return;
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    GLIFI_REQUIRE(!failed.load());
    const auto got = store.get("shared");
    GLIFI_REQUIRE(got.has_value());
    const auto text = std::string_view(reinterpret_cast<const char*>(got->bytes.data()), got->bytes.size());
    GLIFI_REQUIRE(!text.empty());
}

GLIFI_TEST("embedded durable_sync combining omits dedicated Writer and keeps RAW") {
    auto pattern =
        (std::filesystem::temp_directory_path() / "glifistore-combine-durable-sync-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    GLIFI_REQUIRE(::mkdtemp(writable.data()) != nullptr);
    const std::filesystem::path store_path{writable.data()};
    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = store_path,
    });
    GLIFI_REQUIRE(opened.has_value());
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(**opened);
    GLIFI_REQUIRE(runtime != nullptr);
    GLIFI_REQUIRE(runtime->combining_enabled());
    GLIFI_REQUIRE(!runtime->dedicated_writer_required());
    GLIFI_REQUIRE((**opened).put("alpha", bytes("one")).has_value());
    const auto first = (**opened).get("alpha");
    GLIFI_REQUIRE(first.has_value());
    GLIFI_REQUIRE(std::string_view(reinterpret_cast<const char*>(first->bytes.data()), first->bytes.size()) ==
                  "one");
    GLIFI_REQUIRE((**opened).close().has_value());
    std::error_code ec;
    std::filesystem::remove_all(store_path, ec);
}

GLIFI_TEST("embedded durable_sync combining preserves same-key FIFO under token contention") {
    auto pattern =
        (std::filesystem::temp_directory_path() / "glifistore-combine-durable-fifo-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    GLIFI_REQUIRE(::mkdtemp(writable.data()) != nullptr);
    const std::filesystem::path store_path{writable.data()};
    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = store_path,
    });
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    GLIFI_REQUIRE(runtime != nullptr);
    GLIFI_REQUIRE(runtime->combining_enabled());
    GLIFI_REQUIRE(!runtime->dedicated_writer_required());

    constexpr int kThreads = 4;
    constexpr int kIters = 25;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    std::atomic_bool failed{false};
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kIters; ++i) {
                const auto value = std::to_string(t * kIters + i);
                if (!store.put("shared-durable", bytes(value)).has_value()) {
                    failed.store(true);
                    return;
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    GLIFI_REQUIRE(!failed.load());
    const auto got = store.get("shared-durable");
    GLIFI_REQUIRE(got.has_value());
    const auto text = std::string_view(reinterpret_cast<const char*>(got->bytes.data()), got->bytes.size());
    GLIFI_REQUIRE(!text.empty());
    GLIFI_REQUIRE(store.close().has_value());
    std::error_code ec;
    std::filesystem::remove_all(store_path, ec);
}

GLIFI_TEST("embedded durable_sync put_batch coalesces until RAW without early ACK") {
    auto pattern =
        (std::filesystem::temp_directory_path() / "glifistore-combine-durable-batch-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    GLIFI_REQUIRE(::mkdtemp(writable.data()) != nullptr);
    const std::filesystem::path store_path{writable.data()};
    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = store_path,
    });
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    GLIFI_REQUIRE(runtime != nullptr);
    GLIFI_REQUIRE(runtime->combining_enabled());
    GLIFI_REQUIRE(!runtime->dedicated_writer_required());

    const auto epoch_before = runtime->stats()[0].writer_epoch;
    std::vector<std::string> keys;
    std::vector<std::string> values;
    std::vector<glifistore::Store::PutItem> items;
    keys.reserve(16);
    values.reserve(16);
    items.reserve(16);
    for (int index = 0; index < 16; ++index) {
        keys.push_back("coalesce-" + std::to_string(index));
        values.push_back("v-" + std::to_string(index));
        items.push_back(glifistore::Store::PutItem{.key = keys.back(), .value = bytes(values.back())});
    }
    // put_batch drains already-queued work under the token (≤32, no wait-to-fill).
    // Success return is the RAW barrier: every key must be visible immediately.
    const auto statuses = store.put_batch(items);
    GLIFI_REQUIRE(statuses.size() == items.size());
    for (const auto& status : statuses) {
        GLIFI_REQUIRE(status.has_value());
    }
    for (std::size_t index = 0; index < items.size(); ++index) {
        const auto got = store.get(keys[index]);
        GLIFI_REQUIRE(got.has_value());
        GLIFI_REQUIRE(std::string_view(reinterpret_cast<const char*>(got->bytes.data()), got->bytes.size()) ==
                      values[index]);
    }
    const auto epoch_after = runtime->stats()[0].writer_epoch;
    GLIFI_REQUIRE(epoch_after > epoch_before);
    GLIFI_REQUIRE(store.close().has_value());
    std::error_code ec;
    std::filesystem::remove_all(store_path, ec);
}

GLIFI_TEST("embedded volatile hot-key does not starve sibling shard progress") {
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 2}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    GLIFI_REQUIRE(runtime != nullptr);
    GLIFI_REQUIRE(runtime->combining_enabled());

    std::string hot_key;
    std::string cold_key;
    for (int index = 0; index < 10'000; ++index) {
        const auto candidate = "wave2-hot-" + std::to_string(index);
        if (glifistore::route_worker(candidate, 2) == 0) {
            hot_key = candidate;
            break;
        }
    }
    for (int index = 0; index < 10'000; ++index) {
        const auto candidate = "wave2-cold-" + std::to_string(index);
        if (glifistore::route_worker(candidate, 2) == 1) {
            cold_key = candidate;
            break;
        }
    }
    GLIFI_REQUIRE(!hot_key.empty());
    GLIFI_REQUIRE(!cold_key.empty());

    std::atomic_bool stop{false};
    std::atomic_bool hot_failed{false};
    std::atomic_bool cold_failed{false};
    std::atomic<std::uint64_t> cold_ok{0};

    std::thread hot{[&] {
        while (!stop.load(std::memory_order_acquire)) {
            if (!store.put(hot_key, bytes("hot")).has_value()) {
                hot_failed.store(true);
                return;
            }
        }
    }};
    std::thread cold{[&] {
        for (int i = 0; i < 200; ++i) {
            const auto value = std::to_string(i);
            if (!store.put(cold_key, bytes(value)).has_value()) {
                cold_failed.store(true);
                return;
            }
            cold_ok.fetch_add(1U, std::memory_order_relaxed);
        }
    }};
    cold.join();
    stop.store(true, std::memory_order_release);
    hot.join();
    GLIFI_REQUIRE(!hot_failed.load());
    GLIFI_REQUIRE(!cold_failed.load());
    GLIFI_REQUIRE(cold_ok.load() == 200U);
    const auto got = store.get(cold_key);
    GLIFI_REQUIRE(got.has_value());
    GLIFI_REQUIRE(store.close().has_value());
}
