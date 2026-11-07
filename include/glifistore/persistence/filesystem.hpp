#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/persistence/compaction_intent.hpp"
#include "glifistore/persistence/filesystem_hooks.hpp"
#include "glifistore/persistence/manifest.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace glifistore {

inline constexpr auto kManifestFilename = "manifest.glifi";
inline constexpr auto kManifestTemporaryFilename = ".manifest.glifi.tmp";
inline constexpr auto kStoreLockFilename = ".glifistore.lock";
inline constexpr auto kBootstrapIntentFilename = ".glifistore.bootstrap";
inline constexpr auto kBootstrapTemporaryFilename = ".glifistore.bootstrap.tmp";
inline constexpr auto kCompactionIntentFilename = ".glifistore.compaction";
inline constexpr auto kCompactionTemporaryFilename = ".glifistore.compaction.tmp";

enum class DataDirectoryOpenMode { existing, open_or_create, create_new };

class FileDescriptor final {
  public:
    FileDescriptor() = default;

    explicit FileDescriptor(int descriptor, FileIoHooks io_hooks = {}) noexcept
        : descriptor_(descriptor), io_hooks_(io_hooks) {}

    ~FileDescriptor();

    FileDescriptor(const FileDescriptor&) = delete;
    auto operator=(const FileDescriptor&) -> FileDescriptor& = delete;
    FileDescriptor(FileDescriptor&& other) noexcept;
    auto operator=(FileDescriptor&& other) noexcept -> FileDescriptor&;

    [[nodiscard]] auto valid() const noexcept -> bool {
        return descriptor_ >= 0;
    }

    [[nodiscard]] auto get() const noexcept -> int {
        return descriptor_;
    }

    [[nodiscard]] auto release() noexcept -> int;
    void reset(int descriptor = -1, FileIoHooks io_hooks = {}) noexcept;

    [[nodiscard]] auto size() const -> Result<std::uint64_t>;
    [[nodiscard]] auto write_all_at(std::span<const std::byte> bytes, std::uint64_t offset) const -> Status;
    [[nodiscard]] auto read_exact_at(std::span<std::byte> bytes, std::uint64_t offset) const -> Status;
    [[nodiscard]] auto sync(FileSyncMode mode) const -> Status;

  private:
    int descriptor_{-1};
    FileIoHooks io_hooks_{};
};

[[nodiscard]] inline auto filesystem_operation_name(const FilesystemOperation operation) -> std::string_view {
    switch (operation) {
    case FilesystemOperation::create_data_directory:
        return "create_data_directory";
    case FilesystemOperation::sync_parent_directory:
        return "sync_parent_directory";
    case FilesystemOperation::write_manifest:
        return "write_manifest";
    case FilesystemOperation::sync_manifest:
        return "sync_manifest";
    case FilesystemOperation::rename_manifest:
        return "rename_manifest";
    case FilesystemOperation::sync_directory:
        return "sync_directory";
    case FilesystemOperation::preallocate_segment:
        return "preallocate_segment";
    case FilesystemOperation::write_segment_header:
        return "write_segment_header";
    case FilesystemOperation::sync_segment_file:
        return "sync_segment_file";
    case FilesystemOperation::rename_segment:
        return "rename_segment";
    case FilesystemOperation::write_record:
        return "write_record";
    case FilesystemOperation::sync_record:
        return "sync_record";
    case FilesystemOperation::write_commit_slot:
        return "write_commit_slot";
    case FilesystemOperation::sync_commit_slot:
        return "sync_commit_slot";
    case FilesystemOperation::write_bootstrap:
        return "write_bootstrap";
    case FilesystemOperation::sync_bootstrap:
        return "sync_bootstrap";
    case FilesystemOperation::rename_bootstrap:
        return "rename_bootstrap";
    case FilesystemOperation::remove_bootstrap:
        return "remove_bootstrap";
    case FilesystemOperation::write_compaction_intent:
        return "write_compaction_intent";
    case FilesystemOperation::sync_compaction_intent:
        return "sync_compaction_intent";
    case FilesystemOperation::rename_compaction_intent:
        return "rename_compaction_intent";
    case FilesystemOperation::remove_compaction_intent:
        return "remove_compaction_intent";
    case FilesystemOperation::remove_compaction_segment:
        return "remove_compaction_segment";
    case FilesystemOperation::copy_backup_segment:
        return "copy_backup_segment";
    case FilesystemOperation::copy_backup_manifest:
        return "copy_backup_manifest";
    case FilesystemOperation::sync_backup_destination:
        return "sync_backup_destination";
    }
    return "unknown";
}

