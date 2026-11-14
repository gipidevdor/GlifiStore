#include "persistence_recovery_test_support.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

// FileIoHooks / GS-PERSIST-FAULT-001 online compaction seams (split for structure budget).
namespace {

// FileIoHooks for compaction staging: EINTR then short pwrite completions (GS-PERSIST-FAULT-001).
struct CompactionFragmentedWriteIo {
    bool armed{};
    std::size_t write_calls{};
    std::size_t remaining_eintr{3};
    std::size_t maximum_chunk{16};

    static auto read_some_at(void*, const int descriptor, const std::span<std::byte> bytes,
                             const std::uint64_t offset) -> std::ptrdiff_t {
        return static_cast<std::ptrdiff_t>(
            ::pread(descriptor, bytes.data(), bytes.size(), static_cast<off_t>(offset)));
    }

    static auto write_some_at(void* context, const int descriptor, const std::span<const std::byte> bytes,
                              const std::uint64_t offset) -> std::ptrdiff_t {
        auto& io = *static_cast<CompactionFragmentedWriteIo*>(context);
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

struct CompactionCapacityWriteIo {
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
        auto& io = *static_cast<CompactionCapacityWriteIo*>(context);
        if (!io.armed) {
            return static_cast<std::ptrdiff_t>(
                ::pwrite(descriptor, bytes.data(), bytes.size(), static_cast<off_t>(offset)));
        }
        io.fired = true;
        errno = io.error_number;
        return -1;
    }
};

// Delayed-writeback class: staging writes succeed; sync reports EIO (GS-PERSIST-FAULT-001).
struct CompactionSyncEioIo {
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
        auto& io = *static_cast<CompactionSyncEioIo*>(context);
        if (!io.armed) {
            return ::fsync(descriptor);
        }
        io.fired = true;
        errno = EIO;
        return -1;
    }
};

// Sync EINTR must retry inside FileDescriptor::sync before staging can publish.
struct CompactionSyncEintrIo {
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
        auto& io = *static_cast<CompactionSyncEintrIo*>(context);
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

auto seed_two_sealed_compaction_fixture(const std::filesystem::path& path,
                                        const glifistore::StoreId& store_id, const std::string_view first_key,
                                        const std::string_view first_value, const std::string_view second_key,
                                        const std::string_view second_value)
    -> std::vector<glifistore::ManifestSegmentEntry> {
    const std::vector entries{
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{1},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::sealed},
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{2},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::sealed},
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{3},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::active},
    };
    auto directory = glifistore::DataDirectory::open_and_lock(path);
    GLIFI_REQUIRE(directory.has_value());
    auto first = create_segment(*directory, store_id, entries[0]);
    append_record(first, 1, first_key, first_value);
    GLIFI_REQUIRE(first.seal().committed());
    auto second = create_segment(*directory, store_id, entries[1]);
    append_record(second, 2, second_key, second_value);
    GLIFI_REQUIRE(second.seal().committed());
    static_cast<void>(create_segment(*directory, store_id, entries[2]));
    GLIFI_REQUIRE(directory->publish_manifest(recovery_manifest(store_id, 1, entries)).durable());
    return entries;
}

} // namespace

