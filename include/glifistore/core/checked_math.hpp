/**
 * @file
 * @brief Checked unsigned size arithmetic and overflow-safe extent validation.
 */
#pragma once

#include "glifistore/core/error.hpp"

#include <concepts>
#include <limits>
#include <type_traits>

namespace glifistore {

/**
 * @brief Add unsigned operands without wraparound.
 * @return The sum, or arithmetic_overflow if it cannot be represented by T.
 */
template <std::unsigned_integral T> [[nodiscard]] constexpr auto checked_add(T lhs, T rhs) -> Result<T> {
    if (rhs > std::numeric_limits<T>::max() - lhs) {
        return fail(ErrorCode::arithmetic_overflow, "unsigned addition overflow");
    }
    return static_cast<T>(lhs + rhs);
}

/**
 * @brief Round upward to a power-of-two alignment without wrapping.
 * @param value Unaligned unsigned size or offset.
 * @param alignment Nonzero power of two in the same units as value.
 * @return Aligned value, invalid_argument for bad alignment, or arithmetic_overflow.
 */
template <std::unsigned_integral T>
[[nodiscard]] constexpr auto align_up_checked(T value, T alignment) -> Result<T> {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return fail(ErrorCode::invalid_argument, "alignment must be a power of two");
    }
    auto with_padding = checked_add(value, static_cast<T>(alignment - 1));
    if (!with_padding) {
        return unexpected(with_padding.error());
    }
    return static_cast<T>(*with_padding & static_cast<T>(~(alignment - 1)));
}

/**
 * @brief Test whether [offset, offset + length) fits without forming an overflowing sum.
 * @note A zero-length range at capacity is valid; an offset beyond capacity is not.
 */
template <std::unsigned_integral T>
[[nodiscard]] constexpr auto range_contains(T capacity, T offset, T length) -> bool {
    return offset <= capacity && length <= static_cast<T>(capacity - offset);
}

} // namespace glifistore