[[nodiscard]] inline auto parse_filesystem_operation(const std::string_view name)
    -> std::optional<FilesystemOperation> {
    for (const auto operation : {FilesystemOperation::create_data_directory,
                                 FilesystemOperation::sync_parent_directory,
                                 FilesystemOperation::write_manifest,
                                 FilesystemOperation::sync_manifest,
                                 FilesystemOperation::rename_manifest,
                                 FilesystemOperation::sync_directory,
                                 FilesystemOperation::preallocate_segment,
                                 FilesystemOperation::write_segment_header,
                                 FilesystemOperation::sync_segment_file,
                                 FilesystemOperation::rename_segment,
                                 FilesystemOperation::write_record,
                                 FilesystemOperation::sync_record,
                                 FilesystemOperation::write_commit_slot,
                                 FilesystemOperation::sync_commit_slot,
                                 FilesystemOperation::write_bootstrap,
                                 FilesystemOperation::sync_bootstrap,
                                 FilesystemOperation::rename_bootstrap,
                                 FilesystemOperation::remove_bootstrap,
                                 FilesystemOperation::write_compaction_intent,
                                 FilesystemOperation::sync_compaction_intent,
                                 FilesystemOperation::rename_compaction_intent,
                                 FilesystemOperation::remove_compaction_intent,
                                 FilesystemOperation::remove_compaction_segment,
                                 FilesystemOperation::copy_backup_segment,
                                 FilesystemOperation::copy_backup_manifest,
                                 FilesystemOperation::sync_backup_destination}) {
        if (name == filesystem_operation_name(operation)) {
            return operation;
        }
    }
    return std::nullopt;
}

namespace detail {
[[nodiscard]] inline auto invoke_filesystem_before(const FilesystemHooks& hooks,
                                                   const FilesystemOperation operation) -> Status {
    if (hooks.before == nullptr) {
        return {};
    }
    return hooks.before(hooks.context, operation);
}

inline void invoke_filesystem_after(const FilesystemHooks& hooks,
                                    const FilesystemOperation operation) noexcept {
    if (hooks.after != nullptr) {
        hooks.after(hooks.context, operation);
    }
}
} // namespace detail

enum class ManifestPublicationOutcome { durable, not_published, indeterminate };

struct ManifestPublicationResult {
    ManifestPublicationOutcome outcome{ManifestPublicationOutcome::not_published};
    std::optional<Error> error;

    [[nodiscard]] auto durable() const noexcept -> bool {
        return outcome == ManifestPublicationOutcome::durable;
    }
};

enum class CompactionIntentPublicationOutcome { durable, not_published, indeterminate };

struct CompactionIntentPublicationResult {
    CompactionIntentPublicationOutcome outcome{CompactionIntentPublicationOutcome::not_published};
    std::optional<Error> error;

    [[nodiscard]] auto durable() const noexcept -> bool {
        return outcome == CompactionIntentPublicationOutcome::durable;
    }
};

enum class CompactionIntentRemovalOutcome { durable, not_removed, indeterminate };

struct CompactionIntentRemovalResult {
    CompactionIntentRemovalOutcome outcome{CompactionIntentRemovalOutcome::not_removed};
    std::optional<Error> error;

    [[nodiscard]] auto durable() const noexcept -> bool {
        return outcome == CompactionIntentRemovalOutcome::durable;
    }
};

enum class CompactionSegmentRetirementOutcome { durable, not_removed, indeterminate };

struct CompactionSegmentRetirementResult {
    CompactionSegmentRetirementOutcome outcome{CompactionSegmentRetirementOutcome::not_removed};
    std::optional<Error> error;

