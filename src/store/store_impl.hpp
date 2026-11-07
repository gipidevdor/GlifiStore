#pragma once

#include "glifistore/core/fault_injection.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/core/types.hpp"
#include "glifistore/persistence/bootstrap.hpp"
#include "glifistore/persistence/resource_limits.hpp"
#include "glifistore/persistence/runtime_catalog.hpp"
#include "glifistore/segment/global_manager.hpp"
#include "glifistore/segment/record.hpp"
#include "glifistore/store/config.hpp"
#include "glifistore/store/maintenance.hpp"
#include "glifistore/store/maintenance_types.hpp"
#include "glifistore/store/paired/shard_pair_runtime.hpp"
#include "glifistore/store/prepared_read.hpp"
#include "glifistore/store/store.hpp"
#include "glifistore/store/value.hpp"
#include "glifistore/worker/pool.hpp"
#include "glifistore/worker/topology.hpp"
#include "glifistore/worker/worker.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace glifistore {
namespace store_detail {

[[nodiscard]] inline auto as_string_view(const std::span<const std::byte> bytes) noexcept
    -> std::string_view {
    if (bytes.empty()) {
        return {};
    }
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] inline auto copy_value(const RecordView& record) -> OwnedValue {
    return OwnedValue::from_bytes(record.value, record.sequence.value, record.expire_at_ns);
}

inline auto durable_status(DurableMutationResult result) -> Status {
    if (result.committed() && !result.error) {
        return {};
    }
    auto error =
        result.error ? std::move(*result.error) : Error{ErrorCode::io_error, "durable mutation failed"};
    if (result.committed()) {
        error.code = ErrorCode::unavailable;
    }
    return unexpected(std::move(error));
}

inline auto public_compaction_result(const std::size_t worker_index, DurableCompactionResult result)
    -> Result<CompactionResult> {
    if (result.outcome == DurableCompactionOutcome::not_beneficial) {
        return CompactionResult{
            .compacted = false,
            .worker_index = worker_index,
            .source_records_verified = result.stats.source_index_records_verified,
            .source_bytes_verified = result.stats.source_bytes_verified,
            .records_copied = 0,
            .bytes_copied = 0,
            .expired_records_dropped = result.stats.expired_records_dropped,
            .pre_intent_duration_ns = result.stats.pre_intent_duration_ns,
            .publication_lease_duration_ns = result.stats.publication_lease_duration_ns,
            .pacing_delay_ns = result.stats.pacing_delay_ns,
            .pacing_sleep_count = result.stats.pacing_sleep_count,
            .pacing_burst_bytes = result.stats.pacing_burst_bytes,
            .transient_metadata_lower_bound_bytes = result.stats.transient_metadata_lower_bound_bytes,
        };
    }
    if (!result.compacted()) {
        auto error =
            result.error ? std::move(*result.error) : Error{ErrorCode::io_error, "durable compaction failed"};
        if (result.outcome == DurableCompactionOutcome::recovery_required) {
            error.code = ErrorCode::unavailable;
        }
        return unexpected(std::move(error));
    }
    return CompactionResult{
        .compacted = true,
        .worker_index = worker_index,
        .source_records_verified = result.stats.source_index_records_verified,
        .source_bytes_verified = result.stats.source_bytes_verified,
        .records_copied = result.stats.records_copied,
        .bytes_copied = result.stats.bytes_copied,
        .expired_records_dropped = result.stats.expired_records_dropped,
        .pre_intent_duration_ns = result.stats.pre_intent_duration_ns,
        .publication_lease_duration_ns = result.stats.publication_lease_duration_ns,
        .pacing_delay_ns = result.stats.pacing_delay_ns,
        .pacing_sleep_count = result.stats.pacing_sleep_count,
        .pacing_burst_bytes = result.stats.pacing_burst_bytes,
        .transient_metadata_lower_bound_bytes = result.stats.transient_metadata_lower_bound_bytes,
    };
}

inline auto public_compaction_result(const std::size_t worker_index, const VacuumStats& stats)
    -> CompactionResult {
    return CompactionResult{
        .compacted = true,
        .worker_index = worker_index,
        .source_records_verified = stats.source_records_verified,
        .source_bytes_verified = stats.source_bytes_verified,
        .records_copied = stats.records_copied,
        .bytes_copied = stats.bytes_copied,
        .expired_records_dropped = stats.expired_records_dropped,
    };
}

[[nodiscard]] inline auto resource_exhausted() -> Unexpected {
    return unexpected(Error{ErrorCode::resource_exhausted, {}});
}

[[nodiscard]] inline auto internal_failure() -> Unexpected {
    return unexpected(Error{ErrorCode::internal_error, {}});
}

[[nodiscard]] inline auto closed_store() -> Unexpected {
    return unexpected(Error{ErrorCode::unavailable, {}});
}

[[nodiscard]] inline auto reject_if_maintenance_emergency(MaintenanceController* controller) -> Status {
    if (controller != nullptr && controller->mutations_rejected()) {
        return fail(ErrorCode::storage_exhausted, std::string{kMaintenanceEmergencyMutationMessage});
    }
    return {};
}

inline auto data_directory_mode(const DurableOpenMode mode) noexcept -> DataDirectoryOpenMode {
    switch (mode) {
    case DurableOpenMode::open_existing:
        return DataDirectoryOpenMode::existing;
    case DurableOpenMode::create_new:
        return DataDirectoryOpenMode::create_new;
    case DurableOpenMode::open_or_create:
        return DataDirectoryOpenMode::open_or_create;
    }
    return DataDirectoryOpenMode::existing;
}

[[nodiscard]] inline auto validate_batch_config(const DurableGroupConfig& batch) -> Status {
    if (batch.max_records == 0 || batch.max_bytes == 0 || batch.max_wait_ms == 0 || batch.min_records == 0) {
        return fail(ErrorCode::invalid_argument,
                    "durable batching requires record, byte, and wait limits greater than zero");
    }
    if (batch.min_records > batch.max_records) {
        return fail(ErrorCode::invalid_argument,
                    "durable batching requires min_records no greater than max_records");
    }
    return {};
}

[[nodiscard]] inline auto system_time_ns() noexcept -> std::uint64_t {
    using namespace std::chrono;
    const auto elapsed = system_clock::now().time_since_epoch();
    if (elapsed <= system_clock::duration::zero()) {
        return 0;
    }

    const auto whole_seconds = duration_cast<seconds>(elapsed);
    const auto fractional_ns = duration_cast<nanoseconds>(elapsed - whole_seconds);
    constexpr auto kNanosecondsPerSecond = std::uint64_t{1'000'000'000};
    constexpr auto kMaximumSeconds = std::numeric_limits<std::uint64_t>::max() / kNanosecondsPerSecond;
    const auto seconds_count = static_cast<std::uint64_t>(whole_seconds.count());
    if (seconds_count > kMaximumSeconds) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    const auto base = seconds_count * kNanosecondsPerSecond;
    const auto fraction = static_cast<std::uint64_t>(fractional_ns.count());
    if (fraction > std::numeric_limits<std::uint64_t>::max() - base) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return base + fraction;
}

[[nodiscard]] inline auto sample_clock(const std::shared_ptr<const StoreClock>& clock) noexcept
    -> std::uint64_t {
    return clock ? clock->now_ns() : system_time_ns();
}

} // namespace store_detail

struct VolatileStoreRuntime {
    VolatileStoreRuntime(const SegmentId first_segment_id, const std::size_t worker_count,
                         const WorkerRoutingState routing)
        : segment_manager(first_segment_id), workers(segment_manager, worker_count, routing) {}

