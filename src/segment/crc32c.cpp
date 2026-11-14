#include "glifistore/segment/crc32c.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__SSE4_2__)
#include <nmmintrin.h>
#elif defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif

namespace glifistore {
namespace {

constexpr std::uint32_t kCrc32cInit = 0xFFFFFFFFU;
constexpr std::uint32_t kCrc32cPoly = 0x82F63B78U;

consteval auto make_crc32c_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::size_t index = 0; index < 256; ++index) {
        std::uint32_t crc = static_cast<std::uint32_t>(index);
        for (int bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::uint32_t>(0U - (crc & 1U));
            crc = (crc >> 1U) ^ (kCrc32cPoly & mask);
        }
        table[index] = crc;
    }
    return table;
}

constexpr auto kCrc32cTable = make_crc32c_table();

[[nodiscard]] auto to_u8(const std::byte value) noexcept -> std::uint8_t {
    return std::to_integer<std::uint8_t>(value);
}

#if defined(__SSE4_2__) || defined(__ARM_FEATURE_CRC32)
[[maybe_unused]]
#endif
[[nodiscard]] auto crc32c_software_update(std::uint32_t crc, std::span<const std::byte> data) noexcept
    -> std::uint32_t {
    for (const auto byte : data) {
        crc = kCrc32cTable[(crc ^ to_u8(byte)) & 0xFFU] ^ (crc >> 8U);
    }
    return crc;
}

#if defined(__SSE4_2__) || defined(__ARM_FEATURE_CRC32)

[[nodiscard]] auto crc32c_hardware_update(std::uint32_t crc, std::span<const std::byte> data) noexcept
    -> std::uint32_t {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data.data());
    std::size_t length = data.size();

#if defined(__SSE4_2__)
    while (length >= 8U) {
        std::uint64_t word{};
        std::memcpy(&word, bytes, sizeof(word));
        crc = static_cast<std::uint32_t>(_mm_crc32_u64(crc, word));
        bytes += 8U;
        length -= 8U;
    }
    while (length >= 4U) {
        std::uint32_t word{};
        std::memcpy(&word, bytes, sizeof(word));
        crc = _mm_crc32_u32(crc, word);
        bytes += 4U;
        length -= 4U;
    }
    while (length > 0U) {
        crc = _mm_crc32_u8(crc, *bytes);
        ++bytes;
        --length;
    }
#elif defined(__ARM_FEATURE_CRC32)
    while (length >= 8U) {
        std::uint64_t word{};
        std::memcpy(&word, bytes, sizeof(word));
        crc = __crc32cd(crc, word);
        bytes += 8U;
        length -= 8U;
    }
    while (length >= 4U) {
        std::uint32_t word{};
        std::memcpy(&word, bytes, sizeof(word));
        crc = __crc32cw(crc, word);
        bytes += 4U;
        length -= 4U;
    }
    while (length >= 2U) {
        std::uint16_t word{};
        std::memcpy(&word, bytes, sizeof(word));
        crc = __crc32ch(crc, word);
        bytes += 2U;
        length -= 2U;
    }
    if (length > 0U) {
        crc = __crc32cb(crc, *bytes);
    }
#endif
    return crc;
}

#endif

[[nodiscard]] auto crc32c_update(std::uint32_t crc, std::span<const std::byte> data) noexcept
    -> std::uint32_t {
#if defined(__SSE4_2__) || defined(__ARM_FEATURE_CRC32)
    return crc32c_hardware_update(crc, data);
#else
    return crc32c_software_update(crc, data);
#endif
}

constexpr std::size_t kRecordChecksumOffset = 20U;
constexpr std::size_t kRecordChecksumBytes = 4U;

} // namespace

auto crc32c(const std::span<const std::byte> bytes) noexcept -> std::uint32_t {
    return ~crc32c_update(kCrc32cInit, bytes);
}

auto crc32c_with_zeroed_field(const std::span<const std::byte> bytes, const std::size_t field_offset,
                              const std::size_t field_bytes) noexcept -> std::uint32_t {
    static constexpr std::array<std::byte, 16> kZeros{};
    const auto prefix_bytes = std::min(field_offset, bytes.size());
    const auto zeroed_bytes = std::min(field_bytes, bytes.size() - prefix_bytes);
    auto crc = crc32c_update(kCrc32cInit, bytes.first(prefix_bytes));
    for (auto remaining = zeroed_bytes; remaining != 0;) {
        const auto chunk = std::min(remaining, kZeros.size());
        crc = crc32c_update(crc, std::span<const std::byte>{kZeros}.first(chunk));
        remaining -= chunk;
    }
    return ~crc32c_update(crc, bytes.subspan(prefix_bytes + zeroed_bytes));
}

auto crc32c_with_zeroed_checksum_field(const std::span<const std::byte> bytes) noexcept -> std::uint32_t {
    return crc32c_with_zeroed_field(bytes, kRecordChecksumOffset, kRecordChecksumBytes);
}

} // namespace glifistore
