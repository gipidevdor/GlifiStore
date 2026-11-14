#include "glifistore/segment/segment.hpp"
#include "test.hpp"

#include <cstddef>
#include <string_view>

namespace {
auto bytes(std::string_view value) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}
} // namespace

GLIFI_TEST("segment is exactly 64 MiB and provides positional record access") {
    glifistore::Segment segment{glifistore::SegmentId{1}};
    GLIFI_REQUIRE(segment.capacity() == 67'108'864);
    const auto ref = segment.append({
        .sequence = glifistore::SequenceNumber{1},
        .key = bytes("key"),
        .value = bytes("value"),
    });
    GLIFI_REQUIRE(ref.has_value());
    GLIFI_REQUIRE(ref->offset.value >= glifistore::kSegmentHeaderReservedBytes);
    const auto view = segment.read(*ref);
    GLIFI_REQUIRE(view.has_value());
    GLIFI_REQUIRE(view->key_string() == "key");
    GLIFI_REQUIRE(segment.base() + ref->offset.value != nullptr);
}

GLIFI_TEST("segment seal forbids future append") {
    glifistore::Segment segment{glifistore::SegmentId{2}};
    GLIFI_REQUIRE(segment.seal().has_value());
    const auto result = segment.append({.sequence = glifistore::SequenceNumber{1}});
    GLIFI_REQUIRE(!result.has_value());
    GLIFI_REQUIRE(result.error().code == glifistore::ErrorCode::segment_sealed);
}

GLIFI_TEST("segment generation protects against stale RecordRef") {
    glifistore::Segment segment{glifistore::SegmentId{3}, {}, glifistore::GenerationId{9}};
    const auto ref = segment.append({.sequence = glifistore::SequenceNumber{1}, .key = bytes("k")});
    GLIFI_REQUIRE(ref.has_value());
    auto stale = *ref;
    stale.generation = glifistore::GenerationId{8};
    GLIFI_REQUIRE(!segment.read(stale).has_value());
}

GLIFI_TEST("segment liveness permits retirement only after all live records die") {
    glifistore::Segment segment{glifistore::SegmentId{4}};
    const auto ref = segment.append({.sequence = glifistore::SequenceNumber{1}, .key = bytes("k")});
    GLIFI_REQUIRE(ref.has_value());
    GLIFI_REQUIRE(segment.mark_live(*ref).has_value());
    GLIFI_REQUIRE(segment.seal().has_value());
    GLIFI_REQUIRE(!segment.retire().has_value());
    GLIFI_REQUIRE(segment.mark_dead(*ref).has_value());
    GLIFI_REQUIRE(segment.retire().has_value());
}
