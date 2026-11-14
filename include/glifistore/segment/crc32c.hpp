/**
 * @file
 * @brief CRC32C integrity checks over explicitly encoded bytes.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace glifistore {

[[nodiscard]] auto crc32c(std::span<const std::byte> bytes) noexcept -> std::uint32_t;

/**
 * @brief CRC32C over @p bytes with [field_offset, field_offset + field_bytes) treated as zero.
 *
 * Computes the checksum of an encoded structure whose own checksum field is part of the covered
 * extent, without copying the input. The field is clamped to the input extent.
 */
[[nodiscard]] auto crc32c_with_zeroed_field(std::span<const std::byte> bytes, std::size_t field_offset,
                                            std::size_t field_bytes) noexcept -> std::uint32_t;

/// CRC32C over a record with checksum bytes [20, 24) treated as zero.
[[nodiscard]] auto crc32c_with_zeroed_checksum_field(std::span<const std::byte> bytes) noexcept
    -> std::uint32_t;

} // namespace glifistore
