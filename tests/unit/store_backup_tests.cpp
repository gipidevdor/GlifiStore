#include "glifistore/client/client.hpp"
#include "glifistore/persistence/filesystem.hpp"
#include "glifistore/persistence/manifest.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "glifistore/persistence/store_backup.hpp"
#include "glifistore/persistence/store_verify.hpp"
#include "glifistore/segment/crc32c.hpp"
#include "glifistore/server/daemon_log.hpp"
#include "glifistore/server/server.hpp"
#include "glifistore/store/store.hpp"
#include "test.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

class BackupTemporaryDirectory final {
  public:
    BackupTemporaryDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "glifistore-backup-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* created = ::mkdtemp(writable.data());
        GLIFI_REQUIRE(created != nullptr);
        path_ = created;
    }

    ~BackupTemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] auto path() const -> const std::filesystem::path& {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

auto bytes(std::string_view value) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

auto value_string(const glifistore::OwnedValue& value) -> std::string_view {
    return {reinterpret_cast<const char*>(value.bytes.data()), value.bytes.size()};
}

inline constexpr std::size_t kManifestChecksumOffset = 80;

void put_u16(std::span<std::byte> out, std::size_t offset, std::uint16_t value) {
    out[offset] = static_cast<std::byte>(value & 0xFFU);
    out[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
}

void put_u32(std::span<std::byte> out, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        out[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
}

void refresh_manifest_checksum(std::span<std::byte> bytes) {
    put_u32(bytes, kManifestChecksumOffset, 0);
    put_u32(bytes, kManifestChecksumOffset, glifistore::crc32c(bytes));
}

auto read_file_bytes(const std::filesystem::path& path) -> std::vector<std::byte> {
    std::ifstream input{path, std::ios::binary};
    GLIFI_REQUIRE(static_cast<bool>(input));
    input.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(input.tellg());
    input.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(size);
    if (size > 0) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        GLIFI_REQUIRE(static_cast<bool>(input));
    }
    return bytes;
}

void write_file_bytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    GLIFI_REQUIRE(static_cast<bool>(output));
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        GLIFI_REQUIRE(static_cast<bool>(output));
    }
}

auto seed_single_worker_store(const std::filesystem::path& data_dir) -> void {
    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = data_dir,
        .durable_open_mode = glifistore::DurableOpenMode::create_new,
    });
    GLIFI_REQUIRE(opened.has_value());
    GLIFI_REQUIRE((*opened)->put("keep", bytes("alive")).has_value());
    GLIFI_REQUIRE((*opened)->close().has_value());
}

} // namespace

GLIFI_TEST("backup_durable_store copies a verified Store and restores committed keys") {
    BackupTemporaryDirectory root;
    const auto source = root.path() / "source";
    const auto backup = root.path() / "backup";
    const auto restored = root.path() / "restored";

    {
        auto opened = glifistore::Store::open({
            .worker_config = {.explicit_count = 1},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = source,
            .durable_open_mode = glifistore::DurableOpenMode::create_new,
        });
        GLIFI_REQUIRE(opened.has_value());
        GLIFI_REQUIRE((*opened)->put("alpha", bytes("one")).has_value());
        GLIFI_REQUIRE((*opened)->put("beta", bytes("two")).has_value());
        GLIFI_REQUIRE((*opened)->close().has_value());
    }

    const auto backed = glifistore::backup_durable_store(source, backup);
    GLIFI_REQUIRE(backed.has_value());
    GLIFI_REQUIRE(backed->files_copied >= 2);
    GLIFI_REQUIRE(backed->admission_fence_ns == 0);
    GLIFI_REQUIRE(backed->catalog_copy_ns > 0);
    GLIFI_REQUIRE(backed->destination_verify_ns > 0);
    GLIFI_REQUIRE(backed->source_crc_scanned);
    GLIFI_REQUIRE(backed->destination_crc_scanned);
    GLIFI_REQUIRE(backed->source_verification.scanned_records > 0);
    GLIFI_REQUIRE(backed->destination_verification.scanned_records > 0);
    GLIFI_REQUIRE(backed->source_verification.segments.size() ==
                  backed->destination_verification.segments.size());

    const auto restored_copy = glifistore::restore_durable_store(backup, restored);
    GLIFI_REQUIRE(restored_copy.has_value());

    auto reopened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = restored,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE(value_string(*(*reopened)->get("alpha")) == "one");
    GLIFI_REQUIRE(value_string(*(*reopened)->get("beta")) == "two");
}

GLIFI_TEST("backup_durable_store fails closed when the source is locked") {
    BackupTemporaryDirectory root;
    const auto source = root.path() / "source";
    const auto backup = root.path() / "backup";
    {
        auto opened = glifistore::Store::open({
            .worker_config = {.explicit_count = 1},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = source,
            .durable_open_mode = glifistore::DurableOpenMode::create_new,
        });
        GLIFI_REQUIRE(opened.has_value());
        GLIFI_REQUIRE((*opened)->put("live", bytes("v")).has_value());

        const auto contested = glifistore::backup_durable_store(source, backup);
        GLIFI_REQUIRE(!contested.has_value());
        GLIFI_REQUIRE(contested.error().code == glifistore::ErrorCode::io_error);
        GLIFI_REQUIRE((*opened)->close().has_value());
    }
}

GLIFI_TEST("backup_durable_store refuses a non-empty destination") {
    BackupTemporaryDirectory root;
    const auto source = root.path() / "source";
    const auto destination = root.path() / "destination";
    {
        auto opened = glifistore::Store::open({
            .worker_config = {.explicit_count = 1},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = source,
            .durable_open_mode = glifistore::DurableOpenMode::create_new,
        });
        GLIFI_REQUIRE(opened.has_value());
        GLIFI_REQUIRE((*opened)->put("k", bytes("v")).has_value());
        GLIFI_REQUIRE((*opened)->close().has_value());
    }
    {
        auto occupied = glifistore::Store::open({
            .worker_config = {.explicit_count = 1},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = destination,
            .durable_open_mode = glifistore::DurableOpenMode::create_new,
        });
        GLIFI_REQUIRE(occupied.has_value());
        GLIFI_REQUIRE((*occupied)->put("other", bytes("x")).has_value());
        GLIFI_REQUIRE((*occupied)->close().has_value());
    }

    const auto refused = glifistore::backup_durable_store(source, destination);
    GLIFI_REQUIRE(!refused.has_value());
    GLIFI_REQUIRE(refused.error().code == glifistore::ErrorCode::sequence_conflict ||
                  refused.error().code == glifistore::ErrorCode::invalid_argument);
}

