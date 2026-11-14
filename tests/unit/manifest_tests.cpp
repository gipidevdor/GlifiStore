#include "glifistore/persistence/manifest.hpp"
#include "glifistore/segment/crc32c.hpp"
#include "hex_fixture.hpp"
#include "test.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

namespace {

inline constexpr std::size_t kManifestChecksumOffset = 80;

auto fixture_manifest() -> glifistore::Manifest {
    return {
        .store_id = {std::byte{0x00}, std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44},
                     std::byte{0x55}, std::byte{0x66}, std::byte{0x77}, std::byte{0x88}, std::byte{0x99},
                     std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}, std::byte{0xDD}, std::byte{0xEE},
                     std::byte{0xFF}},
        .manifest_generation = 0x0102030405060708ULL,
        .routing_algorithm = glifistore::RoutingAlgorithm::fnv1a64_v1,
        .worker_count = 2,
        .routing_epoch = 0x1112131415161718ULL,
        .next_segment_id = glifistore::SegmentId{4},
        .next_segment_generation = glifistore::GenerationId{1},
        .segments =
            {
                {.segment_id = glifistore::SegmentId{1},
                 .generation = glifistore::GenerationId{1},
                 .owner_worker = glifistore::WorkerId{0},
                 .role = glifistore::ManifestSegmentRole::sealed},
                {.segment_id = glifistore::SegmentId{2},
                 .generation = glifistore::GenerationId{1},
                 .owner_worker = glifistore::WorkerId{0},
                 .role = glifistore::ManifestSegmentRole::active},
                {.segment_id = glifistore::SegmentId{3},
                 .generation = glifistore::GenerationId{2},
                 .owner_worker = glifistore::WorkerId{1},
                 .role = glifistore::ManifestSegmentRole::active},
            },
    };
}

void put_u32(std::span<std::byte> out, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        out[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
}

void refresh_checksum(std::span<std::byte> bytes) {
    put_u32(bytes, kManifestChecksumOffset, 0);
    put_u32(bytes, kManifestChecksumOffset, glifistore::crc32c(bytes));
}

} // namespace

GLIFI_TEST("manifest v1 matches its golden little-endian fixture") {
    const auto manifest = fixture_manifest();
    const auto encoded = glifistore::encode_manifest(manifest);
    GLIFI_REQUIRE(encoded.has_value());
    const auto fixture = glifistore::test::read_hex_fixture(std::filesystem::path{GLIFISTORE_SOURCE_DIR} /
                                                            "tests/fixtures/manifest_v1.hex");
    GLIFI_REQUIRE(*encoded == fixture);

    const auto decoded = glifistore::decode_manifest(*encoded);
    GLIFI_REQUIRE(decoded.has_value());
    GLIFI_REQUIRE(*decoded == manifest);
}

GLIFI_TEST("manifest decoder rejects truncation trailing bytes and checksum corruption") {
    auto encoded = glifistore::encode_manifest(fixture_manifest());
    GLIFI_REQUIRE(encoded.has_value());

    const auto truncated = glifistore::decode_manifest(
        std::span<const std::byte>{*encoded}.first(glifistore::kManifestHeaderBytes - 1));
    GLIFI_REQUIRE(!truncated.has_value());
    GLIFI_REQUIRE(truncated.error().code == glifistore::ErrorCode::invalid_record);

    encoded->push_back(std::byte{0});
    const auto trailing = glifistore::decode_manifest(*encoded);
    GLIFI_REQUIRE(!trailing.has_value());
    GLIFI_REQUIRE(trailing.error().code == glifistore::ErrorCode::invalid_record);
    encoded->pop_back();

    (*encoded)[glifistore::kManifestHeaderBytes] ^= std::byte{1};
    const auto corrupted = glifistore::decode_manifest(*encoded);
    GLIFI_REQUIRE(!corrupted.has_value());
    GLIFI_REQUIRE(corrupted.error().code == glifistore::ErrorCode::checksum_mismatch);
}

