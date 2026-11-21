/**
 * @file
 * @brief Shared bounded volatile-read helpers; the originating Reactor owns every epoch lease.
 */
#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/server/connection_token.hpp"
#include "glifistore/server/read_capacity_controller.hpp"
#include "glifistore/server/wakeup.hpp"
#include "glifistore/store/paired/read_generation.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace glifistore::server {

/// No value allocation on a helper. A large value is an exact-selection continuation:
/// the Reactor copies/encodes this view before releasing its generation lease.
struct ReadHelperResult final {
    std::size_t helper{};
    ConnectionToken connection{};
    std::uint64_t request_id{};
    std::uint64_t generation_epoch{};
    std::uint64_t service_cpu_ns{};
    std::optional<RecordView> record{};
    std::optional<ErrorCode> error{};
};

/// One cell per helper, including running and unconsumed completion. No per-GET
/// global mutex, queue allocation or copied value. Keys above 1024 bytes use the
/// primary path. Helpers sleep on an atomic signal when there is no useful work.
class ReadHelperPool final {
  public:
    [[nodiscard]] static auto create(std::size_t helpers) -> Result<std::unique_ptr<ReadHelperPool>>;
    ~ReadHelperPool();
    explicit ReadHelperPool(std::size_t helpers);
    ReadHelperPool(const ReadHelperPool&) = delete;
    auto operator=(const ReadHelperPool&) -> ReadHelperPool& = delete;

    [[nodiscard]] auto start() -> Status;
    /// After all Reactor threads join, drains computation and joins helpers. Pending
    /// views must be discarded before Store/generation teardown, including failure paths.
    void stop() noexcept;
    /// Control-thread only. Assignment changes succeed only on empty cells. A failed
    /// park retains the occupied slot and its lease until completion is consumed.
    [[nodiscard]] auto assign(std::size_t helper, std::size_t owner) noexcept -> bool;
    [[nodiscard]] auto park(std::size_t helper) noexcept -> bool;
    /// Caller is the sole Reactor for owner, already protecting generation->epoch().
    /// Keep that epoch in every adoption frontier until finish() after consuming result.
    [[nodiscard]] auto try_submit(std::size_t owner, ConnectionToken connection, std::uint64_t request_id,
                                  const store::paired::PairReadGeneration& generation, std::string_view key,
                                  std::uint64_t key_hash, std::uint64_t now_ns, Wakeup& wakeup) noexcept
        -> bool;
    [[nodiscard]] auto take(std::size_t owner) noexcept -> std::optional<ReadHelperResult>;
    void finish(std::size_t helper) noexcept;
    [[nodiscard]] auto size() const noexcept -> std::size_t;

  private:
    struct Cell;
    void run(Cell& cell) noexcept;
    std::vector<std::unique_ptr<Cell>> cells_;
    bool started_{};
};

} // namespace glifistore::server
