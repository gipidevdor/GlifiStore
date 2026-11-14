#include "glifistore/segment/crc32c.hpp"
#include "glifistore/segment/segment_header.hpp"
#include "hex_fixture.hpp"
#include "test.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace {

auto fixture_identity() -> glifistore::SegmentHeaderIdentity {
    return {
        .store_id = {std::byte{0x00}, std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44},
                     std::byte{0x55}, std::byte{0x66}, std::byte{0x77}, std::byte{0x88}, std::byte{0x99},
                     std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}, std::byte{0xDD}, std::byte{0xEE},
                     std::byte{0xFF}},
        .segment_id = glifistore::SegmentId{0x0102030405060708ULL},
        .generation = glifistore::GenerationId{0x11223344U},
        .owner_worker = glifistore::WorkerId{0x55667788U},
    };
}

auto empty_commit(std::uint64_t generation = 1) -> glifistore::SegmentCommit {
    return {
        .commit_generation = generation,
        .committed_end = glifistore::kSegmentHeaderReservedBytes,
        .state = glifistore::PersistedSegmentState::active,
    };
}

auto populated_commit(std::uint64_t generation = 2) -> glifistore::SegmentCommit {
    return {
        .commit_generation = generation,
        .committed_end = 0x1100,
        .state = glifistore::PersistedSegmentState::sealed,
        .record_count = 2,
        .first_sequence = glifistore::SequenceNumber{10},
        .last_sequence = glifistore::SequenceNumber{11},
    };
}

} // namespace

GLIFI_TEST("segment header v1 matches its golden little-endian fixture") {
    const glifistore::SegmentHeader header{
        .identity = fixture_identity(),
        .commits = {empty_commit(), populated_commit()},
    };
    std::array<std::byte, glifistore::kSegmentHeaderReservedBytes> encoded{};
    GLIFI_REQUIRE(glifistore::encode_segment_header(encoded, header).has_value());

    const auto fixture = glifistore::test::read_hex_fixture(std::filesystem::path{GLIFISTORE_SOURCE_DIR} /
                                                            "tests/fixtures/segment_header_v1.hex");
    GLIFI_REQUIRE(fixture.size() ==
                  glifistore::kSegmentCommitSlotsOffset +
                      glifistore::kSegmentCommitSlotCount * glifistore::kSegmentCommitSlotBytes);
    GLIFI_REQUIRE(std::equal(fixture.begin(), fixture.end(), encoded.begin()));
    GLIFI_REQUIRE(std::all_of(encoded.begin() + static_cast<std::ptrdiff_t>(fixture.size()), encoded.end(),
                              [](std::byte value) { return value == std::byte{0}; }));

    const auto decoded = glifistore::decode_segment_header(encoded);
    GLIFI_REQUIRE(decoded.has_value());
    GLIFI_REQUIRE(decoded->identity == header.identity);
    const auto selected = glifistore::select_newest_segment_commit(*decoded);
    GLIFI_REQUIRE(selected.has_value());
    GLIFI_REQUIRE(selected->slot_index == 1);
    GLIFI_REQUIRE(selected->commit == populated_commit());
}

GLIFI_TEST("segment header rejects truncation and immutable identity corruption") {
    glifistore::SegmentHeader header{
        .identity = fixture_identity(),
        .commits = {empty_commit(), std::nullopt},
    };
    std::array<std::byte, glifistore::kSegmentHeaderReservedBytes> encoded{};
    GLIFI_REQUIRE(glifistore::encode_segment_header(encoded, header).has_value());

    const auto truncated = glifistore::decode_segment_header(
        std::span<const std::byte>{encoded}.first(glifistore::kSegmentHeaderReservedBytes - 1));
    GLIFI_REQUIRE(!truncated.has_value());
    GLIFI_REQUIRE(truncated.error().code == glifistore::ErrorCode::invalid_record);

    encoded[16] ^= std::byte{1};
    const auto corrupted = glifistore::decode_segment_header(encoded);
    GLIFI_REQUIRE(!corrupted.has_value());
    GLIFI_REQUIRE(corrupted.error().code == glifistore::ErrorCode::checksum_mismatch);
}

