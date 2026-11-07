#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace glifistore::store::paired {
namespace spsc_detail {

inline constexpr auto kMaximumCapacity = std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 2U);

[[nodiscard]] inline auto normalized_capacity(const std::size_t requested) -> std::size_t {
    if (requested > kMaximumCapacity) {
        throw std::invalid_argument{"SPSC queue capacity exceeds its modular cursor bound"};
    }
    return std::bit_ceil(std::max(std::size_t{2}, requested));
}

} // namespace spsc_detail

// Runtime-sized, preallocated SPSC ring. Exactly one producer owns head and
// exactly one consumer owns tail. The acquire/release edge transfers ownership
// of a fully constructed cell; no allocation or compare/exchange occurs in the
// steady-state queue path.
template <typename T> class BoundedSpscQueue final {
    static_assert(std::is_nothrow_move_constructible_v<T>);

  public:
    explicit BoundedSpscQueue(const std::size_t requested_capacity)
        : capacity_(spsc_detail::normalized_capacity(requested_capacity)), mask_(capacity_ - 1U),
          cells_(std::make_unique<std::optional<T>[]>(capacity_)) {}

    BoundedSpscQueue(const BoundedSpscQueue&) = delete;
    auto operator=(const BoundedSpscQueue&) -> BoundedSpscQueue& = delete;
    BoundedSpscQueue(BoundedSpscQueue&&) = delete;
    auto operator=(BoundedSpscQueue&&) -> BoundedSpscQueue& = delete;

    [[nodiscard]] auto capacity() const noexcept -> std::size_t {
        return capacity_;
    }

    [[nodiscard]] auto try_push(T value) noexcept -> bool {
        const auto head = producer_.head.load(std::memory_order_relaxed);
        if (head - producer_.cached_tail == capacity_) {
            producer_.cached_tail = consumer_.tail.load(std::memory_order_acquire);
            if (head - producer_.cached_tail == capacity_) {
                return false;
            }
        }
        cells_[head & mask_].emplace(std::move(value));
        producer_.head.store(head + 1U, std::memory_order_release);
        return true;
    }

    [[nodiscard]] auto try_pop() noexcept -> std::optional<T> {
        const auto tail = consumer_.tail.load(std::memory_order_relaxed);
        if (consumer_.cached_head == tail) {
            consumer_.cached_head = producer_.head.load(std::memory_order_acquire);
            if (consumer_.cached_head == tail) {
                return std::nullopt;
            }
        }
        auto value = std::move(cells_[tail & mask_]);
        cells_[tail & mask_].reset();
        consumer_.tail.store(tail + 1U, std::memory_order_release);
        return value;
    }

    [[nodiscard]] auto size() const noexcept -> std::size_t {
        const auto head = producer_.head.load(std::memory_order_acquire);
        const auto tail = consumer_.tail.load(std::memory_order_acquire);
        return head - tail;
    }

    [[nodiscard]] auto empty() const noexcept -> bool {
        return size() == 0;
    }

  private:
    struct alignas(128) ProducerState final {
        std::atomic<std::size_t> head{};
        std::size_t cached_tail{};
    };

    struct alignas(128) ConsumerState final {
        std::atomic<std::size_t> tail{};
        std::size_t cached_head{};
    };

    const std::size_t capacity_;
    const std::size_t mask_;
    std::unique_ptr<std::optional<T>[]> cells_;
    ProducerState producer_{};
    ConsumerState consumer_{};
};

} // namespace glifistore::store::paired
