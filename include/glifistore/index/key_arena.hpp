/**
 * @file
 * @brief Offset-based heap-key arena whose storage is compacted during table rebuilds.
 */
#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/core/memory_reserve.hpp"
#include "glifistore/core/pageable_array.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <utility>

namespace glifistore {

/// Bump allocator for SwissTable heap keys. Individual frees are not supported; rehash compacts live keys.
class KeyArena final {
  public:
    KeyArena() noexcept = default;

    /// Charge arena storage to the Store memory reserve (ADR 0046); null keeps it uncharged.
    explicit KeyArena(std::shared_ptr<MemoryReserve> reserve) noexcept : storage_(std::move(reserve)) {}

    [[nodiscard]] auto memory_reserve() const noexcept -> const std::shared_ptr<MemoryReserve>& {
        return storage_.memory_reserve();
    }

    void set_memory_reserve(std::shared_ptr<MemoryReserve> reserve) noexcept {
        storage_.set_memory_reserve(std::move(reserve));
    }

    /**
     * @brief Reserve bytes in the bump arena and return a checked 32-bit offset; growth may invalidate
     * borrowed spans.
     */
    [[nodiscard]] auto allocate(std::size_t size) -> Result<std::uint32_t>;
    /**
     * @brief Borrow a validated arena subrange; growth, clear, or destruction invalidates the view.
     */
    [[nodiscard]] auto data(std::uint32_t offset, std::size_t size) const -> std::span<const std::byte>;
    [[nodiscard]] auto allocated_bytes() const noexcept -> std::size_t;
    void clear() noexcept;
    [[nodiscard]] auto reserve(std::size_t bytes) -> Status;
    [[nodiscard]] auto prepare_allocate(std::size_t size) -> Status;

  private:
    PageableArray<std::byte> storage_;
    std::size_t bump_{};
};

} // namespace glifistore