GLIFI_TEST("Store::backup_to copies while the Store remains open under writer fence") {
    BackupTemporaryDirectory root;
    const auto source = root.path() / "source";
    const auto backup = root.path() / "backup";
    const auto restored = root.path() / "restored";

    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = source,
        .durable_open_mode = glifistore::DurableOpenMode::create_new,
    });
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(store.put("alpha", bytes("one")).has_value());
    GLIFI_REQUIRE(store.put("beta", bytes("two")).has_value());

    // Offline path still fails closed against the live lock.
    const auto contested = glifistore::backup_durable_store(source, root.path() / "offline");
    GLIFI_REQUIRE(!contested.has_value());
    GLIFI_REQUIRE(contested.error().code == glifistore::ErrorCode::io_error);

    std::error_code ec;
    std::filesystem::create_directories(backup, ec);
    GLIFI_REQUIRE(!ec);

    // Single-threaded online backup first (Store stays open; flock retained).
    {
        const auto dest = backup / "solo";
        const auto backed = store.backup_to(dest);
        GLIFI_REQUIRE(backed.has_value());
        GLIFI_REQUIRE(backed->files_copied >= 2);
        GLIFI_REQUIRE(backed->admission_fence_ns > 0);
        GLIFI_REQUIRE(backed->catalog_copy_ns > 0);
        GLIFI_REQUIRE(backed->destination_verify_ns > 0);
        // Online fence uses structural source check only; CRC scan is on the destination.
        GLIFI_REQUIRE(!backed->source_crc_scanned);
        GLIFI_REQUIRE(backed->destination_crc_scanned);
        GLIFI_REQUIRE(backed->destination_verification.segments.size() >= 1);
    }
    GLIFI_REQUIRE(store.put("gamma", bytes("three")).has_value());

    std::atomic_bool stop{false};
    std::atomic_uint64_t writes{0};
    std::thread writer{[&] {
        std::uint64_t i = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            const auto key = "w" + std::to_string(i++);
            if (store.put(key, bytes("v")).has_value()) {
                writes.fetch_add(1, std::memory_order_relaxed);
                writes.notify_one();
            } else {
                // Admission fence during backup: brief pause then retry.
                std::this_thread::yield();
            }
        }
    }};

    writes.wait(0, std::memory_order_relaxed);

    for (int round = 0; round < 3; ++round) {
        const auto dest = backup / ("round-" + std::to_string(round));
        const auto backed = store.backup_to(dest);
        GLIFI_REQUIRE(backed.has_value());
        GLIFI_REQUIRE(backed->files_copied >= 2);
    }

    stop.store(true, std::memory_order_relaxed);
    writer.join();
    GLIFI_REQUIRE(writes.load(std::memory_order_relaxed) > 0);

    const auto final_backup = store.backup_to(backup / "final");
    GLIFI_REQUIRE(final_backup.has_value());
    GLIFI_REQUIRE(store.close().has_value());

    const auto restored_copy = glifistore::restore_durable_store(backup / "final", restored);
    GLIFI_REQUIRE(restored_copy.has_value());
    auto reopened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = restored,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE(value_string(*(*reopened)->get("alpha")) == "one");
    GLIFI_REQUIRE(value_string(*(*reopened)->get("beta")) == "two");
    GLIFI_REQUIRE(value_string(*(*reopened)->get("gamma")) == "three");
}

GLIFI_TEST("concurrent Store::backup_to retains its counted admission fence while waiting") {
    // First backup releases its fence when done; a second backup waiting on
    // compaction_mutex must retain its own fence so writers cannot be admitted
    // while its catalog copy is in progress.
    BackupTemporaryDirectory root;
    const auto source = root.path() / "source";
    const auto backup = root.path() / "backup";

    struct FenceProbe final {
        std::atomic_bool stall_first_segment{true};
        std::atomic_bool release_first{false};
        std::atomic_int manifest_copies{0};
        std::atomic_bool put_during_second_manifest{false};
        glifistore::Store* store{};

        static auto before(void* opaque, const glifistore::FilesystemOperation operation)
            -> glifistore::Status {
            auto& probe = *static_cast<FenceProbe*>(opaque);
            if (operation == glifistore::FilesystemOperation::copy_backup_segment) {
                if (probe.stall_first_segment.exchange(false, std::memory_order_acq_rel)) {
                    while (!probe.release_first.load(std::memory_order_acquire)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds{1});
                    }
                }
                return {};
            }
            if (operation == glifistore::FilesystemOperation::copy_backup_manifest) {
                const auto which = probe.manifest_copies.fetch_add(1, std::memory_order_acq_rel);
                if (which == 1 && probe.store != nullptr) {
                    // Second backup's manifest copy: admissions must still be closed.
                    const auto admitted = probe.store->put("during-second-backup", bytes("no"));
                    probe.put_during_second_manifest.store(admitted.has_value(), std::memory_order_release);
                }
                return {};
            }
            return {};
        }
    } probe;

    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = source,
        .durable_open_mode = glifistore::DurableOpenMode::create_new,
        .filesystem_hooks = {.context = &probe, .before = &FenceProbe::before},
    });
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    probe.store = &store;
    GLIFI_REQUIRE(store.put("seed", bytes("v")).has_value());

    std::error_code ec;
    std::filesystem::create_directories(backup, ec);
    GLIFI_REQUIRE(!ec);

    std::atomic_bool first_done{false};
    std::atomic_bool second_done{false};
    std::optional<glifistore::Error> first_error;
    std::optional<glifistore::Error> second_error;

    std::thread first{[&] {
        auto backed = store.backup_to(backup / "first");
        if (!backed) {
            first_error = backed.error();
        }
        first_done.store(true, std::memory_order_release);
    }};
    while (probe.stall_first_segment.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    std::thread second{[&] {
        auto backed = store.backup_to(backup / "second");
        if (!backed) {
            second_error = backed.error();
        }
        second_done.store(true, std::memory_order_release);
    }};
    // Give the second backup time to acquire its fence and reach the
    // compaction_mutex wait.
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    probe.release_first.store(true, std::memory_order_release);

    first.join();
    second.join();
    GLIFI_REQUIRE(!first_error.has_value());
    GLIFI_REQUIRE(!second_error.has_value());
    GLIFI_REQUIRE(probe.manifest_copies.load(std::memory_order_acquire) >= 2);
    GLIFI_REQUIRE(!probe.put_during_second_manifest.load(std::memory_order_acquire));
    GLIFI_REQUIRE(store.put("after-both", bytes("ok")).has_value());
    GLIFI_REQUIRE(store.close().has_value());
}

