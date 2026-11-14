#include "glifistore/persistence/namespace_audit.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "test.hpp"

#include <cstddef>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

class NamespaceTemporaryDirectory final {
  public:
    NamespaceTemporaryDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "glifistore-namespace-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* created = ::mkdtemp(writable.data());
        GLIFI_REQUIRE(created != nullptr);
        path_ = created;
    }

    ~NamespaceTemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] auto path() const -> const std::filesystem::path& {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

auto namespace_store_id() -> glifistore::StoreId {
    return {std::byte{0x31}, std::byte{0x32}, std::byte{0x33}, std::byte{0x34},
            std::byte{0x35}, std::byte{0x36}, std::byte{0x37}, std::byte{0x38},
            std::byte{0x39}, std::byte{0x3A}, std::byte{0x3B}, std::byte{0x3C},
            std::byte{0x3D}, std::byte{0x3E}, std::byte{0x3F}, std::byte{0x40}};
}

auto namespace_manifest() -> glifistore::Manifest {
    return {
        .store_id = namespace_store_id(),
        .manifest_generation = 1,
        .worker_count = 1,
        .routing_epoch = 1,
        .next_segment_id = glifistore::SegmentId{2},
        .next_segment_generation = glifistore::GenerationId{1},
        .segments = {{.segment_id = glifistore::SegmentId{1},
                      .generation = glifistore::GenerationId{1},
                      .owner_worker = glifistore::WorkerId{0},
                      .role = glifistore::ManifestSegmentRole::active}},
    };
}

auto segment_name(const glifistore::Manifest& manifest, glifistore::SegmentId id,
                  glifistore::GenerationId generation) -> std::string {
    return glifistore::segment_filename({.store_id = manifest.store_id,
                                         .segment_id = id,
                                         .generation = generation,
                                         .owner_worker = glifistore::WorkerId{0}});
}

void create_private_file(const std::filesystem::path& path) {
    const auto descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    GLIFI_REQUIRE(descriptor >= 0);
    GLIFI_REQUIRE(::close(descriptor) == 0);
}

auto prepare_catalog(NamespaceTemporaryDirectory& temporary)
    -> std::pair<glifistore::DataDirectory, glifistore::Manifest> {
    auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
    GLIFI_REQUIRE(directory.has_value());
    auto manifest = namespace_manifest();
    GLIFI_REQUIRE(directory->publish_manifest(manifest).durable());
    create_private_file(temporary.path() /
                        segment_name(manifest, glifistore::SegmentId{1}, glifistore::GenerationId{1}));
    return {std::move(*directory), std::move(manifest)};
}

} // namespace

GLIFI_TEST("Segment filename parser accepts only exact lowercase non-zero identities") {
    const auto parsed = glifistore::parse_segment_filename("segment-0123456789abcdef-0000002a.glifi");
    GLIFI_REQUIRE(parsed.has_value());
    GLIFI_REQUIRE(parsed->segment_id.value == 0x0123456789ABCDEFULL);
    GLIFI_REQUIRE(parsed->generation.value == 0x2AU);
    GLIFI_REQUIRE(!parsed->temporary);

    const auto temporary = glifistore::parse_segment_filename(".segment-0123456789abcdef-0000002a.glifi.tmp");
    GLIFI_REQUIRE(temporary.has_value());
    GLIFI_REQUIRE(temporary->temporary);
    GLIFI_REQUIRE(!glifistore::parse_segment_filename("segment-0123456789abcdeF-0000002a.glifi").has_value());
    GLIFI_REQUIRE(!glifistore::parse_segment_filename("segment-0000000000000000-0000002a.glifi").has_value());
    GLIFI_REQUIRE(!glifistore::parse_segment_filename("segment-0123456789abcdef-00000000.glifi").has_value());
}

GLIFI_TEST("namespace audit is repeatable and tolerates only canonical crash temporaries") {
    NamespaceTemporaryDirectory temporary;
    auto [directory, manifest] = prepare_catalog(temporary);

    const auto clean = glifistore::audit_data_directory(directory, manifest);
    GLIFI_REQUIRE(clean.has_value());
    GLIFI_REQUIRE(clean->clean());
    GLIFI_REQUIRE(clean->entries_scanned == 3);
    GLIFI_REQUIRE(clean->catalog_segments_seen == 1);

    create_private_file(temporary.path() / glifistore::kManifestTemporaryFilename);
    create_private_file(
        temporary.path() /
        ('.' + segment_name(manifest, glifistore::SegmentId{1}, glifistore::GenerationId{1}) + ".tmp"));
    const auto first = glifistore::audit_data_directory(directory, manifest);
    const auto second = glifistore::audit_data_directory(directory, manifest);
    GLIFI_REQUIRE(first.has_value());
    GLIFI_REQUIRE(second.has_value());
    GLIFI_REQUIRE(first->entries_scanned == 5);
    GLIFI_REQUIRE(second->entries_scanned == first->entries_scanned);
    GLIFI_REQUIRE(first->issues.size() == 2);
    GLIFI_REQUIRE(first->issues[0].kind == glifistore::NamespaceIssueKind::stale_manifest_temporary);
    GLIFI_REQUIRE(first->issues[1].kind == glifistore::NamespaceIssueKind::stale_segment_temporary);
    GLIFI_REQUIRE(first->recovery_safe());
    GLIFI_REQUIRE(glifistore::validate_namespace_for_recovery(*first).has_value());
}

