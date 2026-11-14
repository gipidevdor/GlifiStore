#include "filesystem_detail.hpp"
#include "glifistore/persistence/filesystem.hpp"
#include "glifistore/persistence/namespace_audit.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "system_error.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <vector>

namespace glifistore {

using persistence_detail::compaction_publication_failure;
using persistence_detail::compaction_removal_failure;
using persistence_detail::compaction_retirement_failure;
using persistence_detail::interrupted_mkdir_at;
using persistence_detail::interrupted_open;
using persistence_detail::interrupted_open_at;
using persistence_detail::lock_exclusive_nonblocking;
using persistence_detail::publication_failure;
using persistence_detail::read_private_compaction_intent_file;
using persistence_detail::read_private_manifest_file;
using persistence_detail::unlink_temporary;
using persistence_detail::validate_directory_descriptor;
using persistence_detail::validate_private_regular_file;

DataDirectory::~DataDirectory() {
    if (health_) {
        health_->store(false, std::memory_order_release);
    }
}

auto DataDirectory::operator=(DataDirectory&& other) noexcept -> DataDirectory& {
    if (this != &other) {
        if (health_) {
            health_->store(false, std::memory_order_release);
        }
        directory_ = std::move(other.directory_);
        lock_ = std::move(other.lock_);
        hooks_ = other.hooks_;
        health_ = std::move(other.health_);
    }
    return *this;
}

auto DataDirectory::open_and_lock(const std::filesystem::path& path, const FilesystemHooks hooks)
    -> Result<DataDirectory> {
    return open_and_lock(path, DataDirectoryOpenMode::existing, hooks);
}

auto DataDirectory::open_and_lock(const std::filesystem::path& path, const DataDirectoryOpenMode mode,
                                  const FilesystemHooks hooks) -> Result<DataDirectory> {
    if (path.empty()) {
        return fail(ErrorCode::invalid_argument, "data directory path cannot be empty");
    }
    FileDescriptor directory;
    if (mode == DataDirectoryOpenMode::existing) {
        directory.reset(interrupted_open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW),
                        hooks.file_io);
    } else {
        const auto leaf = path.filename();
        auto parent = path.parent_path();
        if (leaf.empty() || leaf == "." || leaf == "..") {
            return fail(ErrorCode::invalid_argument,
                        "creatable data directory must have a canonical leaf name");
        }
        if (parent.empty()) {
            parent = ".";
        }
        FileDescriptor parent_directory{
            interrupted_open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW), hooks.file_io};
        if (!parent_directory.valid()) {
            return persistence_system_error("open(data-directory parent)");
        }
        struct stat existing{};
        int inspected{};
        do {
            inspected = ::fstatat(parent_directory.get(), leaf.c_str(), &existing, AT_SYMLINK_NOFOLLOW);
        } while (inspected != 0 && errno == EINTR);
        if (inspected == 0 && mode == DataDirectoryOpenMode::create_new) {
            return fail(ErrorCode::sequence_conflict, "data directory already exists");
        }
        if (inspected != 0 && errno != ENOENT) {
            return persistence_system_error("fstatat(data directory)");
        }
        if (inspected != 0) {
            if (auto allowed =
                    detail::invoke_filesystem_before(hooks, FilesystemOperation::create_data_directory);
                !allowed) {
                return unexpected(allowed.error());
            }
            if (interrupted_mkdir_at(parent_directory.get(), leaf.c_str(), S_IRWXU) == 0) {
                detail::invoke_filesystem_after(hooks, FilesystemOperation::create_data_directory);
                if (auto allowed =
                        detail::invoke_filesystem_before(hooks, FilesystemOperation::sync_parent_directory);
                    !allowed) {
                    return unexpected(allowed.error());
                }
                if (auto synced = parent_directory.sync(FileSyncMode::full); !synced) {
                    return unexpected(synced.error());
                }
                detail::invoke_filesystem_after(hooks, FilesystemOperation::sync_parent_directory);
            } else if (errno != EEXIST || mode == DataDirectoryOpenMode::create_new) {
                if (errno == EEXIST) {
                    return fail(ErrorCode::sequence_conflict, "data directory already exists");
                }
                return persistence_system_error("mkdirat(data directory)");
            }
        }
        directory.reset(interrupted_open_at(parent_directory.get(), leaf.c_str(),
                                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW),
                        hooks.file_io);
    }
    if (!directory.valid()) {
        return persistence_system_error("open(data directory)");
    }
    if (auto valid = validate_directory_descriptor(directory.get()); !valid) {
        return unexpected(valid.error());
    }

    FileDescriptor lock{interrupted_open_at(directory.get(), kStoreLockFilename,
                                            O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
                                            S_IRUSR | S_IWUSR)};
    if (!lock.valid()) {
        return persistence_system_error("openat(Store lock)");
    }
    if (auto valid = validate_private_regular_file(lock.get(), "Store lock"); !valid) {
        return unexpected(valid.error());
    }
    if (auto acquired = lock_exclusive_nonblocking(lock.get()); !acquired) {
        return unexpected(acquired.error());
    }
    return DataDirectory{std::move(directory), std::move(lock), hooks};
}