GLIFI_TEST("Server::backup_to copies a live durable daemon catalog") {
    BackupTemporaryDirectory root;
    const auto source = root.path() / "source";
    const auto backup = root.path() / "backup";
    const auto restored = root.path() / "restored";

    auto opened =
        glifistore::server::Server::create({.port = 0, .maximum_connections = 4},
                                           {.worker_config = {.explicit_count = 1},
                                            .storage_mode = glifistore::StorageMode::durable_sync,
                                            .data_directory = source,
                                            .durable_open_mode = glifistore::DurableOpenMode::create_new});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    auto connected = glifistore::client::Client::connect({.port = server.port()});
    GLIFI_REQUIRE(connected.has_value());
    auto& client = *connected;
    GLIFI_REQUIRE(client.put("daemon-key", "daemon-value").committed());

    std::error_code ec;
    std::filesystem::create_directories(backup, ec);
    GLIFI_REQUIRE(!ec);
    const auto dest = backup / "live";
    const auto backed = server.backup_to(dest);
    GLIFI_REQUIRE(backed.has_value());
    GLIFI_REQUIRE(backed->files_copied >= 2);

    const auto contested = glifistore::backup_durable_store(source, backup / "offline-contested");
    GLIFI_REQUIRE(!contested.has_value());

    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());

    const auto restored_copy = glifistore::restore_durable_store(dest, restored);
    GLIFI_REQUIRE(restored_copy.has_value());
    auto reopened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = restored,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE(value_string(*(*reopened)->get("daemon-key")) == "daemon-value");
}

GLIFI_TEST("Server READY fails while online backup fences admissions") {
    // Wire/docs: READY requires Store admission open. backup_to close_admission must
    // fail READY (and classify admission_fenced) while HEALTH/live stay up.
    BackupTemporaryDirectory root;
    const auto source = root.path() / "source";
    const auto backup = root.path() / "backup";

    struct StallBackup final {
        std::atomic_bool entered{false};
        std::atomic_bool release{false};

        static auto before(void* opaque, const glifistore::FilesystemOperation operation)
            -> glifistore::Status {
            auto& probe = *static_cast<StallBackup*>(opaque);
            if (operation == glifistore::FilesystemOperation::copy_backup_segment) {
                probe.entered.store(true, std::memory_order_release);
                while (!probe.release.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
            }
            return {};
        }
    } stall;

    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 4},
        {.worker_config = {.explicit_count = 1},
         .storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = source,
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .filesystem_hooks = {.context = &stall, .before = &StallBackup::before}});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());
    GLIFI_REQUIRE(server.ready());
    GLIFI_REQUIRE(server.admissions_open());
    GLIFI_REQUIRE(server.live());

    auto connected = glifistore::client::Client::connect({.port = server.port()});
    GLIFI_REQUIRE(connected.has_value());
    GLIFI_REQUIRE(connected->put("seed", "v").committed());

    std::error_code ec;
    std::filesystem::create_directories(backup, ec);
    GLIFI_REQUIRE(!ec);

    std::optional<glifistore::Error> backup_error;
    std::thread backup_thread{[&] {
        auto backed = server.backup_to(backup / "fenced");
        if (!backed) {
            backup_error = backed.error();
        }
    }};
    while (!stall.entered.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    GLIFI_REQUIRE(server.live());
    GLIFI_REQUIRE(!server.admissions_open());
    GLIFI_REQUIRE(!server.ready());
    GLIFI_REQUIRE(glifistore::server::classify_ready_loss(server) ==
                  glifistore::server::ReadyLossReason::admission_fenced);

    auto probe = glifistore::client::Client::connect({.port = server.port()});
    GLIFI_REQUIRE(probe.has_value());
    // Fence is known-not-committed: wire OVERLOADED / rejected — not INTERNAL_ERROR /
    // reconcile_first (mutation never entered Store append).
    const auto fenced_put = probe->put("during-fence", "no");
    GLIFI_REQUIRE(!fenced_put.committed());
    GLIFI_REQUIRE(fenced_put.outcome == glifistore::client::MutationOutcome::rejected);
    GLIFI_REQUIRE(fenced_put.error.has_value());
    GLIFI_REQUIRE(fenced_put.error->code == glifistore::ErrorCode::resource_exhausted);
    GLIFI_REQUIRE(fenced_put.error->mutation_outcome == "rejected" ||
                  fenced_put.error->mutation_outcome.empty());
    GLIFI_REQUIRE(fenced_put.error->category != "indeterminate");
    if (!fenced_put.error->retryability.empty()) {
        GLIFI_REQUIRE(fenced_put.error->retryability != "reconcile_first");
    }
    // Durable GET under the same fence must also be OVERLOADED, not INTERNAL_ERROR.
    const auto fenced_get = probe->get("seed");
    GLIFI_REQUIRE(!fenced_get.has_value());
    GLIFI_REQUIRE(fenced_get.error().code == glifistore::ErrorCode::resource_exhausted);
    GLIFI_REQUIRE(fenced_get.error().category != "indeterminate");
    if (!fenced_get.error().retryability.empty()) {
        GLIFI_REQUIRE(fenced_get.error().retryability != "reconcile_first");
    }

    stall.release.store(true, std::memory_order_release);
    backup_thread.join();
    GLIFI_REQUIRE(!backup_error.has_value());
    GLIFI_REQUIRE(server.admissions_open());
    GLIFI_REQUIRE(server.ready());
    GLIFI_REQUIRE(glifistore::server::classify_ready_loss(server) ==
                  glifistore::server::ReadyLossReason::none);

    // After the fence lifts, the seeded key is readable again.
    auto after = glifistore::client::Client::connect({.port = server.port()});
    GLIFI_REQUIRE(after.has_value());
    const auto seed = after->get("seed");
    GLIFI_REQUIRE(seed.has_value());
    GLIFI_REQUIRE(std::string_view(reinterpret_cast<const char*>(seed->data()), seed->size()) == "v");

    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("Client::backup copies a live durable daemon catalog") {
    BackupTemporaryDirectory root;
    const auto source = root.path() / "source";
    const auto dest = root.path() / "client-backup";
    const auto restored = root.path() / "restored";

    auto opened =
        glifistore::server::Server::create({.port = 0, .maximum_connections = 4},
                                           {.worker_config = {.explicit_count = 1},
                                            .storage_mode = glifistore::StorageMode::durable_sync,
                                            .data_directory = source,
                                            .durable_open_mode = glifistore::DurableOpenMode::create_new});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    auto connected = glifistore::client::Client::connect({.port = server.port()});
    GLIFI_REQUIRE(connected.has_value());
    auto& client = *connected;
    GLIFI_REQUIRE(client.put("client-backup-key", "client-backup-value").committed());

    auto backed = client.backup(dest.string());
    GLIFI_REQUIRE(backed.has_value());
    const auto report = std::string_view{reinterpret_cast<const char*>(backed->data()), backed->size()};
    GLIFI_REQUIRE(report.find("status=ok") != std::string_view::npos);

    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());

    const auto restored_copy = glifistore::restore_durable_store(dest, restored);
    GLIFI_REQUIRE(restored_copy.has_value());
    auto reopened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = restored,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE(value_string(*(*reopened)->get("client-backup-key")) == "client-backup-value");
}

