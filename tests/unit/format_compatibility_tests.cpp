#include "glifistore/persistence/compaction_intent.hpp"
#include "glifistore/persistence/filesystem.hpp"
#include "glifistore/persistence/manifest.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "glifistore/segment/record.hpp"
#include "glifistore/segment/segment_header.hpp"
#include "glifistore/store/config.hpp"
#include "glifistore/store/store.hpp"
#include "hex_fixture.hpp"
#include "test.hpp"

#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

[[nodiscard]] auto fixture_root() -> std::filesystem::path {
    return std::filesystem::path{GLIFISTORE_SOURCE_DIR} / "tests/fixtures";
}

[[nodiscard]] auto bytes(std::string_view value) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

class CompatibilityTemporaryDirectory final {
  public:
    CompatibilityTemporaryDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "glifistore-compat-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* created = ::mkdtemp(writable.data());
        GLIFI_REQUIRE(created != nullptr);
        root_ = created;
    }

    ~CompatibilityTemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] auto store_path() const -> std::filesystem::path {
        return root_ / "store";
    }

  private:
    std::filesystem::path root_;
};

} // namespace

GLIFI_TEST("format fixtures decode independently without encoder round-trip") {
    const auto manifest_bytes = glifistore::test::read_hex_fixture(fixture_root() / "manifest_v1.hex");
    const auto decoded_manifest = glifistore::decode_manifest(manifest_bytes);
    GLIFI_REQUIRE(decoded_manifest.has_value());
    GLIFI_REQUIRE(decoded_manifest->manifest_generation == 0x0102030405060708ULL);
    GLIFI_REQUIRE(decoded_manifest->segments.size() == 3);

    const auto intent_bytes = glifistore::test::read_hex_fixture(fixture_root() / "compaction_intent_v1.hex");
    const auto decoded_intent = glifistore::decode_compaction_intent(intent_bytes);
    GLIFI_REQUIRE(decoded_intent.has_value());
    GLIFI_REQUIRE(decoded_intent->worker_id == glifistore::WorkerId{0});
    GLIFI_REQUIRE(decoded_intent->old_manifest.manifest_generation == 9);
    GLIFI_REQUIRE(decoded_intent->old_manifest.segments.size() == 4);
    GLIFI_REQUIRE(decoded_intent->next_manifest.manifest_generation == 10);
    GLIFI_REQUIRE(decoded_intent->next_manifest.segments.size() == 2);
    GLIFI_REQUIRE(decoded_intent->next_manifest.segments.front().segment_id == glifistore::SegmentId{1});
    GLIFI_REQUIRE(decoded_intent->next_manifest.segments.front().generation == glifistore::GenerationId{2});

    const auto header_bytes = glifistore::test::read_hex_fixture(fixture_root() / "segment_header_v1.hex");
    GLIFI_REQUIRE(header_bytes.size() ==
                  glifistore::kSegmentCommitSlotsOffset +
                      glifistore::kSegmentCommitSlotCount * glifistore::kSegmentCommitSlotBytes);
    std::array<std::byte, glifistore::kSegmentHeaderReservedBytes> encoded{};
    GLIFI_REQUIRE(header_bytes.size() <= encoded.size());
    std::copy(header_bytes.begin(), header_bytes.end(), encoded.begin());
    const auto decoded_header = glifistore::decode_segment_header(encoded);
    GLIFI_REQUIRE(decoded_header.has_value());
    const auto selected = glifistore::select_newest_segment_commit(*decoded_header);
    GLIFI_REQUIRE(selected.has_value());
    GLIFI_REQUIRE(selected->commit.commit_generation == 2);

    const auto record_bytes = glifistore::test::read_hex_fixture(fixture_root() / "record_v1.hex");
    const auto decoded_record = glifistore::decode_record(record_bytes);
    GLIFI_REQUIRE(decoded_record.has_value());
    GLIFI_REQUIRE(decoded_record->sequence.value == 0x0102030405060708ULL);

    const auto segment_header_region =
        glifistore::test::read_hex_fixture(fixture_root() / "segment_v1_header.hex");
    GLIFI_REQUIRE(segment_header_region.size() ==
                  glifistore::kSegmentCommitSlotsOffset +
                      glifistore::kSegmentCommitSlotCount * glifistore::kSegmentCommitSlotBytes);
    std::array<std::byte, glifistore::kSegmentHeaderReservedBytes> segment_container{};
    std::copy(segment_header_region.begin(), segment_header_region.end(), segment_container.begin());
    const auto decoded_segment_container = glifistore::decode_segment_header(segment_container);
    GLIFI_REQUIRE(decoded_segment_container.has_value());
}

