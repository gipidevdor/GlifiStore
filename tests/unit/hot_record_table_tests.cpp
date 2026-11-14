#include "glifistore/core/key_hash.hpp"
#include "glifistore/index/swiss_control_group.hpp"
#include "persistence/hot_record_table.hpp"
#include "test.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

auto make_entry(const std::uint64_t sequence, const std::string_view value)
    -> glifistore::detail::HotRecordEntry {
    glifistore::detail::HotRecordEntry entry;
    entry.reference = {glifistore::SegmentId{1}, glifistore::RecordOffset{100},
                       glifistore::RecordSize{static_cast<std::uint32_t>(value.size())},
                       glifistore::SequenceNumber{sequence}, glifistore::GenerationId{1}};
    entry.value_size = value.size();
    if (entry.is_inline()) {
        std::copy_n(reinterpret_cast<const std::byte*>(value.data()), value.size(), entry.inline_value);
    } else {
        auto mutable_value = std::make_shared<std::byte[]>(value.size());
        std::copy_n(reinterpret_cast<const std::byte*>(value.data()), value.size(), mutable_value.get());
        entry.heap_value = std::move(mutable_value);
    }
    return entry;
}

GLIFI_TEST("hot record inline representation is derived from value size") {
    const auto inline_value = std::string(glifistore::detail::HotRecordEntry::kInlineValueBytes, 'i');
    const auto heap_value = std::string(glifistore::detail::HotRecordEntry::kInlineValueBytes + 1U, 'h');

    const auto inline_entry = make_entry(11, inline_value);
    GLIFI_REQUIRE(inline_entry.is_inline());
    GLIFI_REQUIRE(inline_entry.reference.sequence == glifistore::SequenceNumber{11});
    GLIFI_REQUIRE(inline_entry.value_span().size() == inline_value.size());

    const auto heap_entry = make_entry(12, heap_value);
    GLIFI_REQUIRE(!heap_entry.is_inline());
    GLIFI_REQUIRE(heap_entry.reference.sequence == glifistore::SequenceNumber{12});
    GLIFI_REQUIRE(heap_entry.value_span().size() == heap_value.size());

    const auto snapshot = glifistore::detail::HotRecordSnapshot::from_entry(heap_entry);
    GLIFI_REQUIRE(!snapshot.is_inline());
    GLIFI_REQUIRE(snapshot.sequence == heap_entry.reference.sequence);
    GLIFI_REQUIRE(snapshot.value_span().data() == heap_entry.value_span().data());
}

} // namespace

GLIFI_TEST("hot record table inserts finds replaces and erases with control fingerprints") {
    glifistore::detail::HotRecordTable table;
    GLIFI_REQUIRE(table.capacity() == 0);
    const auto key = std::string{"alpha"};
    const auto hash = glifistore::hash_key(std::as_bytes(std::span{key}));
    GLIFI_REQUIRE(table.insert_or_assign(key, hash, make_entry(1, "one")).has_value());
    GLIFI_REQUIRE(table.capacity() == glifistore::detail::HotRecordTable::kMinimumCapacity);
    GLIFI_REQUIRE(table.size() == 1);
    GLIFI_REQUIRE(table.find(key, hash) != nullptr);
    GLIFI_REQUIRE(table.find(key, hash)->reference.sequence == glifistore::SequenceNumber{1});

    GLIFI_REQUIRE(table.insert_or_assign(key, hash, make_entry(2, "two")).has_value());
    GLIFI_REQUIRE(table.size() == 1);
    GLIFI_REQUIRE(table.find(key, hash)->reference.sequence == glifistore::SequenceNumber{2});

    GLIFI_REQUIRE(table.erase(key, hash));
    GLIFI_REQUIRE(table.size() == 0);
    GLIFI_REQUIRE(table.tombstone_count() == 1);
    GLIFI_REQUIRE(table.find(key, hash) == nullptr);
    GLIFI_REQUIRE(table.insert_or_assign(key, hash, make_entry(3, "three")).has_value());
    GLIFI_REQUIRE(table.size() == 1);
    GLIFI_REQUIRE(table.tombstone_count() == 0);
    GLIFI_REQUIRE(table.find(key, hash)->reference.sequence == glifistore::SequenceNumber{3});
}

GLIFI_TEST("hot record accounting charges bucket storage exactly once") {
    const auto inline_payload = glifistore::detail::hot_record_accounted_bytes(8, 16);
    GLIFI_REQUIRE(inline_payload.has_value());
    GLIFI_REQUIRE(*inline_payload == 8);

    const auto heap_payload = glifistore::detail::hot_record_accounted_bytes(8, 64);
    GLIFI_REQUIRE(heap_payload.has_value());
    GLIFI_REQUIRE(*heap_payload == 72);
    GLIFI_REQUIRE(glifistore::detail::hot_record_slot_bytes() > 0);
}