GLIFI_TEST("backup_durable_store copies multi-Worker catalogs with parallel Segment workers") {
    BackupTemporaryDirectory root;
    const auto source = root.path() / "source";
    const auto backup = root.path() / "backup";
    const auto restored = root.path() / "restored";

    {
        auto opened = glifistore::Store::open({
            .worker_config = {.explicit_count = 4},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = source,
            .durable_open_mode = glifistore::DurableOpenMode::create_new,
        });
        GLIFI_REQUIRE(opened.has_value());
        for (int i = 0; i < 64; ++i) {
            const auto key = "k" + std::to_string(i);
            GLIFI_REQUIRE((*opened)->put(key, bytes("v")).has_value());
        }
        GLIFI_REQUIRE((*opened)->close().has_value());
    }

    const auto backed = glifistore::backup_durable_store(source, backup);
    GLIFI_REQUIRE(backed.has_value());
    GLIFI_REQUIRE(backed->source_verification.segments.size() >= 2);
    GLIFI_REQUIRE(backed->segment_copy_workers >= 2);
    GLIFI_REQUIRE(backed->segment_copy_workers <= backed->source_verification.segments.size());
    GLIFI_REQUIRE(backed->files_copied == backed->source_verification.segments.size() + 1);
    GLIFI_REQUIRE(backed->destination_verification.segments.size() ==
                  backed->source_verification.segments.size());

    const auto restored_copy = glifistore::restore_durable_store(backup, restored);
    GLIFI_REQUIRE(restored_copy.has_value());
    auto reopened = glifistore::Store::open({
        .worker_config = {.explicit_count = 4},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = restored,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE(value_string(*(*reopened)->get("k0")) == "v");
    GLIFI_REQUIRE(value_string(*(*reopened)->get("k63")) == "v");
}

GLIFI_TEST("incomplete backup destination fails verify and leaves source healthy (HAZ-021)") {
    BackupTemporaryDirectory root;
    const auto source = root.path() / "source";
    const auto complete = root.path() / "complete";
    const auto incomplete = root.path() / "incomplete";
    const auto restored = root.path() / "restored";

    {
        auto opened = glifistore::Store::open({
            .worker_config = {.explicit_count = 1},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = source,
            .durable_open_mode = glifistore::DurableOpenMode::create_new,
        });
        GLIFI_REQUIRE(opened.has_value());
        GLIFI_REQUIRE((*opened)->put("keep", bytes("alive")).has_value());
        GLIFI_REQUIRE((*opened)->close().has_value());
    }

    const auto backed = glifistore::backup_durable_store(source, complete);
    GLIFI_REQUIRE(backed.has_value());

    std::error_code ec;
    std::filesystem::create_directories(incomplete, ec);
    GLIFI_REQUIRE(!ec);
    // Simulate crash mid-copy: Segment file present, Manifest missing.
    for (const auto& entry : std::filesystem::directory_iterator{complete}) {
        if (entry.path().filename() == glifistore::kManifestFilename) {
            continue;
        }
        std::filesystem::copy_file(entry.path(), incomplete / entry.path().filename(), ec);
        GLIFI_REQUIRE(!ec);
    }
    GLIFI_REQUIRE(!std::filesystem::exists(incomplete / glifistore::kManifestFilename));

    const auto verified = glifistore::verify_durable_store_path(incomplete);
    GLIFI_REQUIRE(!verified.has_value());

    const auto restore_refused = glifistore::restore_durable_store(incomplete, restored);
    GLIFI_REQUIRE(!restore_refused.has_value());

    auto reopened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = source,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE(value_string(*(*reopened)->get("keep")) == "alive");
}

GLIFI_TEST("failed online backup leaves the live Store usable (HAZ-021)") {
    BackupTemporaryDirectory root;
    const auto source = root.path() / "source";
    const auto bad_destination = root.path() / "not-a-directory";

    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = source,
        .durable_open_mode = glifistore::DurableOpenMode::create_new,
    });
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(store.put("before", bytes("1")).has_value());

    {
        std::ofstream blocker{bad_destination};
        blocker << "occupied";
        GLIFI_REQUIRE(static_cast<bool>(blocker));
    }

    const auto failed = store.backup_to(bad_destination);
    GLIFI_REQUIRE(!failed.has_value());

    GLIFI_REQUIRE(store.put("after", bytes("2")).has_value());
    GLIFI_REQUIRE(value_string(*store.get("before")) == "1");
    GLIFI_REQUIRE(value_string(*store.get("after")) == "2");
    GLIFI_REQUIRE(store.close().has_value());
}

