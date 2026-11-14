#include "glifistore/segment/segment.hpp"
#include "test.hpp"

#include <cstddef>
#include <string_view>

namespace {
auto bytes(std::string_view value) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

void flip_record_payload_byte(glifistore::Segment& segment, const glifistore::RecordRef& ref) {
    segment.mutable_base()[ref.offset.value + 32U] ^= std::byte{0xFF};
}
} // namespace

GLIFI_TEST("active resident segment is not trusted") {
    glifistore::Segment segment{glifistore::SegmentId{1}};
    GLIFI_REQUIRE(segment.residency() == glifistore::ResidencyState::resident);
    GLIFI_REQUIRE(segment.state() == glifistore::SegmentState::active);
    GLIFI_REQUIRE(!segment.is_trusted());
}

GLIFI_TEST("sealed resident segment is trusted for read_trusted gate") {
    glifistore::Segment segment{glifistore::SegmentId{2}};
    const auto ref = segment.append({
        .sequence = glifistore::SequenceNumber{1},
        .key = bytes("key"),
        .value = bytes("value"),
    });
    GLIFI_REQUIRE(ref.has_value());
    GLIFI_REQUIRE(segment.seal().has_value());
    GLIFI_REQUIRE(segment.is_trusted());
}

GLIFI_TEST("verified read detects corruption read_trusted skips checksum on sealed segment") {
    glifistore::Segment segment{glifistore::SegmentId{3}};
    const auto ref = segment.append({
        .sequence = glifistore::SequenceNumber{1},
        .key = bytes("probe"),
        .value = bytes("payload"),
    });
    GLIFI_REQUIRE(ref.has_value());
    GLIFI_REQUIRE(segment.seal().has_value());
    flip_record_payload_byte(segment, *ref);

    const auto verified = segment.read(*ref);
    GLIFI_REQUIRE(!verified.has_value());
    GLIFI_REQUIRE(verified.error().code == glifistore::ErrorCode::checksum_mismatch);

    const auto trusted = segment.read_trusted(*ref);
    GLIFI_REQUIRE(trusted.has_value());
    GLIFI_REQUIRE(trusted->key_string() == "probe");
}
