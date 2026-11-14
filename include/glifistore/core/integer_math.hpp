/**
 * @file
 * @brief Saturating counters and portable overflow-safe basis-point arithmetic.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace glifistore {

/**
 * @brief Add counters, clamping at UINT64_MAX instead of wrapping.
 */
[[nodiscard]] constexpr auto saturating_add(const std::uint64_t left, const std::uint64_t right) noexcept
    -> std::uint64_t {
    return right > std::numeric_limits<std::uint64_t>::max() - left
               ? std::numeric_limits<std::uint64_t>::max()
               : left + right;
}

/// Exact floor(numerator * 10'000 / denominator) for the bounded ratio [0, 1].
/// The quotient/remainder doubling avoids a wider non-standard integer and never
/// forms the potentially overflowing product.
/**
 * @return Zero for a zero denominator, 10,000 for numerator >= denominator, otherwise the floored ratio.
 */
[[nodiscard]] constexpr auto basis_points(const std::uint64_t numerator,
                                          const std::uint64_t denominator) noexcept -> std::uint32_t {
    constexpr std::uint32_t kScale{10'000};
    if (denominator == 0) {
        return 0;
    }
    if (numerator >= denominator) {
        return kScale;
    }

    struct Fraction final {
        std::uint32_t quotient{};
        std::uint64_t remainder{};
    };

    const auto add = [denominator](const Fraction left, const Fraction right) constexpr noexcept {
        Fraction result{.quotient = static_cast<std::uint32_t>(left.quotient + right.quotient)};
        if (right.remainder != 0 && left.remainder >= denominator - right.remainder) {
            ++result.quotient;
            result.remainder = left.remainder - (denominator - right.remainder);
        } else {
            result.remainder = left.remainder + right.remainder;
        }
        return result;
    };

    Fraction result{};
    Fraction multiple{.remainder = numerator};
    auto scale = kScale;
    while (scale != 0) {
        if ((scale & 1U) != 0) {
            result = add(result, multiple);
        }
        scale >>= 1U;
        if (scale != 0) {
            multiple = add(multiple, multiple);
        }
    }
    return result.quotient;
}

} // namespace glifistore