GLIFI_TEST("future Manifest version fails closed on verify restore and open (HAZ-022)") {
    BackupTemporaryDirectory root;
    const auto source = root.path() / "source";
    const auto backup = root.path() / "backup";
    const auto future = root.path() / "future";
    const auto restored = root.path() / "restored";

    seed_single_worker_store(source);
    const auto backed = glifistore::backup_durable_store(source, backup);
    GLIFI_REQUIRE(backed.has_value());

    std::error_code ec;
    std::filesystem::create_directories(future, ec);
    GLIFI_REQUIRE(!ec);
    for (const auto& entry : std::filesystem::directory_iterator{backup}) {
        std::filesystem::copy_file(entry.path(), future / entry.path().filename(), ec);
        GLIFI_REQUIRE(!ec);
    }

    const auto manifest_path = future / glifistore::kManifestFilename;
    auto manifest_bytes = read_file_bytes(manifest_path);
    GLIFI_REQUIRE(manifest_bytes.size() >= glifistore::kManifestHeaderBytes);
    // Correctly checksummed future Manifest format version (STORE-FUTURE-REQUIRED).
    put_u16(manifest_bytes, 4, 2);
    refresh_manifest_checksum(manifest_bytes);
    write_file_bytes(manifest_path, manifest_bytes);

    const auto verified = glifistore::verify_durable_store_path(future);
    GLIFI_REQUIRE(!verified.has_value());
    GLIFI_REQUIRE(verified.error().code == glifistore::ErrorCode::invalid_record ||
                  verified.error().code == glifistore::ErrorCode::corrupted_data);

    const auto restore_refused = glifistore::restore_durable_store(future, restored);
    GLIFI_REQUIRE(!restore_refused.has_value());

    auto opened_future = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = future,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    GLIFI_REQUIRE(!opened_future.has_value());

    auto reopened_source = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = source,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    GLIFI_REQUIRE(reopened_source.has_value());
    GLIFI_REQUIRE(value_string(*(*reopened_source)->get("keep")) == "alive");
}

GLIFI_TEST("future pinned Record version in Manifest fails closed (HAZ-022)") {
    BackupTemporaryDirectory root;
    const auto source = root.path() / "source";
    const auto backup = root.path() / "backup";
    const auto future = root.path() / "future-record";
    const auto restored = root.path() / "restored";

    seed_single_worker_store(source);
    GLIFI_REQUIRE(glifistore::backup_durable_store(source, backup).has_value());

    std::error_code ec;
    std::filesystem::create_directories(future, ec);
    GLIFI_REQUIRE(!ec);
    for (const auto& entry : std::filesystem::directory_iterator{backup}) {
        std::filesystem::copy_file(entry.path(), future / entry.path().filename(), ec);
        GLIFI_REQUIRE(!ec);
    }

    const auto manifest_path = future / glifistore::kManifestFilename;
    auto manifest_bytes = read_file_bytes(manifest_path);
    // Manifest pins Record format at offset 62; bump while keeping checksum valid.
    put_u16(manifest_bytes, 62, 2);
    refresh_manifest_checksum(manifest_bytes);
    write_file_bytes(manifest_path, manifest_bytes);

    GLIFI_REQUIRE(!glifistore::verify_durable_store_path(future).has_value());
    GLIFI_REQUIRE(!glifistore::restore_durable_store(future, restored).has_value());
    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = future,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    GLIFI_REQUIRE(!opened.has_value());
}

GLIFI_TEST("truncated Manifest backup refuses restore and open (HAZ-022)") {
    BackupTemporaryDirectory root;
    const auto source = root.path() / "source";
    const auto backup = root.path() / "backup";
    const auto truncated = root.path() / "truncated";
    const auto restored = root.path() / "restored";

    seed_single_worker_store(source);
    GLIFI_REQUIRE(glifistore::backup_durable_store(source, backup).has_value());

    std::error_code ec;
    std::filesystem::create_directories(truncated, ec);
    GLIFI_REQUIRE(!ec);
    for (const auto& entry : std::filesystem::directory_iterator{backup}) {
        std::filesystem::copy_file(entry.path(), truncated / entry.path().filename(), ec);
        GLIFI_REQUIRE(!ec);
    }

    const auto manifest_path = truncated / glifistore::kManifestFilename;
    auto manifest_bytes = read_file_bytes(manifest_path);
    GLIFI_REQUIRE(manifest_bytes.size() > glifistore::kManifestHeaderBytes / 2);
    manifest_bytes.resize(glifistore::kManifestHeaderBytes / 2);
    write_file_bytes(manifest_path, manifest_bytes);

    GLIFI_REQUIRE(!glifistore::verify_durable_store_path(truncated).has_value());
    GLIFI_REQUIRE(!glifistore::restore_durable_store(truncated, restored).has_value());
    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = truncated,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    GLIFI_REQUIRE(!opened.has_value());
}

