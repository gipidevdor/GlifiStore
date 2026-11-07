#include "glifistore/core/error.hpp"
#include "glifistore/persistence/blob.hpp"
#include "glifistore/persistence/filesystem.hpp"
#include "glifistore/persistence/filesystem_hooks.hpp"
#include "glifistore/store/blob.hpp"
#include "glifistore/store/config.hpp"
#include "glifistore/store/store.hpp"
#include "persistence/blob_storage.hpp"
#include "test.hpp"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using namespace glifistore;

class BlobLifecycleDirectory {
  public:
    BlobLifecycleDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "glifi-blob-lifecycle-XXXXXX").string();
        const auto* created = ::mkdtemp(pattern.data());
        if (created == nullptr) {
            throw std::runtime_error{"mkdtemp failed"};
        }
        path = created;
    }

    ~BlobLifecycleDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    BlobLifecycleDirectory(const BlobLifecycleDirectory&) = delete;
    auto operator=(const BlobLifecycleDirectory&) -> BlobLifecycleDirectory& = delete;
    std::filesystem::path path;
};

auto lifecycle_config(const std::filesystem::path& path) -> StoreConfig {
    StoreConfig config;
    config.data_directory = path;
    config.enable_blobs = true;
    config.storage_mode = StorageMode::durable_sync;
    config.worker_config.explicit_count = 1;
    config.durable_limits.reserved_free_bytes = 0;
    config.maintenance.mode = MaintenanceMode::disabled;
    config.blob_limits.max_transfers = 1;
    config.blob_limits.max_transfers_per_shard = 1;
    config.blob_limits.max_files = 1;
    return config;
}

template <class T> auto require_value(Result<T> result) -> T {
    if (!result) {
        throw std::runtime_error{result.error().message};
    }
    return std::move(*result);
}

auto lifecycle_file_count(const std::filesystem::path& path) -> std::size_t {
    std::size_t count{};
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator{path / "blobs"}) {
        ++count;
    }
    return count;
}
} // namespace

GLIFI_TEST("blob staged abort and destruction remove files and return quota without GC") {
    BlobLifecycleDirectory directory;
    auto store = require_value(Store::open(lifecycle_config(directory.path)));
    const std::vector<std::byte> chunk(kBlobChunkBytes, std::byte{0x31});
    for (unsigned iteration = 0; iteration < 4; ++iteration) {
        auto writer = require_value(store->begin_blob_put("key", {.expected_size = kBlobChunkBytes}));
        GLIFI_REQUIRE(writer->write(chunk).has_value());
        GLIFI_REQUIRE(lifecycle_file_count(directory.path) == 1);
        if (iteration % 2 == 0) {
            writer->abort();
            writer->abort();
            GLIFI_REQUIRE(!writer->commit());
        }
        writer.reset();
        GLIFI_REQUIRE(lifecycle_file_count(directory.path) == 0);
        GLIFI_REQUIRE(!store->get("key"));
    }
}

GLIFI_TEST("blob failed chunk dispatch is terminal cleans staging and returns credits") {
    BlobLifecycleDirectory directory;
    auto config = lifecycle_config(directory.path);
    std::atomic_bool inject{};
    config.filesystem_hooks.context = &inject;
    config.filesystem_hooks.before = [](void* context, FilesystemOperation operation) -> Status {
        if (operation == FilesystemOperation::write_blob_chunk &&
            static_cast<std::atomic_bool*>(context)->exchange(false)) {
            throw std::bad_alloc{};
        }
        return {};
    };
    auto store = require_value(Store::open(config));
    const std::vector<std::byte> chunk(kBlobChunkBytes, std::byte{0x42});
    auto writer = require_value(store->begin_blob_put("failed", {.expected_size = kBlobChunkBytes}));
    inject = true;
    const auto written = writer->write(chunk);
    GLIFI_REQUIRE(!written);
    GLIFI_REQUIRE(written.error().code == ErrorCode::resource_exhausted);
    const auto committed = writer->commit();
    GLIFI_REQUIRE(!committed);
    GLIFI_REQUIRE(committed.error().code == ErrorCode::resource_exhausted);
    GLIFI_REQUIRE(!writer->commit());
    GLIFI_REQUIRE(!store->get("failed"));
    GLIFI_REQUIRE(lifecycle_file_count(directory.path) == 0);
    auto next = require_value(store->begin_blob_put("next", {.expected_size = kBlobChunkBytes}));
    GLIFI_REQUIRE(next->write(chunk).has_value());
    GLIFI_REQUIRE(next->commit().has_value());
}

GLIFI_TEST("blob quota reserves the peak staging extent before creating a file") {
    BlobLifecycleDirectory directory;
    auto config = lifecycle_config(directory.path);
    config.blob_limits.max_bytes = blob_file_bytes(kBlobChunkBytes);
    auto store = require_value(Store::open(config));
    const std::vector<std::byte> chunk(kBlobChunkBytes, std::byte{0x53});
    auto writer = require_value(store->begin_blob_put("quota", {.expected_size = kBlobChunkBytes}));
    const auto written = writer->write(chunk);
    GLIFI_REQUIRE(!written);
    GLIFI_REQUIRE(written.error().code == ErrorCode::storage_exhausted);
    GLIFI_REQUIRE(lifecycle_file_count(directory.path) == 0);
    GLIFI_REQUIRE(!store->get("quota"));
}

