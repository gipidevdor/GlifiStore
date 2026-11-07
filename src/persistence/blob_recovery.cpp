#include "filesystem_detail.hpp"
#include "glifistore/core/little_endian.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "persistence/blob_storage.hpp"
#include "system_error.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

namespace glifistore::detail {
auto verify_blob_closure(DataDirectory& directory, const Manifest& manifest, BlobStorage& storage)
    -> Result<std::vector<BlobId>> {
    // Recovery authority is the complete committed catalog history, including
    // overwritten descriptors. Each ID must agree on digest, size, and owner.
    std::map<BlobId, std::pair<BlobRef, WorkerId>> seen;
    std::vector<std::byte> scratch;
    for (const auto& entry : manifest.segments) {
        const SegmentHeaderIdentity identity{.store_id = manifest.store_id,
                                             .segment_id = entry.segment_id,
                                             .generation = entry.generation,
                                             .owner_worker = entry.owner_worker,
                                             .format_version = manifest.format_version};
        const auto name = segment_filename(identity);
        FileDescriptor file{
            persistence_detail::interrupted_open_at(directory.directory_descriptor(), name.c_str(),
                                                    O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK),
            directory.hooks().file_io};
        if (!file.valid()) {
            return persistence_system_error("openat(blob descriptor closure)");
        }
        if (auto valid =
                persistence_detail::validate_private_regular_file(file.get(), "blob descriptor Segment");
            !valid) {
            return unexpected(valid.error());
        }
        const auto size = file.size();
        if (!size || *size != kSegmentSizeBytes) {
            return fail(ErrorCode::corrupted_data, "blob closure Segment size mismatch");
        }
        std::array<std::byte, kSegmentHeaderReservedBytes> header{};
        if (auto read = file.read_exact_at(header, 0); !read) {
            return unexpected(read.error());
        }
        auto decoded = decode_segment_header(header);
        if (!decoded || decoded->identity != identity) {
            return fail(ErrorCode::corrupted_data, "blob closure Segment identity mismatch");
        }
        auto selected = select_newest_segment_commit(*decoded);
        if (!selected) {
            return unexpected(selected.error());
        }
        std::uint64_t offset = kSegmentHeaderReservedBytes;
        std::array<std::byte, kEncodedRecordHeaderSize> prefix{};
        while (offset < selected->commit.committed_end) {
            if (selected->commit.committed_end - offset < prefix.size()) {
                return fail(ErrorCode::corrupted_data, "truncated blob descriptor Record");
            }
            if (auto read = file.read_exact_at(prefix, offset); !read) {
                return unexpected(read.error());
            }
            const auto length = le::get_u32(prefix, 8);
            if (length < prefix.size() || length > kMaxNormalRecordSize ||
                length > selected->commit.committed_end - offset) {
                return fail(ErrorCode::corrupted_data, "invalid blob descriptor Record extent");
            }
            scratch.resize(length);
            if (auto read = file.read_exact_at(scratch, offset); !read) {
                return unexpected(read.error());
            }
            auto record = decode_record(scratch);
            if (!record || record->format_version != manifest.format_version) {
                return fail(ErrorCode::corrupted_data, "invalid blob descriptor Record");
            }
            if (record->type == ValueType::blob_ref) {
                auto ref = decode_blob_ref(record->value);
                if (!ref) {
                    return unexpected(ref.error());
                }
                bool lower{};
                for (std::size_t i = 16; i > 0; --i) {
                    if (ref->id[i - 1] != manifest.next_blob_id[i - 1]) {
                        lower = ref->id[i - 1] < manifest.next_blob_id[i - 1];
                        break;
                    }
                }
                if (!lower) {
                    return fail(ErrorCode::corrupted_data, "descriptor exceeds blob ID high-water mark");
                }
                const auto found = seen.find(ref->id);
                if (found != seen.end()) {
                    if (found->second.first != *ref || found->second.second != entry.owner_worker) {
                        return fail(ErrorCode::corrupted_data, "contradictory blob descriptors");
                    }
                } else {
                    if (seen.size() >= 1'000'000) {
                        return fail(ErrorCode::storage_exhausted, "blob closure metadata limit");
                    }
                    if (auto valid = storage.verify(*ref, entry.owner_worker); !valid) {
                        return unexpected(valid.error());
                    }
                    seen.emplace(ref->id, std::pair{*ref, entry.owner_worker});
                }
            }
            offset += length;
        }
    }
    std::vector<BlobId> roots;
    roots.reserve(seen.size());
    for (const auto& [id, metadata] : seen) {
        static_cast<void>(metadata);
        roots.push_back(id);
    }
    return roots;
}

auto copy_blob_closure(DataDirectory& source, DataDirectory& destination, const Manifest& manifest,
                       const BlobResourceLimits limits, std::size_t& files_copied,
                       std::uint64_t& bytes_copied) -> Status {
    auto storage = BlobStorage::open(source, manifest, limits, 0);
    if (!storage) {
        return unexpected(storage.error());
    }
    auto roots = verify_blob_closure(source, manifest, **storage);
    if (!roots) {
        return unexpected(roots.error());
    }
    if (::mkdirat(destination.directory_descriptor(), "blobs", 0700) != 0 && errno != EEXIST) {
        return persistence_system_error("mkdirat(backup blobs)");
    }
    FileDescriptor src{persistence_detail::interrupted_open_at(
        source.directory_descriptor(), "blobs", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    FileDescriptor dst{persistence_detail::interrupted_open_at(
        destination.directory_descriptor(), "blobs", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (!src.valid() || !dst.valid()) {
        return persistence_system_error("openat(backup blobs)");
    }
    std::vector<std::byte> buffer(kBlobChunkBytes);
    for (const auto& id : *roots) {
        const auto name = blob_filename(id);
        FileDescriptor from{persistence_detail::interrupted_open_at(src.get(), name.c_str(),
                                                                    O_RDONLY | O_CLOEXEC | O_NOFOLLOW),
                            source.hooks().file_io};
        FileDescriptor to{
            persistence_detail::interrupted_open_at(
                dst.get(), name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600),
            destination.hooks().file_io};
        if (!from.valid() || !to.valid()) {
            return persistence_system_error("openat(backup blob file)");
        }
        const auto size = from.size();
        if (!size) {
            return unexpected(size.error());
        }
        for (std::uint64_t offset = 0; offset < *size;) {
            const auto count =
                static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), *size - offset));
            auto part = std::span{buffer}.first(count);
            if (auto read = from.read_exact_at(part, offset); !read) {
                return read;
            }
            if (auto written = to.write_all_at(part, offset); !written) {
                return written;
            }
            offset += count;
        }
        if (auto synced = to.sync(FileSyncMode::full); !synced) {
            return synced;
        }
        ++files_copied;
        bytes_copied += *size;
    }
    if (auto synced = dst.sync(FileSyncMode::full); !synced) {
        return synced;
    }
    if (auto synced = sync_blob_parent(destination); !synced) {
        return synced;
    }
    auto copied = BlobStorage::open(destination, manifest, limits, 0);
    if (!copied) {
        return unexpected(copied.error());
    }
    auto verified = verify_blob_closure(destination, manifest, **copied);
    return verified ? Status{} : Status{unexpected(verified.error())};
}
} // namespace glifistore::detail
