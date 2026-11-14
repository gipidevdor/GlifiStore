#include "glifistore/persistence/segment_file.hpp"

#include "glifistore/core/fault_injection.hpp"
#include "segment_file_detail.hpp"
#include "system_error.hpp"

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace glifistore {

using segment_file_detail::creation_failure;
using segment_file_detail::fixed_hex;
using segment_file_detail::interrupted_open;
using segment_file_detail::interrupted_open_at;
using segment_file_detail::preallocate_segment;
using segment_file_detail::unlink_named;
using segment_file_detail::validate_private_segment;

auto segment_filename(const SegmentHeaderIdentity& identity) -> std::string {
    return "segment-" + fixed_hex(identity.segment_id.value, 16) + '-' +
           fixed_hex(identity.generation.value, 8) + ".glifi";
}

auto DurableSegmentFile::create(DataDirectory& directory, const SegmentHeaderIdentity& identity)
    -> SegmentFileCreationResult {
    if (!directory.healthy()) {
        return creation_failure(
            SegmentFileCreationOutcome::indeterminate,
            Error{ErrorCode::io_error, "data directory is poisoned by an indeterminate publication"});
    }

    const SegmentCommit initial_commit{
        .commit_generation = 1,
        .committed_end = static_cast<std::uint32_t>(kSegmentHeaderReservedBytes),
        .state = PersistedSegmentState::active,
        .record_count = 0,
        .first_sequence = SequenceNumber{0},
        .last_sequence = SequenceNumber{0},
    };
    std::array<std::byte, kSegmentHeaderReservedBytes> header_bytes{};
    const SegmentHeader header{.identity = identity, .commits = {initial_commit, std::nullopt}};
    if (auto encoded = encode_segment_header(header_bytes, header); !encoded) {
        return creation_failure(SegmentFileCreationOutcome::not_published, encoded.error());
    }

    const auto final_name = segment_filename(identity);
    const auto temporary_name = '.' + final_name + ".tmp";
    struct stat existing{};
    if (::fstatat(directory.directory_.get(), final_name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0) {
        return creation_failure(
            SegmentFileCreationOutcome::not_published,
            Error{ErrorCode::sequence_conflict, "Segment identity already exists in the data directory"});
    }
    if (errno != ENOENT) {
        return creation_failure(SegmentFileCreationOutcome::not_published,
                                persistence_system_error("fstatat(segment name)").error);
    }
    if (auto removed = unlink_named(directory.directory_.get(), temporary_name, "Segment temporary");
        !removed) {
        return creation_failure(SegmentFileCreationOutcome::not_published, removed.error());
    }

    FileDescriptor temporary{
        interrupted_open_at(directory.directory_.get(), temporary_name.c_str(),
                            O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
                            S_IRUSR | S_IWUSR),
        directory.hooks_.file_io};
    if (!temporary.valid()) {
        return creation_failure(SegmentFileCreationOutcome::not_published,
                                persistence_system_error("openat(Segment temporary)").error);
    }

    const auto cleanup = [&directory, &temporary_name]() {
        static_cast<void>(unlink_named(directory.directory_.get(), temporary_name, "Segment temporary"));
    };
    if (auto allowed = directory.before(FilesystemOperation::preallocate_segment); !allowed) {
        cleanup();
        return creation_failure(SegmentFileCreationOutcome::not_published, allowed.error());
    }
    if (auto allocated = preallocate_segment(temporary); !allocated) {
        cleanup();
        return creation_failure(SegmentFileCreationOutcome::not_published, allocated.error());
    }
    directory.after(FilesystemOperation::preallocate_segment);
    if (auto allowed = directory.before(FilesystemOperation::write_segment_header); !allowed) {
        cleanup();
        return creation_failure(SegmentFileCreationOutcome::not_published, allowed.error());
    }
    if (auto written = temporary.write_all_at(header_bytes, 0); !written) {
        cleanup();
        return creation_failure(SegmentFileCreationOutcome::not_published, written.error());
    }
    directory.after(FilesystemOperation::write_segment_header);
    if (auto allowed = directory.before(FilesystemOperation::sync_segment_file); !allowed) {
        cleanup();
        return creation_failure(SegmentFileCreationOutcome::not_published, allowed.error());
    }
    if (auto synced = temporary.sync(FileSyncMode::full); !synced) {
        cleanup();
        return creation_failure(SegmentFileCreationOutcome::not_published, synced.error());
    }
    directory.after(FilesystemOperation::sync_segment_file);
    if (auto allowed = directory.before(FilesystemOperation::rename_segment); !allowed) {
        cleanup();
        return creation_failure(SegmentFileCreationOutcome::not_published, allowed.error());
    }
    if (::renameat(directory.directory_.get(), temporary_name.c_str(), directory.directory_.get(),
                   final_name.c_str()) != 0) {
        directory.health_->store(false, std::memory_order_release);
        return creation_failure(SegmentFileCreationOutcome::indeterminate,
                                persistence_system_error("renameat(Segment)").error);
    }
    directory.after(FilesystemOperation::rename_segment);
    if (auto allowed = directory.before(FilesystemOperation::sync_directory); !allowed) {
        directory.health_->store(false, std::memory_order_release);
        return creation_failure(SegmentFileCreationOutcome::indeterminate, allowed.error());
    }
    if (auto synced = directory.sync_directory(); !synced) {
        directory.health_->store(false, std::memory_order_release);
        return creation_failure(SegmentFileCreationOutcome::indeterminate, synced.error());
    }
    directory.after(FilesystemOperation::sync_directory);

    DurableSegmentFile file{std::move(temporary),
                            identity,
                            SelectedSegmentCommit{.slot_index = 0, .commit = initial_commit},
                            directory.hooks_,
                            directory.health_,
                            true};
    return {.outcome = SegmentFileCreationOutcome::durable, .file = std::move(file), .error = std::nullopt};
}

auto DurableSegmentFile::create_staged(DataDirectory& directory, const SegmentHeaderIdentity& identity)
    -> Result<DurableSegmentFile> {
    if (!directory.healthy()) {
        return fail(ErrorCode::io_error, "cannot stage a Segment through a poisoned data directory");
    }

    const SegmentCommit initial_commit{
        .commit_generation = 1,
        .committed_end = static_cast<std::uint32_t>(kSegmentHeaderReservedBytes),
        .state = PersistedSegmentState::active,
        .record_count = 0,
        .first_sequence = SequenceNumber{0},
        .last_sequence = SequenceNumber{0},
    };
    std::array<std::byte, kSegmentHeaderReservedBytes> header_bytes{};
    const SegmentHeader header{.identity = identity, .commits = {initial_commit, std::nullopt}};
    if (auto encoded = encode_segment_header(header_bytes, header); !encoded) {
        return unexpected(encoded.error());
    }

    const auto final_name = segment_filename(identity);
    const auto temporary_name = '.' + final_name + ".tmp";
    struct stat existing{};
    if (::fstatat(directory.directory_.get(), final_name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0) {
        return fail(ErrorCode::sequence_conflict,
                    "staged Segment identity already exists in the data directory");
    }
    if (errno != ENOENT) {
        return persistence_system_error("fstatat(staged Segment final name)");
    }

    FileDescriptor temporary{
        interrupted_open_at(directory.directory_.get(), temporary_name.c_str(),
                            O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
                            S_IRUSR | S_IWUSR),
        directory.hooks_.file_io};
    if (!temporary.valid()) {
        if (errno == EEXIST) {
            return fail(ErrorCode::sequence_conflict,
                        "staged Segment temporary identity is already owned by another build");
        }
        return persistence_system_error("openat(staged Segment)");
    }
    const auto cleanup = [&directory, &temporary_name]() noexcept {
        static_cast<void>(::unlinkat(directory.directory_.get(), temporary_name.c_str(), 0));
    };
    if (auto allowed = directory.before(FilesystemOperation::preallocate_segment); !allowed) {
        cleanup();
        return unexpected(allowed.error());
    }
    if (auto allocated = preallocate_segment(temporary); !allocated) {
        cleanup();
        return unexpected(allocated.error());
    }
    directory.after(FilesystemOperation::preallocate_segment);
    if (auto allowed = directory.before(FilesystemOperation::write_segment_header); !allowed) {
        cleanup();
        return unexpected(allowed.error());
    }
    if (auto written = temporary.write_all_at(header_bytes, 0); !written) {
        cleanup();
        return unexpected(written.error());
    }
    directory.after(FilesystemOperation::write_segment_header);

    // This handle intentionally owns private health until promotion. Any
    // staging I/O failure invalidates only disposable output, not Mold.
    auto staged_health = std::make_shared<std::atomic_bool>(true);
    return DurableSegmentFile{std::move(temporary),
                              identity,
                              SelectedSegmentCommit{.slot_index = 0, .commit = initial_commit},
                              directory.hooks_,
                              std::move(staged_health),
                              true};
}

auto DurableSegmentFile::open_staged(DataDirectory& directory, const SegmentHeaderIdentity& expected_identity)
    -> Result<DurableSegmentFile> {
    if (!directory.healthy()) {
        return fail(ErrorCode::io_error, "cannot open a staged Segment through a poisoned data directory");
    }
    const auto name = '.' + segment_filename(expected_identity) + ".tmp";
    FileDescriptor file{interrupted_open_at(directory.directory_.get(), name.c_str(),
                                            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK),
                        directory.hooks_.file_io};
    if (!file.valid()) {
        if (errno == ENOENT) {
            return fail(ErrorCode::not_found, "staged Segment does not exist");
        }
        return persistence_system_error("openat(staged Segment)");
    }
    if (auto valid = validate_private_segment(file.get()); !valid) {
        return unexpected(valid.error());
    }
    const auto size = file.size();
    if (!size) {
        return unexpected(size.error());
    }
    if (*size != kSegmentSizeBytes) {
        return fail(ErrorCode::invalid_record, "staged Segment file is not exactly 64 MiB");
    }
    std::array<std::byte, kSegmentHeaderReservedBytes> header_bytes{};
    if (auto read = file.read_exact_at(header_bytes, 0); !read) {
        return unexpected(read.error());
    }
    const auto header = decode_segment_header(header_bytes);
    if (!header) {
        return unexpected(header.error());
    }
    if (header->identity != expected_identity) {
        return fail(ErrorCode::corrupted_data,
                    "staged Segment header identity does not match the planned identity");
    }
    const auto selected = select_newest_segment_commit(*header);
    if (!selected) {
        return unexpected(selected.error());
    }
    auto staged_health = std::make_shared<std::atomic_bool>(true);
    return DurableSegmentFile{std::move(file),  header->identity,         *selected,
                              directory.hooks_, std::move(staged_health), false};
}

auto DurableSegmentFile::promote_staged(DataDirectory& directory,
                                        const std::span<const SegmentHeaderIdentity> identities) -> Status {
    if (!directory.healthy()) {
        return fail(ErrorCode::io_error, "cannot promote staged Segments through a poisoned data directory");
    }
    if (identities.empty()) {
        return {};
    }

    for (const auto& identity : identities) {
        auto staged = open_staged(directory, identity);
        if (!staged) {
            return unexpected(staged.error());
        }
        if (staged->selected_commit().commit.state != PersistedSegmentState::sealed) {
            return fail(ErrorCode::corrupted_data, "compaction staged Segment is not sealed");
        }
        const auto final_name = segment_filename(identity);
        struct stat existing{};
        if (::fstatat(directory.directory_.get(), final_name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0) {
            return fail(ErrorCode::sequence_conflict,
                        "compaction staged Segment final identity already exists");
        }
        if (errno != ENOENT) {
            return persistence_system_error("fstatat(compaction staged final name)");
        }
    }

    bool renamed_any{};
    for (const auto& identity : identities) {
        const auto final_name = segment_filename(identity);
        const auto temporary_name = '.' + final_name + ".tmp";
        if (auto allowed = directory.before(FilesystemOperation::rename_segment); !allowed) {
            if (renamed_any) {
                directory.health_->store(false, std::memory_order_release);
            }
            return allowed;
        }
        if (::renameat(directory.directory_.get(), temporary_name.c_str(), directory.directory_.get(),
                       final_name.c_str()) != 0) {
            directory.health_->store(false, std::memory_order_release);
            return persistence_system_error("renameat(staged Segment)");
        }
        renamed_any = true;
        directory.after(FilesystemOperation::rename_segment);
    }
    if (auto allowed = directory.before(FilesystemOperation::sync_directory); !allowed) {
        directory.health_->store(false, std::memory_order_release);
        return allowed;
    }
    if (auto synced = directory.sync_directory(); !synced) {
        directory.health_->store(false, std::memory_order_release);
        return synced;
    }
    directory.after(FilesystemOperation::sync_directory);
    return {};
}

void DurableSegmentFile::discard_staged(DataDirectory& directory,
                                        const std::span<const SegmentHeaderIdentity> identities) noexcept {
    for (const auto& identity : identities) {
        try {
            const auto name = '.' + segment_filename(identity) + ".tmp";
            static_cast<void>(::unlinkat(directory.directory_.get(), name.c_str(), 0));
        } catch (...) {
            continue;
        }
    }
}

auto DurableSegmentFile::open(DataDirectory& directory, const SegmentHeaderIdentity& expected_identity,
                              const SegmentFileOpenMode mode) -> Result<DurableSegmentFile> {
    if (glifistore::fault::consume_fail(glifistore::fault::Site::segment_open)) {
        return fail(ErrorCode::descriptor_exhausted, "injected Segment open failure");
    }
    if (!directory.healthy()) {
        return fail(ErrorCode::io_error, "cannot open a Segment through a poisoned data directory");
    }
    const auto name = segment_filename(expected_identity);
    const auto access = mode == SegmentFileOpenMode::read_only ? O_RDONLY : O_RDWR;
    FileDescriptor file{interrupted_open_at(directory.directory_.get(), name.c_str(),
                                            access | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK),
                        directory.hooks_.file_io};
    if (!file.valid()) {
        if (errno == ENOENT) {
            return fail(ErrorCode::not_found, "Segment does not exist");
        }
        return persistence_system_error("openat(Segment)");
    }
    if (auto valid = validate_private_segment(file.get()); !valid) {
        return unexpected(valid.error());
    }
    const auto size = file.size();
    if (!size) {
        return unexpected(size.error());
    }
    if (*size != kSegmentSizeBytes) {
        return fail(ErrorCode::invalid_record, "Segment file is not exactly 64 MiB");
    }

    std::array<std::byte, kSegmentHeaderReservedBytes> header_bytes{};
    if (auto read = file.read_exact_at(header_bytes, 0); !read) {
        return unexpected(read.error());
    }
    const auto header = decode_segment_header(header_bytes);
    if (!header) {
        return unexpected(header.error());
    }
    if (header->identity != expected_identity) {
        return fail(ErrorCode::corrupted_data, "Segment header identity does not match its catalog identity");
    }
    const auto selected = select_newest_segment_commit(*header);
    if (!selected) {
        return unexpected(selected.error());
    }
    return DurableSegmentFile{std::move(file),  header->identity,  *selected,
                              directory.hooks_, directory.health_, mode == SegmentFileOpenMode::read_write};
}

auto DurableSegmentFile::open_path(const std::filesystem::path& path) -> Result<DurableSegmentFile> {
    if (path.empty()) {
        return fail(ErrorCode::invalid_argument, "Segment path cannot be empty");
    }
    FileDescriptor file{interrupted_open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (!file.valid()) {
        if (errno == ENOENT) {
            return fail(ErrorCode::not_found, "Segment does not exist");
        }
        return persistence_system_error("open(Segment path)");
    }
    if (auto valid = validate_private_segment(file.get()); !valid) {
        return unexpected(valid.error());
    }
    const auto size = file.size();
    if (!size) {
        return unexpected(size.error());
    }
    if (*size != kSegmentSizeBytes) {
        return fail(ErrorCode::invalid_record, "Segment file is not exactly 64 MiB");
    }

    std::array<std::byte, kSegmentHeaderReservedBytes> header_bytes{};
    if (auto read = file.read_exact_at(header_bytes, 0); !read) {
        return unexpected(read.error());
    }
    const auto header = decode_segment_header(header_bytes);
    if (!header) {
        return unexpected(header.error());
    }
    const auto selected = select_newest_segment_commit(*header);
    if (!selected) {
        return unexpected(selected.error());
    }
    auto health = std::make_shared<std::atomic_bool>(true);
    return DurableSegmentFile{std::move(file), header->identity, *selected, {}, std::move(health), false};
}

auto DurableSegmentFile::before(const FilesystemOperation operation) const -> Status {
    if (!hooks_.before) {
        return {};
    }
    // before() runs ahead of the matching write/sync — known not committed whether the
    // hook returns Status or throws (see segment-filesystem.md / error-taxonomy-v1).
    try {
        return hooks_.before(hooks_.context, operation);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::resource_exhausted, "filesystem before-hook allocation failed");
    } catch (...) {
        return fail(ErrorCode::resource_exhausted, "filesystem before-hook failed");
    }
}

void DurableSegmentFile::after(const FilesystemOperation operation) const noexcept {
    detail::invoke_filesystem_after(hooks_, operation);
}

void DurableSegmentFile::poison() noexcept {
    directory_health_->store(false, std::memory_order_release);
}

void DurableSegmentFile::rollback_pending_metadata() noexcept {
    selected_.commit.committed_end = persisted_.commit.committed_end;
    selected_.commit.record_count = persisted_.commit.record_count;
    selected_.commit.first_sequence = persisted_.commit.first_sequence;
    selected_.commit.last_sequence = persisted_.commit.last_sequence;
    pending_record_count_ = 0;
    pending_bytes_ = 0;
}

} // namespace glifistore
