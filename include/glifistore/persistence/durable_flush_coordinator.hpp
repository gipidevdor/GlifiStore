/**
 * @file
 * @brief Periodic/group durable flush scheduling and per-Worker batch coordination.
 */
#pragma once

#include "glifistore/core/error.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace glifistore {

namespace detail {
class DurableFlushCoordinatorAccess;
}

/**
 * @brief Own the periodic/group timer thread and serialize explicit blocking flush completion; the callback
 * captures state that must outlive stop().
 */
class DurableFlushCoordinator final {
  public:
    using FlushCallback = std::function<Status(bool force_all)>;

    DurableFlushCoordinator(std::uint32_t sync_interval_ms, std::uint32_t batch_max_wait_ms,
                            bool periodic_sync_enabled, bool batch_timer_enabled,
                            FlushCallback flush_callback);
    ~DurableFlushCoordinator();

    DurableFlushCoordinator(const DurableFlushCoordinator&) = delete;
    auto operator=(const DurableFlushCoordinator&) -> DurableFlushCoordinator& = delete;

    void request_flush();
    void request_flush_all();
    void request_flush_at(std::chrono::steady_clock::time_point deadline);
    void notify_batch_activity();
    [[nodiscard]] auto flush_all_blocking() -> Status;
    void stop();

  private:
    void run();

    std::uint32_t sync_interval_ms_;
    std::uint32_t batch_max_wait_ms_;
    bool periodic_sync_enabled_;
    bool batch_timer_enabled_;
    FlushCallback flush_callback_;
    std::mutex flush_all_call_mutex_;
    std::mutex stop_mutex_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable completed_;
    bool flush_requested_{};
    bool flush_all_requested_{};
    bool deadline_changed_{};
    bool stopped_{};
    std::optional<std::chrono::steady_clock::time_point> requested_deadline_;
    std::uint64_t flush_all_generation_{};
    std::uint64_t completed_generation_{};
    std::optional<Error> background_error_;
    std::thread worker_;

    friend class detail::DurableFlushCoordinatorAccess;
};

} // namespace glifistore