GLIFI_TEST("manifest encoder enforces canonical catalog and active ownership") {
    auto manifest = fixture_manifest();
    manifest.segments[1].segment_id = manifest.segments[0].segment_id;
    GLIFI_REQUIRE(!glifistore::encode_manifest(manifest).has_value());

    manifest = fixture_manifest();
    manifest.segments.back().role = glifistore::ManifestSegmentRole::sealed;
    GLIFI_REQUIRE(!glifistore::encode_manifest(manifest).has_value());

    manifest = fixture_manifest();
    manifest.segments.back().owner_worker = glifistore::WorkerId{2};
    GLIFI_REQUIRE(!glifistore::encode_manifest(manifest).has_value());

    manifest = fixture_manifest();
    manifest.next_segment_id = manifest.segments.back().segment_id;
    GLIFI_REQUIRE(!glifistore::encode_manifest(manifest).has_value());

    manifest = fixture_manifest();
    manifest.worker_count = 0;
    GLIFI_REQUIRE(!glifistore::encode_manifest(manifest).has_value());
}

GLIFI_TEST("manifest decoder distinguishes structural and semantic corruption") {
    auto encoded = glifistore::encode_manifest(fixture_manifest());
    GLIFI_REQUIRE(encoded.has_value());

    const auto third_owner_offset =
        glifistore::kManifestHeaderBytes + 2 * glifistore::kManifestSegmentEntryBytes + 12;
    put_u32(*encoded, third_owner_offset, 2);
    refresh_checksum(*encoded);
    const auto invalid_owner = glifistore::decode_manifest(*encoded);
    GLIFI_REQUIRE(!invalid_owner.has_value());
    GLIFI_REQUIRE(invalid_owner.error().code == glifistore::ErrorCode::corrupted_data);

    encoded = glifistore::encode_manifest(fixture_manifest());
    GLIFI_REQUIRE(encoded.has_value());
    (*encoded)[glifistore::kManifestHeaderBytes + 20] = std::byte{1};
    refresh_checksum(*encoded);
    const auto reserved = glifistore::decode_manifest(*encoded);
    GLIFI_REQUIRE(!reserved.has_value());
    GLIFI_REQUIRE(reserved.error().code == glifistore::ErrorCode::invalid_record);
}

GLIFI_TEST("manifest decoder fails closed for supported-checksum unknown versions") {
    auto encoded = glifistore::encode_manifest(fixture_manifest());
    GLIFI_REQUIRE(encoded.has_value());
    (*encoded)[4] = std::byte{2};
    refresh_checksum(*encoded);
    const auto future = glifistore::decode_manifest(*encoded);
    GLIFI_REQUIRE(!future.has_value());
    GLIFI_REQUIRE(future.error().code == glifistore::ErrorCode::invalid_record);

    encoded = glifistore::encode_manifest(fixture_manifest());
    GLIFI_REQUIRE(encoded.has_value());
    put_u32(*encoded, 56, static_cast<std::uint32_t>(glifistore::kMaximumManifestSegmentCount + 1));
    const auto excessive = glifistore::decode_manifest(*encoded);
    GLIFI_REQUIRE(!excessive.has_value());
    GLIFI_REQUIRE(excessive.error().code == glifistore::ErrorCode::invalid_record);
}

GLIFI_TEST("manifest publication selection uses generation and rejects ambiguity") {
    auto older = fixture_manifest();
    older.manifest_generation = 4;
    auto newest = fixture_manifest();
    newest.manifest_generation = 9;
    auto middle = fixture_manifest();
    middle.manifest_generation = 7;
    const std::array candidates{older, newest, middle};
    const auto selected = glifistore::select_newest_manifest(candidates);
    GLIFI_REQUIRE(selected.has_value());
    GLIFI_REQUIRE(selected->candidate_index == 1);
    GLIFI_REQUIRE(selected->manifest_generation == 9);

    auto conflict = newest;
    conflict.routing_epoch += 1;
    const std::array conflicting{newest, conflict};
    const auto ambiguous = glifistore::select_newest_manifest(conflicting);
    GLIFI_REQUIRE(!ambiguous.has_value());
    GLIFI_REQUIRE(ambiguous.error().code == glifistore::ErrorCode::corrupted_data);

    const std::array identical{newest, newest};
    const auto duplicate = glifistore::select_newest_manifest(identical);
    GLIFI_REQUIRE(duplicate.has_value());
    GLIFI_REQUIRE(duplicate->candidate_index == 0);
}