GLIFI_TEST("durable store artifact survives simulated upgrade reopen") {
    CompatibilityTemporaryDirectory temporary;
    constexpr std::string_view kKey{"compat-key"};
    constexpr std::string_view kValue{"compat-value-v1"};
    {
        auto writer = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                               .storage_mode = glifistore::StorageMode::durable_sync,
                                               .data_directory = temporary.store_path(),
                                               .durable_open_mode = glifistore::DurableOpenMode::create_new});
        GLIFI_REQUIRE(writer.has_value());
        GLIFI_REQUIRE((*writer)->put(kKey, bytes(kValue)).has_value());
        GLIFI_REQUIRE((*writer)->verify_index().has_value());
    }

    {
        auto reader =
            glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                     .storage_mode = glifistore::StorageMode::durable_sync,
                                     .data_directory = temporary.store_path(),
                                     .durable_open_mode = glifistore::DurableOpenMode::open_existing});
        GLIFI_REQUIRE(reader.has_value());
        const auto value = (*reader)->get(kKey);
        GLIFI_REQUIRE(value.has_value());
        GLIFI_REQUIRE(value->bytes.size() == kValue.size());
        GLIFI_REQUIRE(
            std::equal(value->bytes.begin(), value->bytes.end(), bytes(kValue).begin(), bytes(kValue).end()));
        GLIFI_REQUIRE((*reader)->verify_index().has_value());
    }

    const auto manifest = glifistore::DataDirectory::open_and_lock(temporary.store_path());
    GLIFI_REQUIRE(manifest.has_value());
    const auto on_disk = manifest->read_manifest();
    GLIFI_REQUIRE(on_disk.has_value());
    GLIFI_REQUIRE(on_disk->segments.size() == 1);
}

GLIFI_TEST("segment container header fixture matches on-disk durable segment prefix") {
    CompatibilityTemporaryDirectory temporary;
    const glifistore::StoreId store_id{std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13},
                                       std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17},
                                       std::byte{0x18}, std::byte{0x19}, std::byte{0x1A}, std::byte{0x1B},
                                       std::byte{0x1C}, std::byte{0x1D}, std::byte{0x1E}, std::byte{0x1F}};
    const glifistore::ManifestSegmentEntry active{
        .segment_id = glifistore::SegmentId{1},
        .generation = glifistore::GenerationId{1},
        .owner_worker = glifistore::WorkerId{0},
        .role = glifistore::ManifestSegmentRole::active,
    };
    {
        auto directory = glifistore::DataDirectory::open_and_lock(
            temporary.store_path(), glifistore::DataDirectoryOpenMode::create_new);
        GLIFI_REQUIRE(directory.has_value());
        auto created =
            glifistore::DurableSegmentFile::create(*directory, {.store_id = store_id,
                                                                .segment_id = active.segment_id,
                                                                .generation = active.generation,
                                                                .owner_worker = active.owner_worker});
        GLIFI_REQUIRE(created.durable());
        const glifistore::Manifest manifest{
            .store_id = store_id,
            .manifest_generation = 1,
            .routing_algorithm = glifistore::RoutingAlgorithm::fnv1a64_v1,
            .worker_count = 1,
            .routing_epoch = 1,
            .next_segment_id = glifistore::SegmentId{2},
            .next_segment_generation = glifistore::GenerationId{1},
            .segments = {active},
        };
        GLIFI_REQUIRE(directory->publish_manifest(manifest).durable());
    }

    const auto segment_name = glifistore::segment_filename({.store_id = store_id,
                                                            .segment_id = active.segment_id,
                                                            .generation = active.generation,
                                                            .owner_worker = active.owner_worker});
    const auto segment_path = temporary.store_path() / segment_name;
    std::ifstream stream(segment_path, std::ios::binary);
    GLIFI_REQUIRE(stream.is_open());
    std::vector<std::byte> prefix(glifistore::kSegmentHeaderReservedBytes);
    stream.read(reinterpret_cast<char*>(prefix.data()), static_cast<std::streamsize>(prefix.size()));
    GLIFI_REQUIRE(stream.gcount() == static_cast<std::streamsize>(prefix.size()));

    const auto decoded = glifistore::decode_segment_header(prefix);
    GLIFI_REQUIRE(decoded.has_value());
    GLIFI_REQUIRE(decoded->identity.store_id == store_id);
    GLIFI_REQUIRE(decoded->identity.segment_id == active.segment_id);
    GLIFI_REQUIRE(std::filesystem::file_size(segment_path) == glifistore::kSegmentSizeBytes);
}