GLIFI_TEST("blob parent directory sync retains configured syscall fault hooks") {
    BlobLifecycleDirectory directory;
    std::atomic_bool armed{};
    std::atomic_uint calls{};

    struct Context {
        std::atomic_bool& armed;
        std::atomic_uint& calls;
    } context{armed, calls};

    FilesystemHooks hooks;
    hooks.file_io.context = &context;
    hooks.file_io.sync_file = [](void* opaque, int, FileSyncMode) -> int {
        auto& state = *static_cast<Context*>(opaque);
        if (state.armed) {
            ++state.calls;
            errno = EIO;
            return -1;
        }
        return 0;
    };
    auto data = require_value(DataDirectory::open_and_lock(directory.path, hooks));
    armed = true;
    const auto synced = detail::sync_blob_parent(data);
    GLIFI_REQUIRE(!synced);
    GLIFI_REQUIRE(calls == 1);
}

GLIFI_TEST("blob exact peak staging quota permits commit and survives reopen") {
    BlobLifecycleDirectory directory;
    auto config = lifecycle_config(directory.path);
    config.blob_limits.max_bytes = kBlobHeaderBytes + 16ULL * kMaximumBlobChunks + kBlobChunkBytes;
    auto store = require_value(Store::open(config));
    const std::vector<std::byte> chunk(kBlobChunkBytes, std::byte{0x64});
    auto writer = require_value(store->begin_blob_put("exact", {.expected_size = kBlobChunkBytes}));
    GLIFI_REQUIRE(writer->write(chunk).has_value());
    GLIFI_REQUIRE(writer->commit().has_value());
    GLIFI_REQUIRE(writer->commit().has_value());
    GLIFI_REQUIRE(store->close().has_value());
    store.reset();
    store = require_value(Store::open(config));
    const auto value = require_value(store->get("exact"));
    GLIFI_REQUIRE(value.bytes.size() == chunk.size());
    GLIFI_REQUIRE(value.bytes.front() == chunk.front());
    GLIFI_REQUIRE(value.bytes.back() == chunk.back());
}

GLIFI_TEST("blob close aborts active staging before revoking retained handles") {
    BlobLifecycleDirectory directory;
    auto config = lifecycle_config(directory.path);
    auto store = require_value(Store::open(config));
    const std::vector<std::byte> chunk(kBlobChunkBytes, std::byte{0x75});
    auto writer = require_value(store->begin_blob_put("closed", {.expected_size = kBlobChunkBytes}));
    GLIFI_REQUIRE(writer->write(chunk).has_value());
    GLIFI_REQUIRE(lifecycle_file_count(directory.path) == 1);
    GLIFI_REQUIRE(store->close().has_value());
    GLIFI_REQUIRE(lifecycle_file_count(directory.path) == 0);
    GLIFI_REQUIRE(!writer->write(chunk));
    GLIFI_REQUIRE(!writer->commit());
    writer->abort();
    store.reset();
    store = require_value(Store::open(config));
    GLIFI_REQUIRE(!store->get("closed"));
    auto next = require_value(store->begin_blob_put("next", {.expected_size = kBlobChunkBytes}));
    GLIFI_REQUIRE(next->write(chunk).has_value());
    GLIFI_REQUIRE(next->commit().has_value());
}

GLIFI_TEST("blob concurrent commit cannot publish an upload after its chunk throws") {
    BlobLifecycleDirectory directory;
    auto config = lifecycle_config(directory.path);

    struct Context {
        std::atomic_bool entered{};
        std::atomic_bool proceed{};
        std::atomic_bool commit_started{};
    } context;

    config.filesystem_hooks.context = &context;
    config.filesystem_hooks.before = [](void* opaque, FilesystemOperation operation) -> Status {
        if (operation == FilesystemOperation::write_blob_chunk) {
            auto& state = *static_cast<Context*>(opaque);
            state.entered = true;
            state.entered.notify_all();
            state.proceed.wait(false);
            throw std::bad_alloc{};
        }
        return {};
    };
    auto store = require_value(Store::open(config));
    auto writer = require_value(store->begin_blob_put("concurrent", {.expected_size = kBlobChunkBytes}));
    const std::vector<std::byte> chunk(kBlobChunkBytes, std::byte{0x26});
    Status written;
    Status committed;
    std::jthread append_thread{[&] { written = writer->write(chunk); }};
    context.entered.wait(false);
    std::jthread commit_thread{[&] {
        context.commit_started = true;
        context.commit_started.notify_all();
        committed = writer->commit();
    }};
    context.commit_started.wait(false);
    context.proceed = true;
    context.proceed.notify_all();
    append_thread.join();
    commit_thread.join();
    GLIFI_REQUIRE(!written);
    GLIFI_REQUIRE(!committed);
    GLIFI_REQUIRE(committed.error().code == ErrorCode::resource_exhausted);
    GLIFI_REQUIRE(!store->get("concurrent"));
    GLIFI_REQUIRE(lifecycle_file_count(directory.path) == 0);
}
