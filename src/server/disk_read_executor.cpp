#include "glifistore/server/disk_read_executor.hpp"

#include "store/store_internal.hpp"

#include <exception>
#include <thread>
#include <type_traits>
#include <utility>

namespace glifistore::server {
namespace {

void set_error_code_without_allocation(std::optional<Error>& destination, const ErrorCode code) noexcept {
    static_assert(std::is_nothrow_move_constructible_v<Error>);
    destination.emplace(Error{code, {}});
}

} // namespace

struct DiskReadExecutor::Lane final {
    explicit Lane(const std::size_t capacity)
        : submitted(capacity), recycled(submitted.capacity()), slots(submitted.capacity()) {
        free_slots.reserve(submitted.capacity());
        for (std::size_t slot = submitted.capacity(); slot != 0; --slot) {
            free_slots.push_back(slot - 1U);
        }
    }

    [[nodiscard]] auto acquire_slot() noexcept -> std::optional<std::size_t> {
        while (auto recycled_slot = recycled.try_pop()) {
            if (free_slots.size() >= free_slots.capacity()) [[unlikely]] {
                std::terminate();
            }
            free_slots.push_back(*recycled_slot);
        }
        if (free_slots.empty()) {
            return std::nullopt;
        }
        const auto slot = free_slots.back();
        free_slots.pop_back();
        return slot;
    }

    // The cross-thread rings transport only indices. Large move-only task
    // payloads live in stable, preallocated slots and are transferred by the
    // release/acquire edge of submitted/recycled.
    BoundedSpscQueue<std::size_t> submitted;
    BoundedSpscQueue<std::size_t> recycled;
    std::vector<std::optional<DiskReadTask>> slots;
    // Producer-private after construction; capacity never grows.
    std::vector<std::size_t> free_slots;
    alignas(128) std::atomic_uint64_t signal{};
    alignas(128) std::atomic_bool stopping{};
    // Consumer-private verified-record buffer. Capacity is retained across
    // cold GETs, removing one allocator round-trip per request without sharing
    // mutable storage with the Reader or Writer.
    std::vector<std::byte> read_scratch;
    std::thread thread;
};

DiskReadExecutor::DiskReadExecutor(Store& store, const std::size_t worker_count,
                                   const std::size_t capacity_per_worker)
    : store_(store) {
    lanes_.reserve(worker_count);
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        lanes_.push_back(std::make_unique<Lane>(capacity_per_worker));
    }
}

DiskReadExecutor::~DiskReadExecutor() {
    stop();
}

auto DiskReadExecutor::create(Store& store, const std::size_t worker_count,
                              const std::size_t capacity_per_worker)
    -> Result<std::unique_ptr<DiskReadExecutor>> try {
    if (worker_count == 0 || worker_count != store.worker_count() || capacity_per_worker == 0) {
        return fail(ErrorCode::invalid_argument,
                    "paired disk-read executor requires one nonempty SPSC lane per Store shard");
    }
    return std::unique_ptr<DiskReadExecutor>(new DiskReadExecutor(store, worker_count, capacity_per_worker));
} catch (const std::bad_alloc&) {

    return fail(ErrorCode::resource_exhausted, "disk-read executor allocation failed");
} catch (...) {
    return fail(ErrorCode::internal_error, "disk-read executor construction failed");
}

auto DiskReadExecutor::start() -> Status try {
    if (started_.exchange(true, std::memory_order_acq_rel)) {
        return fail(ErrorCode::invalid_argument, "disk-read executor has already been started");
    }
    if (stopping_.load(std::memory_order_acquire)) {
        return fail(ErrorCode::unavailable, "disk-read executor has been stopped");
    }
    for (std::size_t worker = 0; worker < lanes_.size(); ++worker) {
        lanes_[worker]->thread = std::thread{[this, worker] { run(worker); }};
    }
    return {};
} catch (const std::exception& exception) {

    stop();
    return fail(ErrorCode::io_error, std::string{"failed to start disk-read executor: "} + exception.what());
} catch (...) {
    stop();
    return fail(ErrorCode::io_error, "failed to start disk-read executor");
}

auto DiskReadExecutor::begin_submission() noexcept -> bool {
    const auto previous = admission_state_.fetch_add(1U, std::memory_order_acq_rel);
    if ((previous & kAdmissionClosed) == 0) {
        return true;
    }
    finish_submission();
    return false;
}

