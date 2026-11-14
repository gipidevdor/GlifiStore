/**
 * @file
 * @brief Host topology observations and bounded startup Worker-count selection.
 */
#pragma once

#include "glifistore/store/config.hpp"

#include <cstddef>

namespace glifistore {

/**
 * @brief Detected CPU availability and memory observations used by automatic startup sizing.
 */
struct WorkerTopology {
    std::size_t logical_cpus{1};
    std::size_t physical_cores{1};
    std::size_t available_cpus{1};
    std::size_t available_memory_bytes{};
};

/**
 * @brief Pure Worker-count selection from explicit configuration and bounded topology inputs.
 */
class WorkerCountPolicy final {
  public:
    /**
     * @brief Apply explicit-count or CPU/memory policy, always retaining at least one Worker.
     */
    [[nodiscard]] static auto choose(const WorkerTopology& topology, const WorkerCountConfig& config) noexcept
        -> std::size_t;
};

/**
 * @brief Observe supported host topology; unavailable probes retain conservative defaults.
 */
[[nodiscard]] auto detect_worker_topology() noexcept -> WorkerTopology;

} // namespace glifistore
