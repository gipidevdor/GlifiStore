#pragma once

#include "filesystem_detail.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "system_error.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace glifistore::segment_file_detail {

using persistence_detail::interrupted_open;
using persistence_detail::interrupted_open_at;

inline auto validate_private_segment(int descriptor) -> Status {
    struct stat status{};
    if (::fstat(descriptor, &status) != 0) {
        return persistence_system_error("fstat(segment)");
    }
    if (!S_ISREG(status.st_mode) || status.st_nlink != 1 || status.st_uid != ::geteuid() ||
        (status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        return fail(ErrorCode::invalid_argument, "Segment must be a private, singly linked regular file");
    }
    return {};
}

inline auto truncate_exact(int descriptor, std::uint64_t size) -> Status {
    if (size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return fail(ErrorCode::invalid_argument, "Segment size exceeds off_t");
    }
    int result{};
    do {
        result = ::ftruncate(descriptor, static_cast<off_t>(size));
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        return persistence_system_error("ftruncate(segment)");
    }
    return {};
}

inline auto preallocate_segment(const FileDescriptor& file) -> Status {
    constexpr auto size = static_cast<std::uint64_t>(kSegmentSizeBytes);
#if defined(__linux__)
    int result{};
    do {
        result = ::fallocate(file.get(), 0, static_cast<off_t>(0), static_cast<off_t>(size));
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        return persistence_system_error("fallocate(segment)");
    }
#elif defined(__APPLE__)
    fstore_t store{};
    store.fst_flags = F_ALLOCATECONTIG | F_ALLOCATEALL;
    store.fst_posmode = F_PEOFPOSMODE;
    store.fst_offset = 0;
    store.fst_length = static_cast<off_t>(size);
    if (::fcntl(file.get(), F_PREALLOCATE, &store) != 0) {
        store.fst_flags = F_ALLOCATEALL;
        store.fst_bytesalloc = 0;
        if (::fcntl(file.get(), F_PREALLOCATE, &store) != 0) {
            return persistence_system_error("fcntl(F_PREALLOCATE)");
        }
    }
    if (std::cmp_less(store.fst_bytesalloc, size)) {
        return fail(ErrorCode::storage_exhausted, "F_PREALLOCATE did not reserve the complete Segment");
    }
#elif defined(__OpenBSD__)
    // OpenBSD has no documented allocation-reservation syscall. Eagerly writing
    // the complete new file avoids accepting a sparse ftruncate-only Segment.
    static constexpr std::array<std::byte, 256U * 1024U> zeros{};
    for (std::uint64_t offset = 0; offset < size; offset += zeros.size()) {
        if (auto written = file.write_all_at(zeros, offset); !written) {
            return unexpected(written.error());
        }
    }
#else
    int result{};
    do {
        result = ::posix_fallocate(file.get(), 0, static_cast<off_t>(size));
    } while (result == EINTR);
    if (result != 0) {
        return persistence_system_error("posix_fallocate(segment)", result);
    }
#endif
    return truncate_exact(file.get(), size);
}

inline auto unlink_named(int directory, const std::string& name, std::string_view description) -> Status {
    if (::unlinkat(directory, name.c_str(), 0) == 0 || errno == ENOENT) {
        return {};
    }
    return persistence_system_error(std::string{"unlinkat("} + std::string{description} + ')');
}

inline auto creation_failure(SegmentFileCreationOutcome outcome, Error error) -> SegmentFileCreationResult {
    return {.outcome = outcome, .file = std::nullopt, .error = std::move(error)};
}

inline auto commit_failure(SegmentCommitOutcome outcome, Error error) -> SegmentCommitResult {
    return {.outcome = outcome, .error = std::move(error)};
}

inline auto fixed_hex(std::uint64_t value, std::size_t width) -> std::string {
    std::array<char, 16> digits{};
    const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(), value, 16);
    const auto count = static_cast<std::size_t>(converted.ptr - digits.data());
    std::string result(width - count, '0');
    result.append(digits.data(), converted.ptr);
    return result;
}

} // namespace glifistore::segment_file_detail
