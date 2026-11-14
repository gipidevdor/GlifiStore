#pragma once

#include "glifistore/core/key_hash.hpp"
#include "glifistore/persistence/recovery.hpp"
#include "glifistore/persistence/runtime_catalog.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "glifistore/segment/record.hpp"
#include "test.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
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

namespace persistence_recovery_test_support {

#if defined(NDEBUG) && !defined(GLIFISTORE_GET_PATH_TIMING)
inline constexpr bool kExpectGetPathTiming = false;
#else
inline constexpr bool kExpectGetPathTiming = true;
#endif

inline constexpr auto kNativeConcurrencyDeadline = std::chrono::seconds{30};

/// Runtime options for tests that observe Segment file reads (blocking hooks, on-disk corruption).
/// Resident images (ADR 0046) serve those reads from memory, so these tests opt out explicitly.
inline auto cold_read_runtime_options() -> glifistore::DurableRuntimeOptions {
    glifistore::DurableRuntimeOptions options{};
    options.limits.resident_reads_enabled = false;
    return options;
}

class RecoveryTemporaryDirectory final {
  public:
    RecoveryTemporaryDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "glifistore-recovery-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* created = ::mkdtemp(writable.data());
        GLIFI_REQUIRE(created != nullptr);
        path_ = created;
    }

