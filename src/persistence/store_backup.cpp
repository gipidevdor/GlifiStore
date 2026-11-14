#include "glifistore/persistence/store_backup.hpp"

#include "glifistore/persistence/segment_file.hpp"
#include "persistence/blob_storage.hpp"
#include "system_error.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace glifistore {
namespace {

// Bounded concurrency for catalog Segment copies. Manifest is always copied last, alone.
// Keeps FD/buffer fan-out finite (BACKUP may run on the reactor thread joining workers).
constexpr std::size_t kBackupSegmentCopyParallelism = 4;

[[nodiscard]] auto interrupted_open(const char* path, int flags) -> int {
    int descriptor{};
    do {
        descriptor = ::open(path, flags);
    } while (descriptor < 0 && errno == EINTR);
    return descriptor;
}

[[nodiscard]] auto interrupted_open_at(int directory, const char* name, int flags, mode_t mode = 0) -> int {
    int descriptor{};
    do {
        descriptor = mode == 0 ? ::openat(directory, name, flags) : ::openat(directory, name, flags, mode);
    } while (descriptor < 0 && errno == EINTR);
    return descriptor;
}

[[nodiscard]] auto copy_named_file(int source_directory, int destination_directory, const char* name,
                                   const FileIoHooks& io_hooks) -> Result<std::uint64_t> {
    FileDescriptor source{interrupted_open_at(source_directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW),
                          io_hooks};
    if (!source.valid()) {
        if (errno == ENOENT) {
            return fail(ErrorCode::not_found, std::string{"backup source file is missing: "} + name);
        }
        return persistence_system_error("openat(backup source file)");
    }
    FileDescriptor destination{interrupted_open_at(destination_directory, name,
                                                   O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                                                   S_IRUSR | S_IWUSR),
                               io_hooks};
    if (!destination.valid()) {
        if (errno == EEXIST) {
            return fail(ErrorCode::sequence_conflict,
                        std::string{"backup destination already contains: "} + name);
        }
        return persistence_system_error("openat(backup destination file)");
    }

    const auto total_size = source.size();
    if (!total_size) {
        return unexpected(total_size.error());
    }

    // Heap buffer: wire BACKUP runs on the reactor thread (smaller stack than main).
    // Positional copy so FilesystemHooks::file_io (EINTR / short / capacity) applies.
    constexpr std::size_t kCopyBufferBytes = 1U << 20;
    const auto buffer = std::make_unique<std::byte[]>(kCopyBufferBytes);
    std::uint64_t offset{};
    while (offset < *total_size) {
        const auto remaining = *total_size - offset;
        const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, kCopyBufferBytes));
        const std::span<std::byte> window{buffer.get(), chunk};
        if (auto read = source.read_exact_at(window, offset); !read) {
            return unexpected(read.error());
        }
        if (auto written = destination.write_all_at(window, offset); !written) {
            return unexpected(written.error());
        }
        offset += chunk;
    }
    if (auto synced = destination.sync(FileSyncMode::full); !synced) {
        return unexpected(synced.error());
    }
    return *total_size;
}

struct SegmentCopyTotals {
    std::size_t files_copied{};
    std::uint64_t bytes_copied{};
    std::size_t workers_used{};
};

