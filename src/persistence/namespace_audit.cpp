#include "glifistore/persistence/namespace_audit.hpp"

#include "glifistore/persistence/segment_file.hpp"
#include "system_error.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace glifistore {
namespace {

inline constexpr std::string_view kSegmentPrefix = "segment-";
inline constexpr std::string_view kSegmentSuffix = ".glifi";
inline constexpr std::string_view kTemporaryPrefix = ".segment-";
inline constexpr std::string_view kTemporarySuffix = ".glifi.tmp";
// segment-<16 hex>-<8 hex>.glifi  /  .segment-<16 hex>-<8 hex>.glifi.tmp
inline constexpr std::size_t kFinalSegmentFilenameBytes =
    kSegmentPrefix.size() + 16U + 1U + 8U + kSegmentSuffix.size();
inline constexpr std::size_t kTemporarySegmentFilenameBytes =
    kTemporaryPrefix.size() + 16U + 1U + 8U + kTemporarySuffix.size();
static_assert(kFinalSegmentFilenameBytes == 39U);
static_assert(kTemporarySegmentFilenameBytes == 44U);

struct DirectoryCloser {
    void operator()(DIR* directory) const noexcept {
        if (directory) {
            static_cast<void>(::closedir(directory));
        }
    }
};

using DirectoryStream = std::unique_ptr<DIR, DirectoryCloser>;

auto is_lower_hex(const char value) noexcept -> bool {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

template <typename Integer> auto parse_fixed_hex(const std::string_view text) -> std::optional<Integer> {
    Integer value{};
    for (const auto character : text) {
        if (!is_lower_hex(character)) {
            return std::nullopt;
        }
        const auto digit = character <= '9' ? static_cast<unsigned>(character - '0')
                                            : static_cast<unsigned>(character - 'a' + 10);
        value = static_cast<Integer>((value << 4U) | static_cast<Integer>(digit));
    }
    return value;
}

auto looks_engine_owned(const std::string_view name) noexcept -> bool {
    return name.starts_with("segment-") || name.starts_with(".segment-") || name.starts_with("manifest") ||
           name.starts_with(".manifest") || name.starts_with(".glifistore");
}

auto private_regular(const struct stat& status) noexcept -> bool {
    return S_ISREG(status.st_mode) && status.st_nlink == 1 && status.st_uid == ::geteuid() &&
           (status.st_mode & (S_IRWXG | S_IRWXO)) == 0;
}

auto status_at(const int directory, const std::string& name) -> Result<struct stat> {
    struct stat status{};
    if (::fstatat(directory, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0) {
        return status;
    }
    if (errno == ENOENT) {
        return fail(ErrorCode::corrupted_data, "data-directory namespace changed while it was being audited");
    }
    return persistence_system_error("fstatat(namespace entry)");
}

auto blocking_for_recovery(const NamespaceIssueKind kind) noexcept -> bool {
    return kind != NamespaceIssueKind::stale_manifest_temporary &&
           kind != NamespaceIssueKind::stale_segment_temporary &&
           kind != NamespaceIssueKind::stale_compaction_temporary;
}

auto issue_less(const NamespaceIssue& left, const NamespaceIssue& right) noexcept -> bool {
    if (left.name != right.name) {
        return left.name < right.name;
    }
    return static_cast<int>(left.kind) < static_cast<int>(right.kind);
}

auto canonical_segment_name(const Manifest& manifest, const ManifestSegmentEntry& entry) -> std::string {
    return segment_filename({.store_id = manifest.store_id,
                             .segment_id = entry.segment_id,
                             .generation = entry.generation,
                             .owner_worker = entry.owner_worker,
                             .format_version = manifest.format_version});
}

auto append_issue(NamespaceAuditReport& report, NamespaceIssue issue) -> Status {
    if (report.issues.size() >= kMaximumNamespaceIssueCount) {
        return fail(ErrorCode::corrupted_data,
                    "data-directory namespace exceeds the bounded issue-report limit");
    }
    report.issues.push_back(std::move(issue));
    return {};
}

} // namespace

auto parse_segment_filename(const std::string_view name) -> Result<ParsedSegmentFilename> {
    const bool temporary = name.size() == kTemporarySegmentFilenameBytes &&
                           name.starts_with(kTemporaryPrefix) && name.ends_with(kTemporarySuffix);
    const bool final = name.size() == kFinalSegmentFilenameBytes && name.starts_with(kSegmentPrefix) &&
                       name.ends_with(kSegmentSuffix);
    if (!temporary && !final) {
        return fail(ErrorCode::invalid_argument, "Segment filename is not canonical");
    }

    const auto id_offset = temporary ? kTemporaryPrefix.size() : kSegmentPrefix.size();
    const auto separator = id_offset + 16U;
    if (name[separator] != '-') {
        return fail(ErrorCode::invalid_argument, "Segment filename is not canonical");
    }
    const auto id = parse_fixed_hex<std::uint64_t>(name.substr(id_offset, 16));
    const auto generation = parse_fixed_hex<std::uint32_t>(name.substr(separator + 1U, 8));
    if (!id || !generation || *id == 0 || *generation == 0) {
        return fail(ErrorCode::invalid_argument, "Segment filename contains an invalid identity");
    }
    return ParsedSegmentFilename{
        .segment_id = SegmentId{*id}, .generation = GenerationId{*generation}, .temporary = temporary};
}

auto namespace_issue_name(const NamespaceIssueKind kind) noexcept -> std::string_view {
    switch (kind) {
    case NamespaceIssueKind::stale_manifest_temporary:
        return "stale manifest temporary";
    case NamespaceIssueKind::stale_segment_temporary:
        return "stale Segment temporary";
    case NamespaceIssueKind::stale_compaction_temporary:
        return "stale compaction temporary";
    case NamespaceIssueKind::compaction_intent:
        return "compaction intent requiring recovery";
    case NamespaceIssueKind::unlisted_segment:
        return "unlisted Segment";
    case NamespaceIssueKind::malformed_engine_name:
        return "malformed engine name";
    case NamespaceIssueKind::unknown_entry:
        return "unknown entry";
    case NamespaceIssueKind::unsafe_entry:
        return "unsafe entry";
    case NamespaceIssueKind::missing_catalog_segment:
        return "missing catalog Segment";
    case NamespaceIssueKind::missing_required_entry:
        return "missing required entry";
    }
    return "unknown namespace issue";
}

auto NamespaceAuditReport::recovery_safe() const noexcept -> bool {
    return std::ranges::none_of(
        issues, [](const NamespaceIssue& issue) { return blocking_for_recovery(issue.kind); });
}

auto audit_data_directory(DataDirectory& directory, const Manifest& manifest)
    -> Result<NamespaceAuditReport> {
    if (const auto valid_manifest = encoded_manifest_size(manifest); !valid_manifest) {
        return unexpected(valid_manifest.error());
    }
    auto descriptor = directory.open_directory_for_enumeration();
    if (!descriptor) {
        return unexpected(descriptor.error());
    }
    auto* raw_stream = ::fdopendir(descriptor->get());
    if (!raw_stream) {
        return persistence_system_error("fdopendir(data directory)");
    }
    static_cast<void>(descriptor->release());
    DirectoryStream stream{raw_stream};
    const auto directory_descriptor = ::dirfd(stream.get());
    if (directory_descriptor < 0) {
        return persistence_system_error("dirfd(data directory)");
    }

    NamespaceAuditReport report;
    std::vector<bool> catalog_seen(manifest.segments.size(), false);
    bool manifest_seen{};
    bool lock_seen{};
    const auto entry_limit = manifest.segments.size() + kNamespaceAnomalyBudget + 4U;

    for (;;) {
        errno = 0;
        const auto* entry = ::readdir(stream.get());
        if (!entry) {
            if (errno != 0) {
                return persistence_system_error("readdir(data directory)");
            }
            break;
        }
        const std::string name{entry->d_name};
        if (name == "." || name == "..") {
            continue;
        }
        ++report.entries_scanned;
        if (report.entries_scanned > entry_limit) {
            return fail(ErrorCode::corrupted_data,
                        "data-directory namespace exceeds the manifest-relative entry limit");
        }

        if (name == "blobs" && manifest.format_version == 2) {
            auto status = status_at(directory_descriptor, name);
            if (!status) {
                return unexpected(status.error());
            }
            if (!S_ISDIR(status->st_mode) || status->st_uid != ::geteuid() || (status->st_mode & 077) != 0) {
                return fail(ErrorCode::corrupted_data, "unsafe blob directory");
            }
            continue;
        }
        if (name == kManifestFilename || name == kStoreLockFilename || name == kManifestTemporaryFilename ||
            name == kCompactionIntentFilename || name == kCompactionTemporaryFilename) {
            const auto status = status_at(directory_descriptor, name);
            if (!status) {
                return unexpected(status.error());
            }
            if (name == kManifestFilename) {
                manifest_seen = true;
            } else if (name == kStoreLockFilename) {
                lock_seen = true;
            }
            if (!private_regular(*status)) {
                if (auto added =
                        append_issue(report, {.kind = NamespaceIssueKind::unsafe_entry, .name = name});
                    !added) {
                    return unexpected(added.error());
                }
            } else if (name == kManifestTemporaryFilename || name == kCompactionTemporaryFilename ||
                       name == kCompactionIntentFilename) {
                const auto kind =
                    name == kManifestTemporaryFilename     ? NamespaceIssueKind::stale_manifest_temporary
                    : name == kCompactionTemporaryFilename ? NamespaceIssueKind::stale_compaction_temporary
                                                           : NamespaceIssueKind::compaction_intent;
                if (auto added = append_issue(report, {.kind = kind, .name = name}); !added) {
                    return unexpected(added.error());
                }
            }
            continue;
        }

        const auto parsed = parse_segment_filename(name);
        if (parsed) {
            const auto status = status_at(directory_descriptor, name);
            if (!status) {
                return unexpected(status.error());
            }
            const auto found =
                std::lower_bound(manifest.segments.begin(), manifest.segments.end(), parsed->segment_id,
                                 [](const ManifestSegmentEntry& candidate, const SegmentId id) {
                                     return candidate.segment_id.value < id.value;
                                 });
            const bool listed = found != manifest.segments.end() && found->segment_id == parsed->segment_id &&
                                found->generation == parsed->generation;
            if (!parsed->temporary && listed) {
                const auto index = static_cast<std::size_t>(found - manifest.segments.begin());
                if (!catalog_seen[index]) {
                    catalog_seen[index] = true;
                    ++report.catalog_segments_seen;
                }
            }

            NamespaceIssueKind issue_kind{};
            bool has_issue{};
            if (!private_regular(*status)) {
                issue_kind = NamespaceIssueKind::unsafe_entry;
                has_issue = true;
            } else if (parsed->temporary) {
                issue_kind = NamespaceIssueKind::stale_segment_temporary;
                has_issue = true;
            } else if (!listed) {
                issue_kind = NamespaceIssueKind::unlisted_segment;
                has_issue = true;
            }
            if (has_issue) {
                if (auto added = append_issue(report, {.kind = issue_kind,
                                                       .name = name,
                                                       .segment_id = parsed->segment_id,
                                                       .generation = parsed->generation});
                    !added) {
                    return unexpected(added.error());
                }
            }
            continue;
        }

        const auto status = status_at(directory_descriptor, name);
        if (!status) {
            return unexpected(status.error());
        }
        if (!private_regular(*status)) {
            if (auto added = append_issue(report, {.kind = NamespaceIssueKind::unsafe_entry, .name = name});
                !added) {
                return unexpected(added.error());
            }
            continue;
        }

        const auto issue_kind = looks_engine_owned(name) ? NamespaceIssueKind::malformed_engine_name
                                                         : NamespaceIssueKind::unknown_entry;
        if (auto added = append_issue(report, {.kind = issue_kind, .name = name}); !added) {
            return unexpected(added.error());
        }
    }

    if (!manifest_seen) {
        if (auto added = append_issue(
                report, {.kind = NamespaceIssueKind::missing_required_entry, .name = kManifestFilename});
            !added) {
            return unexpected(added.error());
        }
    }
    if (!lock_seen) {
        if (auto added = append_issue(
                report, {.kind = NamespaceIssueKind::missing_required_entry, .name = kStoreLockFilename});
            !added) {
            return unexpected(added.error());
        }
    }
    for (std::size_t index = 0; index < manifest.segments.size(); ++index) {
        if (catalog_seen[index]) {
            continue;
        }
        const auto& catalog_entry = manifest.segments[index];
        if (auto added = append_issue(report, {.kind = NamespaceIssueKind::missing_catalog_segment,
                                               .name = canonical_segment_name(manifest, catalog_entry),
                                               .segment_id = catalog_entry.segment_id,
                                               .generation = catalog_entry.generation});
            !added) {
            return unexpected(added.error());
        }
    }

    std::sort(report.issues.begin(), report.issues.end(), issue_less);
    return report;
}

auto validate_namespace_for_recovery(const NamespaceAuditReport& report) -> Status {
    const auto first =
        std::find_if(report.issues.begin(), report.issues.end(),
                     [](const NamespaceIssue& issue) { return blocking_for_recovery(issue.kind); });
    if (first == report.issues.end()) {
        return {};
    }
    const auto blocking_count = static_cast<std::size_t>(
        std::count_if(report.issues.begin(), report.issues.end(),
                      [](const NamespaceIssue& issue) { return blocking_for_recovery(issue.kind); }));
    return fail(ErrorCode::corrupted_data,
                "data-directory namespace has " + std::to_string(blocking_count) +
                    " blocking issue(s); first: " + std::string{namespace_issue_name(first->kind)} + " '" +
                    first->name + "'");
}

auto validate_namespace_for_compaction_recovery(const NamespaceAuditReport& report, const Manifest& authority,
                                                const DurableCompactionIntent& intent) -> Status {
    const bool old_authority = authority == intent.old_manifest;
    const bool next_authority = authority == intent.next_manifest;
    if (!old_authority && !next_authority) {
        return fail(ErrorCode::corrupted_data,
                    "compaction intent matches neither authoritative manifest generation");
    }

    std::vector<std::string> allowed_unlisted;
    const auto& obsolete = old_authority ? intent.next_manifest.segments : intent.old_manifest.segments;
    const auto& listed = old_authority ? intent.old_manifest.segments : intent.next_manifest.segments;
    for (const auto& entry : obsolete) {
        const auto same_identity = std::ranges::find(listed, entry);
        if (same_identity == listed.end()) {
            allowed_unlisted.push_back(
                canonical_segment_name(old_authority ? intent.next_manifest : intent.old_manifest, entry));
        }
    }
    std::ranges::sort(allowed_unlisted);

    bool saw_intent{};
    for (const auto& issue : report.issues) {
        if (issue.kind == NamespaceIssueKind::stale_manifest_temporary ||
            issue.kind == NamespaceIssueKind::stale_segment_temporary ||
            issue.kind == NamespaceIssueKind::stale_compaction_temporary) {
            continue;
        }
        if (issue.kind == NamespaceIssueKind::compaction_intent && issue.name == kCompactionIntentFilename &&
            !saw_intent) {
            saw_intent = true;
            continue;
        }
        if (issue.kind == NamespaceIssueKind::unlisted_segment &&
            std::ranges::binary_search(allowed_unlisted, issue.name)) {
            continue;
        }
        return fail(ErrorCode::corrupted_data, "compaction recovery namespace has blocking " +
                                                   std::string{namespace_issue_name(issue.kind)} + " '" +
                                                   issue.name + "'");
    }
    if (!saw_intent) {
        return fail(ErrorCode::corrupted_data,
                    "compaction recovery namespace is missing its canonical intent");
    }
    return {};
}

} // namespace glifistore