// GS-PERSIST-FAULT-001 / Wave 3 L4: compaction staging retries EINTR and completes short
// pwrite transfers end-to-end (FileIoHooks), then publishes cleanly. E0–E2 only.
GLIFI_TEST("online compaction staging retries EINTR and short writes before intent") {
    RecoveryTemporaryDirectory temporary;
    const auto store_id = recovery_store_id();
    const auto entries =
        seed_two_sealed_compaction_fixture(temporary.path(), store_id, "eintr-a", "alpha", "eintr-b", "beta");

    CompactionFragmentedWriteIo io{};
    auto directory = glifistore::DataDirectory::open_and_lock(
        temporary.path(), glifistore::FilesystemHooks{
                              .file_io = {.context = &io,
                                          .read_some_at = &CompactionFragmentedWriteIo::read_some_at,
                                          .write_some_at = &CompactionFragmentedWriteIo::write_some_at}});
    GLIFI_REQUIRE(directory.has_value());
    auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory));
    GLIFI_REQUIRE(runtime.has_value());
    io.armed = true;
    const auto result = (*runtime)->compact_worker(0, 0);
    GLIFI_REQUIRE(result.compacted());
    GLIFI_REQUIRE(io.write_calls > 0);
    GLIFI_REQUIRE(io.remaining_eintr == 0);
    GLIFI_REQUIRE((*runtime)->healthy());
    GLIFI_REQUIRE((*runtime)->manifest().manifest_generation == 2);
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
    GLIFI_REQUIRE(owned_text(*(*runtime)->get("eintr-a")) == "alpha");
    GLIFI_REQUIRE(owned_text(*(*runtime)->get("eintr-b")) == "beta");
    runtime->reset();

    auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE((*reopened)->namespace_audit().clean());
    GLIFI_REQUIRE(owned_text(*(*reopened)->get("eintr-a")) == "alpha");
    GLIFI_REQUIRE(owned_text(*(*reopened)->get("eintr-b")) == "beta");
    GLIFI_REQUIRE(entries.size() == 3);
}

// GS-PERSIST-FAULT-001 / Wave 3 L4: FileDescriptor::sync retries EINTR from FileIoHooks
// during pre-intent staging, then publishes cleanly. E0–E2 only.
GLIFI_TEST("online compaction staging retries sync EINTR before intent") {
    RecoveryTemporaryDirectory temporary;
    const auto store_id = recovery_store_id();
    const auto entries =
        seed_two_sealed_compaction_fixture(temporary.path(), store_id, "sync-a", "alpha", "sync-b", "beta");

    CompactionSyncEintrIo io{};
    auto directory = glifistore::DataDirectory::open_and_lock(
        temporary.path(),
        glifistore::FilesystemHooks{.file_io = {.context = &io,
                                                .read_some_at = &CompactionSyncEintrIo::read_some_at,
                                                .write_some_at = &CompactionSyncEintrIo::write_some_at,
                                                .sync_file = &CompactionSyncEintrIo::sync_file}});
    GLIFI_REQUIRE(directory.has_value());
    auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory));
    GLIFI_REQUIRE(runtime.has_value());
    io.armed = true;
    const auto result = (*runtime)->compact_worker(0, 0);
    GLIFI_REQUIRE(result.compacted());
    GLIFI_REQUIRE(io.sync_calls > 0);
    GLIFI_REQUIRE(io.remaining_eintr == 0);
    GLIFI_REQUIRE((*runtime)->healthy());
    GLIFI_REQUIRE((*runtime)->manifest().manifest_generation == 2);
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
    GLIFI_REQUIRE(owned_text(*(*runtime)->get("sync-a")) == "alpha");
    GLIFI_REQUIRE(owned_text(*(*runtime)->get("sync-b")) == "beta");
    runtime->reset();

    auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE((*reopened)->namespace_audit().clean());
    GLIFI_REQUIRE(owned_text(*(*reopened)->get("sync-a")) == "alpha");
    GLIFI_REQUIRE(owned_text(*(*reopened)->get("sync-b")) == "beta");
}