GLIFI_TEST("hot record table resolves same-H2 collisions by full key bytes") {
    glifistore::detail::HotRecordTable table;
    // Force many inserts so probe groups fill; identity stays full key compare.
    for (std::uint64_t i = 0; i < 96; ++i) {
        const auto key = std::string{"k"} + std::to_string(i);
        const auto hash = glifistore::hash_key(std::as_bytes(std::span{key}));
        GLIFI_REQUIRE(table.insert_or_assign(key, hash, make_entry(i + 1, key)).has_value());
    }
    for (std::uint64_t i = 0; i < 96; ++i) {
        const auto key = std::string{"k"} + std::to_string(i);
        const auto hash = glifistore::hash_key(std::as_bytes(std::span{key}));
        const auto* found = table.find(key, hash);
        GLIFI_REQUIRE(found != nullptr);
        GLIFI_REQUIRE(found->reference.sequence == glifistore::SequenceNumber{i + 1});
        const auto span = found->value_span();
        GLIFI_REQUIRE(std::string_view(reinterpret_cast<const char*>(span.data()), span.size()) == key);
    }
    GLIFI_REQUIRE(table.size() == 96);
    GLIFI_REQUIRE(table.capacity() == 128);
}

GLIFI_TEST("hot record table erase_if and clear drop resident entries") {
    glifistore::detail::HotRecordTable table;
    for (std::uint64_t i = 0; i < 16; ++i) {
        const auto key = std::string{"e"} + std::to_string(i);
        const auto hash = glifistore::hash_key(std::as_bytes(std::span{key}));
        GLIFI_REQUIRE(table.insert_or_assign(key, hash, make_entry(i + 1, "v")).has_value());
    }
    table.erase_if(
        [](const std::string& key, std::uint64_t, const auto&) { return key == "e0" || key == "e1"; });
    GLIFI_REQUIRE(table.size() == 14);
    table.clear();
    GLIFI_REQUIRE(table.size() == 0);
    GLIFI_REQUIRE(table.tombstone_count() == 0);
    const auto probe = std::string{"e2"};
    GLIFI_REQUIRE(table.find(probe, glifistore::hash_key(probe)) == nullptr);
}

GLIFI_TEST("hot record table control matcher agrees with scalar masks") {
    std::array<std::uint8_t, glifistore::kSwissGroupSize> control{
        glifistore::kSwissEmpty,
        0x11,
        glifistore::kSwissDeleted,
        0x22,
        0x11,
        glifistore::kSwissEmpty,
        0x7F,
        0x01,
    };
    for (unsigned byte = 0; byte < 256; ++byte) {
        GLIFI_REQUIRE(
            glifistore::detail::equal_byte_mask(control.data(), static_cast<std::uint8_t>(byte)) ==
            glifistore::detail::equal_byte_mask_scalar(control.data(), static_cast<std::uint8_t>(byte)));
    }
}

GLIFI_TEST("hot record reserve plan grows geometrically at load 0.75") {
    const auto idle = glifistore::detail::plan_hot_record_reserve(10, 0, 64);
    GLIFI_REQUIRE(!idle.overflow);
    GLIFI_REQUIRE(idle.target == 0);

    const auto within = glifistore::detail::plan_hot_record_reserve(47, 1, 64);
    GLIFI_REQUIRE(!within.overflow);
    GLIFI_REQUIRE(within.target == 0);

    const auto grow = glifistore::detail::plan_hot_record_reserve(48, 1, 64);
    GLIFI_REQUIRE(!grow.overflow);
    GLIFI_REQUIRE(grow.target == 128);
}

GLIFI_TEST("hot record replacement at occupancy boundary does not grow") {
    glifistore::detail::HotRecordTable table;
    for (std::uint64_t index = 0; index < 48; ++index) {
        const auto key = std::string{"boundary-"} + std::to_string(index);
        GLIFI_REQUIRE(
            table.insert_or_assign(key, glifistore::hash_key(key), make_entry(index + 1, "v")).has_value());
    }
    GLIFI_REQUIRE(table.capacity() == 64);
    GLIFI_REQUIRE(table
                      .insert_or_assign("boundary-0", glifistore::hash_key("boundary-0"),
                                        make_entry(100, "replacement"))
                      .has_value());
    GLIFI_REQUIRE(table.capacity() == 64);

    GLIFI_REQUIRE(
        table.insert_or_assign("boundary-48", glifistore::hash_key("boundary-48"), make_entry(101, "v"))
            .has_value());
    GLIFI_REQUIRE(table.capacity() == 128);
}