GLIFI_TEST("namespace audit distinguishes compaction temporary from durable recovery intent") {
    NamespaceTemporaryDirectory temporary;
    auto [directory, manifest] = prepare_catalog(temporary);
    create_private_file(temporary.path() / glifistore::kCompactionTemporaryFilename);
    auto report = glifistore::audit_data_directory(directory, manifest);
    GLIFI_REQUIRE(report.has_value());
    GLIFI_REQUIRE(report->issues.size() == 1);
    GLIFI_REQUIRE(report->issues[0].kind == glifistore::NamespaceIssueKind::stale_compaction_temporary);
    GLIFI_REQUIRE(report->recovery_safe());

    create_private_file(temporary.path() / glifistore::kCompactionIntentFilename);
    report = glifistore::audit_data_directory(directory, manifest);
    GLIFI_REQUIRE(report.has_value());
    GLIFI_REQUIRE(report->issues.size() == 2);
    GLIFI_REQUIRE(!report->recovery_safe());
    GLIFI_REQUIRE(report->issues[0].kind == glifistore::NamespaceIssueKind::compaction_intent);
    GLIFI_REQUIRE(report->issues[1].kind == glifistore::NamespaceIssueKind::stale_compaction_temporary);
}

GLIFI_TEST("namespace audit deterministically rejects unlisted malformed and unknown entries") {
    NamespaceTemporaryDirectory temporary;
    auto [directory, manifest] = prepare_catalog(temporary);
    create_private_file(temporary.path() /
                        segment_name(manifest, glifistore::SegmentId{2}, glifistore::GenerationId{7}));
    create_private_file(temporary.path() / "segment-0000000000000003-0000000A.glifi");
    create_private_file(temporary.path() / "operator-note.txt");

    const auto report = glifistore::audit_data_directory(directory, manifest);
    GLIFI_REQUIRE(report.has_value());
    GLIFI_REQUIRE(report->issues.size() == 3);
    GLIFI_REQUIRE(report->issues[0].name == "operator-note.txt");
    GLIFI_REQUIRE(report->issues[0].kind == glifistore::NamespaceIssueKind::unknown_entry);
    GLIFI_REQUIRE(report->issues[1].kind == glifistore::NamespaceIssueKind::unlisted_segment);
    GLIFI_REQUIRE(report->issues[2].kind == glifistore::NamespaceIssueKind::malformed_engine_name);
    GLIFI_REQUIRE(!report->recovery_safe());
    const auto policy = glifistore::validate_namespace_for_recovery(*report);
    GLIFI_REQUIRE(!policy.has_value());
    GLIFI_REQUIRE(policy.error().code == glifistore::ErrorCode::corrupted_data);
    GLIFI_REQUIRE(policy.error().message.find("operator-note.txt") != std::string::npos);
}

GLIFI_TEST("namespace audit reports canonical symlinks and hard links as unsafe") {
    {
        NamespaceTemporaryDirectory temporary;
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        const auto manifest = namespace_manifest();
        GLIFI_REQUIRE(directory->publish_manifest(manifest).durable());
        const auto expected = segment_name(manifest, glifistore::SegmentId{1}, glifistore::GenerationId{1});
        std::filesystem::create_symlink(glifistore::kManifestFilename, temporary.path() / expected);
        const auto report = glifistore::audit_data_directory(*directory, manifest);
        GLIFI_REQUIRE(report.has_value());
        GLIFI_REQUIRE(report->issues.size() == 1);
        GLIFI_REQUIRE(report->issues[0].kind == glifistore::NamespaceIssueKind::unsafe_entry);
        GLIFI_REQUIRE(report->catalog_segments_seen == 1);
    }
    {
        NamespaceTemporaryDirectory temporary;
        auto [directory, manifest] = prepare_catalog(temporary);
        const auto expected = segment_name(manifest, glifistore::SegmentId{1}, glifistore::GenerationId{1});
        GLIFI_REQUIRE(::link((temporary.path() / expected).c_str(),
                             (temporary.path() / "extra-hard-link").c_str()) == 0);
        const auto report = glifistore::audit_data_directory(directory, manifest);
        GLIFI_REQUIRE(report.has_value());
        GLIFI_REQUIRE(!report->recovery_safe());
        GLIFI_REQUIRE(report->issues.size() == 2);
        GLIFI_REQUIRE(report->issues[0].kind == glifistore::NamespaceIssueKind::unsafe_entry);
        GLIFI_REQUIRE(report->issues[0].name == "extra-hard-link");
        GLIFI_REQUIRE(report->issues[1].kind == glifistore::NamespaceIssueKind::unsafe_entry);
        GLIFI_REQUIRE(report->issues[1].name == expected);
    }
}

GLIFI_TEST("namespace audit reports missing catalog files and bounds hostile enumeration") {
    {
        NamespaceTemporaryDirectory temporary;
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        const auto manifest = namespace_manifest();
        GLIFI_REQUIRE(directory->publish_manifest(manifest).durable());
        const auto report = glifistore::audit_data_directory(*directory, manifest);
        GLIFI_REQUIRE(report.has_value());
        GLIFI_REQUIRE(report->issues.size() == 1);
        GLIFI_REQUIRE(report->issues[0].kind == glifistore::NamespaceIssueKind::missing_catalog_segment);
    }
    {
        NamespaceTemporaryDirectory temporary;
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        const auto manifest = namespace_manifest();
        GLIFI_REQUIRE(directory->publish_manifest(manifest).durable());
        for (std::size_t index = 0; index < glifistore::kNamespaceAnomalyBudget + 4U; ++index) {
            create_private_file(temporary.path() / ("unknown-" + std::to_string(index)));
        }
        const auto report = glifistore::audit_data_directory(*directory, manifest);
        GLIFI_REQUIRE(!report.has_value());
        GLIFI_REQUIRE(report.error().code == glifistore::ErrorCode::corrupted_data);
    }
}