void DiskReadExecutor::finish_submission() noexcept {
    const auto previous = admission_state_.fetch_sub(1U, std::memory_order_acq_rel);
    if ((previous & kAdmissionClosed) != 0 && (previous & kAdmissionCountMask) == 1U) {
        admission_state_.notify_all();
    }
}

auto DiskReadExecutor::try_submit(DiskReadTask task) -> bool {
    if (task.worker_index >= lanes_.size() || !begin_submission()) {
        return false;
    }

    struct SubmissionGuard final {
        DiskReadExecutor& executor;

        ~SubmissionGuard() {
            executor.finish_submission();
        }
    } submission{*this};

    if (!started_.load(std::memory_order_acquire) || stopping_.load(std::memory_order_acquire)) {
        return false;
    }
    auto& lane = *lanes_[task.worker_index];
    if (lane.stopping.load(std::memory_order_acquire)) {
        return false;
    }
    const auto slot = lane.acquire_slot();
    if (!slot) {
        return false;
    }
    lane.slots[*slot].emplace(std::move(task));
    if (!lane.submitted.try_push(*slot)) {
        lane.slots[*slot].reset();
        lane.free_slots.push_back(*slot);
        return false;
    }
    lane.signal.fetch_add(1U, std::memory_order_release);
    lane.signal.notify_one();
    return true;
}

void DiskReadExecutor::stop() noexcept {
    if (!stopping_.exchange(true, std::memory_order_acq_rel)) {
        auto observed = admission_state_.fetch_or(kAdmissionClosed, std::memory_order_acq_rel);
        while ((observed & kAdmissionCountMask) != 0) {
            admission_state_.wait(observed, std::memory_order_acquire);
            observed = admission_state_.load(std::memory_order_acquire);
        }
        for (auto& lane : lanes_) {
            lane->stopping.store(true, std::memory_order_release);
            lane->signal.fetch_add(1U, std::memory_order_release);
            lane->signal.notify_one();
        }
    }
    for (auto& lane : lanes_) {
        if (lane->thread.joinable()) {
            lane->thread.join();
        }
        while (auto slot = lane->submitted.try_pop()) {
            // Reactor ownership has ended. Destroy every cancelled queued pin
            // before Store::close releases the durable directory lifetime.
            lane->slots[*slot].reset();
        }
    }
}

void DiskReadExecutor::run(const std::size_t worker_index) noexcept {
    auto& lane = *lanes_[worker_index];
    while (true) {
        auto slot = lane.submitted.try_pop();
        if (!slot) {
            if (lane.stopping.load(std::memory_order_acquire)) {
                return;
            }
            const auto observed = lane.signal.load(std::memory_order_acquire);
            slot = lane.submitted.try_pop();
            if (!slot && !lane.stopping.load(std::memory_order_acquire)) {
                lane.signal.wait(observed, std::memory_order_acquire);
            }
            if (!slot) {
                continue;
            }
        }
        const auto slot_index = slot.value();
        auto& task_cell = lane.slots[slot_index];
        if (!task_cell) {
            std::terminate();
        }
        auto& task = task_cell.value();

        DiskReadCompletion completion{.connection = task.connection,
                                      .request_id = task.request_id,
                                      .generation_epoch = task.generation_epoch};
        try {
            auto result =
                task.cancellation.cancelled()
                    ? fail(ErrorCode::unavailable, "cold read was cancelled")
                    : detail::StoreAccess::complete_get_owned(store_, task.worker_index, std::move(task.read),
                                                              &task.cancellation, &lane.read_scratch);
            if (!result) {
                completion.error.emplace(std::move(result.error()));
            } else if (result->bytes.size() > task.maximum_value_bytes) {
                completion.error.emplace(ErrorCode::record_too_large,
                                         "cold-read response exceeds its connection budget");
            } else {
                completion.value.emplace(std::move(*result));
            }
        } catch (const std::bad_alloc&) {
            set_error_code_without_allocation(completion.error, ErrorCode::resource_exhausted);
        } catch (...) {
            set_error_code_without_allocation(completion.error, ErrorCode::internal_error);
        }

        // Admission is capped by the destination Reactor queue capacity, so a
        // completion cell must exist for every accepted request.
        auto* completions = task.completions;
        auto* wakeup = task.wakeup;
        if (!completions->try_push(std::move(completion))) {
            std::terminate();
        }
        task_cell.reset();
        if (!lane.recycled.try_push(slot_index)) {
            std::terminate();
        }
        static_cast<void>(wakeup->notify());
    }
}

} // namespace glifistore::server
