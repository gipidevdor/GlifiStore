/**
 * @file
 * @brief Explicit blob reference and file codecs for the opt-in ADR 0042 profile.
 */
#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/segment/segment_header.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace glifistore {

inline constexpr std::uint64_t kMaximumBlobValueBytes = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::uint32_t kBlobChunkBytes = 1024U * 1024U;
inline constexpr std::size_t kBlobRefBytes = 80;
inline constexpr std::size_t kMaximumBlobKeyBytes = 1024U * 1024U - 56U - kBlobRefBytes;
inline constexpr std::size_t kBlobHeaderBytes = 128;
inline constexpr std::size_t kBlobFooterBytes = 64;
inline constexpr std::size_t kMaximumBlobChunks = 64;
using BlobId = std::array<std::byte, 16>;
using BlobDigest = std::array<std::byte, 32>;

/**
 * @brief Logical blob identity, length, and digest encoded inside a blob_ref Record; no C++ layout is
 * persisted.
 */
struct BlobRef {
    BlobId id{};                  ///< Non-reused blob identity allocated from the persisted high-water mark.
    std::uint64_t logical_size{}; ///< Logical value bytes, excluding file metadata.
    BlobDigest digest{};          ///< Whole-value SHA-256 integrity digest, not authentication.
    auto operator==(const BlobRef&) const -> bool = default;
};

/**
 * @brief Physical chunk extent and CRC32C metadata used before exposing read bytes.
 */
struct BlobChunk {
    std::uint64_t offset{};   ///< Absolute chunk byte offset in the blob file.
    std::uint32_t length{};   ///< Chunk payload length in bytes.
    std::uint32_t checksum{}; ///< CRC32C of the chunk payload.
};

/**
 * @brief File identity, owning Worker, and fixed-capacity chunk table for one blob.
 */
struct BlobFileMetadata {
    BlobRef reference{};
    StoreId store_id{};
    WorkerId owner{};
    std::array<BlobChunk, kMaximumBlobChunks> chunks{};
};

/// Incremental, allocation-free SHA-256 for blob integrity (not authentication).
class BlobHasher final {
  public:
    /**
     * @brief Consume borrowed bytes into incremental SHA-256 state without allocating.
     */
    void update(std::span<const std::byte> bytes) noexcept;
    /**
     * @brief Finalize a copy of the current hash state; the hasher remains available for further updates.
     */
    [[nodiscard]] auto finish() const noexcept -> BlobDigest;

  private:
    void transform(const std::byte* block) noexcept;
    std::array<std::uint32_t, 8> state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<std::byte, 64> buffer_{};
    std::uint64_t bytes_{};
    std::size_t buffered_{};
};

/**
 * @brief Return the number of payload chunks needed for a bounded logical size.
 */
[[nodiscard]] auto blob_chunk_count(std::uint64_t size) noexcept -> std::uint32_t;
/**
 * @brief Compute physical file bytes for payload plus fixed metadata/footer.
 * @pre size obeys kMaximumBlobValueBytes.
 */
[[nodiscard]] auto blob_file_bytes(std::uint64_t size) noexcept -> std::uint64_t;
/**
 * @brief Validate and explicitly encode canonical little-endian bytes; C++ object layout is never persisted.
 */
[[nodiscard]] auto encode_blob_ref(const BlobRef& reference) -> Result<std::array<std::byte, kBlobRefBytes>>;
/**
 * @brief Validate encoded size, identity/version, and integrity before returning decoded metadata; input is
 * not retained.
 */
[[nodiscard]] auto decode_blob_ref(std::span<const std::byte> bytes) -> Result<BlobRef>;
/**
 * @brief Validate and explicitly encode canonical little-endian bytes; C++ object layout is never persisted.
 */
[[nodiscard]] auto encode_blob_metadata(const BlobFileMetadata& metadata)
    -> Result<std::array<std::byte, kBlobHeaderBytes + kMaximumBlobChunks * 16>>;
/**
 * @brief Validate encoded size, identity/version, and integrity before returning decoded metadata; input is
 * not retained.
 */
[[nodiscard]] auto decode_blob_metadata(std::span<const std::byte> bytes) -> Result<BlobFileMetadata>;
/**
 * @brief Validate and explicitly encode canonical little-endian bytes; C++ object layout is never persisted.
 */
[[nodiscard]] auto encode_blob_footer(const BlobRef& reference)
    -> Result<std::array<std::byte, kBlobFooterBytes>>;
/**
 * @brief Validate footer encoding/integrity and require agreement with the expected blob reference.
 */
[[nodiscard]] auto validate_blob_footer(std::span<const std::byte> bytes, const BlobRef& reference) -> Status;

} // namespace glifistore
