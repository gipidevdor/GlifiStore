/**
 * @file
 * @brief Fixed-capacity multiple-producer, single-consumer queue for server handoffs.
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace glifistore::server {
namespace mpsc_detail {

inline constexpr auto kMaximumCapacity = std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 2U);

[[nodiscard]] inline auto normalized_capacity(const std::size_t requested) -> std::size_t {
    if (requested > kMaximumCapacity) {
        throw std::invalid_argument{"MPSC queue capacity exceeds its modular sequence bound"};
    }
    return std::bit_ceil(std::max(std::size_t{2}, requested));
}

/// Unsigned modular ordering. A sequence behind position has a distance in the
/// upper half of size_t; keeping capacity below half range makes the relation
/// unambiguous without implementation-defined casts or signed overflow.
[[nodiscard]] inline constexpr auto sequence_precedes_position(const std::size_t sequence,
                                                               const std::size_t position) noexcept -> bool {
    return sequence - position > std::numeric_limits<std::size_t>::max() / 2U;
}

} // namespace mpsc_detail

/// Bounded multi-producer/single-consumer ring. Producers reserve cells with an
/// atomic sequence; the sole consumer owns dequeue_position_. No allocation is
/// of ring storage is performed after construction; T operations retain their own behavior.
template <typename T> class BoundedMpscQueue final {
  public:
    explicit BoundedMpscQueue(const std::size_t requested_capacity)
        : capacity_(mpsc_detail::normalized_capacity(requested_capacity)), mask_(capacity_ - 1U),
          cells_(std::make_unique<Cell[]>(capacity_)) {
        for (std::size_t index = 0; index < capacity_; ++index) {
            cells_[index].sequence.store(index, std::memory_order_relaxed);
        }
    }

    BoundedMpscQueue(const BoundedMpscQueue&) = delete;
    auto operator=(const BoundedMpscQueue&) -> BoundedMpscQueue& = delete;
    BoundedMpscQueue(BoundedMpscQueue&&) = delete;
    auto operator=(BoundedMpscQueue&&) -> BoundedMpscQueue& = delete;

    [[nodiscard]] auto capacity() const noexcept -> std::size_t {
        return capacity_;
    }

    /**
     * @brief Reserve and release-publish one value; false leaves the rvalue reference unmoved.
     * @note T construction/move must not throw after a producer reserves its sequence;
     * this implementation does not roll a reserved cell back on an exception.
     */
    [[nodiscard]] auto try_push(T&& value) -> bool {
        auto position = enqueue_position_.load(std::memory_order_relaxed);
        Cell* cell = nullptr;
        while (true) {
            cell = &cells_[position & mask_];
            const auto sequence = cell->sequence.load(std::memory_order_acquire);
            const auto difference = sequence - position;
            if (difference == 0U) {
                if (enqueue_position_.compare_exchange_weak(position, position + 1U,
                                                            std::memory_order_relaxed)) {
                    break;
                }
                continue;
            }
            if (mpsc_detail::sequence_precedes_position(sequence, position)) {
                return false;
            }
            position = enqueue_position_.load(std::memory_order_relaxed);
        }
        cell->value.emplace(std::move(value));
        cell->sequence.store(position + 1U, std::memory_order_release);
        return true;
    }

    /**
     * @brief Sole-consumer acquire/move; nullopt can include a producer reservation not yet published.
     */
    [[nodiscard]] auto try_pop() -> std::optional<T> {
        auto& cell = cells_[dequeue_position_ & mask_];
        const auto sequence = cell.sequence.load(std::memory_order_acquire);
        if (sequence != dequeue_position_ + 1U) {
            return std::nullopt;
        }
        auto value = std::move(cell.value);
        cell.value.reset();
        cell.sequence.store(dequeue_position_ + capacity_, std::memory_order_release);
        ++dequeue_position_;
        return value;
    }

  private:
    struct Cell {
        std::atomic<std::size_t> sequence{};
        std::optional<T> value;
    };

    const std::size_t capacity_;
    const std::size_t mask_;
    std::unique_ptr<Cell[]> cells_;
    alignas(64) std::atomic<std::size_t> enqueue_position_{};
    alignas(64) std::size_t dequeue_position_{};
};

} // namespace glifistore::server
