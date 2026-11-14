#include "glifistore/index/index.hpp"
#include "glifistore/index/swiss_control_group.hpp"
#include "test.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <string>

namespace {

auto test_ref(const std::uint64_t sequence) -> glifistore::RecordRef {
    return {glifistore::SegmentId{1}, glifistore::RecordOffset{4096}, glifistore::RecordSize{64},
            glifistore::SequenceNumber{sequence}, glifistore::GenerationId{1}};
}

auto mixed_hash(const std::uint64_t value) -> std::uint64_t {
    auto hash = value ^ 0x243F6A8885A308D3ULL;
    hash *= 0x9E3779B97F4A7C15ULL;
    hash ^= hash >> 33U;
    hash *= 0x9E3779B97F4A7C15ULL;
    hash ^= hash >> 29U;
    return hash;
}

} // namespace

GLIFI_TEST("index inserts replaces finds erases and iterates") {
    glifistore::Index index;
    const glifistore::RecordRef first{glifistore::SegmentId{1}, glifistore::RecordOffset{10},
                                      glifistore::RecordSize{20}, glifistore::SequenceNumber{1},
                                      glifistore::GenerationId{1}};
    const auto inserted = index.insert_or_assign("key", first);
    GLIFI_REQUIRE(inserted.has_value());
    GLIFI_REQUIRE(inserted->inserted);
    GLIFI_REQUIRE(index.find("key") == first);

    auto second = first;
    second.sequence = glifistore::SequenceNumber{2};
    const auto replaced = index.insert_or_assign("key", second);
    GLIFI_REQUIRE(replaced.has_value());
    GLIFI_REQUIRE(!replaced->inserted);
    GLIFI_REQUIRE(replaced->previous == first);
    GLIFI_REQUIRE(index.entries().size() == 1);
    GLIFI_REQUIRE(index.erase("key").previous == second);
    GLIFI_REQUIRE(!index.find("key").has_value());
}