    [[nodiscard]] auto durable() const noexcept -> bool {
        return outcome == CompactionSegmentRetirementOutcome::durable;
    }
};

class DurableSegmentFile;

class DataDirectory final {
  public:
    [[nodiscard]] static auto open_and_lock(const std::filesystem::path& path, FilesystemHooks hooks = {})
        -> Result<DataDirectory>;
    [[nodiscard]] static auto open_and_lock(const std::filesystem::path& path, DataDirectoryOpenMode mode,
                                            FilesystemHooks hooks = {}) -> Result<DataDirectory>;

    ~DataDirectory();
    DataDirectory(const DataDirectory&) = delete;
    auto operator=(const DataDirectory&) -> DataDirectory& = delete;
    DataDirectory(DataDirectory&&) noexcept = default;
    auto operator=(DataDirectory&& other) noexcept -> DataDirectory&;

    [[nodiscard]] auto publish_manifest(const Manifest& manifest,
                                        std::size_t max_bytes = kMaximumManifestBytes)
        -> ManifestPublicationResult;
    [[nodiscard]] auto read_manifest(std::size_t max_bytes = kMaximumManifestBytes) const -> Result<Manifest>;
    [[nodiscard]] auto publish_bootstrap_intent(const Manifest& manifest,
                                                std::size_t max_bytes = kMaximumManifestBytes) -> Status;
    [[nodiscard]] auto read_bootstrap_intent(std::size_t max_bytes = kMaximumManifestBytes) const
        -> Result<Manifest>;
    [[nodiscard]] auto finish_bootstrap() -> Status;
    [[nodiscard]] auto publish_compaction_intent(const DurableCompactionIntent& intent,
                                                 std::size_t max_manifest_bytes = kMaximumManifestBytes)
        -> CompactionIntentPublicationResult;
    [[nodiscard]] auto read_compaction_intent(std::size_t max_manifest_bytes = kMaximumManifestBytes) const
        -> Result<DurableCompactionIntent>;
    [[nodiscard]] auto remove_compaction_intent() -> CompactionIntentRemovalResult;
    [[nodiscard]] auto retire_compaction_segments(const StoreId& store_id,
                                                  std::span<const ManifestSegmentEntry> segments)
        -> CompactionSegmentRetirementResult;
    // Recovery-only cleanup after a complete authoritative scan. Every name
    // must be a recognized private engine temporary; the batch is removed and
    // directory-synchronized without ever touching a canonical Segment name.
    [[nodiscard]] auto cleanup_recovery_temporaries(std::span<const std::string> names) -> Status;
    [[nodiscard]] auto pristine_for_bootstrap() const -> Result<bool>;
    [[nodiscard]] auto available_space_bytes() const -> Result<std::uint64_t>;
    // Returns an independently positioned descriptor for descriptor-relative
    // namespace enumeration. The caller owns the returned descriptor.
    [[nodiscard]] auto open_directory_for_enumeration() const -> Result<FileDescriptor>;

    // Descriptor-relative openat for catalog backup/copy while the Store lock is held.
    [[nodiscard]] auto directory_descriptor() const noexcept -> int {
        return directory_.get();
    }

    [[nodiscard]] auto hooks() const noexcept -> const FilesystemHooks& {
        return hooks_;
    }

    [[nodiscard]] auto healthy() const noexcept -> bool {
        return health_ && health_->load(std::memory_order_acquire);
    }

  private:
    friend class DurableSegmentFile;

    DataDirectory(FileDescriptor directory, FileDescriptor lock, FilesystemHooks hooks)
        : directory_(std::move(directory)), lock_(std::move(lock)), hooks_(hooks),
          health_(std::make_shared<std::atomic_bool>(true)) {}

    [[nodiscard]] auto before(FilesystemOperation operation) const -> Status;
    void after(FilesystemOperation operation) const noexcept;
    [[nodiscard]] auto sync_directory() const -> Status;

    FileDescriptor directory_;
    FileDescriptor lock_;
    FilesystemHooks hooks_{};
    std::shared_ptr<std::atomic_bool> health_;
};

} // namespace glifistore