GLIFI_TEST("segment header recovers through the older slot when the newer slot is corrupt") {
    const glifistore::SegmentHeader header{
        .identity = fixture_identity(),
        .commits = {empty_commit(4), populated_commit(5)},
    };
    std::array<std::byte, glifistore::kSegmentHeaderReservedBytes> encoded{};
    GLIFI_REQUIRE(glifistore::encode_segment_header(encoded, header).has_value());
    encoded[glifistore::kSegmentCommitSlotsOffset + glifistore::kSegmentCommitSlotBytes + 24] ^= std::byte{1};

    const auto decoded = glifistore::decode_segment_header(encoded);
    GLIFI_REQUIRE(decoded.has_value());
    GLIFI_REQUIRE(decoded->slots[1].validity == glifistore::CommitSlotValidity::invalid);
    const auto selected = glifistore::select_newest_segment_commit(*decoded);
    GLIFI_REQUIRE(selected.has_value());
    GLIFI_REQUIRE(selected->slot_index == 0);
    GLIFI_REQUIRE(selected->commit == empty_commit(4));
}

GLIFI_TEST("segment header selects by generation rather than physical slot order") {
    const glifistore::SegmentHeader header{
        .identity = fixture_identity(),
        .commits = {populated_commit(9), empty_commit(8)},
    };
    std::array<std::byte, glifistore::kSegmentHeaderReservedBytes> encoded{};
    GLIFI_REQUIRE(glifistore::encode_segment_header(encoded, header).has_value());
    const auto decoded = glifistore::decode_segment_header(encoded);
    GLIFI_REQUIRE(decoded.has_value());
    const auto selected = glifistore::select_newest_segment_commit(*decoded);
    GLIFI_REQUIRE(selected.has_value());
    GLIFI_REQUIRE(selected->slot_index == 0);
    GLIFI_REQUIRE(selected->commit.commit_generation == 9);
}

GLIFI_TEST("segment header fails closed for conflicting equal commit generations") {
    const auto first = empty_commit(7);
    const auto second = populated_commit(7);
    glifistore::DecodedSegmentHeader decoded{
        .identity = fixture_identity(),
        .slots = {{{.validity = glifistore::CommitSlotValidity::valid, .commit = first},
                   {.validity = glifistore::CommitSlotValidity::valid, .commit = second}}},
    };
    const auto selected = glifistore::select_newest_segment_commit(decoded);
    GLIFI_REQUIRE(!selected.has_value());
    GLIFI_REQUIRE(selected.error().code == glifistore::ErrorCode::corrupted_data);

    const glifistore::SegmentHeader header{
        .identity = fixture_identity(),
        .commits = {first, second},
    };
    std::array<std::byte, glifistore::kSegmentHeaderReservedBytes> encoded{};
    const auto status = glifistore::encode_segment_header(encoded, header);
    GLIFI_REQUIRE(!status.has_value());
    GLIFI_REQUIRE(status.error().code == glifistore::ErrorCode::invalid_argument);
}

GLIFI_TEST("segment commit codec rejects invalid metadata and isolates unknown slot versions") {
    auto invalid = populated_commit();
    invalid.committed_end += 1;
    std::array<std::byte, glifistore::kSegmentCommitSlotBytes> slot{};
    GLIFI_REQUIRE(!glifistore::encode_segment_commit_slot(slot, invalid).has_value());

    GLIFI_REQUIRE(glifistore::encode_segment_commit_slot(slot, populated_commit()).has_value());
    slot[4] = std::byte{2};
    const auto decoded = glifistore::decode_segment_commit_slot(slot);
    GLIFI_REQUIRE(decoded.has_value());
    GLIFI_REQUIRE(decoded->validity == glifistore::CommitSlotValidity::invalid);

    slot[48] = std::byte{0};
    slot[49] = std::byte{0};
    slot[50] = std::byte{0};
    slot[51] = std::byte{0};
    const auto checksum = glifistore::crc32c(slot);
    for (std::size_t index = 0; index < 4; ++index) {
        slot[48 + index] = static_cast<std::byte>((checksum >> (index * 8U)) & 0xFFU);
    }
    const auto future_slot = glifistore::decode_segment_commit_slot(slot);
    GLIFI_REQUIRE(!future_slot.has_value());
    GLIFI_REQUIRE(future_slot.error().code == glifistore::ErrorCode::invalid_record);
}

GLIFI_TEST("segment header without a valid commit cannot define a recovery boundary") {
    const glifistore::SegmentHeader header{
        .identity = fixture_identity(),
        .commits = {std::nullopt, std::nullopt},
    };
    std::array<std::byte, glifistore::kSegmentHeaderReservedBytes> encoded{};
    GLIFI_REQUIRE(glifistore::encode_segment_header(encoded, header).has_value());
    const auto decoded = glifistore::decode_segment_header(encoded);
    GLIFI_REQUIRE(decoded.has_value());
    const auto selected = glifistore::select_newest_segment_commit(*decoded);
    GLIFI_REQUIRE(!selected.has_value());
    GLIFI_REQUIRE(selected.error().code == glifistore::ErrorCode::corrupted_data);
}