auto DataDirectory::before(const FilesystemOperation operation) const -> Status {
    return detail::invoke_filesystem_before(hooks_, operation);
}

void DataDirectory::after(const FilesystemOperation operation) const noexcept {
    detail::invoke_filesystem_after(hooks_, operation);
}

auto DataDirectory::sync_directory() const -> Status {
    return directory_.sync(FileSyncMode::full);
}

auto DataDirectory::open_directory_for_enumeration() const -> Result<FileDescriptor> {
    if (!healthy()) {
        return fail(ErrorCode::io_error, "cannot enumerate through a poisoned data-directory instance");
    }
    FileDescriptor enumeration{interrupted_open_at(
        directory_.get(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (!enumeration.valid()) {
        return persistence_system_error("openat(data directory for enumeration)");
    }
    if (auto valid = validate_directory_descriptor(enumeration.get()); !valid) {
        return unexpected(valid.error());
    }
    return enumeration;
}

auto DataDirectory::available_space_bytes() const -> Result<std::uint64_t> {
    if (hooks_.available_space_bytes != nullptr) {
        return hooks_.available_space_bytes(hooks_.context);
    }
    struct statvfs status{};
    if (::fstatvfs(directory_.get(), &status) != 0) {
        return persistence_system_error("fstatvfs(data directory)");
    }
    const auto block_size = status.f_frsize != 0 ? status.f_frsize : status.f_bsize;
    const auto available_blocks = status.f_bavail;
    if (block_size != 0 && available_blocks > std::numeric_limits<std::uint64_t>::max() / block_size) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(available_blocks) * static_cast<std::uint64_t>(block_size);
}

auto DataDirectory::publish_manifest(const Manifest& manifest, const std::size_t max_bytes)
    -> ManifestPublicationResult {
    if (!healthy()) {
        return publication_failure(
            ManifestPublicationOutcome::indeterminate,
            Error{ErrorCode::io_error, "data directory is poisoned by an indeterminate publication"});
    }
    const auto encoded = encode_manifest(manifest);
    if (!encoded) {
        return publication_failure(ManifestPublicationOutcome::not_published, encoded.error());
    }
    if (encoded->size() > max_bytes) {
        return publication_failure(
            ManifestPublicationOutcome::not_published,
            Error{ErrorCode::storage_exhausted, "manifest exceeds the configured byte budget"});
    }
    const auto current = read_manifest(max_bytes);
    if (current) {
        if (manifest.manifest_generation <= current->manifest_generation) {
            return publication_failure(ManifestPublicationOutcome::not_published,
                                       Error{ErrorCode::sequence_conflict,
                                             "manifest publication generation must advance monotonically"});
        }
    } else if (current.error().code != ErrorCode::not_found) {
        return publication_failure(ManifestPublicationOutcome::not_published, current.error());
    }
    if (auto removed = unlink_temporary(directory_.get()); !removed) {
        return publication_failure(ManifestPublicationOutcome::not_published, removed.error());
    }

    FileDescriptor temporary{
        interrupted_open_at(directory_.get(), kManifestTemporaryFilename,
                            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
                            S_IRUSR | S_IWUSR),
        hooks_.file_io};
    if (!temporary.valid()) {
        return publication_failure(ManifestPublicationOutcome::not_published,
                                   persistence_system_error("openat(manifest temporary)").error);
    }

    if (auto allowed = before(FilesystemOperation::write_manifest); !allowed) {
        static_cast<void>(unlink_temporary(directory_.get()));
        return publication_failure(ManifestPublicationOutcome::not_published, allowed.error());
    }
    if (auto written = temporary.write_all_at(*encoded, 0); !written) {
        static_cast<void>(unlink_temporary(directory_.get()));
        return publication_failure(ManifestPublicationOutcome::not_published, written.error());
    }
    after(FilesystemOperation::write_manifest);
    if (auto allowed = before(FilesystemOperation::sync_manifest); !allowed) {
        static_cast<void>(unlink_temporary(directory_.get()));
        return publication_failure(ManifestPublicationOutcome::not_published, allowed.error());
    }
    if (auto synced = temporary.sync(FileSyncMode::full); !synced) {
        static_cast<void>(unlink_temporary(directory_.get()));
        return publication_failure(ManifestPublicationOutcome::not_published, synced.error());
    }
    after(FilesystemOperation::sync_manifest);
    if (auto allowed = before(FilesystemOperation::rename_manifest); !allowed) {
        static_cast<void>(unlink_temporary(directory_.get()));
        return publication_failure(ManifestPublicationOutcome::not_published, allowed.error());
    }

    if (::renameat(directory_.get(), kManifestTemporaryFilename, directory_.get(), kManifestFilename) != 0) {
        health_->store(false, std::memory_order_release);
        return publication_failure(ManifestPublicationOutcome::indeterminate,
                                   persistence_system_error("renameat(manifest)").error);
    }
    after(FilesystemOperation::rename_manifest);
    if (auto allowed = before(FilesystemOperation::sync_directory); !allowed) {
        health_->store(false, std::memory_order_release);
        return publication_failure(ManifestPublicationOutcome::indeterminate, allowed.error());
    }
    if (auto synced = sync_directory(); !synced) {
        health_->store(false, std::memory_order_release);
        return publication_failure(ManifestPublicationOutcome::indeterminate, synced.error());
    }
    after(FilesystemOperation::sync_directory);
    return {.outcome = ManifestPublicationOutcome::durable, .error = std::nullopt};
}

auto DataDirectory::publish_bootstrap_intent(const Manifest& manifest, const std::size_t max_bytes)
    -> Status {
    if (!healthy()) {
        return fail(ErrorCode::io_error, "cannot publish bootstrap intent through a poisoned directory");
    }
    auto encoded = encode_manifest(manifest);
    if (!encoded) {
        return unexpected(encoded.error());
    }
    if (encoded->size() > max_bytes) {
        return fail(ErrorCode::storage_exhausted, "bootstrap intent exceeds the configured byte budget");
    }
    struct stat existing{};
    if (::fstatat(directory_.get(), kBootstrapIntentFilename, &existing, AT_SYMLINK_NOFOLLOW) == 0) {
        return fail(ErrorCode::sequence_conflict, "bootstrap intent already exists");
    }
    if (errno != ENOENT) {
        return persistence_system_error("fstatat(bootstrap intent)");
    }
    if (::unlinkat(directory_.get(), kBootstrapTemporaryFilename, 0) != 0 && errno != ENOENT) {
        return persistence_system_error("unlinkat(bootstrap temporary)");
    }
    FileDescriptor intent_file{
        interrupted_open_at(directory_.get(), kBootstrapTemporaryFilename,
                            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
                            S_IRUSR | S_IWUSR),
        hooks_.file_io};
    if (!intent_file.valid()) {
        return persistence_system_error("openat(bootstrap temporary)");
    }
    const auto cleanup = [this]() -> Status {
        if (::unlinkat(directory_.get(), kBootstrapTemporaryFilename, 0) != 0 && errno != ENOENT) {
            return persistence_system_error("unlinkat(bootstrap temporary cleanup)");
        }
        return sync_directory();
    };
    if (auto allowed = before(FilesystemOperation::write_bootstrap); !allowed) {
        if (auto cleaned = cleanup(); !cleaned) {
            health_->store(false, std::memory_order_release);
            return cleaned;
        }
        return allowed;
    }
    if (auto written = intent_file.write_all_at(*encoded, 0); !written) {
        if (auto cleaned = cleanup(); !cleaned) {
            health_->store(false, std::memory_order_release);
            return cleaned;
        }
        return unexpected(written.error());
    }
    after(FilesystemOperation::write_bootstrap);
    if (auto allowed = before(FilesystemOperation::sync_bootstrap); !allowed) {
        if (auto cleaned = cleanup(); !cleaned) {
            health_->store(false, std::memory_order_release);
            return cleaned;
        }
        return allowed;
    }
    if (auto synced = intent_file.sync(FileSyncMode::full); !synced) {
        if (auto cleaned = cleanup(); !cleaned) {
            health_->store(false, std::memory_order_release);
            return cleaned;
        }
        return unexpected(synced.error());
    }
    after(FilesystemOperation::sync_bootstrap);
    if (auto allowed = before(FilesystemOperation::rename_bootstrap); !allowed) {
        if (auto cleaned = cleanup(); !cleaned) {
            health_->store(false, std::memory_order_release);
            return cleaned;
        }
        return allowed;
    }
    if (::renameat(directory_.get(), kBootstrapTemporaryFilename, directory_.get(),
                   kBootstrapIntentFilename) != 0) {
        health_->store(false, std::memory_order_release);
        return persistence_system_error("renameat(bootstrap intent)");
    }
    after(FilesystemOperation::rename_bootstrap);
    if (auto allowed = before(FilesystemOperation::sync_directory); !allowed) {
        health_->store(false, std::memory_order_release);
        return allowed;
    }
    if (auto synced = sync_directory(); !synced) {
        health_->store(false, std::memory_order_release);
        return synced;
    }
    after(FilesystemOperation::sync_directory);
    return {};
}

auto DataDirectory::read_bootstrap_intent(const std::size_t max_bytes) const -> Result<Manifest> {
    if (!healthy()) {
        return fail(ErrorCode::io_error, "cannot read bootstrap intent through a poisoned directory");
    }
    return read_private_manifest_file(directory_.get(), kBootstrapIntentFilename, "bootstrap intent",
                                      max_bytes, hooks_.file_io);
}

auto DataDirectory::finish_bootstrap() -> Status {
    if (!healthy()) {
        return fail(ErrorCode::io_error, "cannot finish bootstrap through a poisoned directory");
    }
    if (auto allowed = before(FilesystemOperation::remove_bootstrap); !allowed) {
        return allowed;
    }
    if (::unlinkat(directory_.get(), kBootstrapIntentFilename, 0) != 0) {
        if (errno == ENOENT) {
            return fail(ErrorCode::not_found, "bootstrap intent does not exist");
        }
        return persistence_system_error("unlinkat(bootstrap intent)");
    }
    after(FilesystemOperation::remove_bootstrap);
    if (auto allowed = before(FilesystemOperation::sync_directory); !allowed) {
        health_->store(false, std::memory_order_release);
        return allowed;
    }
    if (auto synced = sync_directory(); !synced) {
        health_->store(false, std::memory_order_release);
        return synced;
    }
    after(FilesystemOperation::sync_directory);
    return {};
}

auto DataDirectory::publish_compaction_intent(const DurableCompactionIntent& intent,
                                              const std::size_t max_manifest_bytes)
    -> CompactionIntentPublicationResult {
    if (!healthy()) {
        return compaction_publication_failure(
            CompactionIntentPublicationOutcome::indeterminate,
            Error{ErrorCode::io_error, "cannot publish compaction intent through a poisoned directory"});
    }
    if (max_manifest_bytes > kMaximumManifestBytes) {
        return compaction_publication_failure(
            CompactionIntentPublicationOutcome::not_published,
            Error{ErrorCode::invalid_argument,
                  "compaction intent manifest byte budget exceeds the supported format bound"});
    }
    const auto old_size = encoded_manifest_size(intent.old_manifest);
    const auto next_size = encoded_manifest_size(intent.next_manifest);
    const auto encoded = encode_compaction_intent(intent);
    if (!old_size || !next_size || !encoded) {
        if (!old_size) {
            return compaction_publication_failure(CompactionIntentPublicationOutcome::not_published,
                                                  old_size.error());
        }
        return compaction_publication_failure(CompactionIntentPublicationOutcome::not_published,
                                              !next_size ? next_size.error() : encoded.error());
    }
    if (*old_size > max_manifest_bytes || *next_size > max_manifest_bytes) {
        return compaction_publication_failure(
            CompactionIntentPublicationOutcome::not_published,
            Error{ErrorCode::storage_exhausted,
                  "compaction intent exceeds the configured manifest byte budget"});
    }
    struct stat existing{};
    int inspected{};
    do {
        inspected = ::fstatat(directory_.get(), kCompactionIntentFilename, &existing, AT_SYMLINK_NOFOLLOW);
    } while (inspected != 0 && errno == EINTR);
    if (inspected == 0) {
        return compaction_publication_failure(
            CompactionIntentPublicationOutcome::not_published,
            Error{ErrorCode::sequence_conflict, "compaction intent already exists"});
    }
    if (errno != ENOENT) {
        return compaction_publication_failure(CompactionIntentPublicationOutcome::not_published,
                                              persistence_system_error("fstatat(compaction intent)").error);
    }
    if (::unlinkat(directory_.get(), kCompactionTemporaryFilename, 0) != 0 && errno != ENOENT) {
        return compaction_publication_failure(
            CompactionIntentPublicationOutcome::not_published,
            persistence_system_error("unlinkat(compaction temporary)").error);
    }

    FileDescriptor intent_file{
        interrupted_open_at(directory_.get(), kCompactionTemporaryFilename,
                            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
                            S_IRUSR | S_IWUSR),
        hooks_.file_io};
    if (!intent_file.valid()) {
        return compaction_publication_failure(CompactionIntentPublicationOutcome::not_published,
                                              persistence_system_error("openat(compaction temporary)").error);
    }
    const auto cleanup = [this]() -> Status {
        if (::unlinkat(directory_.get(), kCompactionTemporaryFilename, 0) != 0 && errno != ENOENT) {
            return persistence_system_error("unlinkat(compaction temporary cleanup)");
        }
        return sync_directory();
    };
    const auto fail_before_rename = [&](Error error) -> CompactionIntentPublicationResult {
        if (auto cleaned = cleanup(); !cleaned) {
            health_->store(false, std::memory_order_release);
            return compaction_publication_failure(CompactionIntentPublicationOutcome::indeterminate,
                                                  cleaned.error());
        }
        return compaction_publication_failure(CompactionIntentPublicationOutcome::not_published,
                                              std::move(error));
    };

    if (auto allowed = before(FilesystemOperation::write_compaction_intent); !allowed) {
        return fail_before_rename(allowed.error());
    }
    if (auto written = intent_file.write_all_at(*encoded, 0); !written) {
        return fail_before_rename(written.error());
    }
    after(FilesystemOperation::write_compaction_intent);
    if (auto allowed = before(FilesystemOperation::sync_compaction_intent); !allowed) {
        return fail_before_rename(allowed.error());
    }
    if (auto synced = intent_file.sync(FileSyncMode::full); !synced) {
        return fail_before_rename(synced.error());
    }
    after(FilesystemOperation::sync_compaction_intent);
    if (auto allowed = before(FilesystemOperation::rename_compaction_intent); !allowed) {
        return fail_before_rename(allowed.error());
    }
    if (::renameat(directory_.get(), kCompactionTemporaryFilename, directory_.get(),
                   kCompactionIntentFilename) != 0) {
        health_->store(false, std::memory_order_release);
        return compaction_publication_failure(CompactionIntentPublicationOutcome::indeterminate,
                                              persistence_system_error("renameat(compaction intent)").error);
    }
    after(FilesystemOperation::rename_compaction_intent);
    if (auto allowed = before(FilesystemOperation::sync_directory); !allowed) {
        health_->store(false, std::memory_order_release);
        return compaction_publication_failure(CompactionIntentPublicationOutcome::indeterminate,
                                              allowed.error());
    }
    if (auto synced = sync_directory(); !synced) {
        health_->store(false, std::memory_order_release);
        return compaction_publication_failure(CompactionIntentPublicationOutcome::indeterminate,
                                              synced.error());
    }
    after(FilesystemOperation::sync_directory);
    return {.outcome = CompactionIntentPublicationOutcome::durable, .error = std::nullopt};
}

auto DataDirectory::read_compaction_intent(const std::size_t max_manifest_bytes) const
    -> Result<DurableCompactionIntent> {
    if (!healthy()) {
        return fail(ErrorCode::io_error, "cannot read compaction intent through a poisoned directory");
    }
    return read_private_compaction_intent_file(directory_.get(), max_manifest_bytes, hooks_.file_io);
}

auto DataDirectory::remove_compaction_intent() -> CompactionIntentRemovalResult {
    if (!healthy()) {
        return compaction_removal_failure(
            CompactionIntentRemovalOutcome::indeterminate,
            Error{ErrorCode::io_error, "cannot remove compaction intent through a poisoned directory"});
    }
    if (::unlinkat(directory_.get(), kCompactionTemporaryFilename, 0) != 0 && errno != ENOENT) {
        health_->store(false, std::memory_order_release);
        return compaction_removal_failure(
            CompactionIntentRemovalOutcome::indeterminate,
            persistence_system_error("unlinkat(compaction temporary during completion)").error);
    }
    if (auto allowed = before(FilesystemOperation::remove_compaction_intent); !allowed) {
        return compaction_removal_failure(CompactionIntentRemovalOutcome::not_removed, allowed.error());
    }
    if (::unlinkat(directory_.get(), kCompactionIntentFilename, 0) != 0) {
        if (errno == ENOENT) {
            return compaction_removal_failure(
                CompactionIntentRemovalOutcome::not_removed,
                Error{ErrorCode::not_found, "compaction intent does not exist"});
        }
        health_->store(false, std::memory_order_release);
        return compaction_removal_failure(CompactionIntentRemovalOutcome::indeterminate,
                                          persistence_system_error("unlinkat(compaction intent)").error);
    }
    after(FilesystemOperation::remove_compaction_intent);
    if (auto allowed = before(FilesystemOperation::sync_directory); !allowed) {
        health_->store(false, std::memory_order_release);
        return compaction_removal_failure(CompactionIntentRemovalOutcome::indeterminate, allowed.error());
    }
    if (auto synced = sync_directory(); !synced) {
        health_->store(false, std::memory_order_release);
        return compaction_removal_failure(CompactionIntentRemovalOutcome::indeterminate, synced.error());
    }
    after(FilesystemOperation::sync_directory);
    return {.outcome = CompactionIntentRemovalOutcome::durable, .error = std::nullopt};
}

auto DataDirectory::retire_compaction_segments(const StoreId& store_id,
                                               const std::span<const ManifestSegmentEntry> segments)
    -> CompactionSegmentRetirementResult {
    if (!healthy()) {
        return compaction_retirement_failure(
            CompactionSegmentRetirementOutcome::indeterminate,
            Error{ErrorCode::io_error, "cannot retire Segments through a poisoned directory"});
    }
    bool removed_any{};
    for (const auto& entry : segments) {
        const auto name = segment_filename({.store_id = store_id,
                                            .segment_id = entry.segment_id,
                                            .generation = entry.generation,
                                            .owner_worker = entry.owner_worker});
        const auto temporary_name = '.' + name + ".tmp";
        if (auto allowed = before(FilesystemOperation::remove_compaction_segment); !allowed) {
            if (removed_any) {
                health_->store(false, std::memory_order_release);
            }
            return compaction_retirement_failure(removed_any
                                                     ? CompactionSegmentRetirementOutcome::indeterminate
                                                     : CompactionSegmentRetirementOutcome::not_removed,
                                                 allowed.error());
        }
        bool removed_identity{};
        if (::unlinkat(directory_.get(), temporary_name.c_str(), 0) == 0) {
            removed_identity = true;
        } else if (errno != ENOENT) {
            health_->store(false, std::memory_order_release);
            return compaction_retirement_failure(
                CompactionSegmentRetirementOutcome::indeterminate,
                persistence_system_error("unlinkat(compaction Segment temporary)").error);
        }
        if (::unlinkat(directory_.get(), name.c_str(), 0) != 0) {
            if (errno == ENOENT && !removed_identity) {
                continue;
            }
            if (errno != ENOENT) {
                health_->store(false, std::memory_order_release);
                return compaction_retirement_failure(
                    CompactionSegmentRetirementOutcome::indeterminate,
                    persistence_system_error("unlinkat(compaction Segment)").error);
            }
        }
        removed_any = true;
        after(FilesystemOperation::remove_compaction_segment);
    }
    if (auto allowed = before(FilesystemOperation::sync_directory); !allowed) {
        health_->store(false, std::memory_order_release);
        return compaction_retirement_failure(CompactionSegmentRetirementOutcome::indeterminate,
                                             allowed.error());
    }
    if (auto synced = sync_directory(); !synced) {
        health_->store(false, std::memory_order_release);
        return compaction_retirement_failure(CompactionSegmentRetirementOutcome::indeterminate,
                                             synced.error());
    }
    after(FilesystemOperation::sync_directory);
    return {.outcome = CompactionSegmentRetirementOutcome::durable, .error = std::nullopt};
}

auto DataDirectory::cleanup_recovery_temporaries(const std::span<const std::string> names) -> Status {
    if (!healthy()) {
        return fail(ErrorCode::io_error, "cannot clean recovery temporaries through a poisoned directory");
    }
    if (names.empty()) {
        return {};
    }

    // Validate the entire batch before the first namespace mutation. The
    // directory lock excludes another Store; O_NOFOLLOW plus the private-file
    // check prevents an unsafe entry from being treated as disposable residue.
    for (const auto& name : names) {
        const auto parsed = parse_segment_filename(name);
        const bool recognized = name == kManifestTemporaryFilename || name == kCompactionTemporaryFilename ||
                                (parsed.has_value() && parsed->temporary);
        if (!recognized) {
            return fail(ErrorCode::invalid_argument, "recovery cleanup received a non-temporary engine name");
        }
        FileDescriptor file{interrupted_open_at(directory_.get(), name.c_str(),
                                                O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK),
                            hooks_.file_io};
        if (!file.valid()) {
            return errno == ENOENT
                       ? fail(ErrorCode::sequence_conflict, "recovery temporary changed before cleanup")
                       : persistence_system_error("openat(recovery temporary)");
        }
        if (auto valid = validate_private_regular_file(file.get(), "recovery temporary"); !valid) {
            return valid;
        }
    }

    bool removed_any{};
    for (const auto& name : names) {
        if (auto allowed = before(FilesystemOperation::remove_compaction_segment); !allowed) {
            if (removed_any) {
                health_->store(false, std::memory_order_release);
            }
            return allowed;
        }
        if (::unlinkat(directory_.get(), name.c_str(), 0) != 0) {
            health_->store(false, std::memory_order_release);
            return persistence_system_error("unlinkat(recovery temporary)");
        }
        removed_any = true;
        after(FilesystemOperation::remove_compaction_segment);
    }
    if (auto allowed = before(FilesystemOperation::sync_directory); !allowed) {
        health_->store(false, std::memory_order_release);
        return allowed;
    }
    if (auto synced = sync_directory(); !synced) {
        health_->store(false, std::memory_order_release);
        return synced;
    }
    after(FilesystemOperation::sync_directory);
    return {};
}

auto DataDirectory::pristine_for_bootstrap() const -> Result<bool> {
    auto descriptor = open_directory_for_enumeration();
    if (!descriptor) {
        return unexpected(descriptor.error());
    }
    auto* stream = ::fdopendir(descriptor->get());
    if (!stream) {
        return persistence_system_error("fdopendir(bootstrap namespace)");
    }
    static_cast<void>(descriptor->release());
    const std::unique_ptr<DIR, int (*)(DIR*)> owned{stream, &::closedir};
    for (;;) {
        errno = 0;
        const auto* entry = ::readdir(owned.get());
        if (!entry) {
            if (errno != 0) {
                return persistence_system_error("readdir(bootstrap namespace)");
            }
            return true;
        }
        const std::string_view name{entry->d_name};
        if (name != "." && name != ".." && name != kStoreLockFilename &&
            name != kBootstrapTemporaryFilename) {
            return false;
        }
    }
}

auto DataDirectory::read_manifest(const std::size_t max_bytes) const -> Result<Manifest> {
    if (!healthy()) {
        return fail(ErrorCode::io_error, "cannot read through a poisoned data-directory instance");
    }
    return read_private_manifest_file(directory_.get(), kManifestFilename, "manifest", max_bytes,
                                      hooks_.file_io);
}

} // namespace glifistore
