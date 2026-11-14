#include "glifistore/persistence/durable_flush_coordinator.hpp"
#include "persistence/durable_flush_coordinator_internal.hpp"
#include "test.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

GLIFI_TEST("durable flush coordinator executes an exact requested deadline") {
    using clock = std::chrono::steady_clock;
    std::mutex mutex;
    std::condition_variable completed;
    bool fired{};
    clock::time_point fired_at{};
    glifistore::DurableFlushCoordinator coordinator{60'000, 60'000, false, true,
                                                    [&](const bool force_all) -> glifistore::Status {
                                                        GLIFI_REQUIRE(!force_all);
                                                        {
                                                            const std::lock_guard lock{mutex};
                                                            fired = true;
                                                            fired_at = clock::now();
                                                        }
                                                        completed.notify_all();
                                                        return {};
                                                    }};

    const auto started = clock::now();
    coordinator.request_flush_at(started + std::chrono::milliseconds{25});
    {
        std::unique_lock lock{mutex};
        GLIFI_REQUIRE(completed.wait_for(lock, std::chrono::seconds{2}, [&] { return fired; }));
    }
    coordinator.stop();
    const auto elapsed = fired_at - started;
    GLIFI_REQUIRE(elapsed >= std::chrono::milliseconds{20});
    GLIFI_REQUIRE(elapsed < std::chrono::milliseconds{500});
}

GLIFI_TEST("blocking durable flush runs on the coordinator and propagates failure") {
    const auto caller = std::this_thread::get_id();
    std::thread::id callback_thread;
    glifistore::DurableFlushCoordinator coordinator{
        60'000, 60'000, false, true, [&](const bool force_all) -> glifistore::Status {
            GLIFI_REQUIRE(force_all);
            callback_thread = std::this_thread::get_id();
            return glifistore::fail(glifistore::ErrorCode::io_error, "injected flush failure");
        }};

    const auto flushed = coordinator.flush_all_blocking();
    coordinator.stop();
    GLIFI_REQUIRE(!flushed.has_value());
    GLIFI_REQUIRE(flushed.error().code == glifistore::ErrorCode::io_error);
    GLIFI_REQUIRE(callback_thread != std::thread::id{});
    GLIFI_REQUIRE(callback_thread != caller);
}

GLIFI_TEST("durable flush coordinator translates callback exceptions and stops") {
    glifistore::DurableFlushCoordinator coordinator{
        60'000, 60'000, false, true,
        [](const bool) -> glifistore::Status { throw std::runtime_error("injected callback exception"); }};

    const auto first = coordinator.flush_all_blocking();
    GLIFI_REQUIRE(!first.has_value());
    GLIFI_REQUIRE(first.error().code == glifistore::ErrorCode::internal_error);
    const auto repeated = coordinator.flush_all_blocking();
    GLIFI_REQUIRE(!repeated.has_value());
    GLIFI_REQUIRE(repeated.error().code == glifistore::ErrorCode::internal_error);
    coordinator.stop();
}

GLIFI_TEST("durable flush coordinator rejects exhausted flush generations without invoking callback") {
    std::atomic calls{0};
    glifistore::DurableFlushCoordinator coordinator{60'000, 60'000, false, true,
                                                    [&](const bool) -> glifistore::Status {
                                                        calls.fetch_add(1, std::memory_order_relaxed);
                                                        return {};
                                                    }};
    glifistore::detail::DurableFlushCoordinatorAccess::set_flush_all_generation(
        coordinator, std::numeric_limits<std::uint64_t>::max());

    const auto exhausted = coordinator.flush_all_blocking();
    GLIFI_REQUIRE(!exhausted.has_value());
    GLIFI_REQUIRE(exhausted.error().code == glifistore::ErrorCode::arithmetic_overflow);
    GLIFI_REQUIRE(calls.load(std::memory_order_relaxed) == 0);
    coordinator.stop();
}

GLIFI_TEST("concurrent coordinator stop releases a blocking flush without deadlock") {
    std::mutex mutex;
    std::condition_variable changed;
    bool callback_entered{};
    bool release_callback{};
    bool flush_completed{};
    glifistore::Status flush_result;
    glifistore::DurableFlushCoordinator coordinator{60'000, 60'000, false, true,
                                                    [&](const bool force_all) -> glifistore::Status {
                                                        GLIFI_REQUIRE(force_all);
                                                        std::unique_lock lock{mutex};
                                                        callback_entered = true;
                                                        changed.notify_all();
                                                        changed.wait(lock, [&] { return release_callback; });
                                                        return {};
                                                    }};

    std::thread flusher{[&] {
        auto result = coordinator.flush_all_blocking();
        {
            const std::lock_guard lock{mutex};
            flush_result = std::move(result);
            flush_completed = true;
        }
        changed.notify_all();
    }};
    {
        std::unique_lock lock{mutex};
        GLIFI_REQUIRE(changed.wait_for(lock, std::chrono::seconds{2}, [&] { return callback_entered; }));
    }
    std::thread stopper{[&] { coordinator.stop(); }};
    {
        std::unique_lock lock{mutex};
        GLIFI_REQUIRE(changed.wait_for(lock, std::chrono::seconds{2}, [&] { return flush_completed; }));
        release_callback = true;
    }
    changed.notify_all();
    flusher.join();
    stopper.join();

    GLIFI_REQUIRE(!flush_result.has_value());
    GLIFI_REQUIRE(flush_result.error().code == glifistore::ErrorCode::unavailable);
}
