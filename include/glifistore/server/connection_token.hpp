/**
 * @file
 * @brief Connection slot plus generation identity for rejecting stale completions.
 */
#pragma once

#include <cstdint>
#include <limits>

namespace glifistore::server {

/**
 * @brief Slot plus nonwrapping generation identity; compare both before accepting asynchronous completion.
 */
struct ConnectionToken {
    std::uint32_t slot{};
    std::uint32_t generation{};

    [[nodiscard]] auto encode() const noexcept -> std::uint64_t {
        return (static_cast<std::uint64_t>(generation) << 32U) | slot;
    }

    [[nodiscard]] static auto decode(const std::uint64_t encoded) noexcept -> ConnectionToken {
        return {.slot = static_cast<std::uint32_t>(encoded & 0xFFFF'FFFFULL),
                .generation = static_cast<std::uint32_t>(encoded >> 32U)};
    }
};

/// A generation value is published to pollers and asynchronous completion
/// queues, so wrapping it would make a sufficiently old token valid again.
/// Permanently retire the individual slot at exhaustion instead of reusing an
/// identity that concurrent observers may still hold.
[[nodiscard]] inline constexpr auto advance_connection_generation(std::uint32_t& generation) noexcept
    -> bool {
    if (generation == 0U) {
        return false;
    }
    if (generation == std::numeric_limits<std::uint32_t>::max()) {
        generation = 0U;
        return false;
    }
    ++generation;
    return true;
}

} // namespace glifistore::server