GLIFI_TEST("restored Store refuses mismatched Worker count (HAZ-022)") {
    BackupTemporaryDirectory root;
    const auto source = root.path() / "source";
    const auto backup = root.path() / "backup";
    const auto restored = root.path() / "restored";

    seed_single_worker_store(source);
    GLIFI_REQUIRE(glifistore::backup_durable_store(source, backup).has_value());
    GLIFI_REQUIRE(glifistore::restore_durable_store(backup, restored).has_value());

    auto wrong_count = glifistore::Store::open({
        .worker_config = {.explicit_count = 2},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = restored,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    GLIFI_REQUIRE(!wrong_count.has_value());
    GLIFI_REQUIRE(wrong_count.error().code == glifistore::ErrorCode::invalid_argument);

    auto matching = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = restored,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    GLIFI_REQUIRE(matching.has_value());
    GLIFI_REQUIRE(value_string(*(*matching)->get("keep")) == "alive");
}

GLIFI_TEST("online backup storage_exhausted matrix leaves incomplete dest and healthy source") {
    // GS-PERSIST-FAULT-001 / HAZ-016: ENOSPC-class faults at backup copy/sync seams must
    // fail closed without poisoning the live source catalog.
    struct OneShotBackupFailure final {
        glifistore::FilesystemOperation target{};
        bool fired{};

        static auto before(void* opaque, const glifistore::FilesystemOperation operation)
            -> glifistore::Status {
            auto& state = *static_cast<OneShotBackupFailure*>(opaque);
            if (!state.fired && operation == state.target) {
                state.fired = true;
                return glifistore::fail(glifistore::ErrorCode::storage_exhausted,
                                        "injected online backup storage_exhausted");
            }
            return {};
        }
    };

    const std::array seams{
        glifistore::FilesystemOperation::copy_backup_segment,
        glifistore::FilesystemOperation::copy_backup_manifest,
        glifistore::FilesystemOperation::sync_backup_destination,
    };

    for (const auto seam : seams) {
        BackupTemporaryDirectory root;
        const auto source = root.path() / "source";
        const auto dest = root.path() / "dest";

        OneShotBackupFailure failure{.target = seam};
        auto opened = glifistore::Store::open({
            .worker_config = {.explicit_count = 1},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = source,
            .durable_open_mode = glifistore::DurableOpenMode::create_new,
            .filesystem_hooks = {.context = &failure, .before = &OneShotBackupFailure::before},
        });
        GLIFI_REQUIRE(opened.has_value());
        auto& store = **opened;
        GLIFI_REQUIRE(store.put("keep", bytes("alive")).has_value());

        const auto backed = store.backup_to(dest);
        GLIFI_REQUIRE(failure.fired);
        GLIFI_REQUIRE(!backed.has_value());
        GLIFI_REQUIRE(backed.error().code == glifistore::ErrorCode::storage_exhausted);

        GLIFI_REQUIRE(store.put("after", bytes("ok")).has_value());
        GLIFI_REQUIRE(value_string(*store.get("keep")) == "alive");
        GLIFI_REQUIRE(value_string(*store.get("after")) == "ok");

        // Failed backup must not be treated as a verified destination (incomplete or
        // unsynced). Segment/manifest faults leave an incomplete catalog; sync fault
        // still failed the promotion API even if bytes were copied.
        if (seam != glifistore::FilesystemOperation::sync_backup_destination) {
            GLIFI_REQUIRE(!glifistore::verify_durable_store_path(dest).has_value());
        } else {
            GLIFI_REQUIRE(std::filesystem::exists(dest));
        }
        GLIFI_REQUIRE(store.close().has_value());
    }
}

GLIFI_TEST("fenced backup concurrent with mutation and compaction keeps source healthy") {
    // Wave 3: while online backup holds the admission fence, put and compact must fail;
    // after release the backup verifies and restore recovers committed keys.
    BackupTemporaryDirectory root;
    const auto source = root.path() / "source";
    const auto backup = root.path() / "backup";
    const auto restored = root.path() / "restored";

    struct StallBackup final {
        std::atomic_bool entered{false};
        std::atomic_bool release{false};

        static auto before(void* opaque, const glifistore::FilesystemOperation operation)
            -> glifistore::Status {
            auto& probe = *static_cast<StallBackup*>(opaque);
            if (operation == glifistore::FilesystemOperation::copy_backup_segment) {
                probe.entered.store(true, std::memory_order_release);
                while (!probe.release.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
            }
            return {};
        }
    } stall;

    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = source,
        .durable_open_mode = glifistore::DurableOpenMode::create_new,
        .filesystem_hooks = {.context = &stall, .before = &StallBackup::before},
    });
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(store.put("seed", bytes("v")).has_value());

    std::error_code ec;
    std::filesystem::create_directories(backup, ec);
    GLIFI_REQUIRE(!ec);

    std::optional<glifistore::Error> backup_error;
    std::thread backup_thread{[&] {
        auto backed = store.backup_to(backup / "fenced");
        if (!backed) {
            backup_error = backed.error();
        }
    }};
    while (!stall.entered.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    const auto fenced_put = store.put("during-fence", bytes("no"));
    GLIFI_REQUIRE(!fenced_put.has_value());
    const auto fenced_compact = store.compact();
    GLIFI_REQUIRE(!fenced_compact.has_value());

    stall.release.store(true, std::memory_order_release);
    backup_thread.join();
    GLIFI_REQUIRE(!backup_error.has_value());

    GLIFI_REQUIRE(store.put("after-fence", bytes("yes")).has_value());
    GLIFI_REQUIRE(value_string(*store.get("seed")) == "v");
    GLIFI_REQUIRE(value_string(*store.get("after-fence")) == "yes");
    GLIFI_REQUIRE(!store.get("during-fence").has_value());
    GLIFI_REQUIRE(store.close().has_value());

    GLIFI_REQUIRE(glifistore::verify_durable_store_path(backup / "fenced").has_value());
    GLIFI_REQUIRE(glifistore::restore_durable_store(backup / "fenced", restored).has_value());
    auto reopened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = restored,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE(value_string(*(*reopened)->get("seed")) == "v");
}

namespace {

struct BackupFragmentedCopyIo {
    bool armed{};
    std::size_t write_calls{};
    std::size_t remaining_eintr{3};
    std::size_t maximum_chunk{32};

    static auto read_some_at(void*, const int descriptor, const std::span<std::byte> bytes,
                             const std::uint64_t offset) -> std::ptrdiff_t {
        return static_cast<std::ptrdiff_t>(
            ::pread(descriptor, bytes.data(), bytes.size(), static_cast<off_t>(offset)));
    }

    static auto write_some_at(void* context, const int descriptor, const std::span<const std::byte> bytes,
                              const std::uint64_t offset) -> std::ptrdiff_t {
        auto& io = *static_cast<BackupFragmentedCopyIo*>(context);
        if (!io.armed) {
            return static_cast<std::ptrdiff_t>(
                ::pwrite(descriptor, bytes.data(), bytes.size(), static_cast<off_t>(offset)));
        }
        ++io.write_calls;
        if (io.remaining_eintr > 0) {
            --io.remaining_eintr;
            errno = EINTR;
            return -1;
        }
        const auto count = std::min(bytes.size(), io.maximum_chunk);
        return static_cast<std::ptrdiff_t>(
            ::pwrite(descriptor, bytes.data(), count, static_cast<off_t>(offset)));
    }
};

struct BackupCapacityCopyIo {
    bool armed{};
    bool fired{};
    int error_number{ENOSPC};

    static auto read_some_at(void*, const int descriptor, const std::span<std::byte> bytes,
                             const std::uint64_t offset) -> std::ptrdiff_t {
        return static_cast<std::ptrdiff_t>(
            ::pread(descriptor, bytes.data(), bytes.size(), static_cast<off_t>(offset)));
    }

    static auto write_some_at(void* context, const int descriptor, const std::span<const std::byte> bytes,
                              const std::uint64_t offset) -> std::ptrdiff_t {
        auto& io = *static_cast<BackupCapacityCopyIo*>(context);
        if (!io.armed) {
            return static_cast<std::ptrdiff_t>(
                ::pwrite(descriptor, bytes.data(), bytes.size(), static_cast<off_t>(offset)));
        }
        io.fired = true;
        errno = io.error_number;
        return -1;
    }
};

struct BackupSyncEioIo {
    bool armed{};
    bool fired{};

    static auto read_some_at(void*, const int descriptor, const std::span<std::byte> bytes,
                             const std::uint64_t offset) -> std::ptrdiff_t {
        return static_cast<std::ptrdiff_t>(
            ::pread(descriptor, bytes.data(), bytes.size(), static_cast<off_t>(offset)));
    }

    static auto write_some_at(void*, const int descriptor, const std::span<const std::byte> bytes,
                              const std::uint64_t offset) -> std::ptrdiff_t {
        return static_cast<std::ptrdiff_t>(
            ::pwrite(descriptor, bytes.data(), bytes.size(), static_cast<off_t>(offset)));
    }

    static auto sync_file(void* context, const int descriptor, const glifistore::FileSyncMode) -> int {
        auto& io = *static_cast<BackupSyncEioIo*>(context);
        if (!io.armed) {
            return ::fsync(descriptor);
        }
        io.fired = true;
        errno = EIO;
        return -1;
    }
};

struct BackupSyncEintrIo {
    bool armed{};
    std::size_t sync_calls{};
    std::size_t remaining_eintr{3};

    static auto read_some_at(void*, const int descriptor, const std::span<std::byte> bytes,
                             const std::uint64_t offset) -> std::ptrdiff_t {
        return static_cast<std::ptrdiff_t>(
            ::pread(descriptor, bytes.data(), bytes.size(), static_cast<off_t>(offset)));
    }

    static auto write_some_at(void*, const int descriptor, const std::span<const std::byte> bytes,
                              const std::uint64_t offset) -> std::ptrdiff_t {
        return static_cast<std::ptrdiff_t>(
            ::pwrite(descriptor, bytes.data(), bytes.size(), static_cast<off_t>(offset)));
    }

    static auto sync_file(void* context, const int descriptor, const glifistore::FileSyncMode) -> int {
        auto& io = *static_cast<BackupSyncEintrIo*>(context);
        if (!io.armed) {
            return ::fsync(descriptor);
        }
        ++io.sync_calls;
        if (io.remaining_eintr > 0) {
            --io.remaining_eintr;
            errno = EINTR;
            return -1;
        }
        return ::fsync(descriptor);
    }
};

} // namespace

// GS-PERSIST-FAULT-001 / Wave 3 L4: backup byte-copy loop uses FileIoHooks (positional)
// and retries EINTR / short pwrite while producing a verified destination.
GLIFI_TEST("online backup copy loop retries EINTR and short writes via FileIoHooks") {
    BackupTemporaryDirectory root;
    const auto source = root.path() / "source";
    const auto dest = root.path() / "dest";

    BackupFragmentedCopyIo io{};
    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = source,
        .durable_open_mode = glifistore::DurableOpenMode::create_new,
        .filesystem_hooks = {.file_io = {.context = &io,
                                         .read_some_at = &BackupFragmentedCopyIo::read_some_at,
                                         .write_some_at = &BackupFragmentedCopyIo::write_some_at}},
    });
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(store.put("keep", bytes("alive")).has_value());

    io.armed = true;
    const auto backed = store.backup_to(dest);
    GLIFI_REQUIRE(backed.has_value());
    GLIFI_REQUIRE(io.write_calls > 0);
    GLIFI_REQUIRE(io.remaining_eintr == 0);
    GLIFI_REQUIRE(glifistore::verify_durable_store_path(dest).has_value());
    GLIFI_REQUIRE(store.put("after", bytes("ok")).has_value());
    GLIFI_REQUIRE(value_string(*store.get("keep")) == "alive");
    GLIFI_REQUIRE(store.close().has_value());
}