[[nodiscard]] auto copy_catalog_segments(int source_directory, int destination_directory,
                                         const Manifest& catalog, const std::size_t max_parallelism,
                                         const FilesystemHooks& hooks) -> Result<SegmentCopyTotals> {
    std::vector<std::string> names;
    names.reserve(catalog.segments.size());
    for (const auto& entry : catalog.segments) {
        const SegmentHeaderIdentity identity{.store_id = catalog.store_id,
                                             .segment_id = entry.segment_id,
                                             .generation = entry.generation,
                                             .owner_worker = entry.owner_worker,
                                             .format_version = catalog.format_version};
        names.push_back(segment_filename(identity));
    }
    if (names.empty()) {
        return SegmentCopyTotals{};
    }

    // Checkpoint/fault hooks are not thread-safe across parallel copy workers; serialize when armed.
    const bool hooks_armed = hooks.before != nullptr || hooks.after != nullptr ||
                             hooks.file_io.read_some_at != nullptr ||
                             hooks.file_io.write_some_at != nullptr || hooks.file_io.sync_file != nullptr;
    const auto workers = hooks_armed ? std::size_t{1}
                                     : std::max<std::size_t>(1, std::min({max_parallelism, names.size(),
                                                                          kBackupSegmentCopyParallelism}));

    if (workers == 1) {
        SegmentCopyTotals totals{.workers_used = 1};
        for (const auto& name : names) {
            if (auto allowed =
                    detail::invoke_filesystem_before(hooks, FilesystemOperation::copy_backup_segment);
                !allowed) {
                return unexpected(allowed.error());
            }
            auto copied =
                copy_named_file(source_directory, destination_directory, name.c_str(), hooks.file_io);
            if (!copied) {
                return unexpected(copied.error());
            }
            ++totals.files_copied;
            totals.bytes_copied += *copied;
            detail::invoke_filesystem_after(hooks, FilesystemOperation::copy_backup_segment);
        }
        return totals;
    }

    std::atomic<std::size_t> next_index{0};
    std::atomic<std::uint64_t> bytes_copied{0};
    std::atomic_bool failed{false};
    std::mutex error_mutex;
    std::optional<Error> first_error;

    const auto worker_fn = [&] {
        for (;;) {
            if (failed.load(std::memory_order_relaxed)) {
                return;
            }
            const auto index = next_index.fetch_add(1, std::memory_order_relaxed);
            if (index >= names.size()) {
                return;
            }
            auto copied =
                copy_named_file(source_directory, destination_directory, names[index].c_str(), hooks.file_io);
            if (!copied) {
                failed.store(true, std::memory_order_relaxed);
                const std::lock_guard lock{error_mutex};
                if (!first_error.has_value()) {
                    first_error = std::move(copied.error());
                }
                return;
            }
            bytes_copied.fetch_add(*copied, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) {
        threads.emplace_back(worker_fn);
    }
    for (auto& thread : threads) {
        thread.join();
    }

    if (first_error.has_value()) {
        return unexpected(std::move(*first_error));
    }
    return SegmentCopyTotals{
        .files_copied = names.size(),
        .bytes_copied = bytes_copied.load(std::memory_order_relaxed),
        .workers_used = workers,
    };
}

} // namespace

auto backup_durable_store_from_open_directory(DataDirectory& source, const Manifest& catalog_manifest,
                                              const std::filesystem::path& destination,
                                              const bool scan_records, const DurableResourceLimits& limits,
                                              const bool verify_destination, const bool scan_source_records)
    -> Result<DurableStoreBackupReport> {
    if (destination.empty()) {
        return fail(ErrorCode::invalid_argument, "backup destination path is required");
    }
    if (!source.healthy()) {
        return fail(ErrorCode::unavailable, "cannot backup through a poisoned data-directory instance");
    }

    const auto copy_started = std::chrono::steady_clock::now();
    DurableStoreBackupReport report{.destination = destination};

    auto source_verification = verify_durable_store(source, scan_source_records, limits);
    if (!source_verification) {
        return unexpected(source_verification.error());
    }
    // Prefer the caller's catalog snapshot when verify rebuilt an equivalent Manifest.
    if (source_verification->manifest.store_id != catalog_manifest.store_id ||
        source_verification->manifest.segments.size() != catalog_manifest.segments.size()) {
        return fail(ErrorCode::corrupted_data,
                    "open-store backup Manifest disagrees with the live catalog snapshot");
    }
    report.source_verification = std::move(*source_verification);
    report.source_crc_scanned = scan_source_records;

    {
        auto destination_locked =
            DataDirectory::open_and_lock(destination, DataDirectoryOpenMode::create_new);
        if (!destination_locked) {
            return unexpected(destination_locked.error());
        }
        if (auto pristine = destination_locked->pristine_for_bootstrap(); !pristine) {
            return unexpected(pristine.error());
        } else if (!*pristine) {
            return fail(ErrorCode::invalid_argument, "backup destination data directory is not empty");
        }

        FileDescriptor destination_root{
            interrupted_open(destination.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
        if (!destination_root.valid()) {
            return persistence_system_error("open(backup destination directory for copy)");
        }

        const int source_fd = source.directory_descriptor();
        const auto& hooks = source.hooks();
        auto segments_copied = copy_catalog_segments(source_fd, destination_root.get(), catalog_manifest,
                                                     kBackupSegmentCopyParallelism, hooks);
        if (!segments_copied) {
            return unexpected(segments_copied.error());
        }
        report.files_copied += segments_copied->files_copied;
        report.bytes_copied += segments_copied->bytes_copied;
        report.segment_copy_workers = segments_copied->workers_used;

        if (catalog_manifest.format_version == 2) {
            if (auto copied = detail::copy_blob_closure(source, *destination_locked, catalog_manifest, {},
                                                        report.files_copied, report.bytes_copied);
                !copied) {
                return unexpected(copied.error());
            }
        }
        if (auto allowed = detail::invoke_filesystem_before(hooks, FilesystemOperation::copy_backup_manifest);
            !allowed) {
            return unexpected(allowed.error());
        }
        auto manifest_copied =
            copy_named_file(source_fd, destination_root.get(), kManifestFilename, hooks.file_io);
        if (!manifest_copied) {
            return unexpected(manifest_copied.error());
        }
        ++report.files_copied;
        report.bytes_copied += *manifest_copied;
        detail::invoke_filesystem_after(hooks, FilesystemOperation::copy_backup_manifest);

        if (auto allowed =
                detail::invoke_filesystem_before(hooks, FilesystemOperation::sync_backup_destination);
            !allowed) {
            return unexpected(allowed.error());
        }
        if (auto synced = destination_root.sync(FileSyncMode::full); !synced) {
            return unexpected(synced.error());
        }
        detail::invoke_filesystem_after(hooks, FilesystemOperation::sync_backup_destination);
    } // release destination exclusive lock before verify re-opens the path

    report.catalog_copy_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - copy_started)
            .count());

    if (!verify_destination) {
        return report;
    }

    const auto verify_started = std::chrono::steady_clock::now();
    auto destination_verification = verify_durable_store_path(destination, scan_records, limits);
    if (!destination_verification) {
        return unexpected(destination_verification.error());
    }
    report.destination_verification = std::move(*destination_verification);
    report.destination_crc_scanned = scan_records;
    report.destination_verify_ns =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - verify_started)
                                       .count());
    return report;
}

