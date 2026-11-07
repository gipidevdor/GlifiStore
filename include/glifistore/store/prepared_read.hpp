/**
 * @file
 * @brief Move-only cold-read pin ownership and generation-based cancellation.
 */
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace glifistore::detail {

class StoreAccess;

/// Allocation-free cancellation token for an asynchronous cold read. The
/// pointed epoch belongs to a stable Reactor connection slot and outlives every
/// disk-read lane. Reusing a slot increments the epoch instead of resetting a
/// boolean, so an older request can never become uncancelled again.
struct ColdReadCancellation final {
    const std::atomic_uint64_t* epoch{};
    std::uint64_t expected{};

    [[nodiscard]] auto cancelled() const noexcept -> bool {
        return epoch != nullptr && epoch->load(std::memory_order_acquire) != expected;
    }
};

/// Opaque, move-only ownership of the exact durable RecordRef and immutable
/// Segment generation needed by one cold read. The state is stored inline so
/// queue admission never allocates a pImpl; only StoreAccess can construct or
/// consume it. Server queues merely transport the lifetime token.
class PreparedColdRead final {
  public:
    PreparedColdRead(PreparedColdRead&&) noexcept;
    auto operator=(PreparedColdRead&&) noexcept -> PreparedColdRead&;
    ~PreparedColdRead();
    PreparedColdRead(const PreparedColdRead&) = delete;
    auto operator=(const PreparedColdRead&) -> PreparedColdRead& = delete;

  private:
    struct State;
    static constexpr std::size_t kStateBytes = 128;

    explicit PreparedColdRead(State&& state) noexcept;
    [[nodiscard]] auto state() noexcept -> State*;
    [[nodiscard]] auto state() const noexcept -> const State*;
    void reset() noexcept;

    alignas(std::max_align_t) std::array<std::byte, kStateBytes> storage_{};
    bool engaged_{};
    friend class StoreAccess;
};

} // namespace glifistore::detail