// GS-PERSIST-FAULT-001 / Wave 3 L4: backup file sync retries EINTR via FileIoHooks and
// still produces a verified destination.
GLIFI_TEST("online backup FileIoHooks sync EINTR retries leave verified dest") {
    BackupTemporaryDirectory root;
    const auto source = root.path() / "source";
    const auto dest = root.path() / "dest";

    BackupSyncEintrIo io{};
    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = source,
        .durable_open_mode = glifistore::DurableOpenMode::create_new,
        .filesystem_hooks = {.file_io = {.context = &io,
                                         .read_some_at = &BackupSyncEintrIo::read_some_at,
                                         .write_some_at = &BackupSyncEintrIo::write_some_at,
                                         .sync_file = &BackupSyncEintrIo::sync_file}},
    });
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(store.put("keep", bytes("alive")).has_value());

    io.armed = true;
    const auto backed = store.backup_to(dest);
    GLIFI_REQUIRE(backed.has_value());
    GLIFI_REQUIRE(io.sync_calls > 0);
    GLIFI_REQUIRE(io.remaining_eintr == 0);
    GLIFI_REQUIRE(glifistore::verify_durable_store_path(dest).has_value());
    GLIFI_REQUIRE(store.put("after", bytes("ok")).has_value());
    GLIFI_REQUIRE(value_string(*store.get("keep")) == "alive");
    GLIFI_REQUIRE(store.close().has_value());
}

