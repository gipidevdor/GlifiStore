/**
 * @file
 * @brief Periodic shared CPU-budget adapter for the deterministic controller and helper pool.
 */
#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/server/read_capacity_controller.hpp"
#include "glifistore/server/read_helper_pool.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace glifistore::server {

struct ReadHelperConfig final {
    std::size_t maximum_helpers{}; ///< Zero disables helper threads and control sampling.
    std::size_t cpu_limit{};       ///< Zero uses hardware_concurrency; an estimate, not a CPU reservation.
    std::vector<ReadCapacityProfile> profiles{}; ///< One qualified profile per logical owner.
    /// Explicit fixed-allocation experiment. No adaptive-capacity claim; never set implicitly.
    std::optional<std::size_t> fixed_owner{};
};

[[nodiscard]] auto read_thread_cpu_ns() noexcept -> std::uint64_t;
[[nodiscard]] auto read_control_time_ms() noexcept -> std::uint64_t;

/// Mutex only on periodic observations, never GET admission/completion. The soft CPU
/// target uses whole-process CPU consumption (including Writers/flush/maintenance)
/// and subtracts measured helper CPU, reserving half a CPU as headroom. Workload and
/// threshold qualification remain external, explicit inputs.
struct ReadCapacityControlStats final {
    std::uint64_t steps{};
    std::size_t soft_target{};
    std::size_t occupied{};
    std::size_t arming{};
    std::size_t active{};
    std::size_t draining{};
    std::uint64_t grants{};
    std::uint64_t retirements{};
};

class ReadCapacityManager final {
  public:
    [[nodiscard]] static auto create(std::size_t owners, const ReadHelperConfig& config)
        -> Result<std::unique_ptr<ReadCapacityManager>>;
    ReadCapacityManager(std::size_t owners, ReadHelperConfig config, std::unique_ptr<ReadHelperPool> pool);
    [[nodiscard]] auto start() -> Status;
    void stop() noexcept;
    [[nodiscard]] auto stats() const -> ReadCapacityControlStats;
    void observe(std::size_t owner, ReadCapacityObservation observation, std::uint64_t helper_cpu_ns);

    [[nodiscard]] auto pool() noexcept -> ReadHelperPool& {
        return *pool_;
    }

  private:
    ReadHelperConfig config_;
    std::unique_ptr<ReadHelperPool> pool_;
    ReadCapacityController controller_;
    std::vector<ReadCapacityObservation> observations_;
    std::vector<std::uint64_t> helper_cpu_ns_;
    mutable std::mutex mutex_;
    ReadCapacityControlStats stats_{};
    std::uint64_t last_step_ms_{};
    std::uint64_t process_cpu_ns_{};
    std::uint64_t previous_helper_cpu_ns_{};
};

} // namespace glifistore::server
