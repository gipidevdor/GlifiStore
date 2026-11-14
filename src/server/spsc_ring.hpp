#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace glifistore::server {

template <typename T, std::size_t Capacity> class SpscRing final {
    static_assert(Capacity >= 2 && std::has_single_bit(Capacity));
    static_assert(std::is_nothrow_move_assignable_v<T>);

  public:
    SpscRing() = default;
    SpscRing(const SpscRing&) = delete;
    auto operator=(const SpscRing&) -> SpscRing& = delete;

    [[nodiscard]] auto try_push(T value) noexcept -> bool {
        const auto head = producer_.head.load(std::memory_order_relaxed);
        if (head - producer_.cached_tail == Capacity) {
            producer_.cached_tail = consumer_.tail.load(std::memory_order_acquire);
            if (head - producer_.cached_tail == Capacity) {
                return false;
            }
        }
        cells_[head & kMask] = std::move(value);
        producer_.head.store(head + 1U, std::memory_order_release);
        return true;
    }

    [[nodiscard]] auto try_pop(T& value) noexcept -> bool {
        const auto tail = consumer_.tail.load(std::memory_order_relaxed);
        if (consumer_.cached_head == tail) {
            consumer_.cached_head = producer_.head.load(std::memory_order_acquire);
            if (consumer_.cached_head == tail) {
                return false;
            }
        }
        value = std::move(cells_[tail & kMask]);
        consumer_.tail.store(tail + 1U, std::memory_order_release);
        return true;
    }

    [[nodiscard]] auto empty() const noexcept -> bool {
        return producer_.head.load(std::memory_order_acquire) ==
               consumer_.tail.load(std::memory_order_acquire);
    }

    [[nodiscard]] auto size() const noexcept -> std::size_t {
        const auto head = producer_.head.load(std::memory_order_acquire);
        const auto tail = consumer_.tail.load(std::memory_order_acquire);
        return head - tail;
    }

    [[nodiscard]] static consteval auto capacity() noexcept -> std::size_t {
        return Capacity;
    }

  private:
    static constexpr std::size_t kMask = Capacity - 1U;

    struct alignas(128) ProducerState final {
        std::atomic<std::size_t> head{};
        std::size_t cached_tail{};
    };

    struct alignas(128) ConsumerState final {
        std::atomic<std::size_t> tail{};
        std::size_t cached_head{};
    };

    std::array<T, Capacity> cells_{};
    ProducerState producer_{};
    ConsumerState consumer_{};
};

} // namespace glifistore::server
