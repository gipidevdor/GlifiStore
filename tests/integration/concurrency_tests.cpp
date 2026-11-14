#include "glifistore/core/key_hash.hpp"
#include "glifistore/store/store.hpp"
#include "test.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
auto bytes(std::string_view value) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

auto value_string(const glifistore::OwnedValue& value) -> std::string_view {
    return {reinterpret_cast<const char*>(value.bytes.data()), value.bytes.size()};
}
} // namespace

GLIFI_TEST("concurrent store puts on distinct routed keys preserve all values") {
    constexpr std::size_t worker_total = 8;
    constexpr std::size_t keys_per_thread = 250;
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = worker_total},
                                           .concurrency = glifistore::StoreConcurrencyMode::legacy_mutex});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;

    std::atomic<bool> failed{false};
    const auto run = [&](const std::size_t thread_id) {
        for (std::size_t offset = 0; offset < keys_per_thread; ++offset) {
            const auto key = "concurrent-key-" + std::to_string(thread_id * keys_per_thread + offset);
            const auto value = "value-" + std::to_string(offset);
            if (!store.put(key, bytes(value)).has_value()) {
                failed.store(true);
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(8);
    for (std::size_t thread_id = 0; thread_id < 8; ++thread_id) {
        threads.emplace_back(run, thread_id);
    }
    for (auto& thread : threads) {
        thread.join();
    }
    GLIFI_REQUIRE(!failed.load());

    for (std::size_t thread_id = 0; thread_id < 8; ++thread_id) {
        for (std::size_t offset = 0; offset < keys_per_thread; ++offset) {
            const auto key = "concurrent-key-" + std::to_string(thread_id * keys_per_thread + offset);
            const auto expected = "value-" + std::to_string(offset);
            const auto record = store.get(key);
            GLIFI_REQUIRE(record.has_value());
            GLIFI_REQUIRE(value_string(*record) == expected);
        }
    }
    GLIFI_REQUIRE(store.verify_index().has_value());
}

GLIFI_TEST("concurrent store read after write on one key serializes updates") {
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 4},
                                           .concurrency = glifistore::StoreConcurrencyMode::legacy_mutex});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    constexpr std::size_t thread_total = 8;
    constexpr std::size_t iterations = 500;
    std::atomic<std::uint64_t> observed_max{0};
    std::atomic<bool> failed{false};

    const auto run = [&]() {
        for (std::uint64_t value = 0; value < iterations; ++value) {
            const auto payload = std::to_string(value);
            if (!store.put("shared-key", bytes(payload)).has_value()) {
                failed.store(true);
                return;
            }
            const auto record = store.get("shared-key");
            if (!record.has_value()) {
                failed.store(true);
                return;
            }
            const auto view = value_string(*record);
            std::uint64_t parsed = 0;
            const auto text = std::string_view{view.data(), view.size()};
            const auto parsed_result = std::from_chars(text.data(), text.data() + text.size(), parsed);
            if (parsed_result.ec != std::errc{}) {
                failed.store(true);
                return;
            }
            auto current = observed_max.load();
            while (parsed > current && !observed_max.compare_exchange_weak(current, parsed)) {
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(thread_total);
    for (std::size_t index = 0; index < thread_total; ++index) {
        threads.emplace_back(run);
    }
    for (auto& thread : threads) {
        thread.join();
    }
    GLIFI_REQUIRE(!failed.load());
    GLIFI_REQUIRE(observed_max.load() == iterations - 1);
    GLIFI_REQUIRE(store.verify_index().has_value());
}

GLIFI_TEST("concurrent store verify index succeeds under mixed traffic") {
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 4},
                                           .concurrency = glifistore::StoreConcurrencyMode::legacy_mutex});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    std::atomic<bool> verify_failed{false};
    std::atomic<bool> traffic_failed{false};

    std::thread verifier([&]() {
        for (std::size_t run = 0; run < 32; ++run) {
            if (!store.verify_index().has_value()) {
                verify_failed.store(true);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    std::vector<std::thread> workers;
    workers.reserve(6);
    for (std::size_t worker_id = 0; worker_id < 6; ++worker_id) {
        workers.emplace_back([&, worker_id]() {
            for (std::uint64_t value = 0; value < 500; ++value) {
                const auto key = "mix-key-" + std::to_string(worker_id) + "-" + std::to_string(value);
                if (!store.put(key, bytes("payload")).has_value()) {
                    traffic_failed.store(true);
                    return;
                }
                if (value % 3 == 0) {
                    const auto record = store.get(key);
                    if (!record.has_value()) {
                        traffic_failed.store(true);
                        return;
                    }
                }
                if (value % 5 == 0 && !store.erase(key).has_value()) {
                    traffic_failed.store(true);
                    return;
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    verifier.join();

    GLIFI_REQUIRE(!traffic_failed.load());
    GLIFI_REQUIRE(!verify_failed.load());
    GLIFI_REQUIRE(store.verify_index().has_value());
}
