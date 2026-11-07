/**
 * @file
 * @brief CRC32C integrity checks over explicitly encoded bytes.
 */
#pragma once

#include <cstdint>
#include <span>

namespace glifistore {

[[nodiscard]] auto crc32c(std::span<const std::byte> bytes) noexcept -> std::uint32_t;

/// CRC32C over a record with checksum bytes [20, 24) treated as zero.
[[nodiscard]] auto crc32c_with_zeroed_checksum_field(std::span<const std::byte> bytes) noexcept
    -> std::uint32_t;

} // namespace glifistore
