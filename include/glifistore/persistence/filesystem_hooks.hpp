/**
 * @file
 * @brief Shared filesystem fault and timing hooks used by deterministic campaigns.
 */
#pragma once

#include "glifistore/core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace glifistore {

/// `ordered` establishes the Record-before-commit ordering boundary. Platforms
/// without a distinct storage barrier conservatively implement it as a data sync.
enum class FileSyncMode { ordered, data, full };

/**
 * @brief Borrowed syscall callbacks with pread/pwrite/errno semantics; context must outlive every descriptor
 * retaining these hooks.
 */
struct FileIoHooks {
    /// Internal deterministic syscall seam. A negative result must set errno;
    /// a non-negative result has the same semantics as pread/pwrite.
    void* context{};
    auto (*read_some_at)(void* context, int descriptor, std::span<std::byte> bytes, std::uint64_t offset)
        -> std::ptrdiff_t{};
    auto (*write_some_at)(void* context, int descriptor, std::span<const std::byte> bytes,
                          std::uint64_t offset) -> std::ptrdiff_t{};
    auto (*sync_file)(void* context, int descriptor, FileSyncMode mode) -> int{};
};

/**
 * @brief Named persistence/backup/blob checkpoints used to inject and observe filesystem work.
 */
enum class FilesystemOperation {
    create_data_directory,
    sync_parent_directory,
    write_manifest,
    sync_manifest,
    rename_manifest,
    sync_directory,
    preallocate_segment,
    write_segment_header,
    sync_segment_file,
    rename_segment,
    write_record,
    sync_record,
    write_commit_slot,
    sync_commit_slot,
    write_bootstrap,
    sync_bootstrap,
    rename_bootstrap,
    remove_bootstrap,
    write_compaction_intent,
    sync_compaction_intent,
    rename_compaction_intent,
    remove_compaction_intent,
    remove_compaction_segment,
    // Online/offline catalog backup copy seams (crash/fault injection).
    copy_backup_segment,
    copy_backup_manifest,
    sync_backup_destination,
    write_blob_chunk,
    sync_blob_file,
    install_blob_file,
    link_blob_file,
    sync_blob_directory,
};

/**
 * @brief Borrowed fault/timing/free-space callbacks propagated into durable I/O; their context must outlive
 * all users.
 */
struct FilesystemHooks {
    /// Internal fault-injection and crash-test checkpoint seam. Production callers leave this empty.
    void* context{};
    auto (*before)(void* context, FilesystemOperation operation) -> Status{};
    void (*after)(void* context, FilesystemOperation operation){};
    auto (*available_space_bytes)(void* context) -> Result<std::uint64_t>{};
    FileIoHooks file_io{};
};

} // namespace glifistore
