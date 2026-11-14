/**
 * @file
 * @brief Bounded byte pacing with an explicit burst and monotonic-time decisions.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace glifistore {

/// Allocation-free, single-owner byte-rate schedule. The caller performs the
/// returned sleep before consuming `granted_bytes`. One bounded burst may run
/// immediately; subsequent grants are spaced by the configured byte rate.
///
/// This object deliberately owns no clock or thread primitive, which keeps its
/// arithmetic deterministic in unit tests and lets storage callers use their
/// platform monotonic clock/sleep implementation. It is not thread-safe: one
/// maintenance job owns it for its complete lifetime.
class BoundedByteRateLimiter final {
  public:
    /**
     * @brief Grant and required pre-consumption delay; the caller implements the sleep.
     */
    struct Decision final {
        std::uint64_t granted_bytes{};
        std::uint64_t sleep_ns{};
    };

    explicit constexpr BoundedByteRateLimiter(const std::uint64_t bytes_per_second) noexcept
        : bytes_per_second_(bytes_per_second), burst_bytes_(recommended_burst_bytes(bytes_per_second)) {}

    [[nodiscard]] static constexpr auto recommended_burst_bytes(const std::uint64_t bytes_per_second) noexcept
        -> std::uint64_t {
        if (bytes_per_second == 0U) {
            return 0U;
        }
        // Ten-millisecond refill quanta bound foreground-visible I/O bursts.
        // The 1 MiB ceiling keeps a single request bounded even for very fast
        // devices; tiny configured rates retain a one-byte progress quantum.
        constexpr std::uint64_t kRefillsPerSecond = 100U;
        constexpr std::uint64_t kMaximumBurstBytes = std::uint64_t{1} * 1024U * 1024U;
        const auto quotient = bytes_per_second / kRefillsPerSecond;
        const auto rounded = quotient + (bytes_per_second % kRefillsPerSecond != 0U ? 1U : 0U);
        return std::min(kMaximumBurstBytes, std::max<std::uint64_t>(1U, rounded));
    }

    [[nodiscard]] constexpr auto request(const std::uint64_t requested_bytes,
                                         const std::uint64_t now_ns) noexcept -> Decision {
        if (requested_bytes == 0U) {
            return {};
        }
        if (bytes_per_second_ == 0U) {
            return {.granted_bytes = requested_bytes, .sleep_ns = 0U};
        }

        const auto granted = std::min(requested_bytes, burst_bytes_);
        const auto sleep_ns = next_ready_ns_ > now_ns ? next_ready_ns_ - now_ns : 0U;
        const auto schedule_base = std::max(next_ready_ns_, now_ns);
        const auto service_ns = service_time_ns(granted);
        next_ready_ns_ = service_ns > std::numeric_limits<std::uint64_t>::max() - schedule_base
                             ? std::numeric_limits<std::uint64_t>::max()
                             : schedule_base + service_ns;
        return {.granted_bytes = granted, .sleep_ns = sleep_ns};
    }

    [[nodiscard]] constexpr auto bytes_per_second() const noexcept -> std::uint64_t {
        return bytes_per_second_;
    }

    [[nodiscard]] constexpr auto burst_bytes() const noexcept -> std::uint64_t {
        return burst_bytes_;
    }

  private:
    [[nodiscard]] constexpr auto service_time_ns(const std::uint64_t bytes) const noexcept -> std::uint64_t {
        constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000U;
        const auto whole_seconds = bytes / bytes_per_second_;
        const auto remainder = bytes % bytes_per_second_;
        auto result = whole_seconds * kNanosecondsPerSecond;
        // bytes <= burst <= 1 MiB, so this product is bounded independently
        // of a hostile/configured uint64 rate.
        const auto tail_product = remainder * kNanosecondsPerSecond;
        result += tail_product / bytes_per_second_;
        if (tail_product % bytes_per_second_ != 0U) {
            ++result;
        }
        return result;
    }

    std::uint64_t bytes_per_second_{};
    std::uint64_t burst_bytes_{};
    std::uint64_t next_ready_ns_{};
};

} // namespace glifistore