    GlobalSegmentManager segment_manager;
    WorkerPool workers;
};

struct Store::Impl {
    enum class LifecycleState : std::uint8_t { open, closing, closed };

    // 128 bytes covers the widest cache line of the currently supported desktop/server targets.
    // Keeping admission counters on separate lines prevents unrelated Workers from contending on
    // Store lifecycle bookkeeping in the steady-state data path.
    struct alignas(128) ActiveOperationCounter {
        std::atomic<std::size_t> state{};
    };

    static constexpr auto kAdmissionClosed = std::size_t{1}
                                             << (std::numeric_limits<std::size_t>::digits - 1U);
    static constexpr auto kAdmissionCountMask = kAdmissionClosed - 1U;

    class OperationGuard final {
      public:
        OperationGuard(Impl& impl, const std::size_t shard) noexcept
            : impl_(impl.begin_operation(shard) ? &impl : nullptr), shard_(shard) {}

        ~OperationGuard() {
            if (impl_ != nullptr) {
                impl_->finish_operation(shard_);
            }
        }

        OperationGuard(const OperationGuard&) = delete;
        auto operator=(const OperationGuard&) -> OperationGuard& = delete;

        explicit operator bool() const noexcept {
            return impl_ != nullptr;
        }

      private:
        Impl* impl_{};
        std::size_t shard_{};
    };

