/**
 * @file
 * @brief Pipeline visibility barriers between mutations and dependent observations.
 */
#pragma once

// Daemon mutation windows + GET visibility barrier (ADR 0037 Phase C).
// Normative: docs/adr/0037-shard-execution-token-flat-combining.md

#include <cstdint>

namespace glifistore::server {

/// Contiguous Store mutations before a GET (or other barrier) form one window.
/// Windows are capped at the Writer publication chunk (≤32). Not a multi-key txn.
inline constexpr std::uint32_t kMaximumMutationWindow = 32;

/**
 * @brief Classify operations that mutate visibility or require a preceding mutation barrier.
 */
enum class PipelineOpClass : std::uint8_t {
    mutation, // PUT / ERASE
    barrier,  // GET and other ops that require prior mutations' visibility
    other,
};

/// After a mutation window is submitted, GET must not adopt a generation older
/// than `required_visible_epoch` (RAW). Zero means no barrier armed.
struct MutationVisibilityBarrier final {
    std::uint64_t required_visible_epoch{};

    [[nodiscard]] constexpr auto armed() const noexcept -> bool {
        return required_visible_epoch != 0;
    }

    [[nodiscard]] constexpr auto allows(const std::uint64_t visible_epoch) const noexcept -> bool {
        return !armed() || visible_epoch >= required_visible_epoch;
    }

    constexpr void clear() noexcept {
        required_visible_epoch = 0;
    }

    constexpr void raise_to(const std::uint64_t epoch) noexcept {
        if (epoch > required_visible_epoch) {
            required_visible_epoch = epoch;
        }
    }
};

} // namespace glifistore::server