// GS-PERSIST-FAULT-001 / Wave 3 L4: capacity errno from FileIoHooks during pre-intent
// compaction staging maps to storage_exhausted; Mold remains sole authority.
GLIFI_TEST("online compaction FileIoHooks capacity faults reject before intent") {
    struct Case {
        int error_number;
        const char* label;
    };

    std::vector<Case> cases{{ENOSPC, "enospc"}};
#if defined(EDQUOT)
    cases.push_back({EDQUOT, "edquot"});
#endif
    for (const auto& fault : cases) {
        RecoveryTemporaryDirectory temporary;
        const auto store_id = recovery_store_id();
        const auto first_key = std::string{fault.label} + "-first";
        const auto second_key = std::string{fault.label} + "-second";
        const auto entries = seed_two_sealed_compaction_fixture(temporary.path(), store_id, first_key,
                                                                "first-value", second_key, "second-value");
        const auto old = recovery_manifest(store_id, 1, entries);

        CompactionCapacityWriteIo io{.error_number = fault.error_number};
        auto directory = glifistore::DataDirectory::open_and_lock(
            temporary.path(), glifistore::FilesystemHooks{
                                  .file_io = {.context = &io,
                                              .read_some_at = &CompactionCapacityWriteIo::read_some_at,
                                              .write_some_at = &CompactionCapacityWriteIo::write_some_at}});
        GLIFI_REQUIRE(directory.has_value());
        auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory));
        GLIFI_REQUIRE(runtime.has_value());
        io.armed = true;
        const auto result = (*runtime)->compact_worker(0, 0);
        GLIFI_REQUIRE(io.fired);
        GLIFI_REQUIRE(!result.compacted());
        GLIFI_REQUIRE(result.error.has_value());
        GLIFI_REQUIRE(result.error->code == glifistore::ErrorCode::storage_exhausted);
        GLIFI_REQUIRE(result.outcome == glifistore::DurableCompactionOutcome::not_compacted);
        GLIFI_REQUIRE((*runtime)->healthy());
        GLIFI_REQUIRE((*runtime)->manifest() == old);
        GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
        runtime->reset();

        auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
        GLIFI_REQUIRE(reopened.has_value());
        GLIFI_REQUIRE((*reopened)->namespace_audit().clean());
        GLIFI_REQUIRE(owned_text(*(*reopened)->get(first_key)) == "first-value");
        GLIFI_REQUIRE(owned_text(*(*reopened)->get(second_key)) == "second-value");
    }
}

// GS-PERSIST-FAULT-001 / Wave 3 L4: write-path EIO / EROFS from FileIoHooks during
// pre-intent staging reject before intent; Mold remains sole authority. E0–E2 only.
GLIFI_TEST("online compaction FileIoHooks write EIO and EROFS reject before intent") {
    struct Case {
        int error_number;
        glifistore::ErrorCode expected;
        const char* label;
    };

    const std::array cases{
        Case{EIO, glifistore::ErrorCode::io_error, "eio"},
        Case{EROFS, glifistore::ErrorCode::read_only_filesystem, "erofs"},
    };
    for (const auto& fault : cases) {
        RecoveryTemporaryDirectory temporary;
        const auto store_id = recovery_store_id();
        const auto first_key = std::string{fault.label} + "-first";
        const auto second_key = std::string{fault.label} + "-second";
        const auto entries = seed_two_sealed_compaction_fixture(temporary.path(), store_id, first_key,
                                                                "first-value", second_key, "second-value");
        const auto old = recovery_manifest(store_id, 1, entries);

        CompactionCapacityWriteIo io{.error_number = fault.error_number};
        auto directory = glifistore::DataDirectory::open_and_lock(
            temporary.path(), glifistore::FilesystemHooks{
                                  .file_io = {.context = &io,
                                              .read_some_at = &CompactionCapacityWriteIo::read_some_at,
                                              .write_some_at = &CompactionCapacityWriteIo::write_some_at}});
        GLIFI_REQUIRE(directory.has_value());
        auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory));
        GLIFI_REQUIRE(runtime.has_value());
        io.armed = true;
        const auto result = (*runtime)->compact_worker(0, 0);
        GLIFI_REQUIRE(io.fired);
        GLIFI_REQUIRE(!result.compacted());
        GLIFI_REQUIRE(result.error.has_value());
        GLIFI_REQUIRE(result.error->code == fault.expected);
        GLIFI_REQUIRE(result.outcome == glifistore::DurableCompactionOutcome::not_compacted);
        GLIFI_REQUIRE((*runtime)->healthy());
        GLIFI_REQUIRE((*runtime)->manifest() == old);
        GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
        runtime->reset();

        auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
        GLIFI_REQUIRE(reopened.has_value());
        GLIFI_REQUIRE((*reopened)->namespace_audit().clean());
        GLIFI_REQUIRE(owned_text(*(*reopened)->get(first_key)) == "first-value");
        GLIFI_REQUIRE(owned_text(*(*reopened)->get(second_key)) == "second-value");
    }
}

