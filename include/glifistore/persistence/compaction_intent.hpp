/**
 * @file
 * @brief Codec for the old/new Manifest recovery decision of one compaction.
 */
#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/persistence/manifest.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace glifistore {

inline constexpr std::uint32_t kCompactionIntentMagic = 0x43594C47U;
inline constexpr std::uint16_t kCompactionIntentFormatVersion = 1;
inline constexpr std::size_t kCompactionIntentHeaderBytes = 128;
inline constexpr std::size_t kMaximumCompactionIntentBytes =
    kCompactionIntentHeaderBytes + 2U * kMaximumManifestBytes;

/**
 * @brief Recovery decision containing complete old/new catalogs for one Worker rewrite; old authority remains
 * until Manifest publication.
 */
struct DurableCompactionIntent {
    WorkerId worker_id{};
    Manifest old_manifest;
    Manifest next_manifest;

    auto operator==(const DurableCompactionIntent&) const -> bool = default;
};

[[nodiscard]] auto encoded_compaction_intent_size(const DurableCompactionIntent& intent)
    -> Result<std::size_t>;
/**
 * @brief Validate and explicitly encode canonical little-endian bytes; C++ object layout is never persisted.
 */
[[nodiscard]] auto encode_compaction_intent(std::span<std::byte> out, const DurableCompactionIntent& intent)
    -> Status;
/**
 * @brief Validate and explicitly encode canonical little-endian bytes; C++ object layout is never persisted.
 */
[[nodiscard]] auto encode_compaction_intent(const DurableCompactionIntent& intent)
    -> Result<std::vector<std::byte>>;
/**
 * @brief Validate encoded size, identity/version, and integrity before returning decoded metadata; input is
 * not retained.
 */
[[nodiscard]] auto decode_compaction_intent(std::span<const std::byte> bytes)
    -> Result<DurableCompactionIntent>;

} // namespace glifistore
