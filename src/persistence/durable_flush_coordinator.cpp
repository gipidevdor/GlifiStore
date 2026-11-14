#include "glifistore/persistence/durable_flush_coordinator.hpp"

#include <algorithm>
#include <limits>
#include <new>

namespace glifistore {

DurableFlushCoordinator::DurableFlushCoordinator(const std::uint32_t sync_interval_ms,
                                                 const std::uint32_t batch_max_wait_ms,
                                                 const bool periodic_sync_enabled,
                                                 const bool batch_timer_enabled, FlushCallback flush_callback)
    : sync_interval_ms_(sync_interval_ms), batch_max_wait_ms_(batch_max_wait_ms),
      periodic_sync_enabled_(periodic_sync_enabled), batch_timer_enabled_(batch_timer_enabled),
      flush_callback_(std::move(flush_callback)), worker_([this] { run(); }) {}

DurableFlushCoordinator::~DurableFlushCoordinator() {
    stop();
}

void DurableFlushCoordinator::request_flush() {
    {
        std::lock_guard lock{mutex_};
        if (stopped_) {
            return;
        }
        flush_requested_ = true;
    }
    wake_.notify_one();
}

void DurableFlushCoordinator::request_flush_all() {
    {
        std::lock_guard lock{mutex_};
        if (stopped_) {
            return;
        }
        flush_all_requested_ = true;
    }
    wake_.notify_one();
}

void DurableFlushCoordinator::request_flush_at(const std::chrono::steady_clock::time_point deadline) {
    {
        std::lock_guard lock{mutex_};
        if (stopped_ || (requested_deadline_ && *requested_deadline_ <= deadline)) {
            return;
        }
        requested_deadline_ = deadline;
        deadline_changed_ = true;
    }
    wake_.notify_one();
}

void DurableFlushCoordinator::notify_batch_activity() {
    // Wake the coordinator so it can observe stop/forced-flush state without
    // moving the absolute timer deadline. Activity alone is not a flush
    // request: otherwise every append degenerates into an immediate fsync.
    wake_.notify_one();
}

auto DurableFlushCoordinator::flush_all_blocking() -> Status {
    if (!flush_callback_) {
        return {};
    }
    const std::lock_guard call_lock{flush_all_call_mutex_};
    std::unique_lock lock{mutex_};
    if (stopped_) {
        if (background_error_) {
            return unexpected(*background_error_);
        }
        return fail(ErrorCode::unavailable, "durable flush coordinator is stopped");
    }
    if (flush_all_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        return fail(ErrorCode::arithmetic_overflow, "durable flush generation is exhausted");
    }
    const auto generation = ++flush_all_generation_;
    flush_all_requested_ = true;
    wake_.notify_one();
    completed_.wait(lock, [&] { return completed_generation_ >= generation || stopped_; });
    if (completed_generation_ < generation) {
        if (background_error_) {
            return unexpected(*background_error_);
        }
        return fail(ErrorCode::unavailable, "durable flush coordinator stopped before completion");
    }
    if (background_error_) {
        return unexpected(*background_error_);
    }
    return {};
}

void DurableFlushCoordinator::stop() {
    const std::lock_guard stop_lock{stop_mutex_};
    if (worker_.joinable()) {
        {
            std::lock_guard lock{mutex_};
            stopped_ = true;
        }
        wake_.notify_all();
        completed_.notify_all();
        worker_.join();
    }
}

void DurableFlushCoordinator::run() {
    using clock = std::chrono::steady_clock;
    using duration = std::chrono::milliseconds;
    const auto batch_wait = batch_timer_enabled_ ? duration{batch_max_wait_ms_} : duration::max();
    const auto sync_wait = periodic_sync_enabled_ ? duration{sync_interval_ms_} : duration::max();
    const auto interval = std::min(batch_wait, sync_wait);
    auto next_deadline = clock::now() + interval;

    for (;;) {
        bool periodic_timed_out = false;
        bool requested_deadline_timed_out = false;
        bool forced = false;
        bool force_all = false;
        std::uint64_t flush_all_generation = 0;
        {
            std::unique_lock lock{mutex_};
            const auto wait_deadline =
                requested_deadline_ ? std::min(next_deadline, *requested_deadline_) : next_deadline;
            wake_.wait_until(lock, wait_deadline, [&] {
                return stopped_ || flush_requested_ || flush_all_requested_ || deadline_changed_;
            });
            if (stopped_) {
                break;
            }
            if (deadline_changed_) {
                deadline_changed_ = false;
                continue;
            }
            const auto now = clock::now();
            periodic_timed_out = now >= next_deadline;
            requested_deadline_timed_out = requested_deadline_ && now >= *requested_deadline_;
            if (requested_deadline_timed_out) {
                requested_deadline_.reset();
            }
            forced = flush_requested_;
            force_all = flush_all_requested_;
            flush_all_generation = flush_all_generation_;
            flush_requested_ = false;
            flush_all_requested_ = false;
            if (forced || force_all) {
                requested_deadline_.reset();
            }
        }
        if (!periodic_timed_out && !requested_deadline_timed_out && !forced && !force_all) {
            continue;
        }
        if (periodic_timed_out) {
            next_deadline = clock::now() + interval;
        }
        Status flushed;
        try {
            flushed = flush_callback_(force_all);
        } catch (const std::bad_alloc&) {
            flushed = unexpected(Error{ErrorCode::resource_exhausted, {}});
        } catch (...) {
            flushed = unexpected(Error{ErrorCode::internal_error, {}});
        }
        if (!flushed) {
            auto error = std::move(flushed.error());
            std::lock_guard lock{mutex_};
            background_error_.emplace(std::move(error));
            if (force_all) {
                completed_generation_ = flush_all_generation;
            }
            stopped_ = true;
            completed_.notify_all();
            return;
        }
        if (force_all) {
            std::lock_guard lock{mutex_};
            completed_generation_ = flush_all_generation;
            completed_.notify_all();
        }
    }
}

} // namespace glifistore
