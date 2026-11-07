#pragma once

#include "allocation_fault_harness.hpp"
#include "glifistore/core/fault_injection.hpp"
#include "glifistore/persistence/filesystem.hpp"
#include "glifistore/persistence/runtime_catalog.hpp"
#include "glifistore/store/store.hpp"
#include "store/store_internal.hpp"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace allocation_fault_test {

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "glifistore-allocation-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* created = ::mkdtemp(writable.data());
        if (created == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        root_ = created;
        path_ = root_ / "store";
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] auto path() const -> const std::filesystem::path& {
        return path_;
    }

  private:
    std::filesystem::path root_;
    std::filesystem::path path_;
};

inline void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

[[nodiscard]] inline auto bytes(const std::string_view value) noexcept -> std::span<const std::byte> {
    return std::as_bytes(std::span{value});
}

[[nodiscard]] inline auto value_text(const glifistore::OwnedValue& value) -> std::string {
    return {reinterpret_cast<const char*>(value.bytes.data()), value.bytes.size()};
}

inline const std::string kLongKey(96, 'k');
inline const std::string kOriginalValue(257, 'o');
inline const std::string kReplacementValue(513, 'r');

enum class MutationKind { put_new, put_update, erase };

struct Scenario {
    std::string_view name;
    MutationKind kind{MutationKind::put_new};
    bool seed{};
    bool force_rotation{};
    glifistore::DurableRuntimeOptions options{};
};

struct WriteBoundaryObserver {
    std::atomic_bool reached{};
    std::atomic_bool forced_full{};
    bool forbid_allocations{};
    bool force_rotation{};

    [[nodiscard]] static auto
    starts_persistent_change(const glifistore::FilesystemOperation operation) noexcept -> bool {
        switch (operation) {
        case glifistore::FilesystemOperation::create_data_directory:
        case glifistore::FilesystemOperation::write_manifest:
        case glifistore::FilesystemOperation::rename_manifest:
        case glifistore::FilesystemOperation::preallocate_segment:
        case glifistore::FilesystemOperation::write_segment_header:
        case glifistore::FilesystemOperation::rename_segment:
        case glifistore::FilesystemOperation::write_record:
        case glifistore::FilesystemOperation::write_commit_slot:
        case glifistore::FilesystemOperation::write_bootstrap:
        case glifistore::FilesystemOperation::rename_bootstrap:
        case glifistore::FilesystemOperation::remove_bootstrap:
        case glifistore::FilesystemOperation::write_compaction_intent:
        case glifistore::FilesystemOperation::rename_compaction_intent:
        case glifistore::FilesystemOperation::remove_compaction_intent:
        case glifistore::FilesystemOperation::remove_compaction_segment:
        case glifistore::FilesystemOperation::copy_backup_segment:
        case glifistore::FilesystemOperation::copy_backup_manifest:
        case glifistore::FilesystemOperation::write_blob_chunk:
        case glifistore::FilesystemOperation::install_blob_file:
        case glifistore::FilesystemOperation::link_blob_file:
            return true;
        case glifistore::FilesystemOperation::sync_parent_directory:
        case glifistore::FilesystemOperation::sync_manifest:
        case glifistore::FilesystemOperation::sync_directory:
        case glifistore::FilesystemOperation::sync_segment_file:
        case glifistore::FilesystemOperation::sync_record:
        case glifistore::FilesystemOperation::sync_commit_slot:
        case glifistore::FilesystemOperation::sync_bootstrap:
        case glifistore::FilesystemOperation::sync_compaction_intent:
        case glifistore::FilesystemOperation::sync_backup_destination:
        case glifistore::FilesystemOperation::sync_blob_file:
        case glifistore::FilesystemOperation::sync_blob_directory:
            return false;
        }
        return false;
    }

    static auto before(void* opaque, const glifistore::FilesystemOperation operation) -> glifistore::Status {
        auto& observer = *static_cast<WriteBoundaryObserver*>(opaque);
        if (observer.force_rotation && operation == glifistore::FilesystemOperation::write_record &&
            !observer.forced_full.exchange(true, std::memory_order_acq_rel)) {
            return glifistore::unexpected(glifistore::Error{glifistore::ErrorCode::segment_full, {}});
        }
        if (starts_persistent_change(operation)) {
            const bool first_boundary = !observer.reached.exchange(true, std::memory_order_acq_rel);
            if (observer.forbid_allocations && first_boundary) {
                allocation_fault::begin_forbid_all();
            }
        }
        return {};
    }
};

inline void initialize_store(const std::filesystem::path& path, const bool seed) {
    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = path,
        .durable_open_mode = glifistore::DurableOpenMode::create_new,
    });
    require(opened.has_value(), "failed to initialize durable allocation test Store");
    if (seed) {
        const auto stored = (*opened)->put(kLongKey, bytes(kOriginalValue));
        require(stored.has_value(), "failed to seed durable allocation test Store");
    }
}

