#include "glifistore/persistence/store_repair.hpp"

#include "glifistore/persistence/segment_file.hpp"
#include "system_error.hpp"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace glifistore {
namespace {

[[nodiscard]] auto interrupted_open(const char* path, int flags, mode_t mode = 0) -> int {
    int descriptor{};
    do {
        descriptor = mode == 0 ? ::open(path, flags) : ::open(path, flags, mode);
    } while (descriptor < 0 && errno == EINTR);
    return descriptor;
}

[[nodiscard]] auto interrupted_open_at(int directory, const char* name, int flags, mode_t mode = 0) -> int {
    int descriptor{};
    do {
        descriptor = mode == 0 ? ::openat(directory, name, flags) : ::openat(directory, name, flags, mode);
    } while (descriptor < 0 && errno == EINTR);
    return descriptor;
}

[[nodiscard]] auto copy_named_file(int source_directory, int destination_directory, const char* name)
    -> Result<std::uint64_t> {
    FileDescriptor source{interrupted_open_at(source_directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (!source.valid()) {
        if (errno == ENOENT) {
            return fail(ErrorCode::not_found, std::string{"repair source file is missing: "} + name);
        }
        return persistence_system_error("openat(repair source file)");
    }
    FileDescriptor destination{interrupted_open_at(destination_directory, name,
                                                   O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                                                   S_IRUSR | S_IWUSR)};
    if (!destination.valid()) {
        if (errno == EEXIST) {
            return fail(ErrorCode::sequence_conflict,
                        std::string{"repair destination already contains: "} + name);
        }
        return persistence_system_error("openat(repair destination file)");
    }

    std::array<std::byte, 1U << 20> buffer{};
    std::uint64_t copied{};
    for (;;) {
        ssize_t read_count{};
        do {
            read_count = ::read(source.get(), buffer.data(), buffer.size());
        } while (read_count < 0 && errno == EINTR);
        if (read_count < 0) {
            return persistence_system_error("read(repair source file)");
        }
        if (read_count == 0) {
            break;
        }
        std::size_t offset{};
        const auto total = static_cast<std::size_t>(read_count);
        while (offset < total) {
            ssize_t written{};
            do {
                written = ::write(destination.get(), buffer.data() + offset, total - offset);
            } while (written < 0 && errno == EINTR);
            if (written < 0) {
                return persistence_system_error("write(repair destination file)");
            }
            offset += static_cast<std::size_t>(written);
        }
        copied += static_cast<std::uint64_t>(read_count);
    }
    if (auto synced = destination.sync(FileSyncMode::full); !synced) {
        return unexpected(synced.error());
    }
    return copied;
}

[[nodiscard]] auto quarantinable(const NamespaceIssueKind kind) noexcept -> bool {
    switch (kind) {
    case NamespaceIssueKind::stale_manifest_temporary:
    case NamespaceIssueKind::stale_segment_temporary:
    case NamespaceIssueKind::stale_compaction_temporary:
    case NamespaceIssueKind::compaction_intent:
    case NamespaceIssueKind::unlisted_segment:
    case NamespaceIssueKind::malformed_engine_name:
    case NamespaceIssueKind::unknown_entry:
        return true;
    case NamespaceIssueKind::unsafe_entry:
    case NamespaceIssueKind::missing_catalog_segment:
    case NamespaceIssueKind::missing_required_entry:
        return false;
    }
    return false;
}

[[nodiscard]] auto blocking_catalog_fault(const NamespaceIssueKind kind) noexcept -> bool {
    return kind == NamespaceIssueKind::missing_catalog_segment ||
           kind == NamespaceIssueKind::missing_required_entry || kind == NamespaceIssueKind::unsafe_entry;
}

[[nodiscard]] auto verify_catalog_only(DataDirectory& directory, const bool scan_records, Manifest manifest)
    -> Result<DurableStoreVerifyReport> {
    DurableStoreVerifyReport report{.manifest = std::move(manifest)};
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
        DurableStoreVerifiedSegment verified{
            .entry = entry,
            .selected = opened->selected_commit(),
        };
        if (entry.role == ManifestSegmentRole::sealed &&
            opened->selected_commit().commit.state != PersistedSegmentState::sealed) {
            return fail(ErrorCode::corrupted_data,
                        "manifest sealed Segment has an active persisted commit state");
        }
        if (entry.role != ManifestSegmentRole::sealed &&
            opened->selected_commit().commit.state == PersistedSegmentState::sealed) {
            verified.active_requires_rotation = true;
            ++report.active_requires_rotation_count;
        }
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
        report.segments.push_back(verified);
    }
    return report;
}

[[nodiscard]] auto write_quarantine_audit(const std::filesystem::path& audit_path,
                                          const DurableStoreRepairReport& report) -> Status {
    std::ofstream out{audit_path, std::ios::out | std::ios::trunc};
    if (!out) {
        return fail(ErrorCode::io_error, "cannot write repair quarantine audit");
    }
    out << "glifistore repair quarantine audit\n";
    out << "source=" << report.source.string() << '\n';
    out << "workspace=" << report.workspace.string() << '\n';
    out << "repaired_store=" << report.repaired_store.string() << '\n';
    out << "quarantined_files=" << report.quarantined.size() << '\n';
    for (const auto& entry : report.quarantined) {
        out << "file=" << entry.source_name << " kind=" << namespace_issue_name(entry.kind)
            << " bytes=" << entry.bytes_copied << " path=" << entry.quarantine_path.string() << '\n';
    }
    out.flush();
    if (!out) {
        return fail(ErrorCode::io_error, "failed while writing repair quarantine audit");
    }
    FileDescriptor audit{interrupted_open(audit_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (!audit.valid()) {
        return persistence_system_error("open(repair quarantine audit)");
    }
    return audit.sync(FileSyncMode::full);
}

} // namespace

auto repair_durable_store(const std::filesystem::path& source, const std::filesystem::path& workspace,
                          const bool scan_records, const DurableResourceLimits& limits)
    -> Result<DurableStoreRepairReport> {
    if (source.empty() || workspace.empty()) {
        return fail(ErrorCode::invalid_argument, "repair source and workspace paths are required");
    }
    if (source == workspace) {
        return fail(ErrorCode::invalid_argument, "repair source and workspace must differ");
    }
    if (auto valid = validate_durable_resource_limits(limits); !valid) {
        return unexpected(valid.error());
    }

    DurableStoreRepairReport report{
        .source = source,
        .workspace = workspace,
        .repaired_store = workspace / "store",
        .quarantine_directory = workspace / "quarantine",
    };

    {
        auto source_locked = DataDirectory::open_and_lock(source, DataDirectoryOpenMode::existing);
        if (!source_locked) {
            return unexpected(source_locked.error());
        }
        auto manifest = source_locked->read_manifest(limits.max_manifest_bytes);
        if (!manifest) {
            return unexpected(manifest.error());
        }
        if (manifest->format_version == 2) {
            return fail(ErrorCode::invalid_argument, "v2 blob repair is unsupported");
        }
        if (auto resources = validate_durable_manifest_resources(*manifest, limits); !resources) {
            return unexpected(resources.error());
        }
        auto namespace_audit = audit_data_directory(*source_locked, *manifest);
        if (!namespace_audit) {
            return unexpected(namespace_audit.error());
        }
        report.source_namespace_audit = std::move(*namespace_audit);

        for (const auto& issue : report.source_namespace_audit.issues) {
            if (blocking_catalog_fault(issue.kind)) {
                return fail(ErrorCode::corrupted_data,
                            std::string{"repair refuses catalog/namespace fault: "} +
                                std::string{namespace_issue_name(issue.kind)} + " '" + issue.name + "'");
            }
        }

        auto catalog = verify_catalog_only(*source_locked, scan_records, *manifest);
        if (!catalog) {
            return unexpected(catalog.error());
        }

        std::error_code mkdir_error;
        if (!std::filesystem::create_directories(workspace, mkdir_error) && mkdir_error) {
            return fail(ErrorCode::io_error, "cannot create repair workspace: " + mkdir_error.message());
        }
        if (!std::filesystem::is_empty(workspace, mkdir_error) || mkdir_error) {
            if (mkdir_error) {
                return fail(ErrorCode::io_error, "cannot inspect repair workspace: " + mkdir_error.message());
            }
            return fail(ErrorCode::invalid_argument, "repair workspace must be empty");
        }

        auto destination_locked =
            DataDirectory::open_and_lock(report.repaired_store, DataDirectoryOpenMode::create_new);
        if (!destination_locked) {
            return unexpected(destination_locked.error());
        }
        if (auto pristine = destination_locked->pristine_for_bootstrap(); !pristine) {
            return unexpected(pristine.error());
        } else if (!*pristine) {
            return fail(ErrorCode::invalid_argument, "repair destination store is not empty");
        }

        if (!std::filesystem::create_directory(report.quarantine_directory, mkdir_error) || mkdir_error) {
            return fail(ErrorCode::io_error,
                        "cannot create repair quarantine directory: " + mkdir_error.message());
        }

        FileDescriptor source_root{
            interrupted_open(source.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
        FileDescriptor destination_root{
            interrupted_open(report.repaired_store.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
        FileDescriptor quarantine_root{interrupted_open(report.quarantine_directory.c_str(),
                                                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
        if (!source_root.valid() || !destination_root.valid() || !quarantine_root.valid()) {
            return persistence_system_error("open(repair directories for copy)");
        }

        for (const auto& issue : report.source_namespace_audit.issues) {
            if (!quarantinable(issue.kind)) {
                continue;
            }
            auto copied = copy_named_file(source_root.get(), quarantine_root.get(), issue.name.c_str());
            if (!copied) {
                return unexpected(copied.error());
            }
            report.quarantined.push_back(DurableStoreQuarantinedFile{
                .source_name = issue.name,
                .kind = issue.kind,
                .quarantine_path = report.quarantine_directory / issue.name,
                .bytes_copied = *copied,
            });
        }

        for (const auto& entry : catalog->manifest.segments) {
            const SegmentHeaderIdentity identity{.store_id = catalog->manifest.store_id,
                                                 .segment_id = entry.segment_id,
                                                 .generation = entry.generation,
                                                 .owner_worker = entry.owner_worker,
                                                 .format_version = catalog->manifest.format_version};
            const auto name = segment_filename(identity);
            auto copied = copy_named_file(source_root.get(), destination_root.get(), name.c_str());
            if (!copied) {
                return unexpected(copied.error());
            }
            ++report.catalog_files_copied;
            report.catalog_bytes_copied += *copied;
        }
        auto manifest_copied = copy_named_file(source_root.get(), destination_root.get(), kManifestFilename);
        if (!manifest_copied) {
            return unexpected(manifest_copied.error());
        }
        ++report.catalog_files_copied;
        report.catalog_bytes_copied += *manifest_copied;

        if (auto synced = destination_root.sync(FileSyncMode::full); !synced) {
            return unexpected(synced.error());
        }
        if (auto synced = quarantine_root.sync(FileSyncMode::full); !synced) {
            return unexpected(synced.error());
        }

        if (auto audit = write_quarantine_audit(report.quarantine_directory / "audit.txt", report); !audit) {
            return unexpected(audit.error());
        }
        if (auto synced = quarantine_root.sync(FileSyncMode::full); !synced) {
            return unexpected(synced.error());
        }
    }

    auto repaired = verify_durable_store_path(report.repaired_store, scan_records, limits);
    if (!repaired) {
        return unexpected(repaired.error());
    }
    report.repaired_verification = std::move(*repaired);
    return report;
}

} // namespace glifistore
