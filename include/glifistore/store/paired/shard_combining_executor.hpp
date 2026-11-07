/**
 * @file
 * @brief Serial shard execution token shared by embedded mutation callers.
 */
#pragma once

// ADR 0037 Phase A+B: per-shard execution token and lost-wakeup-safe release.
// Normative: docs/adr/0037-shard-execution-token-flat-combining.md

#include <atomic>
#include <cstdint>

namespace glifistore::store::paired {

/**
 * @brief Execution ownership token serializing one shard across embedded combiner callers.
 */
enum class ShardExecutionToken : std::uint32_t {
    idle = 0,
    executing = 1,
};

[[nodiscard]] inline auto execution_token_idle() noexcept -> std::uint32_t {
    return static_cast<std::uint32_t>(ShardExecutionToken::idle);
}

[[nodiscard]] inline auto execution_token_executing() noexcept -> std::uint32_t {
    return static_cast<std::uint32_t>(ShardExecutionToken::executing);
}

/// IDLE → EXECUTING. Returns true when this caller holds the token.
[[nodiscard]] inline auto try_acquire_execution_token(std::atomic<std::uint32_t>& token) noexcept -> bool {
    auto expected = execution_token_idle();
    return token.compare_exchange_strong(expected, execution_token_executing(), std::memory_order_acq_rel,
                                         std::memory_order_acquire);
}

inline void release_execution_token(std::atomic<std::uint32_t>& token) noexcept {
    token.store(execution_token_idle(), std::memory_order_release);
}

/// After EXECUTING → IDLE, re-acquire if `pending_work` is true (lost-wakeup-safe).
/// Returns true when this caller holds the token again.
[[nodiscard]] inline auto try_reacquire_execution_token_if_pending(std::atomic<std::uint32_t>& token,
                                                                   const bool pending_work) noexcept -> bool {
    if (!pending_work) {
        return false;
    }
    return try_acquire_execution_token(token);
}

} // namespace glifistore::store::paired
