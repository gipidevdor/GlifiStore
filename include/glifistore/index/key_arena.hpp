/**
 * @file
 * @brief Offset-based heap-key arena whose storage is compacted during table rebuilds.
 */
#pragma once

#include "glifistore/core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace glifistore {

/// Bump allocator for SwissTable heap keys. Individual frees are not supported; rehash compacts live keys.
class KeyArena final {
  public:
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
    std::vector<std::byte> storage_;
    std::size_t bump_{};
};

} // namespace glifistore
