#pragma once

#include "glifistore/persistence/filesystem.hpp"
#include "system_error.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace glifistore::persistence_detail {

inline auto interrupted_open(const char* path, int flags) -> int {
    int descriptor{};
    do {
        descriptor = ::open(path, flags);
    } while (descriptor < 0 && errno == EINTR);
    return descriptor;
}

inline auto interrupted_open_at(int directory, const char* name, int flags, mode_t mode = 0) -> int {
    int descriptor{};
    do {
        descriptor = ::openat(directory, name, flags, mode);
    } while (descriptor < 0 && errno == EINTR);
    return descriptor;
}

inline auto interrupted_mkdir_at(int directory, const char* name, mode_t mode) -> int {
    int result{};
    do {
        result = ::mkdirat(directory, name, mode);
    } while (result != 0 && errno == EINTR);
    return result;
}

inline auto descriptor_stat(int descriptor) -> Result<struct stat> {
    struct stat status{};
    if (::fstat(descriptor, &status) != 0) {
        return persistence_system_error("fstat");
    }
    return status;
}

inline auto validate_directory_descriptor(int descriptor) -> Status {
    const auto status = descriptor_stat(descriptor);
    if (!status) {
        return unexpected(status.error());
    }
    if (!S_ISDIR(status->st_mode)) {
        return fail(ErrorCode::invalid_argument, "data directory path is not a directory");
    }
    if (status->st_uid != ::geteuid()) {
        return fail(ErrorCode::invalid_argument, "data directory is not owned by the effective user");
    }
    if ((status->st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        return fail(ErrorCode::invalid_argument, "data directory is writable by group or other users");
    }
    return {};
}

inline auto validate_private_regular_file(int descriptor, std::string_view description) -> Status {
    const auto status = descriptor_stat(descriptor);
    if (!status) {
        return unexpected(status.error());
    }
    if (!S_ISREG(status->st_mode) || status->st_nlink != 1 || status->st_uid != ::geteuid() ||
        (status->st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        return fail(ErrorCode::invalid_argument,
                    std::string{description} + " must be a private, singly linked regular file");
    }
    return {};
}

inline auto lock_exclusive_nonblocking(int descriptor) -> Status {
    int result{};
    do {
        result = ::flock(descriptor, LOCK_EX | LOCK_NB);
    } while (result != 0 && errno == EINTR);
    if (result == 0) {
        return {};
    }
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
        return fail(ErrorCode::io_error, "data directory is already locked by another Store");
    }
    return persistence_system_error("flock(LOCK_EX | LOCK_NB)");
}

inline auto unlink_temporary(int directory) -> Status {
    if (::unlinkat(directory, kManifestTemporaryFilename, 0) == 0 || errno == ENOENT) {
        return {};
    }
    return persistence_system_error("unlinkat(manifest temporary)");
}

inline auto publication_failure(ManifestPublicationOutcome outcome, Error error)
    -> ManifestPublicationResult {
    return {.outcome = outcome, .error = std::move(error)};
}

inline auto compaction_publication_failure(CompactionIntentPublicationOutcome outcome, Error error)
    -> CompactionIntentPublicationResult {
    return {.outcome = outcome, .error = std::move(error)};
}

inline auto compaction_removal_failure(CompactionIntentRemovalOutcome outcome, Error error)
    -> CompactionIntentRemovalResult {
    return {.outcome = outcome, .error = std::move(error)};
}

inline auto compaction_retirement_failure(CompactionSegmentRetirementOutcome outcome, Error error)
    -> CompactionSegmentRetirementResult {
    return {.outcome = outcome, .error = std::move(error)};
}

inline auto read_private_manifest_file(int directory, const char* name, std::string_view description,
                                       const std::size_t max_bytes, const FileIoHooks io_hooks)
    -> Result<Manifest> {
    FileDescriptor file{interrupted_open_at(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK),
                        io_hooks};
    if (!file.valid()) {
        if (errno == ENOENT) {
            return fail(ErrorCode::not_found, std::string{description} + " does not exist");
        }
        return persistence_system_error("openat(" + std::string{description} + ')');
    }
    if (auto valid = validate_private_regular_file(file.get(), description); !valid) {
        return unexpected(valid.error());
    }
    const auto size = file.size();
    if (!size) {
        return unexpected(size.error());
    }
    if (*size < kManifestHeaderBytes || *size > kMaximumManifestBytes) {
        return fail(ErrorCode::invalid_record,
                    std::string{description} + " size is outside manifest v1 bounds");
    }
    if (*size > max_bytes) {
        return fail(ErrorCode::storage_exhausted,
                    std::string{description} + " exceeds the configured byte budget");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(*size));
    if (auto read = file.read_exact_at(bytes, 0); !read) {
        return unexpected(read.error());
    }
    const auto size_after_read = file.size();
    if (!size_after_read || *size_after_read != *size) {
        return fail(ErrorCode::corrupted_data, std::string{description} + " changed while being read");
    }
    return decode_manifest(bytes);
}

inline auto read_private_compaction_intent_file(const int directory, const std::size_t max_manifest_bytes,
                                                const FileIoHooks io_hooks)
    -> Result<DurableCompactionIntent> {
    static_assert(kMaximumManifestBytes <=
                  (std::numeric_limits<std::size_t>::max() - kCompactionIntentHeaderBytes) / 2U);
    if (max_manifest_bytes > kMaximumManifestBytes) {
        return fail(ErrorCode::invalid_argument,
                    "compaction intent manifest byte budget is outside supported bounds");
    }
    const auto max_bytes = kCompactionIntentHeaderBytes + 2U * max_manifest_bytes;
    FileDescriptor file{interrupted_open_at(directory, kCompactionIntentFilename,
                                            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK),
                        io_hooks};
    if (!file.valid()) {
        if (errno == ENOENT) {
            return fail(ErrorCode::not_found, "compaction intent does not exist");
        }
        return persistence_system_error("openat(compaction intent)");
    }
    if (auto valid = validate_private_regular_file(file.get(), "compaction intent"); !valid) {
        return unexpected(valid.error());
    }
    const auto size = file.size();
    if (!size) {
        return unexpected(size.error());
    }
    if (*size < kCompactionIntentHeaderBytes || *size > kMaximumCompactionIntentBytes) {
        return fail(ErrorCode::invalid_record, "compaction intent size is outside its format bounds");
    }
    if (*size > max_bytes) {
        return fail(ErrorCode::storage_exhausted,
                    "compaction intent exceeds the configured manifest byte budget");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(*size));
    if (auto read = file.read_exact_at(bytes, 0); !read) {
        return unexpected(read.error());
    }
    const auto size_after_read = file.size();
    if (!size_after_read || *size_after_read != *size) {
        return fail(ErrorCode::corrupted_data, "compaction intent changed while being read");
    }
    return decode_compaction_intent(bytes);
}

} // namespace glifistore::persistence_detail