inline void append_compaction_record(glifistore::DurableSegmentFile& segment, const std::uint64_t sequence,
                                     const std::string_view key, const std::string_view value) {
    const auto encoded = glifistore::encode_record({
        .sequence = glifistore::SequenceNumber{sequence},
        .opcode = glifistore::Opcode::put,
        .type = glifistore::ValueType::bytes,
        .flags = 0,
        .key_hash = glifistore::hash_key(key),
        .expire_at_ns = 0,
        .key = bytes(key),
        .value = bytes(value),
    });
    require(encoded.has_value(), "failed to encode allocation compaction Record");
    require(segment.append(*encoded).committed(), "failed to append allocation compaction Record");
}

inline void initialize_compaction_store(const std::filesystem::path& path) {
    initialize_store(path, false);
    auto directory = glifistore::DataDirectory::open_and_lock(path);
    require(directory.has_value(), "failed to lock allocation compaction Store");
    auto manifest = directory->read_manifest();
    require(manifest.has_value() && manifest->segments.size() == 1,
            "allocation compaction Store has an unexpected initial manifest");

    auto first_entry = manifest->segments.front();
    const glifistore::SegmentHeaderIdentity first_identity{
        .store_id = manifest->store_id,
        .segment_id = first_entry.segment_id,
        .generation = first_entry.generation,
        .owner_worker = first_entry.owner_worker,
    };
    auto first = glifistore::DurableSegmentFile::open(*directory, first_identity,
                                                      glifistore::SegmentFileOpenMode::read_write);
    require(first.has_value(), "failed to open first allocation compaction Segment");
    append_compaction_record(*first, 1, "first", "first-value");
    require(first->seal().committed(), "failed to seal first allocation compaction Segment");
    first_entry.role = glifistore::ManifestSegmentRole::sealed;

    const glifistore::ManifestSegmentEntry second_entry{
        .segment_id = glifistore::SegmentId{2},
        .generation = glifistore::GenerationId{1},
        .owner_worker = glifistore::WorkerId{0},
        .role = glifistore::ManifestSegmentRole::sealed,
    };
    const glifistore::SegmentHeaderIdentity second_identity{
        .store_id = manifest->store_id,
        .segment_id = second_entry.segment_id,
        .generation = second_entry.generation,
        .owner_worker = second_entry.owner_worker,
    };
    auto second_created = glifistore::DurableSegmentFile::create(*directory, second_identity);
    require(second_created.durable() && second_created.file.has_value(),
            "failed to create second allocation compaction Segment");
    append_compaction_record(*second_created.file, 2, "second", "second-value");
    require(second_created.file->seal().committed(), "failed to seal second allocation compaction Segment");

    const glifistore::ManifestSegmentEntry active_entry{
        .segment_id = glifistore::SegmentId{3},
        .generation = glifistore::GenerationId{1},
        .owner_worker = glifistore::WorkerId{0},
        .role = glifistore::ManifestSegmentRole::active,
    };
    const glifistore::SegmentHeaderIdentity active_identity{
        .store_id = manifest->store_id,
        .segment_id = active_entry.segment_id,
        .generation = active_entry.generation,
        .owner_worker = active_entry.owner_worker,
    };
    auto active_created = glifistore::DurableSegmentFile::create(*directory, active_identity);
    require(active_created.durable(), "failed to create active allocation compaction Segment");

    ++manifest->manifest_generation;
    manifest->next_segment_id = glifistore::SegmentId{4};
    manifest->segments = {first_entry, second_entry, active_entry};
    require(directory->publish_manifest(*manifest).durable(),
            "failed to publish allocation compaction manifest");
}

[[nodiscard]] inline auto open_runtime(const std::filesystem::path& path,
                                       const glifistore::DurableRuntimeOptions options,
                                       WriteBoundaryObserver* observer = nullptr)
    -> std::unique_ptr<glifistore::DurableRuntimeCatalog> {
    glifistore::FilesystemHooks hooks{};
    if (observer != nullptr) {
        hooks = {.context = observer, .before = &WriteBoundaryObserver::before};
    }
    auto directory = glifistore::DataDirectory::open_and_lock(path, hooks);
    require(directory.has_value(), "failed to lock durable allocation test Store");
    auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory), 0, options);
    require(runtime.has_value(), "failed to recover durable allocation test Store");
    return std::move(*runtime);
}

[[nodiscard]] inline auto mutate(glifistore::DurableRuntimeCatalog& runtime, const MutationKind kind)
    -> glifistore::DurableMutationResult {
    switch (kind) {
    case MutationKind::put_new:
    case MutationKind::put_update:
        return runtime.put(bytes(kLongKey), bytes(kReplacementValue));
    case MutationKind::erase:
        return runtime.erase(bytes(kLongKey));
    }
    throw std::runtime_error("unsupported allocation test mutation");
}

inline void require_recovered_prewrite_state(const std::filesystem::path& path, const bool seeded) {
    auto reopened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = path,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    require(reopened.has_value(), "failed to reopen allocation test Store");
    const auto recovered = (*reopened)->get(kLongKey);
    if (!seeded) {
        require(!recovered.has_value(), "pre-write allocation failure recovered a new value");
        require(recovered.error().code == glifistore::ErrorCode::not_found,
                "pre-write allocation failure returned an unexpected read error");
        return;
    }
    require(recovered.has_value(), "pre-write allocation failure removed the original value");
    require(value_text(*recovered) == kOriginalValue,
            "pre-write allocation failure changed the original value");
}

} // namespace allocation_fault_test
