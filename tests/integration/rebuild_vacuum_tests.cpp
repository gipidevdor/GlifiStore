#include "glifistore/index/index.hpp"
#include "glifistore/vacuum/vacuum.hpp"
#include "test.hpp"

#include <string_view>

namespace {
auto bytes(std::string_view value) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

auto append(glifistore::Segment& segment, std::uint64_t seq, glifistore::Opcode op, std::string_view key,
            std::string_view value = {}, std::uint64_t hash = 7, std::uint64_t expiry = 0)
    -> glifistore::RecordRef {
    auto ref = segment.append({
        .sequence = glifistore::SequenceNumber{seq},
        .opcode = op,
        .key_hash = hash,
        .expire_at_ns = expiry,
        .key = bytes(key),
        .value = bytes(value),
    });
    GLIFI_REQUIRE(ref.has_value());
    return *ref;
}
} // namespace

GLIFI_TEST("rebuild chooses highest sequence independent of segment order and hash collision") {
    auto first = std::make_shared<glifistore::Segment>(glifistore::SegmentId{1});
    auto second = std::make_shared<glifistore::Segment>(glifistore::SegmentId{2});
    const auto old = append(*first, 10, glifistore::Opcode::put, "alpha", "old", 99);
    const auto other = append(*first, 11, glifistore::Opcode::put, "beta", "other", 99);
    const auto newest = append(*second, 30, glifistore::Opcode::put, "alpha", "new", 99);
    static_cast<void>(old);
    static_cast<void>(other);
    const std::vector<glifistore::SegmentPtr> segments{second, first};
    auto rebuilt = glifistore::rebuild_index_from_segments(segments);
    GLIFI_REQUIRE(rebuilt.has_value());
    GLIFI_REQUIRE(rebuilt->index.find("alpha") == newest);
    GLIFI_REQUIRE(rebuilt->index.find("beta").has_value());
}

GLIFI_TEST("rebuild applies tombstone and expiration semantics") {
    auto segment = std::make_shared<glifistore::Segment>(glifistore::SegmentId{3});
    static_cast<void>(append(*segment, 1, glifistore::Opcode::put, "gone", "v"));
    static_cast<void>(append(*segment, 2, glifistore::Opcode::erase, "gone"));
    static_cast<void>(append(*segment, 3, glifistore::Opcode::put, "expired", "v", 8, 100));
    const std::vector<glifistore::SegmentPtr> segments{segment};
    auto rebuilt = glifistore::rebuild_index_from_segments(segments, 101);
    GLIFI_REQUIRE(rebuilt.has_value());
    GLIFI_REQUIRE(!rebuilt->index.find("gone").has_value());
    GLIFI_REQUIRE(!rebuilt->index.find("expired").has_value());
    GLIFI_REQUIRE(rebuilt->stats.tombstones == 1);
    GLIFI_REQUIRE(rebuilt->stats.expired == 1);
}

GLIFI_TEST("vacuum copies exactly visible records and leaves source immutable") {
    auto source = std::make_shared<glifistore::Segment>(glifistore::SegmentId{10});
    auto first = append(*source, 1, glifistore::Opcode::put, "key", "old");
    auto current = append(*source, 2, glifistore::Opcode::put, "key", "new");
    GLIFI_REQUIRE(source->mark_live(current).has_value());
    GLIFI_REQUIRE(source->seal().has_value());
    glifistore::Index index;
    GLIFI_REQUIRE(index.insert_or_assign("key", current).has_value());
    const auto before = source->stats();
    const std::vector<glifistore::SegmentPtr> segments{source};

    glifistore::VacuumBuilder builder;
    auto vacuumed = builder.rebuild(index, segments, glifistore::SegmentId{100});
    GLIFI_REQUIRE(vacuumed.has_value());
    GLIFI_REQUIRE(vacuumed->index.find("key").has_value());
    GLIFI_REQUIRE(vacuumed->segments.size() == 1);
    GLIFI_REQUIRE(vacuumed->stats.records_copied == 1);
    GLIFI_REQUIRE(source->stats().record_count == before.record_count);
    GLIFI_REQUIRE(source->read(first).has_value());
}

GLIFI_TEST("selective vacuum preserves references outside its candidate set") {
    auto candidate = std::make_shared<glifistore::Segment>(glifistore::SegmentId{20});
    const auto moved = append(*candidate, 10, glifistore::Opcode::put, "moved", "value");
    GLIFI_REQUIRE(candidate->mark_live(moved).has_value());
    GLIFI_REQUIRE(candidate->seal().has_value());

    auto retained = std::make_shared<glifistore::Segment>(glifistore::SegmentId{21});
    const auto stable = append(*retained, 11, glifistore::Opcode::put, "stable", "value");
    GLIFI_REQUIRE(retained->mark_live(stable).has_value());

    glifistore::Index index;
    GLIFI_REQUIRE(index.insert_or_assign("moved", moved).has_value());
    GLIFI_REQUIRE(index.insert_or_assign("stable", stable).has_value());
    const std::vector<glifistore::SegmentPtr> segments{candidate, retained};
    const std::vector<glifistore::SegmentId> candidates{candidate->id()};

    glifistore::VacuumBuilder builder;
    auto vacuumed =
        builder.rebuild(index, segments, candidates, []() -> glifistore::Result<glifistore::SegmentPtr> {
            return std::make_shared<glifistore::Segment>(glifistore::SegmentId{100});
        });
    GLIFI_REQUIRE(vacuumed.has_value());
    GLIFI_REQUIRE(vacuumed->index.find("stable") == stable);
    GLIFI_REQUIRE(vacuumed->index.find("moved").has_value());
    GLIFI_REQUIRE(vacuumed->index.find("moved") != moved);
    GLIFI_REQUIRE(vacuumed->stats.source_records_verified == 1);
    GLIFI_REQUIRE(vacuumed->stats.records_copied == 1);
    GLIFI_REQUIRE(vacuumed->segments.size() == 1);
}

GLIFI_TEST("selective vacuum drops expired candidates without allocating output") {
    auto candidate = std::make_shared<glifistore::Segment>(glifistore::SegmentId{30});
    const auto expired = append(*candidate, 20, glifistore::Opcode::put, "expired", "value", 7, 100);
    GLIFI_REQUIRE(candidate->mark_live(expired).has_value());
    GLIFI_REQUIRE(candidate->seal().has_value());
    glifistore::Index index;
    GLIFI_REQUIRE(index.insert_or_assign("expired", expired).has_value());
    const std::vector<glifistore::SegmentPtr> segments{candidate};
    const std::vector<glifistore::SegmentId> candidates{candidate->id()};
    std::size_t allocations{};

    glifistore::VacuumBuilder builder;
    auto vacuumed = builder.rebuild(
        index, segments, candidates,
        [&allocations]() -> glifistore::Result<glifistore::SegmentPtr> {
            ++allocations;
            return std::make_shared<glifistore::Segment>(glifistore::SegmentId{101});
        },
        101);
    GLIFI_REQUIRE(vacuumed.has_value());
    GLIFI_REQUIRE(!vacuumed->index.find("expired").has_value());
    GLIFI_REQUIRE(vacuumed->segments.empty());
    GLIFI_REQUIRE(vacuumed->stats.expired_records_dropped == 1);
    GLIFI_REQUIRE(allocations == 0);
}