    ~RecoveryTemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] auto path() const -> const std::filesystem::path& {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

class BlockingRecordRead final {
  public:
    void arm() {
        const std::lock_guard lock{mutex_};
        armed_ = true;
    }

    [[nodiscard]] auto
    wait_until_blocked(const std::chrono::milliseconds timeout = kNativeConcurrencyDeadline) -> bool {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(lock, timeout, [&] { return blocked_; });
    }

    void release() {
        {
            const std::lock_guard lock{mutex_};
            released_ = true;
        }
        condition_.notify_all();
    }

    void force_next_record_write_full() {
        const std::lock_guard lock{mutex_};
        force_record_full_ = true;
    }

    static auto before(void* opaque, const glifistore::FilesystemOperation operation) -> glifistore::Status {
        auto& state = *static_cast<BlockingRecordRead*>(opaque);
        const std::lock_guard lock{state.mutex_};
        if (operation == glifistore::FilesystemOperation::write_record && state.force_record_full_) {
            state.force_record_full_ = false;
            return glifistore::fail(glifistore::ErrorCode::segment_full,
                                    "injected full Segment during blocked compaction");
        }
        return {};
    }

    static auto read_some_at(void* opaque, const int descriptor, const std::span<std::byte> bytes,
                             const std::uint64_t offset) -> std::ptrdiff_t {
        auto& state = *static_cast<BlockingRecordRead*>(opaque);
        if (offset >= glifistore::kSegmentHeaderReservedBytes) {
            std::unique_lock lock{state.mutex_};
            if (state.armed_ && !state.claimed_) {
                state.claimed_ = true;
                state.blocked_ = true;
                state.condition_.notify_all();
                state.condition_.wait(lock, [&] { return state.released_; });
            }
        }
        return ::pread(descriptor, bytes.data(), bytes.size(), static_cast<off_t>(offset));
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool armed_{};
    bool claimed_{};
    bool blocked_{};
    bool released_{};
    bool force_record_full_{};
};

class BlockingFilesystemOperation final {
  public:
    explicit BlockingFilesystemOperation(const glifistore::FilesystemOperation target,
                                         const bool armed = true)
        : target_(target), armed_(armed) {}

    void arm() {
        const std::lock_guard lock{mutex_};
        armed_ = true;
    }

    void force_next_record_write_full() {
        const std::lock_guard lock{mutex_};
        force_record_full_ = true;
    }

    [[nodiscard]] auto
    wait_until_blocked(const std::chrono::milliseconds timeout = kNativeConcurrencyDeadline) -> bool {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(lock, timeout, [&] { return blocked_; });
    }

    void release() {
        {
            const std::lock_guard lock{mutex_};
            released_ = true;
        }
        condition_.notify_all();
    }

    static auto before(void* opaque, const glifistore::FilesystemOperation operation) -> glifistore::Status {
        auto& state = *static_cast<BlockingFilesystemOperation*>(opaque);
        {
            const std::lock_guard lock{state.mutex_};
            if (operation == glifistore::FilesystemOperation::write_record && state.force_record_full_) {
                state.force_record_full_ = false;
                return glifistore::fail(glifistore::ErrorCode::segment_full,
                                        "injected full Segment while manifest publisher is blocked");
            }
        }
        if (operation != state.target_) {
            return {};
        }
        std::unique_lock lock{state.mutex_};
        if (!state.armed_ || state.claimed_) {
            return {};
        }
        state.claimed_ = true;
        state.blocked_ = true;
        state.condition_.notify_all();
        state.condition_.wait(lock, [&] { return state.released_; });
        return {};
    }

  private:
    glifistore::FilesystemOperation target_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool claimed_{};
    bool blocked_{};
    bool released_{};
    bool armed_{};
    bool force_record_full_{};
};

class BlockingRotationSeal final {
  public:
    void arm() {
        const std::lock_guard lock{mutex_};
        armed_ = true;
        force_record_full_ = true;
    }

    [[nodiscard]] auto
    wait_until_blocked(const std::chrono::milliseconds timeout = kNativeConcurrencyDeadline) -> bool {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(lock, timeout, [&] { return blocked_; });
    }

    void release() {
        {
            const std::lock_guard lock{mutex_};
            released_ = true;
        }
        condition_.notify_all();
    }

    static auto before(void* opaque, const glifistore::FilesystemOperation operation) -> glifistore::Status {
        auto& state = *static_cast<BlockingRotationSeal*>(opaque);
        std::unique_lock lock{state.mutex_};
        if (!state.armed_) {
            return {};
        }
        if (operation == glifistore::FilesystemOperation::write_record && state.force_record_full_) {
            state.force_record_full_ = false;
            return glifistore::fail(glifistore::ErrorCode::segment_full,
                                    "injected full Segment before blocked rotation seal");
        }
        if (operation != glifistore::FilesystemOperation::sync_commit_slot || state.claimed_) {
            return {};
        }
        state.claimed_ = true;
        state.blocked_ = true;
        state.condition_.notify_all();
        state.condition_.wait(lock, [&] { return state.released_; });
        return {};
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool armed_{};
    bool force_record_full_{};
    bool claimed_{};
    bool blocked_{};
    bool released_{};
};

struct OneShotFilesystemFailure {
    glifistore::FilesystemOperation target;
    glifistore::ErrorCode code{glifistore::ErrorCode::io_error};
    std::size_t target_occurrence{1};
    std::size_t occurrences{};
    bool fired{};

    static auto before(void* opaque, glifistore::FilesystemOperation operation) -> glifistore::Status {
        auto& state = *static_cast<OneShotFilesystemFailure*>(opaque);
        if (operation == state.target) {
            ++state.occurrences;
        }
        if (!state.fired && operation == state.target && state.occurrences == state.target_occurrence) {
            state.fired = true;
            return glifistore::fail(state.code, "injected durable runtime filesystem failure");
        }
        return {};
    }
};

// Systematic fail-the-Nth logical filesystem seam. Unlike the targeted matrix
// above, this enumerates the operation's actual execution trace and stops only
// after an iteration reaches no further seam.
struct NthFilesystemFailure {
    std::size_t target_occurrence{1};
    std::size_t occurrences{};
    std::optional<glifistore::FilesystemOperation> fired_operation{};

    static auto before(void* opaque, const glifistore::FilesystemOperation operation) -> glifistore::Status {
        auto& state = *static_cast<NthFilesystemFailure*>(opaque);
        ++state.occurrences;
        if (!state.fired_operation && state.occurrences == state.target_occurrence) {
            state.fired_operation = operation;
            return glifistore::fail(glifistore::ErrorCode::io_error,
                                    "injected Nth durable runtime filesystem failure");
        }
        return {};
    }
};

struct SyncThreadObserver {
    std::mutex mutex;
    std::thread::id sync_thread;
    std::vector<std::thread::id> sync_threads;

    static auto before(void* opaque, const glifistore::FilesystemOperation operation) -> glifistore::Status {
        if (operation == glifistore::FilesystemOperation::sync_record) {
            auto& observer = *static_cast<SyncThreadObserver*>(opaque);
            const std::lock_guard lock{observer.mutex};
            observer.sync_thread = std::this_thread::get_id();
            observer.sync_threads.push_back(observer.sync_thread);
        }
        return {};
    }
};

struct BatchBoundaryObserver {
    std::mutex mutex;
    std::size_t writes_since_sync{};
    std::size_t maximum_writes_before_sync{};
    std::size_t sync_count{};

    static auto before(void* opaque, const glifistore::FilesystemOperation operation) -> glifistore::Status {
        auto& observer = *static_cast<BatchBoundaryObserver*>(opaque);
        const std::lock_guard lock{observer.mutex};
        if (operation == glifistore::FilesystemOperation::write_record) {
            ++observer.writes_since_sync;
        } else if (operation == glifistore::FilesystemOperation::sync_record) {
            observer.maximum_writes_before_sync =
                std::max(observer.maximum_writes_before_sync, observer.writes_since_sync);
            observer.writes_since_sync = 0;
            ++observer.sync_count;
        }
        return {};
    }
};

struct RecordWriteObserver {
    std::mutex mutex;
    std::condition_variable written;
    bool record_written{};

    static auto before(void* opaque, const glifistore::FilesystemOperation operation) -> glifistore::Status {
        if (operation == glifistore::FilesystemOperation::write_record) {
            auto& observer = *static_cast<RecordWriteObserver*>(opaque);
            {
                const std::lock_guard lock{observer.mutex};
                observer.record_written = true;
            }
            observer.written.notify_all();
        }
        return {};
    }
};

struct RotationBudgetObserver {
    bool force_segment_full{true};
    std::uint64_t available_bytes{};

    static auto before(void* opaque, const glifistore::FilesystemOperation operation) -> glifistore::Status {
        auto& observer = *static_cast<RotationBudgetObserver*>(opaque);
        if (operation == glifistore::FilesystemOperation::write_record && observer.force_segment_full) {
            observer.force_segment_full = false;
            return glifistore::fail(glifistore::ErrorCode::segment_full,
                                    "injected full Segment before rotation");
        }
        return {};
    }

    static auto available(void* opaque) -> glifistore::Result<std::uint64_t> {
        return static_cast<RotationBudgetObserver*>(opaque)->available_bytes;
    }
};

[[nodiscard]] auto recovery_store_id(std::byte first = std::byte{0x20}) -> glifistore::StoreId;
[[nodiscard]] auto key_for_worker(std::size_t worker, std::size_t worker_count, std::string_view prefix)
    -> std::string;
[[nodiscard]] auto segment_identity(const glifistore::StoreId& store_id,
                                    const glifistore::ManifestSegmentEntry& entry)
    -> glifistore::SegmentHeaderIdentity;
[[nodiscard]] auto create_segment(glifistore::DataDirectory& directory, const glifistore::StoreId& store_id,
                                  const glifistore::ManifestSegmentEntry& entry)
    -> glifistore::DurableSegmentFile;
void append_record(glifistore::DurableSegmentFile& file, std::uint64_t sequence, std::string_view key,
                   std::string_view value = {}, glifistore::Opcode opcode = glifistore::Opcode::put,
                   std::uint64_t expire_at_ns = 0, std::optional<std::uint64_t> stored_hash = std::nullopt);
void create_private_file(const std::filesystem::path& path);
[[nodiscard]] auto recovery_manifest(const glifistore::StoreId& store_id, std::uint32_t workers,
                                     std::vector<glifistore::ManifestSegmentEntry> segments)
    -> glifistore::Manifest;
[[nodiscard]] auto owned_text(const glifistore::OwnedValue& value) -> std::string;

} // namespace persistence_recovery_test_support

using persistence_recovery_test_support::append_record;
using persistence_recovery_test_support::BatchBoundaryObserver;
using persistence_recovery_test_support::BlockingFilesystemOperation;
using persistence_recovery_test_support::BlockingRecordRead;
using persistence_recovery_test_support::BlockingRotationSeal;
using persistence_recovery_test_support::cold_read_runtime_options;
using persistence_recovery_test_support::create_private_file;
using persistence_recovery_test_support::create_segment;
using persistence_recovery_test_support::kExpectGetPathTiming;
using persistence_recovery_test_support::key_for_worker;
using persistence_recovery_test_support::kNativeConcurrencyDeadline;
using persistence_recovery_test_support::NthFilesystemFailure;
using persistence_recovery_test_support::OneShotFilesystemFailure;
using persistence_recovery_test_support::owned_text;
using persistence_recovery_test_support::RecordWriteObserver;
using persistence_recovery_test_support::recovery_manifest;
using persistence_recovery_test_support::recovery_store_id;
using persistence_recovery_test_support::RecoveryTemporaryDirectory;
using persistence_recovery_test_support::RotationBudgetObserver;
using persistence_recovery_test_support::segment_identity;
using persistence_recovery_test_support::SyncThreadObserver;
