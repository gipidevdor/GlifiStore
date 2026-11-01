#include "glifistore/server/abuse_limits.hpp"
#include "test.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

GLIFI_TEST("abuse controller admits within accept and request windows") {
    using glifistore::server::AbuseController;
    using glifistore::server::AbuseLimits;

    AbuseController controller{AbuseLimits{
        .max_accepts_per_sec = 2,
        .connection_max_requests_per_sec = 2,
        .principal_max_requests_per_sec = 2,
        .principal_max_bytes_per_sec = 32,
    }};
    const auto now = std::chrono::steady_clock::now();
    GLIFI_REQUIRE(controller.try_admit_accept(now));
    GLIFI_REQUIRE(controller.try_admit_accept(now));
    GLIFI_REQUIRE(!controller.try_admit_accept(now));
    GLIFI_REQUIRE(controller.stats().accepts_rejected == 1);

    std::uint64_t window_start = 0;
    std::uint32_t used = 0;
    GLIFI_REQUIRE(controller.try_admit_connection_request(window_start, used, now));
    GLIFI_REQUIRE(controller.try_admit_connection_request(window_start, used, now));
    GLIFI_REQUIRE(!controller.try_admit_connection_request(window_start, used, now));
    GLIFI_REQUIRE(controller.stats().connection_rate_rejected == 1);

    GLIFI_REQUIRE(controller.try_admit_principal("alice", 8, now));
    GLIFI_REQUIRE(controller.try_admit_principal("alice", 8, now));
    GLIFI_REQUIRE(!controller.try_admit_principal("alice", 8, now));
    GLIFI_REQUIRE(controller.stats().principal_request_rejected == 1);

    AbuseController bandwidth{AbuseLimits{.principal_max_bytes_per_sec = 16}};
    GLIFI_REQUIRE(bandwidth.try_admit_principal("bob", 16, now));
    GLIFI_REQUIRE(!bandwidth.try_admit_principal("bob", 1, now));
    GLIFI_REQUIRE(bandwidth.stats().principal_bandwidth_rejected == 1);
    // Anonymous / empty principal skips identity quotas.
    GLIFI_REQUIRE(bandwidth.try_admit_principal("", 1'000'000, now));
}

GLIFI_TEST("abuse controller windows reset after one second") {
    using glifistore::server::AbuseController;
    using glifistore::server::AbuseLimits;

    AbuseController controller{AbuseLimits{.max_accepts_per_sec = 1}};
    auto now = std::chrono::steady_clock::now();
    GLIFI_REQUIRE(controller.try_admit_accept(now));
    GLIFI_REQUIRE(!controller.try_admit_accept(now));
    now += std::chrono::seconds{1};
    GLIFI_REQUIRE(controller.try_admit_accept(now));
}

GLIFI_TEST("abuse controller charges oversized responses and zero byte followups") {
    using glifistore::server::AbuseController;
    using glifistore::server::AbuseLimits;
    const auto now = std::chrono::steady_clock::now();
    for (const auto response_bytes :
         {std::uint64_t{16}, std::uint64_t{17}, std::numeric_limits<std::uint64_t>::max()}) {
        AbuseController controller{AbuseLimits{.principal_max_bytes_per_sec = 16}};
        GLIFI_REQUIRE(controller.try_admit_principal("reader", 1, now));
        controller.record_principal_response_bytes("reader", response_bytes, now);
        GLIFI_REQUIRE(!controller.try_admit_principal("reader", 1, now));
        // INIT has no request payload but produces a routing identity response.
        GLIFI_REQUIRE(!controller.try_admit_principal("reader", 0, now));
        GLIFI_REQUIRE(controller.try_admit_principal("other", 1, now));
        GLIFI_REQUIRE(controller.try_admit_principal("reader", 1, now + std::chrono::seconds{1}));
    }
    AbuseController maximum{
        AbuseLimits{.principal_max_bytes_per_sec = std::numeric_limits<std::uint64_t>::max()}};
    GLIFI_REQUIRE(maximum.try_admit_principal("reader", 1, now));
    maximum.record_principal_response_bytes("reader", std::numeric_limits<std::uint64_t>::max(), now);
    GLIFI_REQUIRE(!maximum.try_admit_principal("reader", 0, now));
}

GLIFI_TEST("secure profile abuse defaults are non-zero") {
    const auto defaults = glifistore::server::secure_profile_abuse_defaults();
    GLIFI_REQUIRE(defaults.any_enabled());
    GLIFI_REQUIRE(defaults.max_accepts_per_sec > 0);
    GLIFI_REQUIRE(defaults.idle_timeout_ms > 0);
    GLIFI_REQUIRE(defaults.request_timeout_ms > 0);
    GLIFI_REQUIRE(defaults.connection_max_requests_per_sec > 0);
    GLIFI_REQUIRE(defaults.principal_max_requests_per_sec > 0);
    GLIFI_REQUIRE(defaults.principal_max_bytes_per_sec > 0);
}

GLIFI_TEST("abuse controller principal quotas are process-wide under concurrency") {
    using glifistore::server::AbuseController;
    using glifistore::server::AbuseLimits;

    AbuseController controller{AbuseLimits{
        .principal_max_requests_per_sec = 32,
        .principal_max_bytes_per_sec = 256,
    }};
    const auto now = std::chrono::steady_clock::now();
    constexpr int kThreads = 8;
    constexpr int kAttempts = 64;
    std::atomic<std::uint64_t> admitted{0};
    std::atomic<std::uint64_t> rejected{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int thread = 0; thread < kThreads; ++thread) {
        workers.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int attempt = 0; attempt < kAttempts; ++attempt) {
                if (controller.try_admit_principal("shared", 8, now)) {
                    admitted.fetch_add(1U, std::memory_order_relaxed);
                } else {
                    rejected.fetch_add(1U, std::memory_order_relaxed);
                }
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) {
        worker.join();
    }

    const auto admitted_count = admitted.load(std::memory_order_relaxed);
    const auto rejected_count = rejected.load(std::memory_order_relaxed);
    GLIFI_REQUIRE(admitted_count <= 32);
    GLIFI_REQUIRE(admitted_count > 0);
    GLIFI_REQUIRE(rejected_count > 0);
    const auto stats = controller.stats();
    GLIFI_REQUIRE(stats.principal_request_rejected + stats.principal_bandwidth_rejected == rejected_count);
}