GLIFI_TEST("index preserves keys larger than 16-bit lengths") {
    glifistore::Index index;
    const std::string key(70'000, 'x');
    const glifistore::RecordRef ref{glifistore::SegmentId{1}, glifistore::RecordOffset{10},
                                    glifistore::RecordSize{20}, glifistore::SequenceNumber{1},
                                    glifistore::GenerationId{1}};
    const auto inserted = index.insert_or_assign(key, ref);
    GLIFI_REQUIRE(inserted.has_value());
    GLIFI_REQUIRE(inserted->inserted);
    GLIFI_REQUIRE(index.find(key) == ref);
    GLIFI_REQUIRE(index.entries().front().key == key);
}

GLIFI_TEST("index grows and rejects impossible reserve sizes") {
    glifistore::Index index;
    for (std::uint64_t value = 0; value < 1'000; ++value) {
        const auto key = std::to_string(value);
        const glifistore::RecordRef ref{glifistore::SegmentId{1}, glifistore::RecordOffset{10},
                                        glifistore::RecordSize{20}, glifistore::SequenceNumber{value},
                                        glifistore::GenerationId{1}};
        GLIFI_REQUIRE(index.insert_or_assign(key, ref).has_value());
    }
    GLIFI_REQUIRE(index.stats().size == 1'000);
    GLIFI_REQUIRE(index.stats().slot_bytes == 64);
    GLIFI_REQUIRE(index.stats().table_allocated_bytes ==
                  index.stats().bucket_count * (index.stats().slot_bytes + sizeof(std::uint8_t)));
    GLIFI_REQUIRE(!index.reserve(std::numeric_limits<std::size_t>::max()).has_value());
    GLIFI_REQUIRE(!index.prepare_batch_insert(std::numeric_limits<std::size_t>::max(), 0).has_value());
}

GLIFI_TEST("index resolves complete-hash collisions by full key bytes") {
    glifistore::Index index;
    constexpr std::uint64_t forced_hash = 0xDEADBEEF12345678ULL;
    const glifistore::HashedKey first{"collision-a", forced_hash};
    const glifistore::HashedKey second{"collision-b", forced_hash};
    const glifistore::RecordRef first_ref{glifistore::SegmentId{1}, glifistore::RecordOffset{10},
                                          glifistore::RecordSize{20}, glifistore::SequenceNumber{11},
                                          glifistore::GenerationId{1}};
    auto second_ref = first_ref;
    second_ref.sequence = glifistore::SequenceNumber{12};
    GLIFI_REQUIRE(index.insert_or_assign(first, first_ref).has_value());
    GLIFI_REQUIRE(index.insert_or_assign(second, second_ref).has_value());
    GLIFI_REQUIRE(index.find(first) == first_ref);
    GLIFI_REQUIRE(index.find(second) == second_ref);
}

GLIFI_TEST("index preflights long-key publication before a durable commit") {
    glifistore::Index index;
    const std::string key(80'000, 'k');
    const glifistore::HashedKey hashed{key, glifistore::hash_key(key)};
    GLIFI_REQUIRE(index.prepare_insert(hashed).has_value());
    const glifistore::RecordRef ref{glifistore::SegmentId{3}, glifistore::RecordOffset{4096},
                                    glifistore::RecordSize{80'056}, glifistore::SequenceNumber{7},
                                    glifistore::GenerationId{1}};
    const auto inserted = index.insert_or_assign(hashed, ref);
    GLIFI_REQUIRE(inserted.has_value());
    GLIFI_REQUIRE(inserted->inserted);
    GLIFI_REQUIRE(index.find(hashed) == ref);
    GLIFI_REQUIRE(index.erase(hashed).previous == ref);
}

GLIFI_TEST("index tracks and reuses deleted slots without changing capacity") {
    glifistore::Index index;
    constexpr std::uint64_t forced_hash = 0xD311E7EDULL;
    const glifistore::HashedKey first{"deleted-a", forced_hash};
    const glifistore::HashedKey second{"deleted-b", forced_hash};
    const glifistore::HashedKey replacement{"deleted-c", forced_hash};
    GLIFI_REQUIRE(index.insert_or_assign(first, test_ref(1)).has_value());
    GLIFI_REQUIRE(index.insert_or_assign(second, test_ref(2)).has_value());
    const auto capacity = index.stats().bucket_count;
    GLIFI_REQUIRE(index.erase_no_compact(first).previous == test_ref(1));
    GLIFI_REQUIRE(index.stats().deleted_count == 1);
    GLIFI_REQUIRE(index.stats().size + index.stats().deleted_count <= capacity);

    GLIFI_REQUIRE(index.insert_or_assign(replacement, test_ref(3)).has_value());
    const auto reused = index.stats();
    GLIFI_REQUIRE(reused.bucket_count == capacity);
    GLIFI_REQUIRE(reused.deleted_count == 0);
    GLIFI_REQUIRE(index.find(second) == test_ref(2));
    GLIFI_REQUIRE(index.find(replacement) == test_ref(3));
}

GLIFI_TEST("index rebuilds tombstones transactionally at the same capacity") {
    glifistore::Index index;
    GLIFI_REQUIRE(index.reserve(100).has_value());
    for (std::uint64_t value = 0; value < 100; ++value) {
        const auto key = std::string{"collision-"} + std::to_string(value);
        GLIFI_REQUIRE(index.insert_or_assign(key, test_ref(value + 1)).has_value());
    }
    const auto capacity = index.stats().bucket_count;
    for (std::uint64_t value = 0; value < 80; ++value) {
        const auto key = std::string{"collision-"} + std::to_string(value);
        const glifistore::HashedKey hashed{key, glifistore::hash_key(key)};
        GLIFI_REQUIRE(index.erase_no_compact(hashed).previous.has_value());
    }
    const auto churned = index.stats();
    GLIFI_REQUIRE(churned.size == 20);
    GLIFI_REQUIRE(churned.deleted_count == 80);
    GLIFI_REQUIRE(churned.effective_load_factor > churned.load_factor);

    const std::string next_key{"collision-next"};
    const glifistore::HashedKey next{next_key, glifistore::hash_key(next_key)};
    GLIFI_REQUIRE(index.prepare_insert(next).has_value());
    const auto rebuilt = index.stats();
    GLIFI_REQUIRE(rebuilt.bucket_count == capacity);
    GLIFI_REQUIRE(rebuilt.deleted_count == 0);
    GLIFI_REQUIRE(rebuilt.tombstone_rebuild_count == 1);
    GLIFI_REQUIRE(rebuilt.rehash_count >= 1);
    GLIFI_REQUIRE(index.insert_or_assign(next, test_ref(101)).has_value());
    for (std::uint64_t value = 80; value < 100; ++value) {
        const auto key = std::string{"collision-"} + std::to_string(value);
        GLIFI_REQUIRE(index.find(key) == test_ref(value + 1));
    }
}

GLIFI_TEST("index bounds full collision probing across table wraparound") {
    glifistore::Index index;
    GLIFI_REQUIRE(index.reserve(32).has_value());
    const auto capacity = index.stats().bucket_count;
    std::uint64_t forced_hash{};
    while (((mixed_hash(forced_hash) >> 7U) & ((capacity / glifistore::kSwissGroupSize) - 1U)) !=
           (capacity / glifistore::kSwissGroupSize) - 1U) {
        ++forced_hash;
    }
    for (std::uint64_t value = 0; value < 16; ++value) {
        const auto key = std::string{"wrap-"} + std::to_string(value);
        GLIFI_REQUIRE(
            index.insert_or_assign(glifistore::HashedKey{key, forced_hash}, test_ref(value + 1)).has_value());
    }
    for (std::uint64_t value = 0; value < 16; ++value) {
        const auto key = std::string{"wrap-"} + std::to_string(value);
        GLIFI_REQUIRE(index.find(glifistore::HashedKey{key, forced_hash}) == test_ref(value + 1));
    }
    const glifistore::HashedKey missing{"wrap-missing", forced_hash};
    GLIFI_REQUIRE(!index.find(missing).has_value());
    const auto stats = index.stats();
    GLIFI_REQUIRE(stats.maximum_probe_groups >= 3);
    GLIFI_REQUIRE(stats.maximum_probe_groups <= stats.bucket_count / glifistore::kSwissGroupSize);
}

GLIFI_TEST("index grows only after reaching seven eighths live occupancy") {
    glifistore::Index index;
    for (std::uint64_t value = 0; value < 7; ++value) {
        GLIFI_REQUIRE(index.insert_or_assign(std::to_string(value), test_ref(value + 1)).has_value());
    }
    GLIFI_REQUIRE(index.stats().bucket_count == 8);
    GLIFI_REQUIRE(index.stats().size == 7);
    GLIFI_REQUIRE(index.stats().effective_load_factor == glifistore::kSwissMaxLoadFactor);
    GLIFI_REQUIRE(index.insert_or_assign("growth", test_ref(8)).has_value());
    GLIFI_REQUIRE(index.stats().bucket_count == 16);
    GLIFI_REQUIRE(index.stats().deleted_count == 0);
}

GLIFI_TEST("selected Swiss control matcher equals scalar masks") {
    std::array<std::uint8_t, glifistore::kSwissGroupSize> control{};
    for (std::uint32_t pattern = 0; pattern < 256; ++pattern) {
        for (std::size_t index = 0; index < control.size(); ++index) {
            control[index] = static_cast<std::uint8_t>((pattern * 37U + index * 53U) & 0xFFU);
        }
        for (std::uint32_t byte = 0; byte < 256; ++byte) {
            GLIFI_REQUIRE(
                glifistore::detail::equal_byte_mask(control.data(), static_cast<std::uint8_t>(byte)) ==
                glifistore::detail::equal_byte_mask_scalar(control.data(), static_cast<std::uint8_t>(byte)));
        }
    }
}

GLIFI_TEST("index keeps inline and heap keys stable through prolonged churn") {
    glifistore::Index index;
    constexpr std::size_t rounds = 32;
    constexpr std::size_t entries_per_round = 96;
    GLIFI_REQUIRE(index.reserve(entries_per_round).has_value());

    for (std::size_t round = 0; round < rounds; ++round) {
        for (std::size_t value = 0; value < entries_per_round; ++value) {
            const auto suffix = std::to_string(round * entries_per_round + value);
            const auto key = value % 2U == 0 ? std::string{"inline-"} + suffix
                                             : std::string(96, static_cast<char>('a' + round % 26U)) + suffix;
            GLIFI_REQUIRE(index.insert_or_assign(key, test_ref(value + 1)).has_value());
        }
        for (std::size_t value = 0; value + 1 < entries_per_round; ++value) {
            const auto suffix = std::to_string(round * entries_per_round + value);
            const auto key = value % 2U == 0 ? std::string{"inline-"} + suffix
                                             : std::string(96, static_cast<char>('a' + round % 26U)) + suffix;
            GLIFI_REQUIRE(index.erase_no_compact(glifistore::HashedKey::compute(key)).previous ==
                          test_ref(value + 1));
        }

        const auto survivor_suffix = std::to_string((round + 1U) * entries_per_round - 1U);
        const auto survivor = std::string(96, static_cast<char>('a' + round % 26U)) + survivor_suffix;
        GLIFI_REQUIRE(index.find(survivor) == test_ref(entries_per_round));
        GLIFI_REQUIRE(!index.find("absent-after-heavy-churn").has_value());
        GLIFI_REQUIRE(index.stats().size + index.stats().deleted_count <= index.stats().bucket_count);
        GLIFI_REQUIRE(index.erase_no_compact(glifistore::HashedKey::compute(survivor)).previous ==
                      test_ref(entries_per_round));
    }

    GLIFI_REQUIRE(index.stats().size == 0);
    GLIFI_REQUIRE(index.prepare_insert(glifistore::HashedKey::compute("final-inline")).has_value());
    GLIFI_REQUIRE(index.stats().deleted_count == 0);
    GLIFI_REQUIRE(index.insert_or_assign("final-inline", test_ref(1)).has_value());
    GLIFI_REQUIRE(index.find("final-inline") == test_ref(1));
}