auto backup_durable_store(const std::filesystem::path& source, const std::filesystem::path& destination,
                          const bool scan_records, const DurableResourceLimits& limits)
    -> Result<DurableStoreBackupReport> {
    if (source.empty() || destination.empty()) {
        return fail(ErrorCode::invalid_argument, "backup source and destination paths are required");
    }
    if (source == destination) {
        return fail(ErrorCode::invalid_argument, "backup source and destination must differ");
    }

    auto source_locked = DataDirectory::open_and_lock(source, DataDirectoryOpenMode::existing);
    if (!source_locked) {
        return unexpected(source_locked.error());
    }
    auto source_verification = verify_durable_store(*source_locked, scan_records, limits);
    if (!source_verification) {
        return unexpected(source_verification.error());
    }
    source_verification->path = source;
    const Manifest catalog = source_verification->manifest;
    // Source already CRC-scanned above; skip a second source scan inside the copy helper.
    auto backed = backup_durable_store_from_open_directory(*source_locked, catalog, destination, scan_records,
                                                           limits, /*verify_destination=*/true,
                                                           /*scan_source_records=*/false);
    if (!backed) {
        return unexpected(backed.error());
    }
    backed->source = source;
    backed->source_verification = std::move(*source_verification);
    backed->source_verification.path = source;
    backed->source_crc_scanned = scan_records;
    return backed;
}

} // namespace glifistore