// GS-PERSIST-FAULT-001 / Wave 3 L4: capacity errno from FileIoHooks during backup copy
// maps to storage_exhausted without poisoning the live source.
GLIFI_TEST("online backup FileIoHooks capacity faults leave source healthy") {
    struct Case {
        int error_number;
    };

    std::vector<Case> cases{{ENOSPC}};
#if defined(EDQUOT)
    cases.push_back({EDQUOT});
#endif
    for (const auto& fault : cases) {
        BackupTemporaryDirectory root;
        const auto source = root.path() / "source";
        const auto dest = root.path() / "dest";

        BackupCapacityCopyIo io{.error_number = fault.error_number};
        auto opened = glifistore::Store::open({
            .worker_config = {.explicit_count = 1},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = source,
            .durable_open_mode = glifistore::DurableOpenMode::create_new,
            .filesystem_hooks = {.file_io = {.context = &io,
                                             .read_some_at = &BackupCapacityCopyIo::read_some_at,
                                             .write_some_at = &BackupCapacityCopyIo::write_some_at}},
        });
        GLIFI_REQUIRE(opened.has_value());
        auto& store = **opened;
        GLIFI_REQUIRE(store.put("keep", bytes("alive")).has_value());

        io.armed = true;
        const auto backed = store.backup_to(dest);
        io.armed = false;
        GLIFI_REQUIRE(io.fired);
        GLIFI_REQUIRE(!backed.has_value());
        GLIFI_REQUIRE(backed.error().code == glifistore::ErrorCode::storage_exhausted);
        GLIFI_REQUIRE(store.put("after", bytes("ok")).has_value());
        GLIFI_REQUIRE(value_string(*store.get("keep")) == "alive");
        GLIFI_REQUIRE(value_string(*store.get("after")) == "ok");
        GLIFI_REQUIRE(!glifistore::verify_durable_store_path(dest).has_value());
        GLIFI_REQUIRE(store.close().has_value());
    }
}

// GS-PERSIST-FAULT-001 / Wave 3 L4: write-path EIO / EROFS from FileIoHooks during backup
// copy leave an incomplete dest and a healthy source.
GLIFI_TEST("online backup FileIoHooks write EIO and EROFS leave source healthy") {
    struct Case {
        int error_number;
        glifistore::ErrorCode expected;
    };

    const std::array cases{
        Case{EIO, glifistore::ErrorCode::io_error},
        Case{EROFS, glifistore::ErrorCode::read_only_filesystem},
    };
    for (const auto& fault : cases) {
        BackupTemporaryDirectory root;
        const auto source = root.path() / "source";
        const auto dest = root.path() / "dest";

        BackupCapacityCopyIo io{.error_number = fault.error_number};
        auto opened = glifistore::Store::open({
            .worker_config = {.explicit_count = 1},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = source,
            .durable_open_mode = glifistore::DurableOpenMode::create_new,
            .filesystem_hooks = {.file_io = {.context = &io,
                                             .read_some_at = &BackupCapacityCopyIo::read_some_at,
                                             .write_some_at = &BackupCapacityCopyIo::write_some_at}},
        });
        GLIFI_REQUIRE(opened.has_value());
        auto& store = **opened;
        GLIFI_REQUIRE(store.put("keep", bytes("alive")).has_value());

        io.armed = true;
        const auto backed = store.backup_to(dest);
        io.armed = false;
        GLIFI_REQUIRE(io.fired);
        GLIFI_REQUIRE(!backed.has_value());
        GLIFI_REQUIRE(backed.error().code == fault.expected);
        GLIFI_REQUIRE(store.put("after", bytes("ok")).has_value());
        GLIFI_REQUIRE(value_string(*store.get("keep")) == "alive");
        GLIFI_REQUIRE(value_string(*store.get("after")) == "ok");
        GLIFI_REQUIRE(!glifistore::verify_durable_store_path(dest).has_value());
        GLIFI_REQUIRE(store.close().has_value());
    }
}

// GS-PERSIST-FAULT-001 / Wave 3 L4: delayed-writeback EIO from FileIoHooks during backup
// file sync maps to io_error without poisoning the live source.
GLIFI_TEST("online backup FileIoHooks sync EIO leaves source healthy") {
    BackupTemporaryDirectory root;
    const auto source = root.path() / "source";
    const auto dest = root.path() / "dest";

    BackupSyncEioIo io{};
    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = source,
        .durable_open_mode = glifistore::DurableOpenMode::create_new,
        .filesystem_hooks = {.file_io = {.context = &io,
                                         .read_some_at = &BackupSyncEioIo::read_some_at,
                                         .write_some_at = &BackupSyncEioIo::write_some_at,
                                         .sync_file = &BackupSyncEioIo::sync_file}},
    });
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(store.put("keep", bytes("alive")).has_value());

    io.armed = true;
    const auto backed = store.backup_to(dest);
    io.armed = false;
    GLIFI_REQUIRE(io.fired);
    GLIFI_REQUIRE(!backed.has_value());
    GLIFI_REQUIRE(backed.error().code == glifistore::ErrorCode::io_error);
    GLIFI_REQUIRE(store.put("after", bytes("ok")).has_value());
    GLIFI_REQUIRE(value_string(*store.get("keep")) == "alive");
    GLIFI_REQUIRE(value_string(*store.get("after")) == "ok");
    GLIFI_REQUIRE(!glifistore::verify_durable_store_path(dest).has_value());
    GLIFI_REQUIRE(store.close().has_value());
}