// GS-PERSIST-FAULT-001 / Wave 3 L4: FileIoHooks sync EIO during pre-intent staging maps to
// io_error; Mold remains sole authority and no intent residue remains. E0–E2 only.
GLIFI_TEST("online compaction FileIoHooks sync EIO rejects before intent") {
    RecoveryTemporaryDirectory temporary;
    const auto store_id = recovery_store_id();
    const auto entries =
        seed_two_sealed_compaction_fixture(temporary.path(), store_id, "eio-a", "alpha", "eio-b", "beta");
    const auto old = recovery_manifest(store_id, 1, entries);

    CompactionSyncEioIo io{};
    auto directory = glifistore::DataDirectory::open_and_lock(
        temporary.path(),
        glifistore::FilesystemHooks{.file_io = {.context = &io,
                                                .read_some_at = &CompactionSyncEioIo::read_some_at,
                                                .write_some_at = &CompactionSyncEioIo::write_some_at,
                                                .sync_file = &CompactionSyncEioIo::sync_file}});
    GLIFI_REQUIRE(directory.has_value());
    auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory));
    GLIFI_REQUIRE(runtime.has_value());
    io.armed = true;
    const auto result = (*runtime)->compact_worker(0, 0);
    GLIFI_REQUIRE(io.fired);
    GLIFI_REQUIRE(!result.compacted());
    GLIFI_REQUIRE(result.error.has_value());
    GLIFI_REQUIRE(result.error->code == glifistore::ErrorCode::io_error);
    GLIFI_REQUIRE(result.outcome == glifistore::DurableCompactionOutcome::not_compacted);
    GLIFI_REQUIRE((*runtime)->healthy());
    GLIFI_REQUIRE((*runtime)->manifest() == old);
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
    runtime->reset();

    auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE((*reopened)->namespace_audit().clean());
    GLIFI_REQUIRE(owned_text(*(*reopened)->get("eio-a")) == "alpha");
    GLIFI_REQUIRE(owned_text(*(*reopened)->get("eio-b")) == "beta");
}

