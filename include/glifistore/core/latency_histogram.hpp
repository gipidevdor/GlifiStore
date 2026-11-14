/**
 * @file
 * @brief Fixed-memory latency buckets for bounded percentile observations.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace glifistore {

/// Fixed-memory latency histogram for STATS export. Each stored count belongs
/// to one inclusive upper-bound bucket in nanoseconds; cumulative() produces
/// Prometheus-style le counts. Access requires external serialization.
struct LatencyHistogram final {
    static constexpr std::array<std::uint64_t, 8> kBoundsNs{
        1'000ULL,                                  // 1 µs
        10'000ULL,                                 // 10 µs
        100'000ULL,                                // 100 µs
        1'000'000ULL,                              // 1 ms
        10'000'000ULL,                             // 10 ms
        100'000'000ULL,                            // 100 ms
        1'000'000'000ULL,                          // 1 s
        std::numeric_limits<std::uint64_t>::max(), // +Inf
    };

    std::array<std::uint64_t, kBoundsNs.size()> counts{};
    std::uint64_t observations{};
    std::uint64_t sum_ns{};

    void observe(const std::uint64_t sample_ns) noexcept {
        for (std::size_t index = 0; index < kBoundsNs.size(); ++index) {
            if (sample_ns <= kBoundsNs[index]) {
                ++counts[index];
                break;
            }
        }
        ++observations;
        sum_ns = sample_ns > std::numeric_limits<std::uint64_t>::max() - sum_ns
                     ? std::numeric_limits<std::uint64_t>::max()
                     : sum_ns + sample_ns;
    }

    /// Cumulative le_* counts for export (counts[i] = observations with value <= bound[i]).
    [[nodiscard]] auto cumulative() const noexcept -> std::array<std::uint64_t, kBoundsNs.size()> {
        std::array<std::uint64_t, kBoundsNs.size()> out{};
        std::uint64_t running{};
        for (std::size_t index = 0; index < kBoundsNs.size(); ++index) {
            running += counts[index];
            out[index] = running;
        }
        return out;
    }

    /// Bucket-interpolated approximate percentile. Returns 0 when empty. The
    /// +Inf bucket contributes its lower bound (1 s); larger samples are not
    /// recoverable from the histogram and this estimate can understate them.
    [[nodiscard]] auto approximate_percentile_ns(const double percentile) const noexcept -> std::uint64_t {
        if (observations == 0 || !(percentile >= 0.0) || percentile > 1.0) {
            return 0;
        }
        const auto target = static_cast<std::uint64_t>(static_cast<long double>(observations) *
                                                       static_cast<long double>(percentile));
        const auto rank = target == 0 ? 1U : std::min(target, observations);
        const auto cumulative = this->cumulative();
        std::uint64_t previous = 0;
        std::uint64_t lower = 0;
        for (std::size_t index = 0; index < kBoundsNs.size(); ++index) {
            if (cumulative[index] < rank) {
                previous = cumulative[index];
                lower =
                    kBoundsNs[index] == std::numeric_limits<std::uint64_t>::max() ? lower : kBoundsNs[index];
                continue;
            }
            const auto upper = kBoundsNs[index] == std::numeric_limits<std::uint64_t>::max()
                                   ? (lower == 0 ? 1'000'000'000ULL : lower)
                                   : kBoundsNs[index];
            const auto bucket = cumulative[index] - previous;
            if (bucket == 0) {
                return upper;
            }
            const auto into = rank - previous;
            const auto span = upper > lower ? upper - lower : 0U;
            return lower + (span * into) / bucket;
        }
        return lower;
    }
};

[[nodiscard]] inline auto latency_histogram_bound_label(const std::uint64_t bound_ns) -> std::string {
    if (bound_ns == std::numeric_limits<std::uint64_t>::max()) {
        return "inf";
    }
    return std::to_string(bound_ns);
}

/// Appends count/sum/le_*/p50/p99 lines under `prefix` (for example "lane[0].queue_wait_ns").
inline void append_latency_histogram(std::string& out, const std::string_view prefix,
                                     const LatencyHistogram& histogram) {
    out += prefix;
    out += ".count=";
    out += std::to_string(histogram.observations);
    out += '\n';
    out += prefix;
    out += ".sum=";
    out += std::to_string(histogram.sum_ns);
    out += '\n';
    const auto cumulative = histogram.cumulative();
    for (std::size_t index = 0; index < LatencyHistogram::kBoundsNs.size(); ++index) {
        out += prefix;
        out += ".le_";
        out += latency_histogram_bound_label(LatencyHistogram::kBoundsNs[index]);
        out += '=';
        out += std::to_string(cumulative[index]);
        out += '\n';
    }
    out += prefix;
    out += ".p50=";
    out += std::to_string(histogram.approximate_percentile_ns(0.50));
    out += '\n';
    out += prefix;
    out += ".p99=";
    out += std::to_string(histogram.approximate_percentile_ns(0.99));
    out += '\n';
}

} // namespace glifistore
