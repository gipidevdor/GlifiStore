/**
 * @file
 * @brief Best-effort executor affinity with observable platform support.
 */
#pragma once

#include <cstddef>
#include <string_view>

namespace glifistore::server {

/**
 * @brief Observed affinity support and enforcement strength on the current platform.
 */
enum class ExecutorAffinityMode { disabled, unavailable, advisory, pinned };

/**
 * @brief Observed affinity support/outcome and selected CPU, with -1 meaning no CPU selection.
 */
struct ExecutorAffinityResult {
    ExecutorAffinityMode mode{ExecutorAffinityMode::disabled};
    int cpu{-1};
};

[[nodiscard]] auto configure_executor_thread(std::size_t executor_id, bool affinity_requested) noexcept
    -> ExecutorAffinityResult;
[[nodiscard]] auto affinity_mode_name(ExecutorAffinityMode mode) noexcept -> std::string_view;

} // namespace glifistore::server
