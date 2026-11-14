#include "glifistore/core/byte_rate_limiter.hpp"
#include "test.hpp"

#include <cstdint>
#include <limits>

GLIFI_TEST("bounded byte rate limiter derives a ten millisecond capped burst") {
    using glifistore::BoundedByteRateLimiter;
    GLIFI_REQUIRE(BoundedByteRateLimiter::recommended_burst_bytes(0) == 0);
    GLIFI_REQUIRE(BoundedByteRateLimiter::recommended_burst_bytes(1) == 1);
    GLIFI_REQUIRE(BoundedByteRateLimiter::recommended_burst_bytes(100) == 1);
    GLIFI_REQUIRE(BoundedByteRateLimiter::recommended_burst_bytes(64U * 1024U * 1024U) == 671'089U);
    GLIFI_REQUIRE(BoundedByteRateLimiter::recommended_burst_bytes(
                      std::numeric_limits<std::uint64_t>::max()) == 1U * 1024U * 1024U);
}

GLIFI_TEST("bounded byte rate limiter spaces requests after one immediate burst") {
    glifistore::BoundedByteRateLimiter limiter{100};
    GLIFI_REQUIRE(limiter.burst_bytes() == 1);

    const auto first = limiter.request(3, 1'000);
    GLIFI_REQUIRE(first.granted_bytes == 1);
    GLIFI_REQUIRE(first.sleep_ns == 0);

    const auto second = limiter.request(2, 1'000);
    GLIFI_REQUIRE(second.granted_bytes == 1);
    GLIFI_REQUIRE(second.sleep_ns == 10'000'000U);

    const auto after_idle = limiter.request(1, 30'000'000U);
    GLIFI_REQUIRE(after_idle.granted_bytes == 1);
    GLIFI_REQUIRE(after_idle.sleep_ns == 0);
}

GLIFI_TEST("disabled byte rate limiter grants the complete request without debt") {
    glifistore::BoundedByteRateLimiter limiter{0};
    const auto first = limiter.request(std::numeric_limits<std::uint64_t>::max(), 10);
    const auto second = limiter.request(17, 10);
    GLIFI_REQUIRE(first.granted_bytes == std::numeric_limits<std::uint64_t>::max());
    GLIFI_REQUIRE(first.sleep_ns == 0);
    GLIFI_REQUIRE(second.granted_bytes == 17);
    GLIFI_REQUIRE(second.sleep_ns == 0);
}
