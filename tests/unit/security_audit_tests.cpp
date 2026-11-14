#include "glifistore/server/security_audit.hpp"
#include "test.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

GLIFI_TEST("security audit counts accept deny authz and tls decisions") {
    glifistore::server::SecurityAudit audit{false, false};
    audit.auth_accept("reader.example");
    audit.auth_deny("handshake_failed", "bad.example");
    audit.authz_deny("reader.example", "put", "capability_denied");
    audit.tls_error("TLS handshake timed out");

    const auto stats = audit.stats();
    GLIFI_REQUIRE(stats.auth_accepts == 1);
    GLIFI_REQUIRE(stats.auth_denies == 1);
    GLIFI_REQUIRE(stats.authz_denies == 1);
    GLIFI_REQUIRE(stats.tls_errors == 1);
}

GLIFI_TEST("security audit quiet suppresses accept events only") {
    glifistore::server::SecurityAudit audit{false, true};
    audit.auth_accept("reader.example");
    audit.auth_deny("no_peer_cert");
    GLIFI_REQUIRE(audit.stats().auth_accepts == 1);
    GLIFI_REQUIRE(audit.stats().auth_denies == 1);
}

GLIFI_TEST("security audit disabled emits no lines but still counts") {
    glifistore::server::SecurityAudit audit{false, false};
    audit.auth_accept("reader.example");
    audit.authz_deny("reader.example", "get", "capability_denied");
    GLIFI_REQUIRE(audit.stats().events_pending == 0);
    GLIFI_REQUIRE(audit.stats().events_written == 0);
    GLIFI_REQUIRE(audit.stats().events_suppressed == 0);
    GLIFI_REQUIRE(audit.stats().auth_accepts == 1);
    GLIFI_REQUIRE(audit.stats().authz_denies == 1);
}

GLIFI_TEST("security audit concurrent deny storm keeps counters coherent") {
    glifistore::server::SecurityAudit audit{false, false};
    constexpr int kThreads = 8;
    constexpr int kPerThread = 128;
    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int thread = 0; thread < kThreads; ++thread) {
        workers.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int index = 0; index < kPerThread; ++index) {
                audit.authz_deny("storm.example", "put", "capability_denied");
                audit.auth_deny("handshake_failed", "storm.example");
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) {
        worker.join();
    }

    const auto stats = audit.stats();
    constexpr std::uint64_t kExpected = static_cast<std::uint64_t>(kThreads) * kPerThread;
    GLIFI_REQUIRE(stats.auth_denies == kExpected);
    GLIFI_REQUIRE(stats.authz_denies == kExpected);
    GLIFI_REQUIRE(stats.auth_accepts == 0);
    GLIFI_REQUIRE(stats.tls_errors == 0);
}