// GS-PERSIST-FAULT-001 / Wave 3 L4: FileIoHooks faults on compaction intent write/sync
// (after successful staging) reject without publishing intent; Mold remains sole authority.
GLIFI_TEST("online compaction FileIoHooks intent write and sync faults reject cleanly") {
    struct Case {
        enum class Mode { write, sync } mode;
        int write_error{EIO};
        glifistore::ErrorCode expected{glifistore::ErrorCode::io_error};
        const char* label;
    };

    struct IntentFault final {
        Case::Mode mode{};
        int write_error{EIO};
        bool arm_write{};
        bool arm_sync{};
        bool fired{};

        static auto before(void* context, const glifistore::FilesystemOperation operation)
            -> glifistore::Status {
            auto& self = *static_cast<IntentFault*>(context);
            if (self.mode == Case::Mode::write &&
                operation == glifistore::FilesystemOperation::write_compaction_intent) {
                self.arm_write = true;
            } else if (self.mode == Case::Mode::sync &&
                       operation == glifistore::FilesystemOperation::sync_compaction_intent) {
                self.arm_sync = true;
            }
            return {};
        }

        static auto read_some_at(void*, const int descriptor, const std::span<std::byte> bytes,
                                 const std::uint64_t offset) -> std::ptrdiff_t {
            return static_cast<std::ptrdiff_t>(
                ::pread(descriptor, bytes.data(), bytes.size(), static_cast<off_t>(offset)));
        }

        static auto write_some_at(void* context, const int descriptor, const std::span<const std::byte> bytes,
                                  const std::uint64_t offset) -> std::ptrdiff_t {
            auto& self = *static_cast<IntentFault*>(context);
            if (self.arm_write) {
                self.arm_write = false;
                self.fired = true;
                errno = self.write_error;
                return -1;
            }
            return static_cast<std::ptrdiff_t>(
                ::pwrite(descriptor, bytes.data(), bytes.size(), static_cast<off_t>(offset)));
        }

        static auto sync_file(void* context, const int descriptor, const glifistore::FileSyncMode) -> int {
            auto& self = *static_cast<IntentFault*>(context);
            if (self.arm_sync) {
                self.arm_sync = false;
                self.fired = true;
                errno = EIO;
                return -1;
            }
            return ::fsync(descriptor);
        }
    };

    const std::vector<Case> cases{
        {.mode = Case::Mode::write,
         .write_error = EIO,
         .expected = glifistore::ErrorCode::io_error,
         .label = "intent-eio"},
        {.mode = Case::Mode::write,
         .write_error = ENOSPC,
         .expected = glifistore::ErrorCode::storage_exhausted,
         .label = "intent-enospc"},
        {.mode = Case::Mode::sync, .expected = glifistore::ErrorCode::io_error, .label = "intent-sync-eio"},
    };
    for (const auto& fault : cases) {
        RecoveryTemporaryDirectory temporary;
        const auto store_id = recovery_store_id();
        const auto first_key = std::string{fault.label} + "-a";
        const auto second_key = std::string{fault.label} + "-b";
        const auto entries = seed_two_sealed_compaction_fixture(temporary.path(), store_id, first_key,
                                                                "alpha", second_key, "beta");
        const auto old = recovery_manifest(store_id, 1, entries);

        IntentFault injected{.mode = fault.mode, .write_error = fault.write_error};
        auto directory = glifistore::DataDirectory::open_and_lock(
            temporary.path(),
            glifistore::FilesystemHooks{.context = &injected,
                                        .before = &IntentFault::before,
                                        .file_io = {.context = &injected,
                                                    .read_some_at = &IntentFault::read_some_at,
                                                    .write_some_at = &IntentFault::write_some_at,
                                                    .sync_file = &IntentFault::sync_file}});
        GLIFI_REQUIRE(directory.has_value());
        auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory));
        GLIFI_REQUIRE(runtime.has_value());
        const auto result = (*runtime)->compact_worker(0, 0);
        GLIFI_REQUIRE(injected.fired);
        GLIFI_REQUIRE(!result.compacted());
        GLIFI_REQUIRE(result.error.has_value());
        GLIFI_REQUIRE(result.error->code == fault.expected);
        GLIFI_REQUIRE(result.outcome == glifistore::DurableCompactionOutcome::not_compacted);
        GLIFI_REQUIRE((*runtime)->healthy());
        GLIFI_REQUIRE((*runtime)->manifest() == old);
        GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
        runtime->reset();

        auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
        GLIFI_REQUIRE(reopened.has_value());
        GLIFI_REQUIRE((*reopened)->namespace_audit().clean());
        GLIFI_REQUIRE(owned_text(*(*reopened)->get(first_key)) == "alpha");
        GLIFI_REQUIRE(owned_text(*(*reopened)->get(second_key)) == "beta");
    }
}

