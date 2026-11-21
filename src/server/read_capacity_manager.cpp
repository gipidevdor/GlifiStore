#include "glifistore/server/read_capacity_manager.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <thread>
#include <utility>

namespace glifistore::server {
namespace {
[[nodiscard]] auto cpu_ns(const clockid_t clock) noexcept -> std::uint64_t {
    timespec time{};
    if (::clock_gettime(clock, &time) != 0 || time.tv_sec < 0 || time.tv_nsec < 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(time.tv_sec) * 1'000'000'000U +
           static_cast<std::uint64_t>(time.tv_nsec);
}
} // namespace

auto read_thread_cpu_ns() noexcept -> std::uint64_t {
    return cpu_ns(CLOCK_THREAD_CPUTIME_ID);
}

auto read_control_time_ms() noexcept -> std::uint64_t {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

ReadCapacityManager::ReadCapacityManager(const std::size_t owners, ReadHelperConfig config,
                                         std::unique_ptr<ReadHelperPool> pool)
    : config_(std::move(config)), pool_(std::move(pool)), controller_(owners, config_.maximum_helpers),
      observations_(owners), helper_cpu_ns_(owners) {}

auto ReadCapacityManager::create(const std::size_t owners, const ReadHelperConfig& config)
    -> Result<std::unique_ptr<ReadCapacityManager>> {
    if (owners == 0 || owners > kMaximumReadOwners || config.maximum_helpers > kMaximumReadHelpers ||
        (!config.profiles.empty() && config.profiles.size() != owners) ||
        (config.fixed_owner && (*config.fixed_owner >= owners || !config.profiles.empty()))) {
        return fail(ErrorCode::invalid_argument, "invalid read helper configuration");
    }
    auto pool = ReadHelperPool::create(config.maximum_helpers);
    if (!pool) {
        return unexpected(pool.error());
    }
    return std::make_unique<ReadCapacityManager>(owners, config, std::move(*pool));
}

auto ReadCapacityManager::start() -> Status {
    if (config_.fixed_owner) {
        for (std::size_t index = 0; index < pool_->size(); ++index) {
            if (!pool_->assign(index, *config_.fixed_owner)) {
                return fail(ErrorCode::internal_error, "fixed helper assignment failed");
            }
        }
    }
    return pool_->start();
}

void ReadCapacityManager::stop() noexcept {
    pool_->stop();
}

auto ReadCapacityManager::stats() const -> ReadCapacityControlStats {
    const std::lock_guard lock{mutex_};
    auto result = stats_;
    if (config_.fixed_owner) {
        result.active = pool_->size();
        result.occupied = pool_->size();
        result.soft_target = pool_->size();
    } else {
        for (const auto& helper : controller_.allocations()) {
            result.occupied += helper.state != ReadHelperState::parked ? 1U : 0U;
            result.arming += helper.state == ReadHelperState::arming ? 1U : 0U;
            result.active += helper.state == ReadHelperState::active ? 1U : 0U;
            result.draining += helper.state == ReadHelperState::draining ? 1U : 0U;
        }
    }
    return result;
}

void ReadCapacityManager::observe(const std::size_t owner, ReadCapacityObservation observation,
                                  const std::uint64_t helper_cpu_ns) {
    if (config_.fixed_owner || owner >= observations_.size()) {
        return;
    }
    const std::lock_guard lock{mutex_};
    observation.profile = config_.profiles.empty() ? ReadCapacityProfile{} : config_.profiles[owner];
    observations_[owner] = observation;
    helper_cpu_ns_[owner] = helper_cpu_ns;
    const auto now_ms = observation.end_ms;
    if (now_ms <= last_step_ms_ || now_ms - last_step_ms_ < 50U) {
        return;
    }
    const auto process = cpu_ns(CLOCK_PROCESS_CPUTIME_ID);
    std::uint64_t helpers{};
    for (const auto value : helper_cpu_ns_) {
        helpers += value;
    }
    std::size_t target{};
    if (last_step_ms_ != 0 && process >= process_cpu_ns_ && helpers >= previous_helper_cpu_ns_) {
        const auto interval_ns = static_cast<double>(now_ms - last_step_ms_) * 1'000'000.0;
        const auto total = static_cast<double>(process - process_cpu_ns_);
        const auto helper = static_cast<double>(helpers - previous_helper_cpu_ns_);
        const auto other_cpu = std::max(0.0, total - helper) / interval_ns;
        const auto limit = config_.cpu_limit == 0 ? std::thread::hardware_concurrency() : config_.cpu_limit;
        const auto spare = std::max(0.0, static_cast<double>(limit) - other_cpu - 0.5);
        target = static_cast<std::size_t>(std::min(spare, static_cast<double>(config_.maximum_helpers)));
    }
    last_step_ms_ = now_ms;
    process_cpu_ns_ = process;
    previous_helper_cpu_ns_ = helpers;
    std::array<ReadHelperState, kMaximumReadHelpers> before{};
    for (std::size_t index = 0; index < pool_->size(); ++index) {
        before[index] = controller_.allocations()[index].state;
    }
    static_cast<void>(controller_.step(now_ms, target, observations_));
    ++stats_.steps;
    stats_.soft_target = target;
    for (std::size_t index = 0; index < pool_->size(); ++index) {
        const auto state = controller_.allocations()[index];
        if (state.state == ReadHelperState::arming && before[index] != state.state) {
            ++stats_.grants;
        }
        if (state.state == ReadHelperState::draining && before[index] != state.state) {
            ++stats_.retirements;
        }
        if (state.state == ReadHelperState::arming && pool_->assign(index, state.owner)) {
            static_cast<void>(controller_.acknowledge_active(index, now_ms));
        } else if (state.state == ReadHelperState::draining && pool_->park(index)) {
            static_cast<void>(controller_.acknowledge_parked(index));
        }
    }
}

} // namespace glifistore::server
