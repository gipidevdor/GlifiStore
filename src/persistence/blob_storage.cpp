#include "persistence/blob_storage.hpp"

#include "filesystem_detail.hpp"
#include "glifistore/core/checked_math.hpp"
#include "glifistore/core/little_endian.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "glifistore/segment/crc32c.hpp"
#include "system_error.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <thread>
#include <unistd.h>

namespace glifistore::detail {
namespace {
constexpr std::uint64_t kStagingPayloadOffset = kBlobHeaderBytes + 16 * kMaximumBlobChunks;

class IoPool final {
  public:
    IoPool() {
        try {
            for (unsigned i = 0; i < 2; ++i) {
                workers_.emplace_back([this] { work(); });
            }
        } catch (...) {
            stop();
            throw;
        }
    }

    ~IoPool() {
        stop();
    }

    template <class F> auto run(F&& fn) -> decltype(fn()) {
        using R = decltype(fn());
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
        auto future = task->get_future();
        {
            std::lock_guard lock{mutex_};
            if (stopped_ || tasks_.size() >= 16) {
                throw std::runtime_error{"blob I/O queue unavailable"};
            }
            tasks_.emplace_back([task] { (*task)(); });
        }
        cv_.notify_one();
        return future.get();
    }

    void stop() noexcept {
        {
            std::lock_guard lock{mutex_};
            stopped_ = true;
        }
        cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

  private:
    void work() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock{mutex_};
                cv_.wait(lock, [&] { return stopped_ || !tasks_.empty(); });
                if (tasks_.empty()) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    bool stopped_{};
};

auto private_file(const int fd, const bool allow_alias = false) -> Status {
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        return persistence_system_error("fstat(blob)");
    }
    if (!S_ISREG(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & 077) != 0 ||
        (st.st_nlink != 1 && !(allow_alias && st.st_nlink == 2))) {
        return fail(ErrorCode::corrupted_data,
                    "blob must be a private regular file without unexpected links");
    }
    return {};
}

auto parse_name(const std::string_view name) -> Result<std::pair<BlobId, bool>> {
    const bool temp = name.starts_with(".blob-") && name.ends_with(".tmp");
    const auto prefix = temp ? 6U : 5U;
    if ((!temp && (!name.starts_with("blob-") || !name.ends_with(".glifi"))) ||
        name.size() != prefix + 32U + (temp ? 4U : 6U)) {
        return fail(ErrorCode::corrupted_data, "unknown blob namespace entry");
    }
    BlobId id{};
    for (std::size_t i = 0; i < 32; ++i) {
        const char c = name[prefix + i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return fail(ErrorCode::corrupted_data, "noncanonical blob ID");
        }
        const auto nibble = static_cast<unsigned>(c <= '9' ? c - '0' : c - 'a' + 10);
        id[i / 2] |= static_cast<std::byte>(nibble << (i % 2 == 0 ? 4U : 0U));
    }
    if (std::ranges::all_of(id, [](auto b) { return b == std::byte{}; })) {
        return fail(ErrorCode::corrupted_data, "zero blob ID");
    }
    return std::pair{id, temp};
}

auto below(const BlobId& a, const BlobId& b) -> bool {
    for (std::size_t i = a.size(); i > 0; --i) {
        if (a[i - 1] != b[i - 1]) {
            return a[i - 1] < b[i - 1];
        }
    }
    return false;
}

auto open_blob_directory(const int root, const bool create) -> Result<FileDescriptor> {
    if (create && ::mkdirat(root, "blobs", 0700) != 0 && errno != EEXIST) {
        return persistence_system_error("mkdirat(blobs)");
    }
    FileDescriptor fd{persistence_detail::interrupted_open_at(
        root, "blobs", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (!fd.valid()) {
        return persistence_system_error("openat(blobs)");
    }
    if (auto status = persistence_detail::validate_directory_descriptor(fd.get()); !status) {
        return unexpected(status.error());
    }
    return fd;
}
} // namespace

auto sync_blob_parent(const DataDirectory& directory) -> Status {
    FileDescriptor root{
        persistence_detail::interrupted_open_at(directory.directory_descriptor(), ".",
                                                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW),
        directory.hooks().file_io};
    if (!root.valid()) {
        return persistence_system_error("openat(blob parent sync)");
    }
    return root.sync(FileSyncMode::full);
}

auto blob_filename(const BlobId& id, const bool temporary) -> std::string {
    constexpr auto digits = "0123456789abcdef";
    std::string name = temporary ? ".blob-" : "blob-";
    name.reserve(43);
    for (auto b : id) {
        const auto v = std::to_integer<unsigned>(b);
        name.push_back(digits[v >> 4U]);
        name.push_back(digits[v & 15U]);
    }
    name += temporary ? ".tmp" : ".glifi";
    return name;
}

struct BlobStorage::State {
    struct File {
        std::uint64_t charge{};
        bool temporary{};
        bool installed{};
    };

    FileDescriptor directory;

    struct Identity {
        StoreId store_id{};
        std::uint32_t worker_count{};
    } manifest;

    BlobResourceLimits limits;
    FilesystemHooks hooks;
    std::uint64_t reserved_free{};
    IoPool pool;
    std::atomic_bool closed{};
    std::atomic_bool close_done{};
    std::atomic_bool healthy{true};
    std::shared_mutex roots_mutex;
    std::mutex mutex;
    std::map<BlobId, File> files;
    std::map<BlobId, std::size_t> pins;
    std::vector<std::weak_ptr<BlobTransfer>> handles;
    std::array<std::size_t, 256> per_shard{};
    std::size_t transfers{};
    std::uint64_t charged{};
    Commit commit;
    Allocate allocate;
    std::function<void()> fail_closed;

    void mark_unhealthy() {
        healthy = false;
        if (fail_closed) {
            fail_closed();
        }
    }

    auto before(const FilesystemOperation op) -> Status {
        return hooks.before ? hooks.before(hooks.context, op) : Status{};
    }

    void after(const FilesystemOperation op) {
        if (hooks.after) {
            hooks.after(hooks.context, op);
        }
    }

    auto admit(const std::size_t shard) -> Status {
        std::lock_guard lock{mutex};
        if (closed || !healthy) {
            return fail(ErrorCode::unavailable, "blob storage is closed or unhealthy");
        }
        if (shard >= manifest.worker_count || transfers >= limits.max_transfers ||
            per_shard[shard] >= limits.max_transfers_per_shard) {
            return fail(ErrorCode::resource_exhausted, "blob transfer credits exhausted");
        }
        ++transfers;
        ++per_shard[shard];
        return {};
    }

    auto reserve(const BlobId& id, const std::uint64_t bytes) -> Status {
        std::lock_guard lock{mutex};
        if (files.size() >= limits.max_files || charged > limits.max_bytes ||
            bytes > limits.max_bytes - charged) {
            return fail(ErrorCode::storage_exhausted, "blob file or byte quota exhausted");
        }
        struct statvfs st{};
        if (::fstatvfs(directory.get(), &st) != 0) {
            return persistence_system_error("fstatvfs(blobs)");
        }
        const auto unit = static_cast<std::uint64_t>(st.f_frsize);
        const auto blocks = static_cast<std::uint64_t>(st.f_bavail);
        const auto free = unit != 0 && blocks > UINT64_MAX / unit ? UINT64_MAX : unit * blocks;
        constexpr auto maintenance = static_cast<std::uint64_t>(kSegmentSizeBytes) + kBlobChunkBytes;
        if (free < reserved_free || maintenance > free - reserved_free ||
            bytes > free - reserved_free - maintenance) {
            return fail(ErrorCode::storage_exhausted, "blob free-space reserve");
        }
        if (files.contains(id) || pins.contains(id)) {
            return fail(ErrorCode::corrupted_data, "reused blob identity");
        }
        auto pin = pins.emplace(id, 0).first;
        try {
            files.emplace(id, File{.charge = bytes, .temporary = true});
        } catch (...) {
            pins.erase(pin);
            throw;
        }
        charged += bytes;
        pin->second = 1;
        return {};
    }

    void release(const std::size_t shard, const std::optional<BlobId>& id) noexcept {
        std::lock_guard lock{mutex};
        if (transfers > 0) {
            --transfers;
        }
        if (shard < per_shard.size() && per_shard[shard] > 0) {
            --per_shard[shard];
        }
        if (id) {
            auto it = pins.find(*id);
            if (it != pins.end() && --it->second == 0) {
                pins.erase(it);
            }
        }
    }

    void forget(const BlobId& id) {
        std::lock_guard lock{mutex};
        const auto it = files.find(id);
        if (it != files.end()) {
            charged -= it->second.charge;
            files.erase(it);
        }
    }
};

struct BlobTransfer {
    std::shared_ptr<BlobStorage::State> storage;
    std::mutex mutex;
    FileDescriptor file;
    std::string key;
    BlobOptions options;
    BlobFileMetadata metadata;
    std::vector<std::byte> buffer;
    std::vector<std::byte> inline_value;
    BlobHasher hash;
    std::uint64_t total{};
    std::uint64_t staged{};
    std::uint64_t position{};
    std::size_t used{};
    std::size_t shard{};
    std::size_t chunk_index{};
    std::size_t chunk_offset{};
    std::size_t loaded{};
    bool reading{};
    bool installed{};
    bool staging_owned{};
    bool terminal{};
    std::atomic_bool released{};
    bool eof_checked{};
    Status outcome{};
    std::optional<BlobId> root;

    ~BlobTransfer() {
        release();
    }

    void release() noexcept {
        if (!released) {
            file.reset();
            // Credits may be reused while callers retain terminal handles. Release
            // their payload and key allocations before making those credits available.
            std::vector<std::byte>{}.swap(buffer);
            std::vector<std::byte>{}.swap(inline_value);
            std::string{}.swap(key);
            storage->release(shard, root);
            released = true;
        }
    }

    // The transfer mutex stays held from the failed operation through terminalization.
    auto terminal_failure_locked(ErrorCode code) noexcept -> Status {
        if (terminal && outcome) {
            release();
            return {};
        }
        if (terminal && !outcome) {
            code = outcome.error().code;
        }
        terminal = true;
        outcome = unexpected(Error{code, {}});
        if (!released && !storage->closed) {
            try {
                static_cast<void>(storage->pool.run([&] { return abort_file(); }));
            } catch (...) {
                // Failed cleanup retains its file/byte charge for conservative GC.
            }
        }
        if (code == ErrorCode::unavailable && !storage->closed && !released) {
            try {
                storage->mark_unhealthy();
            } catch (...) {
                // Health is already revoked; callback failure cannot retain credits.
            }
        }
        release();
        return unexpected(Error{code, {}});
    }

    auto check() -> Status {
        if (storage->closed || !storage->healthy) {
            return fail(ErrorCode::unavailable, "blob handle revoked");
        }
        return {};
    }

    auto stage() -> Status {
        auto id = storage->allocate();
        if (!id) {
            return unexpected(id.error());
        }
        const auto expected = options.expected_size.value_or(kMaximumBlobValueBytes);
        const auto payload_bytes = std::max<std::uint64_t>(expected, 1);
        const auto staging_bytes = checked_add(kStagingPayloadOffset, payload_bytes);
        if (!staging_bytes) {
            return unexpected(staging_bytes.error());
        }
        // Staging reserves all chunk metadata up front; the canonical sealed file
        // can be smaller. Charge the peak extent before creating either name.
        const auto charge = std::max(blob_file_bytes(payload_bytes), *staging_bytes);
        if (auto reserved = storage->reserve(*id, charge); !reserved) {
            return reserved;
        }
        root = *id;
        metadata.reference.id = *id;
        metadata.store_id = storage->manifest.store_id;
        metadata.owner = WorkerId{static_cast<std::uint32_t>(shard)};
        const auto name = blob_filename(*id, true);
        file = FileDescriptor{
            persistence_detail::interrupted_open_at(storage->directory.get(), name.c_str(),
                                                    O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600),
            storage->hooks.file_io};
        if (!file.valid()) {
            storage->forget(*id);
            return persistence_system_error("openat(blob staging)");
        }
        staging_owned = true;
        return {};
    }

    auto flush_chunk() -> Status {
        if (used == 0) {
            return {};
        }
        if (!file.valid()) {
            if (auto started = stage(); !started) {
                return started;
            }
        }
        const auto index = static_cast<std::size_t>(staged / kBlobChunkBytes);
        if (index >= kMaximumBlobChunks) {
            return fail(ErrorCode::record_too_large, "too many blob chunks");
        }
        if (auto allowed = storage->before(FilesystemOperation::write_blob_chunk); !allowed) {
            return allowed;
        }
        const auto bytes = std::span{buffer}.first(used);
        if (auto written = file.write_all_at(bytes, kStagingPayloadOffset + staged); !written) {
            return written;
        }
        metadata.chunks[index] = {.length = static_cast<std::uint32_t>(used), .checksum = crc32c(bytes)};
        staged += used;
        used = 0;
        storage->after(FilesystemOperation::write_blob_chunk);
        return {};
    }

    auto append(std::span<const std::byte> bytes) -> Status {
        if (bytes.size() > kBlobChunkBytes || bytes.size() > kMaximumBlobValueBytes - total ||
            (options.expected_size &&
             (total > *options.expected_size || bytes.size() > *options.expected_size - total))) {
            return fail(ErrorCode::record_too_large, "blob write exceeds declared size or chunk limit");
        }
        hash.update(bytes);
        total += bytes.size();
        while (!bytes.empty()) {
            const auto count = std::min(bytes.size(), buffer.size() - used);
            std::memcpy(buffer.data() + used, bytes.data(), count);
            used += count;
            bytes = bytes.subspan(count);
            if (used == buffer.size()) {
                if (auto flushed = flush_chunk(); !flushed) {
                    return flushed;
                }
            }
        }
        return {};
    }

    auto seal() -> Status;

    auto abort_file() -> Status {
        file.reset();
        if (root && !installed) {
            if (staging_owned) {
                const auto name = blob_filename(*root, true);
                if (::unlinkat(storage->directory.get(), name.c_str(), 0) != 0 && errno != ENOENT) {
                    return persistence_system_error("unlinkat(blob staging)");
                }
                staging_owned = false;
            }
            storage->forget(*root);
        }
        return {};
    }
};

namespace {
auto read_metadata(const FileDescriptor& file, const BlobRef& ref, const StoreId& store, const WorkerId owner)
    -> Result<BlobFileMetadata> {
    if (auto valid = private_file(file.get()); !valid) {
        return unexpected(valid.error());
    }
    auto size = file.size();
    if (!size || *size != blob_file_bytes(ref.logical_size)) {
        return fail(ErrorCode::corrupted_data, "blob file size mismatch");
    }
    std::array<std::byte, kBlobHeaderBytes + kMaximumBlobChunks * 16> bytes{};
    const auto extent = kBlobHeaderBytes + 16U * blob_chunk_count(ref.logical_size);
    if (auto read = file.read_exact_at(std::span{bytes}.first(extent), 0); !read) {
        return unexpected(read.error());
    }
    auto decoded = decode_blob_metadata(std::span{bytes}.first(extent));
    if (!decoded) {
        return unexpected(decoded.error());
    }
    if (decoded->reference.id != ref.id || decoded->reference.logical_size != ref.logical_size ||
        decoded->store_id != store || decoded->owner != owner) {
        return fail(ErrorCode::corrupted_data, "blob identity does not match its descriptor");
    }
    std::array<std::byte, kBlobFooterBytes> footer{};
    if (auto read = file.read_exact_at(footer, *size - kBlobFooterBytes); !read) {
        return unexpected(read.error());
    }
    if (auto valid = validate_blob_footer(footer, ref); !valid) {
        return unexpected(valid.error());
    }
    decoded->reference = ref;
    return decoded;
}

auto verify_file(const FileDescriptor& file, const BlobFileMetadata& meta) -> Status {
    std::vector<std::byte> buffer(kBlobChunkBytes);
    BlobHasher hash;
    for (std::size_t i = 0; i < blob_chunk_count(meta.reference.logical_size); ++i) {
        const auto& chunk = meta.chunks[i];
        const auto bytes = std::span{buffer}.first(chunk.length);
        if (auto read = file.read_exact_at(bytes, chunk.offset); !read) {
            return read;
        }
        if (crc32c(bytes) != chunk.checksum) {
            return fail(ErrorCode::checksum_mismatch, "blob chunk checksum mismatch");
        }
        hash.update(bytes);
    }
    if (hash.finish() != meta.reference.digest) {
        return fail(ErrorCode::checksum_mismatch, "blob value digest mismatch");
    }
    return {};
}
} // namespace

auto BlobTransfer::seal() -> Status {
    if (auto flushed = flush_chunk(); !flushed) {
        return flushed;
    }
    if (!root) {
        return fail(ErrorCode::internal_error, "sealed blob has no staging identity");
    }
    const auto blob_id = *root;
    metadata.reference.logical_size = total;
    metadata.reference.digest = hash.finish();
    const auto count = blob_chunk_count(total);
    std::uint64_t dest = kBlobHeaderBytes + 16ULL * count;
    for (std::size_t i = 0; i < count; ++i) {
        auto& chunk = metadata.chunks[i];
        auto bytes = std::span{buffer}.first(chunk.length);
        if (auto read = file.read_exact_at(bytes, kStagingPayloadOffset + i * kBlobChunkBytes); !read) {
            return read;
        }
        if (crc32c(bytes) != chunk.checksum) {
            return fail(ErrorCode::checksum_mismatch, "staged blob checksum mismatch");
        }
        if (auto written = file.write_all_at(bytes, dest); !written) {
            return written;
        }
        chunk.offset = dest;
        dest += chunk.length;
    }
    auto encoded = encode_blob_metadata(metadata);
    auto footer = encode_blob_footer(metadata.reference);
    if (!encoded || !footer) {
        return fail(ErrorCode::invalid_record, "cannot encode sealed blob");
    }
    if (auto written = file.write_all_at(std::span{*encoded}.first(kBlobHeaderBytes + 16U * count), 0);
        !written) {
        return written;
    }
    if (auto written = file.write_all_at(*footer, dest); !written) {
        return written;
    }
    int truncated{};
    do {
        truncated = ::ftruncate(file.get(), static_cast<off_t>(blob_file_bytes(total)));
    } while (truncated != 0 && errno == EINTR);
    if (truncated != 0) {
        return persistence_system_error("ftruncate(blob)");
    }
    if (auto verified = verify_file(file, metadata); !verified) {
        return verified;
    }
    if (auto allowed = storage->before(FilesystemOperation::sync_blob_file); !allowed) {
        return allowed;
    }
    if (auto synced = file.sync(FileSyncMode::full); !synced) {
        return synced;
    }
    storage->after(FilesystemOperation::sync_blob_file);
    const auto temp = blob_filename(blob_id, true), final = blob_filename(blob_id);
    if (auto allowed = storage->before(FilesystemOperation::install_blob_file); !allowed) {
        return allowed;
    }
    // linkat installs without replacement on every target. A crash between link and unlink
    // leaves a recognizable two-name orphan, never an admitted descriptor.
    if (::linkat(storage->directory.get(), temp.c_str(), storage->directory.get(), final.c_str(), 0) != 0) {
        return persistence_system_error("linkat(blob install)");
    }
    installed = true;
    storage->after(FilesystemOperation::link_blob_file);
    if (::unlinkat(storage->directory.get(), temp.c_str(), 0) != 0) {
        return persistence_system_error("unlinkat(blob install staging)");
    }
    storage->after(FilesystemOperation::install_blob_file);
    if (auto allowed = storage->before(FilesystemOperation::sync_blob_directory); !allowed) {
        return allowed;
    }
    if (auto synced = storage->directory.sync(FileSyncMode::full); !synced) {
        return synced;
    }
    storage->after(FilesystemOperation::sync_blob_directory);
    {
        std::lock_guard lock{storage->mutex};
        auto& entry = storage->files.at(blob_id);
        storage->charged -= entry.charge - blob_file_bytes(total);
        entry = {.charge = blob_file_bytes(total), .installed = true};
    }
    return {};
}

BlobStorage::BlobStorage() = default;

BlobStorage::~BlobStorage() {
    close();
}

auto BlobStorage::open(DataDirectory& directory, const Manifest& manifest, BlobResourceLimits limits,
                       const std::uint64_t reserved_free_bytes) -> Result<std::shared_ptr<BlobStorage>> {
    if (manifest.format_version != 2 || limits.max_files == 0 || limits.max_files > 1'000'000 ||
        limits.max_bytes == 0 || limits.max_transfers == 0 || limits.max_transfers > 8 ||
        limits.max_transfers_per_shard == 0 || limits.max_transfers_per_shard > limits.max_transfers) {
        return fail(ErrorCode::invalid_argument, "invalid blob profile or resource limits");
    }
    auto state = std::make_shared<State>();
    state->manifest = {.store_id = manifest.store_id, .worker_count = manifest.worker_count};
    state->limits = limits;
    state->hooks = directory.hooks();
    state->reserved_free = reserved_free_bytes;
    const bool initial = manifest.manifest_generation == 1;
    auto fd = open_blob_directory(directory.directory_descriptor(), initial);
    if (!fd) {
        return unexpected(fd.error());
    }
    state->directory = std::move(*fd);
    if (initial) {
        if (auto sync = sync_blob_parent(directory); !sync) {
            return unexpected(sync.error());
        }
    }
    FileDescriptor enumeration{persistence_detail::interrupted_open_at(
        state->directory.get(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (!enumeration.valid()) {
        return persistence_system_error("openat(blob enumeration)");
    }
    auto* raw = ::fdopendir(enumeration.get());
    if (!raw) {
        return persistence_system_error("fdopendir(blobs)");
    }
    static_cast<void>(enumeration.release());

    struct CloseDir {
        void operator()(DIR* p) const noexcept {
            if (p) {
                static_cast<void>(::closedir(p));
            }
        }
    };

    std::unique_ptr<DIR, CloseDir> stream{raw};
    std::size_t entries{};
    for (;;) {
        errno = 0;
        const auto* entry = ::readdir(stream.get());
        if (!entry) {
            if (errno != 0) {
                return persistence_system_error("readdir(blobs)");
            }
            break;
        }
        const std::string name{entry->d_name};
        if (name == "." || name == "..") {
            continue;
        }
        if (++entries > limits.max_files * 2) {
            return fail(ErrorCode::storage_exhausted, "blob namespace file limit");
        }
        auto parsed = parse_name(name);
        if (!parsed) {
            return unexpected(parsed.error());
        }
        const auto [id, temp] = *parsed;
        if (!below(id, manifest.next_blob_id)) {
            return fail(ErrorCode::corrupted_data, "blob ID exceeds durable reservation");
        }
        FileDescriptor file{persistence_detail::interrupted_open_at(
            state->directory.get(), name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
        if (!file.valid()) {
            return persistence_system_error("openat(blob audit)");
        }
        if (auto valid = private_file(file.get(), true); !valid) {
            return unexpected(valid.error());
        }
        struct stat st{};
        if (::fstat(file.get(), &st) != 0 || st.st_size < 0 ||
            static_cast<std::uint64_t>(st.st_size) > blob_file_bytes(kMaximumBlobValueBytes)) {
            return fail(ErrorCode::corrupted_data, "blob file extent outside bounds");
        }
        if (st.st_nlink == 2) {
            struct stat other{};
            const auto peer = blob_filename(id, !temp);
            if (::fstatat(state->directory.get(), peer.c_str(), &other, AT_SYMLINK_NOFOLLOW) != 0 ||
                !S_ISREG(other.st_mode) || other.st_dev != st.st_dev || other.st_ino != st.st_ino ||
                other.st_nlink != 2) {
                return fail(ErrorCode::corrupted_data, "unexpected blob hard-link alias");
            }
        }
        auto [it, inserted] =
            state->files.try_emplace(id, State::File{.charge = static_cast<std::uint64_t>(st.st_size)});
        if (inserted) {
            state->charged += it->second.charge;
        } else if (st.st_nlink != 2) {
            return fail(ErrorCode::corrupted_data, "distinct blob files share one ID");
        }
        if (temp) {
            it->second.temporary = true;
        } else {
            it->second.installed = true;
        }
        if (state->files.size() > limits.max_files || state->charged > limits.max_bytes) {
            return fail(ErrorCode::storage_exhausted, "blob recovery byte/file quota");
        }
    }
    auto result = std::make_shared<BlobStorage>();
    result->state_ = std::move(state);
    return result;
}

void BlobStorage::bind(Commit commit, Allocate allocate, std::function<void()> fail_closed) {
    state_->commit = std::move(commit);
    state_->allocate = std::move(allocate);
    state_->fail_closed = std::move(fail_closed);
}

auto BlobStorage::healthy() const noexcept -> bool {
    return state_ && state_->healthy && !state_->closed;
}

auto BlobStorage::snapshot_lock() -> std::shared_lock<std::shared_mutex> {
    return std::shared_lock{state_->roots_mutex};
}

auto BlobStorage::collection_lock() -> std::unique_lock<std::shared_mutex> {
    return std::unique_lock{state_->roots_mutex};
}

auto BlobStorage::begin(const std::string_view key, BlobOptions options, const std::size_t shard)
    -> Result<std::unique_ptr<BlobWriter>> {
    if (key.empty() || key.size() > kMaximumBlobKeyBytes ||
        (options.expected_size && *options.expected_size > kMaximumBlobValueBytes)) {
        return fail(ErrorCode::record_too_large, "blob key or expected value exceeds limit");
    }
    auto transfer = std::make_shared<BlobTransfer>();
    transfer->storage = state_;
    transfer->shard = shard;
    transfer->released = true;
    if (auto admitted = state_->admit(shard); !admitted) {
        return unexpected(admitted.error());
    }
    transfer->released = false;
    transfer->key = key;
    transfer->options = options;
    transfer->buffer.resize(kBlobChunkBytes);
    {
        std::lock_guard lock{state_->mutex};
        if (state_->closed) {
            return fail(ErrorCode::unavailable, "blob storage closed during admission");
        }
        std::erase_if(state_->handles, [](const auto& p) {
            auto h = p.lock();
            return !h || h->released.load();
        });
        state_->handles.push_back(transfer);
    }
    return std::make_unique<BlobWriter>(std::move(transfer));
}

auto BlobStorage::reader(OwnedValue value, const std::size_t shard) -> Result<std::unique_ptr<BlobReader>> {
    auto transfer = std::make_shared<BlobTransfer>();
    transfer->storage = state_;
    transfer->shard = shard;
    transfer->released = true;
    if (auto admitted = state_->admit(shard); !admitted) {
        return unexpected(admitted.error());
    }
    transfer->released = false;
    transfer->reading = true;
    transfer->total = value.bytes.size();
    if (value.storage_type == ValueType::blob_ref) {
        auto ref = decode_blob_ref(value.view());
        if (!ref) {
            state_->mark_unhealthy();
            return unexpected(ref.error());
        }
        auto opened = state_->pool.run([&, ref = *ref]() -> Status {
            const auto name = blob_filename(ref.id);
            transfer->file = FileDescriptor{
                persistence_detail::interrupted_open_at(state_->directory.get(), name.c_str(),
                                                        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK),
                state_->hooks.file_io};
            if (!transfer->file.valid()) {
                return persistence_system_error("openat(blob read)");
            }
            auto meta = read_metadata(transfer->file, ref, state_->manifest.store_id,
                                      WorkerId{static_cast<std::uint32_t>(shard)});
            if (!meta) {
                return unexpected(meta.error());
            }
            transfer->metadata = *meta;
            transfer->total = ref.logical_size;
            transfer->buffer.resize(kBlobChunkBytes);
            return {};
        });
        if (!opened) {
            state_->mark_unhealthy();
            return unexpected(opened.error());
        }
        transfer->root = ref->id;
        {
            std::lock_guard lock{state_->mutex};
            ++state_->pins[*transfer->root];
        }
    } else {
        transfer->inline_value.assign(value.view().begin(), value.view().end());
    }
    {
        std::lock_guard lock{state_->mutex};
        if (state_->closed) {
            return fail(ErrorCode::unavailable, "blob storage closed during read admission");
        }
        std::erase_if(state_->handles, [](const auto& p) {
            auto h = p.lock();
            return !h || h->released.load();
        });
        state_->handles.push_back(transfer);
    }
    return std::make_unique<BlobReader>(std::move(transfer));
}

auto BlobStorage::verify(const BlobRef& ref, const WorkerId owner) -> Status {
    return state_->pool.run([&]() -> Status {
        const auto name = blob_filename(ref.id);
        FileDescriptor file{
            persistence_detail::interrupted_open_at(state_->directory.get(), name.c_str(),
                                                    O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK),
            state_->hooks.file_io};
        if (!file.valid()) {
            return persistence_system_error("openat(blob verify)");
        }
        auto meta = read_metadata(file, ref, state_->manifest.store_id, owner);
        if (!meta) {
            return unexpected(meta.error());
        }
        return verify_file(file, *meta);
    });
}

auto BlobStorage::collect(const std::span<const BlobId> roots) -> Result<BlobGcResult> {
    return state_->pool.run([&]() -> Result<BlobGcResult> {
        std::set<BlobId> keep{roots.begin(), roots.end()};
        std::lock_guard lock{state_->mutex};
        BlobGcResult result;
        for (auto it = state_->files.begin(); it != state_->files.end();) {
            if (keep.contains(it->first) || state_->pins.contains(it->first)) {
                ++result.files_retained;
                ++it;
                continue;
            }
            for (const bool temp : {true, false}) {
                const auto name = blob_filename(it->first, temp);
                if (::unlinkat(state_->directory.get(), name.c_str(), 0) != 0 && errno != ENOENT) {
                    return persistence_system_error("unlinkat(blob GC)");
                }
            }
            if (auto synced = state_->directory.sync(FileSyncMode::full); !synced) {
                return unexpected(synced.error());
            }
            result.bytes_removed += it->second.charge;
            ++result.files_removed;
            state_->charged -= it->second.charge;
            it = state_->files.erase(it);
        }
        return result;
    });
}

void BlobStorage::close() noexcept {
    if (!state_) {
        return;
    }
    if (state_->closed.exchange(true)) {
        while (!state_->close_done.load()) {
            state_->close_done.wait(false);
        }
        return;
    }
    std::array<std::shared_ptr<BlobTransfer>, 8> handles{};
    std::size_t count{};
    {
        std::lock_guard lock{state_->mutex};
        for (auto& weak : state_->handles) {
            if (auto h = weak.lock(); h && !h->released) {
                if (count == handles.size()) {
                    std::terminate();
                }
                handles[count++] = std::move(h);
            }
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        std::lock_guard lock{handles[i]->mutex};
        if (!handles[i]->reading && !handles[i]->terminal) {
            handles[i]->terminal = true;
            handles[i]->outcome = unexpected(Error{ErrorCode::unavailable, {}});
            try {
                static_cast<void>(state_->pool.run([&] { return handles[i]->abort_file(); }));
            } catch (...) {
                // An unremoved orphan remains charged and is audited at the next open.
            }
        }
        handles[i]->release();
    }
    state_->pool.stop();
    state_->close_done = true;
    state_->close_done.notify_all();
}

} // namespace glifistore::detail

namespace glifistore {
BlobWriter::BlobWriter(std::shared_ptr<detail::BlobTransfer> state) noexcept : state_(std::move(state)) {}

BlobWriter::~BlobWriter() {
    abort();
}

auto BlobWriter::write(const std::span<const std::byte> bytes) -> Status try {
    if (!state_) {
        return fail(ErrorCode::invalid_argument, "empty blob handle");
    }
    auto& t = *state_;
    std::lock_guard lock{t.mutex};
    try {
        if (t.terminal) {
            return fail(ErrorCode::invalid_argument, "blob writer is terminal");
        }
        if (auto valid = t.check(); !valid) {
            return valid;
        }
        auto roots = std::shared_lock{t.storage->roots_mutex};
        auto result = t.storage->pool.run([&] { return t.append(bytes); });
        if (!result) {
            t.outcome = result;
            t.terminal = true;
            static_cast<void>(t.storage->pool.run([&] { return t.abort_file(); }));
            t.release();
        }
        return result;
    } catch (const std::bad_alloc&) {
        return t.terminal_failure_locked(ErrorCode::resource_exhausted);
    } catch (...) {
        return t.terminal_failure_locked(ErrorCode::unavailable);
    }
} catch (const std::bad_alloc&) {

    return unexpected(Error{ErrorCode::resource_exhausted, {}});
} catch (...) {
    return unexpected(Error{ErrorCode::unavailable, {}});
}

auto BlobWriter::commit() -> Status try {
    if (!state_) {
        return fail(ErrorCode::invalid_argument, "empty blob handle");
    }
    auto& t = *state_;
    std::lock_guard lock{t.mutex};
    try {
        if (t.terminal) {
            return t.outcome;
        }
        if (auto valid = t.check(); !valid) {
            return valid;
        }
        auto roots = std::shared_lock{t.storage->roots_mutex};
        if (t.options.expected_size && *t.options.expected_size != t.total) {
            t.outcome = fail(ErrorCode::invalid_argument, "blob length does not match expected_size");
            // A fully buffered small upload can use an ordinary inline Record; the
            // Store commit callback remains the only key-visibility publication point.
        } else if (t.staged == 0 && t.total <= kMaximumBlobKeyBytes + 80 - t.key.size()) {
            t.outcome = t.storage->commit(t.key, std::span{t.buffer}.first(t.used), t.options.expire_at_ns,
                                          ValueType::bytes);
        } else {
            t.outcome = t.storage->pool.run([&] { return t.seal(); });
            if (t.outcome) {
                auto encoded = encode_blob_ref(t.metadata.reference);
                t.outcome =
                    encoded ? t.storage->commit(t.key, *encoded, t.options.expire_at_ns, ValueType::blob_ref)
                            : Status{unexpected(encoded.error())};
            }
        }
        t.terminal = true;
        if (!t.installed && t.file.valid()) {
            static_cast<void>(t.storage->pool.run([&] { return t.abort_file(); }));
        }
        t.release();
        return t.outcome;
    } catch (const std::bad_alloc&) {
        return t.terminal_failure_locked(ErrorCode::resource_exhausted);
    } catch (...) {
        return t.terminal_failure_locked(ErrorCode::unavailable);
    }
} catch (const std::bad_alloc&) {

    return unexpected(Error{ErrorCode::resource_exhausted, {}});
} catch (...) {
    return unexpected(Error{ErrorCode::unavailable, {}});
}

void BlobWriter::abort() noexcept {
    if (!state_) {
        return;
    }
    try {
        auto& t = *state_;
        std::lock_guard lock{t.mutex};
        if (!t.terminal) {
            t.outcome = unexpected(Error{ErrorCode::unavailable, {}});
            t.terminal = true;
            if (!t.storage->closed) {
                try {
                    static_cast<void>(t.storage->pool.run([&] { return t.abort_file(); }));
                } catch (...) {
                    // Keep orphan accounting even if cleanup dispatch cannot allocate.
                }
            }
        }
        t.release();
    } catch (...) {
    }
}

BlobReader::BlobReader(std::shared_ptr<detail::BlobTransfer> state) noexcept : state_(std::move(state)) {}

BlobReader::~BlobReader() = default;

auto BlobReader::size() const noexcept -> std::uint64_t {
    return state_ ? state_->total : 0;
}

auto BlobReader::read(const std::span<std::byte> bytes) -> Result<std::size_t> try {
    if (!state_) {
        return fail(ErrorCode::invalid_argument, "empty blob handle");
    }
    auto& t = *state_;
    std::lock_guard lock{t.mutex};
    if (auto valid = t.check(); !valid) {
        return unexpected(valid.error());
    }
    if (!t.outcome) {
        return unexpected(t.outcome.error());
    }
    if (bytes.empty() && t.position < t.total) {
        return fail(ErrorCode::invalid_argument, "nonempty buffer required before EOF");
    }
    if (!t.root) {
        const auto count =
            std::min<std::size_t>(bytes.size(), t.inline_value.size() - static_cast<std::size_t>(t.position));
        if (count) {
            std::memcpy(bytes.data(), t.inline_value.data() + t.position, count);
        }
        t.position += count;
        return count;
    }
    auto result = t.storage->pool.run([&]() -> Result<std::size_t> {
        if (t.position == t.total) {
            return std::size_t{0};
        }
        if (t.chunk_offset == t.loaded) {
            const auto& chunk = t.metadata.chunks[t.chunk_index];
            auto part = std::span{t.buffer}.first(chunk.length);
            if (auto read = t.file.read_exact_at(part, chunk.offset); !read) {
                return unexpected(read.error());
            }
            if (crc32c(part) != chunk.checksum) {
                return fail(ErrorCode::checksum_mismatch, "blob read chunk checksum mismatch");
            }
            t.hash.update(part);
            // Verify the full digest before exposing the final chunk. Earlier
            // reads establish chunk CRC integrity only until this point.
            if (t.chunk_index + 1 == blob_chunk_count(t.total) &&
                t.hash.finish() != t.metadata.reference.digest) {
                return fail(ErrorCode::checksum_mismatch, "blob read digest mismatch");
            }
            ++t.chunk_index;
            t.loaded = chunk.length;
            t.chunk_offset = 0;
        }
        const auto count = std::min(bytes.size(), t.loaded - t.chunk_offset);
        std::memcpy(bytes.data(), t.buffer.data() + t.chunk_offset, count);
        t.chunk_offset += count;
        t.position += count;
        return count;
    });
    if (!result) {
        t.outcome = unexpected(result.error());
        t.storage->mark_unhealthy();
    }
    return result;
} catch (const std::bad_alloc&) {

    return fail(ErrorCode::resource_exhausted, "blob read allocation failed");
} catch (...) {
    return fail(ErrorCode::unavailable, "blob read failed");
}
} // namespace glifistore