// GS-PERSIST-FAULT-001 / Wave 3 L4: FileIoHooks faults on Manifest write/sync during
// post-intent promotion leave recovery_required; reopen selects one clean authority.
GLIFI_TEST("online compaction FileIoHooks promotion manifest faults recover cleanly") {
    struct Case {
        enum class Mode { write, sync } mode;
        int write_error{EIO};
        glifistore::ErrorCode expected{glifistore::ErrorCode::io_error};
        const char* label;
    };

    struct PromotionFault final {
        Case::Mode mode{};
        int write_error{EIO};
        bool arm_write{};
        bool arm_sync{};
        bool fired{};

        static auto before(void* context, const glifistore::FilesystemOperation operation)
            -> glifistore::Status {
            auto& self = *static_cast<PromotionFault*>(context);
            if (self.mode == Case::Mode::write &&
                operation == glifistore::FilesystemOperation::write_manifest) {
                self.arm_write = true;
            } else if (self.mode == Case::Mode::sync &&
                       operation == glifistore::FilesystemOperation::sync_manifest) {
                self.arm_sync = true;
            }
            return {};
        }

        static auto read_some_at(void*, const int descriptor, const std::span<std::byte> bytes,
                                 const std::uint64_t offset) -> std::ptrdiff_t {
            return static_cast<std::ptrdiff_t>(
                ::pread(descriptor, bytes.data(), bytes.size(), static_cast<off_t>(offset)));
        }

        static auto write_some_at(void* context, const int descriptor, const std::span<const std::byte> bytes,
                                  const std::uint64_t offset) -> std::ptrdiff_t {
            auto& self = *static_cast<PromotionFault*>(context);
            if (self.arm_write) {
                self.arm_write = false;
                self.fired = true;
                errno = self.write_error;
                return -1;
            }
            return static_cast<std::ptrdiff_t>(
                ::pwrite(descriptor, bytes.data(), bytes.size(), static_cast<off_t>(offset)));
        }

        static auto sync_file(void* context, const int descriptor, const glifistore::FileSyncMode) -> int {
            auto& self = *static_cast<PromotionFault*>(context);
            if (self.arm_sync) {
                self.arm_sync = false;
                self.fired = true;
                errno = EIO;
                return -1;
            }
            return ::fsync(descriptor);
        }
    };

    const std::vector<Case> cases{
        {.mode = Case::Mode::write,
         .write_error = EIO,
         .expected = glifistore::ErrorCode::io_error,
         .label = "promo-eio"},
        {.mode = Case::Mode::write,
         .write_error = ENOSPC,
         .expected = glifistore::ErrorCode::storage_exhausted,
         .label = "promo-enospc"},
        {.mode = Case::Mode::sync, .expected = glifistore::ErrorCode::io_error, .label = "promo-sync-eio"},
    };
    for (const auto& fault : cases) {
        RecoveryTemporaryDirectory temporary;
        const auto store_id = recovery_store_id();
        const auto first_key = std::string{fault.label} + "-a";
        const auto second_key = std::string{fault.label} + "-b";
        const auto entries = seed_two_sealed_compaction_fixture(temporary.path(), store_id, first_key,
                                                                "alpha", second_key, "beta");

        PromotionFault injected{.mode = fault.mode, .write_error = fault.write_error};
        auto directory = glifistore::DataDirectory::open_and_lock(
            temporary.path(),
            glifistore::FilesystemHooks{.context = &injected,
                                        .before = &PromotionFault::before,
                                        .file_io = {.context = &injected,
                                                    .read_some_at = &PromotionFault::read_some_at,
                                                    .write_some_at = &PromotionFault::write_some_at,
                                                    .sync_file = &PromotionFault::sync_file}});
        GLIFI_REQUIRE(directory.has_value());
        auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory));
        GLIFI_REQUIRE(runtime.has_value());
        const auto result = (*runtime)->compact_worker(0, 0);
        GLIFI_REQUIRE(injected.fired);
        GLIFI_REQUIRE(!result.compacted());
        GLIFI_REQUIRE(result.error.has_value());
        GLIFI_REQUIRE(result.error->code == fault.expected);
        // Intent already durable: promotion FileIoHooks faults require recovery.
        GLIFI_REQUIRE(result.outcome == glifistore::DurableCompactionOutcome::recovery_required);
        GLIFI_REQUIRE(!(*runtime)->healthy());
        runtime->reset();

        auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
        GLIFI_REQUIRE(reopened.has_value());
        GLIFI_REQUIRE((*reopened)->healthy());
        GLIFI_REQUIRE((*reopened)->namespace_audit().clean());
        GLIFI_REQUIRE(owned_text(*(*reopened)->get(first_key)) == "alpha");
        GLIFI_REQUIRE(owned_text(*(*reopened)->get(second_key)) == "beta");
    }
}
