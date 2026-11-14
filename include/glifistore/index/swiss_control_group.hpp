/**
 * @file
 * @brief SwissTable control-byte matching without reading key payloads.
 */
#pragma once

#include "glifistore/index/swiss_table.hpp"

#include <bit>
#include <cstdint>
#include <cstring>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace glifistore::detail {

[[nodiscard]] inline auto load_control_group64(const std::uint8_t* control) noexcept -> std::uint64_t {
    std::uint64_t word = 0;
    std::memcpy(&word, control, kSwissGroupSize);
    return word;
}

[[nodiscard]] inline auto control_byte_at(const std::uint64_t control, const std::size_t offset) noexcept
    -> std::uint8_t {
    return static_cast<std::uint8_t>((control >> (offset * 8U)) & 0xFFU);
}

[[nodiscard]] inline auto equal_byte_mask_scalar(const std::uint8_t* control,
                                                 const std::uint8_t byte) noexcept -> std::uint64_t {
    const auto word = load_control_group64(control);
    std::uint64_t mask = 0;
    for (std::size_t index = 0; index < kSwissGroupSize; ++index) {
        if (control_byte_at(word, index) == byte) {
            mask |= 1ULL << index;
        }
    }
    return mask;
}

[[nodiscard]] inline auto equal_byte_mask(const std::uint8_t* control, const std::uint8_t byte) noexcept
    -> std::uint64_t {
#if defined(__SSE2__)
    std::uint64_t word = 0;
    std::memcpy(&word, control, kSwissGroupSize);
    const __m128i group = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(&word));
    const __m128i needle = _mm_set1_epi8(static_cast<char>(byte));
    const auto movemask = static_cast<unsigned>(_mm_movemask_epi8(_mm_cmpeq_epi8(group, needle)));
    return static_cast<std::uint64_t>(movemask) & ((1ULL << kSwissGroupSize) - 1ULL);
#elif defined(__ARM_NEON) || defined(__aarch64__)
    const uint8x8_t compared = vceq_u8(vld1_u8(control), vdup_n_u8(byte));
    const uint8x8_t weights = {1U, 2U, 4U, 8U, 16U, 32U, 64U, 128U};
    auto weighted = vand_u8(compared, weights);
#if defined(__aarch64__)
    return static_cast<std::uint64_t>(vaddv_u8(weighted));
#else
    weighted = vpadd_u8(weighted, weighted);
    weighted = vpadd_u8(weighted, weighted);
    weighted = vpadd_u8(weighted, weighted);
    return static_cast<std::uint64_t>(vget_lane_u8(weighted, 0));
#endif
#else
    return equal_byte_mask_scalar(control, byte);
#endif
}

[[nodiscard]] inline auto first_set_bit(const std::uint64_t mask) noexcept -> std::size_t {
    return static_cast<std::size_t>(std::countr_zero(mask));
}

[[nodiscard]] inline auto clear_lowest_set_bit(const std::uint64_t mask) noexcept -> std::uint64_t {
    return mask & (mask - 1ULL);
}

} // namespace glifistore::detail
