#include "glifistore/persistence/store_verify.hpp"

#include "glifistore/persistence/segment_file.hpp"
#include "persistence/blob_storage.hpp"

#include <utility>

namespace glifistore {
namespace {

[[nodiscard]] auto validate_lifecycle(const ManifestSegmentEntry& entry, const SegmentCommit& commit,
                                      bool& active_requires_rotation) -> Status {
    if (entry.role == ManifestSegmentRole::sealed) {
        if (commit.state != PersistedSegmentState::sealed) {
            return fail(ErrorCode::corrupted_data,
                        "manifest sealed Segment has an active persisted commit state");
        }
        return {};
    }
    if (commit.state == PersistedSegmentState::sealed) {
        active_requires_rotation = true;
    }
    return {};
}

} // namespace

auto verify_durable_store(DataDirectory& directory, const bool scan_records,
                          const DurableResourceLimits& limits) -> Result<DurableStoreVerifyReport> {
    if (auto valid = validate_durable_resource_limits(limits); !valid) {
        return unexpected(valid.error());
    }
    auto manifest = directory.read_manifest(limits.max_manifest_bytes);
    if (!manifest) {
        return unexpected(manifest.error());
    }
    if (auto resources = validate_durable_manifest_resources(*manifest, limits); !resources) {
        return unexpected(resources.error());
    }

    auto namespace_audit = audit_data_directory(directory, *manifest);
    if (!namespace_audit) {
        return unexpected(namespace_audit.error());
    }
    if (auto safe = validate_namespace_for_recovery(*namespace_audit); !safe) {
        return unexpected(safe.error());
    }

    DurableStoreVerifyReport report{
        .path = {},
        .manifest = std::move(*manifest),
        .namespace_audit = std::move(*namespace_audit),
    };
    report.segments.reserve(report.manifest.segments.size());

    for (const auto& entry : report.manifest.segments) {
        const SegmentHeaderIdentity identity{.store_id = report.manifest.store_id,
                                             .segment_id = entry.segment_id,
                                             .generation = entry.generation,
                                             .owner_worker = entry.owner_worker,
                                             .format_version = report.manifest.format_version};
        auto opened = DurableSegmentFile::open(directory, identity, SegmentFileOpenMode::read_only);
        if (!opened) {
            return unexpected(opened.error());
        }

        bool active_requires_rotation{};
        if (auto lifecycle =
                validate_lifecycle(entry, opened->selected_commit().commit, active_requires_rotation);
            !lifecycle) {
            return unexpected(lifecycle.error());
        }

        DurableStoreVerifiedSegment verified{
            .entry = entry,
            .selected = opened->selected_commit(),
            .active_requires_rotation = active_requires_rotation,
        };
        if (scan_records) {
            std::uint64_t counted{};
            const auto count = [](void* context, const RecordRef&, const RecordView&) -> Status {
                ++*static_cast<std::uint64_t*>(context);
                return {};
            };
            if (auto scanned = opened->visit_committed_records(&counted, count); !scanned) {
                return unexpected(scanned.error());
            }
            verified.scanned_records = counted;
        } else {
            verified.scanned_records = verified.selected.commit.record_count;
        }
        report.scanned_records += verified.scanned_records;
        if (active_requires_rotation) {
            ++report.active_requires_rotation_count;
        }
        report.segments.push_back(verified);
    }
    if (report.manifest.format_version == 2) {
        auto blobs = detail::BlobStorage::open(directory, report.manifest, {}, 0);
        if (!blobs) {
            return unexpected(blobs.error());
        }
        auto roots = detail::verify_blob_closure(directory, report.manifest, **blobs);
        if (!roots) {
            return unexpected(roots.error());
        }
    }
    return report;
}

auto verify_durable_store_path(const std::filesystem::path& path, const bool scan_records,
                               const DurableResourceLimits& limits) -> Result<DurableStoreVerifyReport> {
    auto directory = DataDirectory::open_and_lock(path, DataDirectoryOpenMode::existing);
    if (!directory) {
        return unexpected(directory.error());
    }
    auto report = verify_durable_store(*directory, scan_records, limits);
    if (!report) {
        return unexpected(report.error());
    }
    report->path = path;
    return report;
}

} // namespace glifistore
