#include "glifistore/persistence/segment_file.hpp"
#include "glifistore/persistence/store_verify.hpp"
#include "glifistore/segment/record.hpp"
#include "test.hpp"

#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

class VerifyTemporaryDirectory final {
  public:
    VerifyTemporaryDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "glifistore-verify-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* created = ::mkdtemp(writable.data());
        GLIFI_REQUIRE(created != nullptr);
        path_ = created;
    }

    ~VerifyTemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] auto path() const -> const std::filesystem::path& {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

auto store_id() -> glifistore::StoreId {
    return {std::byte{0x21}, std::byte{0x22}, std::byte{0x23}, std::byte{0x24},
            std::byte{0x25}, std::byte{0x26}, std::byte{0x27}, std::byte{0x28},
            std::byte{0x29}, std::byte{0x2A}, std::byte{0x2B}, std::byte{0x2C},
            std::byte{0x2D}, std::byte{0x2E}, std::byte{0x2F}, std::byte{0x30}};
}

auto identity_for(const glifistore::Manifest& manifest, const glifistore::ManifestSegmentEntry& entry)
    -> glifistore::SegmentHeaderIdentity {
    return {.store_id = manifest.store_id,
            .segment_id = entry.segment_id,
            .generation = entry.generation,
            .owner_worker = entry.owner_worker};
}

auto single_active_manifest() -> glifistore::Manifest {
    return {
        .store_id = store_id(),
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

auto encoded_record(std::uint64_t sequence, std::string key, std::string value)
    -> glifistore::Result<std::vector<std::byte>> {
    return glifistore::encode_record({
        .sequence = glifistore::SequenceNumber{sequence},
        .opcode = glifistore::Opcode::put,
        .type = glifistore::ValueType::bytes,
        .flags = 0,
        .key_hash = sequence * 17,
        .expire_at_ns = 0,
        .key = std::as_bytes(std::span{key}),
        .value = std::as_bytes(std::span{value}),
    });
}

} // namespace

GLIFI_TEST("verify_durable_store accepts a locked catalog with a valid active Segment") {
    VerifyTemporaryDirectory temporary;
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        auto manifest = single_active_manifest();
        GLIFI_REQUIRE(directory->publish_manifest(manifest).durable());
        auto created =
            glifistore::DurableSegmentFile::create(*directory, identity_for(manifest, manifest.segments[0]));
        GLIFI_REQUIRE(created.durable());
        const auto record = encoded_record(1, "verify", "ok");
        GLIFI_REQUIRE(record.has_value());
        GLIFI_REQUIRE(created.file->append(*record).committed());
    }

    const auto report = glifistore::verify_durable_store_path(temporary.path());
    GLIFI_REQUIRE(report.has_value());
    GLIFI_REQUIRE(report->manifest.segments.size() == 1);
    GLIFI_REQUIRE(report->segments.size() == 1);
    GLIFI_REQUIRE(report->scanned_records == 1);
    GLIFI_REQUIRE(report->namespace_audit.recovery_safe());
    GLIFI_REQUIRE(report->active_requires_rotation_count == 0);
    GLIFI_REQUIRE(report->segments[0].selected.commit.record_count == 1);
}

GLIFI_TEST("verify_durable_store --no-scan skips committed Record corruption") {
    VerifyTemporaryDirectory temporary;
    glifistore::Manifest manifest;
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        manifest = single_active_manifest();
        GLIFI_REQUIRE(directory->publish_manifest(manifest).durable());
        auto created =
            glifistore::DurableSegmentFile::create(*directory, identity_for(manifest, manifest.segments[0]));
        GLIFI_REQUIRE(created.durable());
        const auto record = encoded_record(2, "corrupt", "later");
        GLIFI_REQUIRE(record.has_value());
        GLIFI_REQUIRE(created.file->append(*record).committed());
    }

    const auto path =
        temporary.path() / glifistore::segment_filename(identity_for(manifest, manifest.segments[0]));
    glifistore::FileDescriptor raw{::open(path.c_str(), O_RDWR | O_CLOEXEC)};
    GLIFI_REQUIRE(raw.valid());
    const std::byte corrupt{0x00};
    GLIFI_REQUIRE(
        raw.write_all_at(std::span{&corrupt, 1}, glifistore::kSegmentHeaderReservedBytes).has_value());
    GLIFI_REQUIRE(raw.sync(glifistore::FileSyncMode::data).has_value());
    raw.reset();

    const auto header_only = glifistore::verify_durable_store_path(temporary.path(), false);
    GLIFI_REQUIRE(header_only.has_value());

    const auto scanned = glifistore::verify_durable_store_path(temporary.path(), true);
    GLIFI_REQUIRE(!scanned.has_value());
    GLIFI_REQUIRE(scanned.error().code == glifistore::ErrorCode::invalid_record ||
                  scanned.error().code == glifistore::ErrorCode::checksum_mismatch);
}

GLIFI_TEST("verify_durable_store fails on unlisted Segment files") {
    VerifyTemporaryDirectory temporary;
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        auto manifest = single_active_manifest();
        GLIFI_REQUIRE(directory->publish_manifest(manifest).durable());
        auto created =
            glifistore::DurableSegmentFile::create(*directory, identity_for(manifest, manifest.segments[0]));
        GLIFI_REQUIRE(created.durable());
        auto extra =
            glifistore::DurableSegmentFile::create(*directory, {.store_id = manifest.store_id,
                                                                .segment_id = glifistore::SegmentId{9},
                                                                .generation = glifistore::GenerationId{1},
                                                                .owner_worker = glifistore::WorkerId{0}});
        GLIFI_REQUIRE(extra.durable());
    }

    const auto report = glifistore::verify_durable_store_path(temporary.path());
    GLIFI_REQUIRE(!report.has_value());
    GLIFI_REQUIRE(report.error().code == glifistore::ErrorCode::corrupted_data ||
                  report.error().code == glifistore::ErrorCode::invalid_argument);
}

GLIFI_TEST("verify_durable_store reports sealed-active Segments as active_requires_rotation") {
    VerifyTemporaryDirectory temporary;
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        auto manifest = single_active_manifest();
        GLIFI_REQUIRE(directory->publish_manifest(manifest).durable());
        auto created =
            glifistore::DurableSegmentFile::create(*directory, identity_for(manifest, manifest.segments[0]));
        GLIFI_REQUIRE(created.durable());
        GLIFI_REQUIRE(created.file->seal().committed());
    }

    const auto report = glifistore::verify_durable_store_path(temporary.path());
    GLIFI_REQUIRE(report.has_value());
    GLIFI_REQUIRE(report->active_requires_rotation_count == 1);
    GLIFI_REQUIRE(report->segments[0].active_requires_rotation);
}

GLIFI_TEST("verify_durable_store fails when the data directory is already locked") {
    VerifyTemporaryDirectory temporary;
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        auto manifest = single_active_manifest();
        GLIFI_REQUIRE(directory->publish_manifest(manifest).durable());
        auto created =
            glifistore::DurableSegmentFile::create(*directory, identity_for(manifest, manifest.segments[0]));
        GLIFI_REQUIRE(created.durable());

        const auto contested = glifistore::verify_durable_store_path(temporary.path());
        GLIFI_REQUIRE(!contested.has_value());
        GLIFI_REQUIRE(contested.error().code == glifistore::ErrorCode::io_error);
        GLIFI_REQUIRE(contested.error().message.find("already locked") != std::string::npos);
    }
}
