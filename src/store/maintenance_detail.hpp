#pragma once

#include "glifistore/store/maintenance.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>

namespace glifistore::maintenance_detail {

[[nodiscard]] inline auto clamp_interval_ms(const MaintenanceConfig& config, const bool use_min,
                                            const bool use_max) -> std::uint32_t {
    const auto min_ms = std::max<std::uint32_t>(config.min_eval_interval_ms, 1U);
    const auto max_ms = std::max(config.max_eval_interval_ms, min_ms);
    if (use_min) {
        return min_ms;
    }
    if (use_max) {
        return max_ms;
    }
    return min_ms + (max_ms - min_ms) / 2U;
}

[[nodiscard]] inline auto elapsed_ns(const std::chrono::steady_clock::time_point start) -> std::uint64_t {
    const auto delta = std::chrono::steady_clock::now() - start;
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(delta).count());
}

[[nodiscard]] inline auto aggressive_pressure(const MaintenancePressureLevel level) noexcept -> bool {
    return level == MaintenancePressureLevel::pressure || level == MaintenancePressureLevel::emergency;
}

[[nodiscard]] inline auto rotate_additional_bytes_for(const MaintenanceObservation& observation) noexcept
    -> std::uint64_t {
    if (observation.rotate_additional_bytes != 0) {
        return observation.rotate_additional_bytes;
    }
    return static_cast<std::uint64_t>(kSegmentSizeBytes);
}

[[nodiscard]] inline auto free_space_blocks_rotation(const MaintenanceObservation& observation) noexcept
    -> bool {
    if (!observation.available_free_bytes.has_value()) {
        return false;
    }
    const auto available = *observation.available_free_bytes;
    const auto reserved = observation.reserved_free_bytes;
    const auto additional = rotate_additional_bytes_for(observation);
    if (additional > std::numeric_limits<std::uint64_t>::max() - reserved) {
        return true;
    }
    return available < reserved + additional;
}

[[nodiscard]] inline auto ceil_percentage(const std::size_t total, const std::uint32_t percentage) noexcept
    -> std::size_t {
    const auto whole = total / 100U;
    const auto remainder = total % 100U;
    return whole * percentage + (remainder * percentage + 99U) / 100U;
}

} // namespace glifistore::maintenance_detail
