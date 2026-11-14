/**
 * @file
 * @brief Authoritative Segment catalog and explicit versioned Manifest codec.
 */
#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/core/types.hpp"
#include "glifistore/core/worker_routing.hpp"
#include "glifistore/persistence/blob.hpp"
#include "glifistore/segment/segment_header.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace glifistore {

inline constexpr std::uint32_t kManifestMagic = 0x4D594C47U;
inline constexpr std::uint16_t kManifestFormatVersion = 1;
inline constexpr std::size_t kManifestHeaderBytes = 128;
inline constexpr std::size_t kManifestSegmentEntryBytes = 32;
inline constexpr std::size_t kMaximumManifestSegmentCount = 1'000'000;
inline constexpr std::size_t kMaximumManifestBytes =
    kManifestHeaderBytes + kMaximumManifestSegmentCount * kManifestSegmentEntryBytes;
inline constexpr std::size_t kManifestWorkerHashSeedOffset = 84;

/**
 * @brief Persisted catalog role; numeric values belong to the format contract.
 */
enum class ManifestSegmentRole : std::uint16_t { active = 1, sealed = 2 };

/**
 * @brief Catalog-authoritative identity, owner, and role of one Segment.
 */
struct ManifestSegmentEntry {
    SegmentId segment_id{};
    GenerationId generation{};
    WorkerId owner_worker{};
    ManifestSegmentRole role{ManifestSegmentRole::sealed};

    auto operator==(const ManifestSegmentEntry&) const -> bool = default;
};

/**
 * @brief Authoritative durable namespace and persisted routing identity. Encode explicitly; never persist the
 * C++ struct layout.
 */
struct Manifest {
    StoreId store_id{};
    std::uint64_t manifest_generation{};
    RoutingAlgorithm routing_algorithm{RoutingAlgorithm::fnv1a64_v1};
    std::uint32_t worker_count{};
    std::uint64_t routing_epoch{};
    std::uint64_t worker_hash_seed{kDefaultWorkerHashSeed};
    SegmentId next_segment_id{};
    GenerationId next_segment_generation{GenerationId{1}};
    std::vector<ManifestSegmentEntry> segments{};
    std::uint16_t format_version{1};
    BlobId next_blob_id{std::byte{1}};

    [[nodiscard]] auto worker_routing() const noexcept -> WorkerRoutingState {
        return WorkerRoutingState{.algorithm = routing_algorithm, .seed = worker_hash_seed};
    }

    auto operator==(const Manifest&) const -> bool = default;
};

/**
 * @brief Index and generation of the newest unambiguous candidate; it does not own the candidate array.
 */
struct SelectedManifest {
    std::size_t candidate_index{};
    std::uint64_t manifest_generation{};
};

[[nodiscard]] auto encoded_manifest_size(const Manifest& manifest) -> Result<std::size_t>;
/**
 * @brief Validate and explicitly encode canonical little-endian bytes; C++ object layout is never persisted.
 */
[[nodiscard]] auto encode_manifest(std::span<std::byte> out, const Manifest& manifest) -> Status;
/**
 * @brief Validate and explicitly encode canonical little-endian bytes; C++ object layout is never persisted.
 */
[[nodiscard]] auto encode_manifest(const Manifest& manifest) -> Result<std::vector<std::byte>>;
/**
 * @brief Validate encoded size, identity/version, and integrity before returning decoded metadata; input is
 * not retained.
 */
[[nodiscard]] auto decode_manifest(std::span<const std::byte> bytes) -> Result<Manifest>;
/**
 * @brief Select unambiguous newest candidate metadata; conflicting authority is a failure, not a fallback
 * choice.
 */
[[nodiscard]] auto select_newest_manifest(std::span<const Manifest> candidates) -> Result<SelectedManifest>;

} // namespace glifistore
