/**
 * @file
 * @brief Record input/view lifetimes and explicit versioned Record encoding.
 */
#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace glifistore {

/**
 * @brief Persisted mutation kind; erase is a recovery-ordering tombstone.
 */
enum class Opcode : std::uint16_t { put = 1, erase = 2 };
/**
 * @brief Persisted representation tag; blob_ref requires the explicitly versioned blob profile.
 */
enum class ValueType : std::uint16_t { bytes = 1, integer = 2, map = 3, lease = 4, blob_ref = 5 };

/**
 * @brief Borrowed binary payload and metadata to encode; key/value storage must outlive the encode/append
 * call.
 */
struct RecordInput {
    SequenceNumber sequence;
    Opcode opcode{Opcode::put};
    ValueType type{ValueType::bytes};
    std::uint32_t flags{};
    std::uint64_t key_hash{};
    std::uint64_t expire_at_ns{};       ///< Absolute Unix nanoseconds; zero disables expiration.
    std::span<const std::byte> key{};   ///< Borrowed binary key; not NUL-terminated.
    std::span<const std::byte> value{}; ///< Borrowed value bytes.
    std::uint16_t format_version{1};    ///< Explicit encoded profile; default is persistence v1.
};

/**
 * @brief Decoded metadata with key/value views borrowing the original encoded buffer; the view owns no
 * Segment pin.
 */
struct RecordView {
    SequenceNumber sequence{};
    Opcode opcode{Opcode::put};
    ValueType type{ValueType::bytes};
    std::uint32_t flags{};
    std::uint64_t key_hash{};
    std::uint64_t expire_at_ns{}; ///< Absolute Unix nanoseconds; zero disables expiration.
    /// key and value span encoded bytes inside a Segment buffer. The spans remain
    /// valid only while the owning Store or Segment that produced this view lives.
    std::span<const std::byte> key{};   ///< Borrowed binary key; not NUL-terminated.
    std::span<const std::byte> value{}; ///< Borrowed value bytes.
    std::uint32_t encoded_size{};       ///< Full aligned encoded Record extent in bytes.
    std::uint16_t format_version{1};    ///< Explicit encoded profile; default is persistence v1.

    [[nodiscard]] auto key_string() const -> std::string_view;
    [[nodiscard]] auto expired(std::uint64_t now_ns) const noexcept -> bool;
};

inline constexpr std::uint32_t kRecordMagic = 0x52594C47U;
inline constexpr std::uint16_t kRecordFormatVersion = 1;
inline constexpr std::uint16_t kEncodedRecordHeaderSize = 56;

/**
 * @brief Validate Record fields and compute the aligned encoded extent without overflow.
 */
[[nodiscard]] auto encoded_record_size(const RecordInput& input) -> Result<std::size_t>;
/// Encodes into out. When encoded_size is supplied it must equal
/// encoded_record_size(input) for the same input (Segment::append uses this to
/// avoid a second size/validation pass). Debug builds assert the match.
/**
 * @brief Encode borrowed input into caller storage after size/field validation.
 */
[[nodiscard]] auto encode_record(std::span<std::byte> out, const RecordInput& input) -> Status;
/**
 * @brief Encode using a previously checked exact encoded size.
 * @pre encoded_size equals encoded_record_size(input) for these unchanged fields.
 * @note This overload avoids duplicate validation in Segment::append().
 */
[[nodiscard]] auto encode_record(std::span<std::byte> out, const RecordInput& input, std::size_t encoded_size)
    -> Status;
/**
 * @brief Allocate independent encoded bytes; no input view is retained.
 */
[[nodiscard]] auto encode_record(const RecordInput& input) -> Result<std::vector<std::byte>>;
/**
 * @brief Decode a bounded Record extent and optionally verify CRC32C.
 * @param bytes Stable encoded Record storage borrowed by the returned key/value spans.
 * @param verify_checksum Disable only when an independent trust invariant already covers immutable bytes.
 * @return A borrowed RecordView or a layout, version, extent, or checksum error.
 * @note No lifetime pin is created; keep the encoded allocation alive and unchanged.
 */
[[nodiscard]] auto decode_record(std::span<const std::byte> bytes, bool verify_checksum = true)
    -> Result<RecordView>;

} // namespace glifistore
