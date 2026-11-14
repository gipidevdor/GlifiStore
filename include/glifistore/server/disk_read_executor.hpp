/**
 * @file
 * @brief Bounded cold-read execution carrying immutable Record lifetime tokens.
 */
#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/server/bounded_spsc_queue.hpp"
#include "glifistore/server/connection_token.hpp"
#include "glifistore/server/wakeup.hpp"
#include "glifistore/store/prepared_read.hpp"
#include "glifistore/store/store.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace glifistore::server {

/**
 * @brief Owning cold-read result tagged with connection identity, request id, and adopted generation epoch.
 */
struct DiskReadCompletion final {
    ConnectionToken connection{};
    std::uint64_t request_id{};
    std::uint64_t generation_epoch{};
    std::optional<OwnedValue> value{};
    std::optional<Error> error{};
};

/**
 * @brief Move-only cold-read pin and borrowed completion/wakeup endpoints that must outlive task execution.
 */
struct DiskReadTask final {
    ConnectionToken connection{};
    std::uint64_t request_id{};
    std::uint64_t generation_epoch{};
    std::size_t worker_index{};
    detail::PreparedColdRead read;
    detail::ColdReadCancellation cancellation{};
    std::size_t maximum_value_bytes{};
    BoundedSpscQueue<DiskReadCompletion>* completions{};
    Wakeup* wakeup{};
};

/// One bounded cold-read lane per ShardPair. Each Reader is the sole producer
/// of its SPSC lane and one persistent I/O worker is the sole consumer; durable
/// GET submission therefore has no process-wide mutex or condition variable.
class DiskReadExecutor final {
  public:
    [[nodiscard]] static auto create(Store& store, std::size_t worker_count, std::size_t capacity_per_worker)
        -> Result<std::unique_ptr<DiskReadExecutor>>;
    ~DiskReadExecutor();

    DiskReadExecutor(const DiskReadExecutor&) = delete;
    auto operator=(const DiskReadExecutor&) -> DiskReadExecutor& = delete;
    DiskReadExecutor(DiskReadExecutor&&) = delete;
    auto operator=(DiskReadExecutor&&) -> DiskReadExecutor& = delete;

    [[nodiscard]] auto start() -> Status;
    /**
     * @brief Attempt bounded cold-read admission without waiting for I/O.
     * @param task Ownership of the read pin is passed by value; completion endpoints must remain alive.
     * @return Whether the task was queued. On rejection the local task and its pin are destroyed.
     */
    [[nodiscard]] auto try_submit(DiskReadTask task) -> bool;
    void stop() noexcept;

  private:
    struct Lane;

    DiskReadExecutor(Store& store, std::size_t worker_count, std::size_t capacity_per_worker);
    void run(std::size_t worker_index) noexcept;
    [[nodiscard]] auto begin_submission() noexcept -> bool;
    void finish_submission() noexcept;

    static constexpr auto kAdmissionClosed = std::size_t{1}
                                             << (std::numeric_limits<std::size_t>::digits - 1U);
    static constexpr auto kAdmissionCountMask = kAdmissionClosed - 1U;

    Store& store_;
    std::vector<std::unique_ptr<Lane>> lanes_;
    std::atomic_size_t admission_state_{};
    std::atomic_bool started_{};
    std::atomic_bool stopping_{};
};

} // namespace glifistore::server