    explicit Impl(std::unique_ptr<VolatileStoreRuntime> runtime, const WorkerRoutingState routing_state,
                  std::shared_ptr<const StoreClock> store_clock, const std::uint64_t initial_now_ns)
        : worker_count_value(runtime->workers.size()), routing(routing_state),
          active_operations(std::make_unique<ActiveOperationCounter[]>(worker_count_value + 1)),
          volatile_runtime(std::move(runtime)), clock(std::move(store_clock)), latest_now_ns(initial_now_ns) {
    }

    explicit Impl(std::unique_ptr<DurableRuntimeCatalog> runtime,
                  std::shared_ptr<const StoreClock> store_clock, const std::uint64_t initial_now_ns)
        : worker_count_value(runtime->worker_count()), routing(runtime->worker_routing()),
          active_operations(std::make_unique<ActiveOperationCounter[]>(worker_count_value + 1)),
          durable_runtime(std::move(runtime)), clock(std::move(store_clock)), latest_now_ns(initial_now_ns) {}

    [[nodiscard]] auto control_shard() const noexcept -> std::size_t {
        return worker_count_value;
    }

    [[nodiscard]] auto begin_operation(const std::size_t shard) noexcept -> bool {
        // The RMW reads its immediate predecessor in this shard's modification order. It therefore
        // either precedes close_admission() and is counted, or follows it and observes the closed bit.
        const auto previous = active_operations[shard].state.fetch_add(1, std::memory_order_relaxed);
        if ((previous & kAdmissionClosed) == 0) {
            return true;
        }
        finish_operation(shard);
        return false;
    }

    void finish_operation(const std::size_t shard) noexcept {
        const auto previous = active_operations[shard].state.fetch_sub(1, std::memory_order_acq_rel);
        if ((previous & kAdmissionClosed) != 0 && (previous & kAdmissionCountMask) == 1U) {
            active_operations[shard].state.notify_all();
        }
    }

    void close_admission() noexcept {
        lock_admission_fence_state();
        if (admission_fence_count++ == 0) {
            // Each RMW is totally ordered with admission on that shard. Completion of this loop is the
            // close linearization point: every later attempt observes its shard's closed bit.
            for (std::size_t shard = 0; shard <= worker_count_value; ++shard) {
                active_operations[shard].state.fetch_or(kAdmissionClosed, std::memory_order_relaxed);
            }
        }
        unlock_admission_fence_state();
    }

    void resume_admission_if_open() noexcept {
        lock_admission_fence_state();
        if (admission_fence_count == 0) {
            unlock_admission_fence_state();
            return;
        }
        --admission_fence_count;
        if (admission_fence_count == 0 && lifecycle.load(std::memory_order_acquire) == LifecycleState::open) {
            for (std::size_t shard = 0; shard <= worker_count_value; ++shard) {
                active_operations[shard].state.fetch_and(~kAdmissionClosed, std::memory_order_release);
            }
        }
        unlock_admission_fence_state();
    }

