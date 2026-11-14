#include "glifistore/persistence/filesystem.hpp"

#include "filesystem_detail.hpp"
#include "system_error.hpp"

#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace glifistore {

using persistence_detail::descriptor_stat;

FileDescriptor::~FileDescriptor() {
    reset();
}

FileDescriptor::FileDescriptor(FileDescriptor&& other) noexcept
    : descriptor_(other.release()), io_hooks_(other.io_hooks_) {
    other.io_hooks_ = {};
}

auto FileDescriptor::operator=(FileDescriptor&& other) noexcept -> FileDescriptor& {
    if (this != &other) {
        reset(other.release());
        io_hooks_ = other.io_hooks_;
        other.io_hooks_ = {};
    }
    return *this;
}

auto FileDescriptor::release() noexcept -> int {
    const auto descriptor = descriptor_;
    descriptor_ = -1;
    return descriptor;
}

void FileDescriptor::reset(const int descriptor, const FileIoHooks io_hooks) noexcept {
    if (descriptor_ >= 0) {
        static_cast<void>(::close(descriptor_));
    }
    descriptor_ = descriptor;
    io_hooks_ = io_hooks;
}

auto FileDescriptor::size() const -> Result<std::uint64_t> {
    const auto status = descriptor_stat(descriptor_);
    if (!status) {
        return unexpected(status.error());
    }
    if (status->st_size < 0) {
        return fail(ErrorCode::io_error, "file reports a negative size");
    }
    return static_cast<std::uint64_t>(status->st_size);
}

auto FileDescriptor::write_all_at(const std::span<const std::byte> bytes, const std::uint64_t offset) const
    -> Status {
    constexpr auto kMaximumOffset = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
    if (offset > kMaximumOffset || bytes.size() > kMaximumOffset - offset) {
        return fail(ErrorCode::invalid_argument, "positional write extent exceeds off_t");
    }

    std::size_t written{};
    while (written < bytes.size()) {
        const auto remaining = bytes.size() - written;
        const auto chunk = std::min(remaining, static_cast<std::size_t>(SSIZE_MAX));
        const auto current = bytes.subspan(written, chunk);
        const auto result =
            io_hooks_.write_some_at != nullptr
                ? io_hooks_.write_some_at(io_hooks_.context, descriptor_, current, offset + written)
                : static_cast<std::ptrdiff_t>(::pwrite(descriptor_, current.data(), current.size(),
                                                       static_cast<off_t>(offset + written)));
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return persistence_system_error("pwrite");
        }
        if (result == 0) {
            return fail(ErrorCode::io_error, "pwrite made no forward progress");
        }
        if (std::cmp_greater(result, chunk)) {
            return fail(ErrorCode::internal_error,
                        "positional write hook returned more bytes than requested");
        }
        written += static_cast<std::size_t>(result);
    }
    return {};
}

auto FileDescriptor::read_exact_at(const std::span<std::byte> bytes, const std::uint64_t offset) const
    -> Status {
    constexpr auto kMaximumOffset = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
    if (offset > kMaximumOffset || bytes.size() > kMaximumOffset - offset) {
        return fail(ErrorCode::invalid_argument, "positional read extent exceeds off_t");
    }

    std::size_t read{};
    while (read < bytes.size()) {
        const auto remaining = bytes.size() - read;
        const auto chunk = std::min(remaining, static_cast<std::size_t>(SSIZE_MAX));
        const auto current = bytes.subspan(read, chunk);
        const auto result =
            io_hooks_.read_some_at != nullptr
                ? io_hooks_.read_some_at(io_hooks_.context, descriptor_, current, offset + read)
                : static_cast<std::ptrdiff_t>(::pread(descriptor_, current.data(), current.size(),
                                                      static_cast<off_t>(offset + read)));
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return persistence_system_error("pread");
        }
        if (result == 0) {
            return fail(ErrorCode::corrupted_data, "file ended before the requested extent");
        }
        if (std::cmp_greater(result, chunk)) {
            return fail(ErrorCode::internal_error, "positional read hook returned more bytes than requested");
        }
        read += static_cast<std::size_t>(result);
    }
    return {};
}

auto FileDescriptor::sync(const FileSyncMode mode) const -> Status {
    int result{};
    do {
        if (io_hooks_.sync_file != nullptr) {
            result = io_hooks_.sync_file(io_hooks_.context, descriptor_, mode);
            continue;
        }
#if defined(__APPLE__)
        result = ::fcntl(descriptor_, mode == FileSyncMode::ordered ? F_BARRIERFSYNC : F_FULLFSYNC, 0);
#else
        result = mode == FileSyncMode::full ? ::fsync(descriptor_) : ::fdatasync(descriptor_);
#endif
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
#if defined(__APPLE__)
        return persistence_system_error(mode == FileSyncMode::ordered ? "fcntl(F_BARRIERFSYNC)"
                                                                      : "fcntl(F_FULLFSYNC)");
#else
        return persistence_system_error(mode == FileSyncMode::full ? "fsync" : "fdatasync");
#endif
    }
    return {};
}

} // namespace glifistore
