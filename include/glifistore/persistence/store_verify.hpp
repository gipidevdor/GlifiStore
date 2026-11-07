/**
 * @file
 * @brief Read-only structural catalog verification with optional committed Record CRC scans.
 */
#pragma once

#include "glifistore/persistence/filesystem.hpp"
#include "glifistore/persistence/manifest.hpp"
#include "glifistore/persistence/namespace_audit.hpp"
#include "glifistore/persistence/resource_limits.hpp"
#include "glifistore/segment/segment_header.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace glifistore {

/**
 * @brief Selected committed extent and optional Record-scan observations for one catalog entry.
 */
struct DurableStoreVerifiedSegment {
    ManifestSegmentEntry entry{};
    SelectedSegmentCommit selected{};
    std::uint64_t scanned_records{};
    bool active_requires_rotation{};
};

/**
 * @brief Structural verification snapshot of a stopped or externally fenced catalog, with optional Record CRC
 * evidence.
 */
struct DurableStoreVerifyReport {
    std::filesystem::path path{};
    Manifest manifest{};
    NamespaceAuditReport namespace_audit{};
    std::vector<DurableStoreVerifiedSegment> segments{};
    std::uint64_t scanned_records{};
    std::size_t active_requires_rotation_count{};
};

/// Read-only structural validation of a stopped durable Store: Manifest decode,
/// namespace audit (recovery-safe), and per-catalog Segment open + optional
/// committed CRC scan. Requires an already-locked DataDirectory. Does not rebuild
/// Indexes, check routing, or repair files.
[[nodiscard]] auto verify_durable_store(DataDirectory& directory, bool scan_records = true,
                                        const DurableResourceLimits& limits = {})
    -> Result<DurableStoreVerifyReport>;

/// Exclusive-locks the data directory, verifies, then releases the lock on return.
[[nodiscard]] auto verify_durable_store_path(const std::filesystem::path& path, bool scan_records = true,
                                             const DurableResourceLimits& limits = {})
    -> Result<DurableStoreVerifyReport>;

} // namespace glifistore