    // nullopt waits unbounded. A set deadline (milliseconds) is a liveness bound:
    // if admitted work remains after the deadline, close surfaces unavailable
    // rather than hanging forever (GS-CONCUR-LIVE-001).
    [[nodiscard]] auto
    wait_for_active_operations(const std::optional<std::uint32_t> deadline_ms = std::nullopt) const noexcept
        -> bool {
        // Waiting directly on each atomic closes the check-to-sleep race of a
        // condition variable whose predicate is not protected by its mutex.
        // finish_operation() notifies the transition of a closed shard to
        // zero, so no completion can be lost between load and wait.
        const auto deadline_at =
            deadline_ms.has_value()
                ? std::optional{std::chrono::steady_clock::now() + std::chrono::milliseconds{*deadline_ms}}
                : std::nullopt;
        for (std::size_t shard = 0; shard <= worker_count_value; ++shard) {
            auto observed = active_operations[shard].state.load(std::memory_order_acquire);
            while ((observed & kAdmissionCountMask) != 0) {
                if (deadline_at.has_value()) {
                    if (std::chrono::steady_clock::now() >= *deadline_at) {
                        return false;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                } else {
                    active_operations[shard].state.wait(observed, std::memory_order_acquire);
                }
                observed = active_operations[shard].state.load(std::memory_order_acquire);
            }
        }
        return true;
    }

    void mark_durable_fail_closed() noexcept {
        OperationGuard operation{*this, control_shard()};
        if (operation && durable_runtime) {
            durable_runtime->mark_fail_closed();
        }
    }

    [[nodiscard]] auto close_status_locked() const -> Status {
        if (!close_error) {
            return {};
        }
        try {
            return unexpected(*close_error);
        } catch (const std::bad_alloc&) {
            return store_detail::resource_exhausted();
        } catch (...) {
            return store_detail::internal_failure();
        }
    }

    [[nodiscard]] auto operational() const noexcept -> bool {
        if (lifecycle.load(std::memory_order_acquire) != LifecycleState::open) {
            return false;
        }
        return !durable_runtime || durable_runtime->healthy();
    }

    // True while new begin_operation admissions are accepted. Online backup fences
    // this independently of durable catalog health (READY must observe the fence).
    [[nodiscard]] auto admissions_open() const noexcept -> bool {
        if (lifecycle.load(std::memory_order_acquire) != LifecycleState::open) {
            return false;
        }
        // close_admission stamps every shard; shard 0 is sufficient to observe the fence.
        return (active_operations[0].state.load(std::memory_order_acquire) & kAdmissionClosed) == 0;
    }

    [[nodiscard]] auto close() -> Status {
        auto expected = LifecycleState::open;
        if (!lifecycle.compare_exchange_strong(expected, LifecycleState::closing, std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
            while (expected != LifecycleState::closed) {
                lifecycle.wait(expected, std::memory_order_acquire);
                expected = lifecycle.load(std::memory_order_acquire);
            }
            const std::lock_guard lock{lifecycle_mutex};
            return close_status_locked();
        }

        struct CloseFinalizer {
            Impl* impl;

            ~CloseFinalizer() {
                if (impl == nullptr) {
                    return;
                }
                try {
                    const std::lock_guard lock{impl->lifecycle_mutex};
                    if (!impl->close_error) {
                        impl->close_error.emplace(Error{ErrorCode::internal_error, {}});
                    }
                    impl->lifecycle.store(LifecycleState::closed, std::memory_order_release);
                } catch (...) {
                    impl->lifecycle.store(LifecycleState::closed, std::memory_order_release);
                }
                impl->lifecycle.notify_all();
            }
        } finalizer{this};

        // Drain paired Writers while Store admission is still open so exclusive
        // StoreAccess mutations nested under the Writer can still admit.
        Status result;
        GS_FAULT_SITE(close);
        if (pair_runtime) {
            std::optional<std::chrono::milliseconds> drain_deadline;
            if (close_drain_deadline_ms.has_value()) {
                drain_deadline = std::chrono::milliseconds{*close_drain_deadline_ms};
            }
            auto drained = pair_runtime->stop_and_drain(drain_deadline);
            if (!drained) {
                result = std::move(drained);
            }
        }

        close_admission();
        // Stop new maintenance evaluations, then drain admitted ops (including compact),
        // then join the controller — matching DurableFlushCoordinator shutdown ordering.
        if (maintenance) {
            maintenance->request_stop();
        }
        try {
            if (durable_runtime) {
                durable_runtime->request_close_flush();
            }
        } catch (const std::bad_alloc&) {
            if (result) {
                result = store_detail::resource_exhausted();
            }
        } catch (...) {
            if (result) {
                result = store_detail::internal_failure();
            }
        }
        if (!result && durable_runtime) {
            // Release any strict-group producer even when posting the close flush itself failed.
            durable_runtime->mark_fail_closed();
        }
        if (!wait_for_active_operations(close_drain_deadline_ms)) {
            if (result) {
                result = fail(ErrorCode::unavailable, "close admission drain deadline exceeded");
            }
        }
        if (maintenance) {
            maintenance->join();
        }
        // All Store operations and maintenance are quiescent. The daemon also
        // joins Reactor and cold-I/O workers before entering Store::close().
        // Revoke raw generation adoption and release generation/file pins while
        // retaining the paired runtime's scalar post-stop telemetry.
        if (pair_runtime) {
            auto finalized = pair_runtime->finalize_reader_shutdown();
            if (result && !finalized) {
                result = std::move(finalized);
            }
        }

        try {
            if (durable_runtime) {
                auto closed = durable_runtime->close();
                if (result && !closed) {
                    result = std::move(closed);
                }
            }
        } catch (const std::bad_alloc&) {
            if (result) {
                result = store_detail::resource_exhausted();
            }
        } catch (...) {
            if (result) {
                result = store_detail::internal_failure();
            }
        }
        durable_runtime.reset();
        volatile_runtime.reset();
        maintenance.reset();
        clock.reset();

        Status final_status;
        {
            const std::lock_guard lock{lifecycle_mutex};
            if (!result) {
                close_error.emplace(std::move(result.error()));
            }
            lifecycle.store(LifecycleState::closed, std::memory_order_release);
            final_status = close_status_locked();
        }
        finalizer.impl = nullptr;
        lifecycle.notify_all();
        return final_status;
    }

    [[nodiscard]] auto now_ns() noexcept -> std::uint64_t {
        const auto sampled = store_detail::sample_clock(clock);
        auto observed = latest_now_ns.load(std::memory_order_relaxed);
        while (observed < sampled &&
               !latest_now_ns.compare_exchange_weak(observed, sampled, std::memory_order_relaxed,
                                                    std::memory_order_relaxed)) {
        }
        return std::max(observed, sampled);
    }

    std::size_t worker_count_value{};
    WorkerRoutingState routing{};
    StoreConcurrencyMode concurrency{StoreConcurrencyMode::paired};
    std::unique_ptr<ActiveOperationCounter[]> active_operations;
    std::unique_ptr<VolatileStoreRuntime> volatile_runtime;
    std::unique_ptr<DurableRuntimeCatalog> durable_runtime;
    std::unique_ptr<store::paired::ShardPairRuntime> pair_runtime;
    std::shared_ptr<const StoreClock> clock;
    std::atomic<std::uint64_t> latest_now_ns{};
    std::mutex compaction_mutex;
    std::atomic_size_t next_compaction_worker{};
    std::unique_ptr<MaintenanceController> maintenance;
    std::atomic<LifecycleState> lifecycle{LifecycleState::open};
    mutable std::mutex lifecycle_mutex;
    std::optional<Error> close_error;
    // Rare lifecycle/backup control path only. A counted fence prevents one
    // concurrent backup from reopening admission while another still owns a
    // closed interval. The normal operation-admission path touches neither
    // this flag nor this counter.
    std::atomic_flag admission_fence_state_lock = ATOMIC_FLAG_INIT;
    std::size_t admission_fence_count{};
    // Optional close liveness bound (Writer drain + admission drain). nullopt = unbounded.
    std::optional<std::uint32_t> close_drain_deadline_ms{};

  private:
    void lock_admission_fence_state() noexcept {
        while (admission_fence_state_lock.test_and_set(std::memory_order_acquire)) {
            admission_fence_state_lock.wait(true, std::memory_order_relaxed);
        }
    }

    void unlock_admission_fence_state() noexcept {
        admission_fence_state_lock.clear(std::memory_order_release);
        admission_fence_state_lock.notify_one();
    }
};

struct detail::PreparedColdRead::State final {
    explicit State(DurableRuntimeCatalog::PinnedRead prepared_read) : prepared(std::move(prepared_read)) {}

    explicit State(DurableRuntimeCatalog::BorrowedPinnedRead prepared_read)
        : prepared(std::move(prepared_read)) {}

    std::variant<DurableRuntimeCatalog::PinnedRead, DurableRuntimeCatalog::BorrowedPinnedRead> prepared;
};

} // namespace glifistore
