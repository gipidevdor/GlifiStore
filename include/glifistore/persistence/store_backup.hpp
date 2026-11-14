/**
 * @file
 * @brief Verified offline or admission-fenced copy of an authoritative durable catalog.
 */
#pragma once

#include "glifistore/persistence/manifest.hpp"
#include "glifistore/persistence/store_verify.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace glifistore {

/**
 * @brief Source/destination verification and measured copy/fence work; counters alone do not certify a
 * durability gate.
 */
struct DurableStoreBackupReport {
    std::filesystem::path source{};
    std::filesystem::path destination{};
    std::size_t files_copied{};
    std::uint64_t bytes_copied{};
    /// Online Store::backup_to only: wall time admissions were closed (excludes destination verify).
    std::uint64_t admission_fence_ns{};
    /// Time spent flush + source verify + catalog file copy (catalog lock for online path).
    std::uint64_t catalog_copy_ns{};
    /// Destination verify after copy (may run after admissions resume on the online path).
    std::uint64_t destination_verify_ns{};
    /// Worker threads used for concurrent catalog Segment copies (Manifest is always sequential/last).
    std::size_t segment_copy_workers{};
    /// True when source/destination verify visited committed Record CRCs (not header record_count only).
    bool source_crc_scanned{};
    bool destination_crc_scanned{};
    DurableStoreVerifyReport source_verification{};
    DurableStoreVerifyReport destination_verification{};
};

/// Offline fail-closed backup: exclusive-lock source, verify, create an empty destination,
/// copy only Manifest + catalog Segment files (Segments first, Manifest last), sync, then
/// verify the destination. Does not copy locks, intents, or crash temporaries.
/// Concurrent external use against a live data directory still fails on the Store lock.
[[nodiscard]] auto backup_durable_store(const std::filesystem::path& source,
                                        const std::filesystem::path& destination, bool scan_records = true,
                                        const DurableResourceLimits& limits = {})
    -> Result<DurableStoreBackupReport>;

/// Online catalog copy from an already-locked DataDirectory (open Store path).
/// Caller must fence mutations for the copy window (Store::backup_to does this).
/// Still copies only Manifest + catalog Segments; destination must be empty.
/// When verify_destination is false, the caller must verify the destination before serving it.
/// When scan_source_records is false, source validation is structural only (no committed CRC scan);
/// online fenced backup uses this so CRC cost stays on the destination after admissions resume.
[[nodiscard]] auto backup_durable_store_from_open_directory(
    DataDirectory& source, const Manifest& catalog_manifest, const std::filesystem::path& destination,
    bool scan_records = true, const DurableResourceLimits& limits = {}, bool verify_destination = true,
    bool scan_source_records = true) -> Result<DurableStoreBackupReport>;

// Restore is the same offline verified copy into an empty destination directory.
[[nodiscard]] inline auto
restore_durable_store(const std::filesystem::path& backup, const std::filesystem::path& destination,
                      bool scan_records = true, const DurableResourceLimits& limits = {})
    -> Result<DurableStoreBackupReport> {
    return backup_durable_store(backup, destination, scan_records, limits);
}

} // namespace glifistore
