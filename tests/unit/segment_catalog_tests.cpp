#include "glifistore/segment/global_manager.hpp"
#include "test.hpp"

#include <string_view>

namespace {
auto bytes(std::string_view value) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}
} // namespace

GLIFI_TEST("segment catalog lookup and snapshot order survive rotation") {
    glifistore::GlobalSegmentManager manager;
    const auto first = manager.allocate_active(glifistore::WorkerId{1});
    GLIFI_REQUIRE(first != nullptr);
    const auto before_rotation = manager.segments();
    const auto rotated = manager.prepare_rotation(first, glifistore::WorkerId{1});
    GLIFI_REQUIRE(rotated.has_value());
    GLIFI_REQUIRE(first->state() == glifistore::SegmentState::active);
    GLIFI_REQUIRE(manager.find((*rotated)->id()) == nullptr);
    GLIFI_REQUIRE(manager.commit_rotation(first, *rotated).has_value());
    GLIFI_REQUIRE((*rotated)->state() == glifistore::SegmentState::active);
    GLIFI_REQUIRE(first->state() == glifistore::SegmentState::sealed);
    GLIFI_REQUIRE(manager.find(first->id()) == first);
    GLIFI_REQUIRE(manager.find((*rotated)->id()) == *rotated);
    GLIFI_REQUIRE(before_rotation.size() == 1);
    const auto after_rotation = manager.segments();
    GLIFI_REQUIRE(after_rotation.size() == 2);
    GLIFI_REQUIRE(after_rotation[0]->id() == first->id());
    GLIFI_REQUIRE(after_rotation[1]->id() == (*rotated)->id());
}

GLIFI_TEST("segment catalog releases ownership of retired segments") {
    glifistore::GlobalSegmentManager manager;
    auto segment = manager.allocate_active(glifistore::WorkerId{2});
    GLIFI_REQUIRE(segment != nullptr);
    const std::weak_ptr<glifistore::Segment> lifetime = segment;
    const auto ref = segment->append({
        .sequence = glifistore::SequenceNumber{1},
        .key = bytes("k"),
        .value = bytes("v"),
    });
    GLIFI_REQUIRE(ref.has_value());
    GLIFI_REQUIRE(segment->mark_live(*ref).has_value());
    GLIFI_REQUIRE(segment->seal().has_value());
    GLIFI_REQUIRE(segment->mark_dead(*ref).has_value());
    const auto before_retirement = manager.retired_count();
    GLIFI_REQUIRE(manager.try_retire(segment->id()).has_value());
    GLIFI_REQUIRE(manager.find(segment->id()) == nullptr);
    GLIFI_REQUIRE(manager.segments().empty());
    GLIFI_REQUIRE(before_retirement == 0);
    GLIFI_REQUIRE(manager.retired_count() == 1);
    GLIFI_REQUIRE(!lifetime.expired());
    segment.reset();
    GLIFI_REQUIRE(lifetime.expired());
}
