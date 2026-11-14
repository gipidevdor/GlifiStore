#include "glifistore/index/index.hpp"
#include "test.hpp"

#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

auto bytes(const std::string& value) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

} // namespace

GLIFI_TEST("generated histories rebuild to their sequential final state") {
    constexpr std::uint64_t seeds = 32;
    constexpr std::uint64_t operations = 250;

    for (std::uint64_t seed = 0; seed < seeds; ++seed) {
        std::mt19937_64 random{seed};
        std::unordered_map<std::string, std::string> expected;
        auto segment = std::make_shared<glifistore::Segment>(glifistore::SegmentId{seed + 1});

        for (std::uint64_t sequence = 1; sequence <= operations; ++sequence) {
            const auto key = std::string{"key-"} + std::to_string(random() % 31U);
            const auto erase = (random() % 5U) == 0U;
            const auto value = std::string{"value-"} + std::to_string(random());
            const auto result = segment->append({
                .sequence = glifistore::SequenceNumber{sequence},
                .opcode = erase ? glifistore::Opcode::erase : glifistore::Opcode::put,
                .key_hash = random(),
                .key = bytes(key),
                .value = erase ? std::span<const std::byte>{} : bytes(value),
            });
            GLIFI_REQUIRE(result.has_value());
            if (erase) {
                expected.erase(key);
            } else {
                expected.insert_or_assign(key, value);
            }
        }

        const std::vector<glifistore::SegmentPtr> segments{segment};
        auto rebuilt = glifistore::rebuild_index_from_segments(segments);
        GLIFI_REQUIRE(rebuilt.has_value());
        GLIFI_REQUIRE(rebuilt->index.stats().size == expected.size());
        for (const auto& [key, value] : expected) {
            const auto ref = rebuilt->index.find(key);
            GLIFI_REQUIRE(ref.has_value());
            const auto record = segment->read(*ref);
            GLIFI_REQUIRE(record.has_value());
            const std::string_view actual{reinterpret_cast<const char*>(record->value.data()),
                                          record->value.size()};
            GLIFI_REQUIRE(actual == value);
        }
    }
}
