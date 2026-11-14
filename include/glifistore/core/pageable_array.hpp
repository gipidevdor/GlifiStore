/**
 * @file
 * @brief Growable array of trivially copyable elements stored in a PageableRegion (ADR 0046).
 *
 * Internal header (not installed). Replaces std::vector for the large Index arrays so that their
 * backing follows the Store's memory reserve. Growth relocates with memcpy; elements are never
 * destroyed individually. Allocation failure throws std::bad_alloc like std::vector.
 */
#pragma once

#include "glifistore/core/memory_reserve.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace glifistore {

template <typename T> class PageableArray final {
    static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>,
                  "PageableArray relocates elements by memcpy and never runs destructors");
    static_assert(alignof(T) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__,
                  "PageableArray storage is aligned to the default new alignment");

  public:
    PageableArray() noexcept = default;

    explicit PageableArray(std::shared_ptr<MemoryReserve> reserve) noexcept : reserve_(std::move(reserve)) {}

    PageableArray(PageableArray&& other) noexcept
        : region_(std::move(other.region_)), size_(std::exchange(other.size_, 0U)),
          capacity_(std::exchange(other.capacity_, 0U)), reserve_(std::move(other.reserve_)) {}

    auto operator=(PageableArray&& other) noexcept -> PageableArray& {
        if (this != &other) {
            region_ = std::move(other.region_);
            size_ = std::exchange(other.size_, 0U);
            capacity_ = std::exchange(other.capacity_, 0U);
            reserve_ = std::move(other.reserve_);
        }
        return *this;
    }

    PageableArray(const PageableArray&) = delete;
    auto operator=(const PageableArray&) -> PageableArray& = delete;
    ~PageableArray() = default;

    /// Charge existing uncharged storage and all future growth to @p reserve.
    void set_memory_reserve(std::shared_ptr<MemoryReserve> reserve) noexcept {
        region_.adopt_index_reserve(reserve);
        reserve_ = std::move(reserve);
    }

    [[nodiscard]] auto memory_reserve() const noexcept -> const std::shared_ptr<MemoryReserve>& {
        return reserve_;
    }

    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return size_;
    }

    [[nodiscard]] auto capacity() const noexcept -> std::size_t {
        return capacity_;
    }

    [[nodiscard]] auto empty() const noexcept -> bool {
        return size_ == 0U;
    }

    [[nodiscard]] auto data() noexcept -> T* {
        return region_.empty() ? nullptr : std::launder(reinterpret_cast<T*>(region_.data()));
    }

    [[nodiscard]] auto data() const noexcept -> const T* {
        return region_.empty() ? nullptr : std::launder(reinterpret_cast<const T*>(region_.data()));
    }

    [[nodiscard]] auto operator[](const std::size_t index) noexcept -> T& {
        return data()[index];
    }

    [[nodiscard]] auto operator[](const std::size_t index) const noexcept -> const T& {
        return data()[index];
    }

    [[nodiscard]] auto span() noexcept -> std::span<T> {
        return {data(), size_};
    }

    [[nodiscard]] auto span() const noexcept -> std::span<const T> {
        return {data(), size_};
    }

    [[nodiscard]] auto backing() const noexcept -> PageableBacking {
        return region_.backing();
    }

    /// Drop every element and release the storage.
    void clear() noexcept {
        region_ = PageableRegion{};
        size_ = 0U;
        capacity_ = 0U;
    }

    /// Ensure capacity for @p count elements without changing size().
    void reserve(const std::size_t count) {
        if (count > capacity_) {
            reallocate(count);
        }
    }

    /// Resize to @p count, value-initializing new elements; growth is geometric.
    void resize(const std::size_t count) {
        if (count > capacity_) {
            reallocate(std::max(count, grown_capacity()));
        }
        if (count > size_) {
            std::uninitialized_value_construct_n(data() + size_, count - size_);
        }
        size_ = count;
    }

    /// Replace the contents with @p count copies of @p value in storage of exactly that capacity.
    void assign(const std::size_t count, const T& value) {
        if (count != capacity_) {
            clear();
            reallocate(count);
        }
        std::uninitialized_fill_n(data(), count, value);
        size_ = count;
    }

  private:
    [[nodiscard]] auto grown_capacity() const noexcept -> std::size_t {
        return capacity_ > std::numeric_limits<std::size_t>::max() / 2U ? capacity_ : capacity_ * 2U;
    }

    void reallocate(const std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::bad_alloc{};
        }
        auto next = PageableRegion::allocate(count * sizeof(T), PageableRegion::Use::index, reserve_);
        if (size_ != 0U) {
            std::memcpy(next.data(), region_.data(), size_ * sizeof(T));
        }
        region_ = std::move(next);
        capacity_ = count;
    }

    PageableRegion region_;
    std::size_t size_{};
    std::size_t capacity_{};
    std::shared_ptr<MemoryReserve> reserve_;
};

} // namespace glifistore
