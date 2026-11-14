#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

namespace glifistore::test {

/// Everything a recovery or compaction path could change about a file that it must leave alone: which
/// inode it is, how large it is, and when it was last written.
struct FileIdentity {
    dev_t device{};
    ino_t inode{};
    std::uintmax_t size_bytes{};
    std::filesystem::file_time_type modified;

    auto operator==(const FileIdentity&) const -> bool = default;
};

[[nodiscard]] inline auto file_identity(const std::filesystem::path& path) -> FileIdentity {
    struct stat status{};
    if (::stat(path.c_str(), &status) != 0) {
        throw std::runtime_error("cannot stat " + path.string());
    }
    return {.device = status.st_dev,
            .inode = status.st_ino,
            .size_bytes = std::filesystem::file_size(path),
            .modified = std::filesystem::last_write_time(path)};
}

} // namespace glifistore::test
