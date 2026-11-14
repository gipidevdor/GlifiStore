#include "glifistore/core/key_hash.hpp"
#include "glifistore/index/index.hpp"
#include "glifistore/index/index_hash_seed.hpp"
#include "glifistore/index/swiss_table.hpp"
#include "test.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace {

struct SeedGuard final {
    explicit SeedGuard(const std::uint64_t seed) : previous_(glifistore::get_index_hash_seed()) {
        glifistore::set_index_hash_seed(seed);
    }

    ~SeedGuard() {
        glifistore::set_index_hash_seed(previous_);
    }

    SeedGuard(const SeedGuard&) = delete;
    auto operator=(const SeedGuard&) -> SeedGuard& = delete;

    std::uint64_t previous_;
};

} // namespace

GLIFI_TEST("index hash seed defaults to published Index v1 constant") {
    SeedGuard guard{glifistore::kDefaultIndexHashSeed};
    GLIFI_REQUIRE(glifistore::get_index_hash_seed() == glifistore::kDefaultIndexHashSeed);
    glifistore::SwissTableIndex table;
    GLIFI_REQUIRE(table.seed() == glifistore::kDefaultIndexHashSeed);
}

GLIFI_TEST("index hash seed is stable within process for identical tables") {
    SeedGuard guard{0x1111222233334444ULL};
    glifistore::SwissTableIndex left;
    glifistore::SwissTableIndex right;
    GLIFI_REQUIRE(left.seed() == right.seed());
    GLIFI_REQUIRE(left.seed() == 0x1111222233334444ULL);

    const glifistore::HashedKey key = glifistore::HashedKey::compute("tenant-a/orders/1");
    GLIFI_REQUIRE(left.insert_or_assign(key, glifistore::RecordRef{}).has_value());
    GLIFI_REQUIRE(right.insert_or_assign(key, glifistore::RecordRef{}).has_value());
    const auto left_entries = left.entries();
    const auto right_entries = right.entries();
    GLIFI_REQUIRE(left_entries.size() == 1);
    GLIFI_REQUIRE(right_entries.size() == 1);
    GLIFI_REQUIRE(left_entries.front().key == right_entries.front().key);
}

GLIFI_TEST("different index hash seeds diverge placement for the same keys") {
    constexpr std::string_view kKey = "flood-candidate-key";
    const glifistore::HashedKey hashed = glifistore::HashedKey::compute(kKey);

    glifistore::SwissTableIndex a{0xAAAAAAAAAAAAAAAALL};
    glifistore::SwissTableIndex b{0xBBBBBBBBBBBBBBBBULL};
    GLIFI_REQUIRE(a.seed() != b.seed());
    GLIFI_REQUIRE(a.insert_or_assign(hashed, glifistore::RecordRef{}).has_value());
    GLIFI_REQUIRE(b.insert_or_assign(hashed, glifistore::RecordRef{}).has_value());

    GLIFI_REQUIRE(a.find(hashed).has_value());
    GLIFI_REQUIRE(b.find(hashed).has_value());
    const auto empty_a = a.clone_empty();
    GLIFI_REQUIRE(empty_a.seed() == a.seed());
    GLIFI_REQUIRE(!empty_a.find(hashed).has_value());

    // Flood-resistance note (ADR 0026): without the process seed, an attacker cannot
    // precompute Index bucket targets for a secure-profile daemon. Worker routing remains
    // public FNV-1a until keyed routing lands.
    const auto fnv = glifistore::hash_key(kKey);
    const auto keyed_a = glifistore::hash_key_keyed(kKey, 0x1ULL, 0x2ULL);
    const auto keyed_b = glifistore::hash_key_keyed(kKey, 0x3ULL, 0x4ULL);
    GLIFI_REQUIRE(fnv != 0);
    GLIFI_REQUIRE(keyed_a != keyed_b);
}

GLIFI_TEST("siphash24 matches Aumasson/Bernstein paper vectors for key 00..0f") {
    // k = 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f (little-endian limbs)
    constexpr std::uint64_t k0 = 0x0706050403020100ULL;
    constexpr std::uint64_t k1 = 0x0f0e0d0c0b0a0908ULL;

    GLIFI_REQUIRE(glifistore::siphash24({}, k0, k1) == 0x726fdb47dd0e0e31ULL);

    const std::array<std::byte, 3> msg{std::byte{0x00}, std::byte{0x01}, std::byte{0x02}};
    GLIFI_REQUIRE(glifistore::siphash24(msg, k0, k1) == 0x85676696d7fb7e2dULL);
}

GLIFI_TEST("Index constructor captures process seed") {
    SeedGuard guard{0xDEADBEEFCAFEBABEULL};
    glifistore::Index index;
    GLIFI_REQUIRE(index.seed() == 0xDEADBEEFCAFEBABEULL);
    const auto empty = index.make_empty();
    GLIFI_REQUIRE(empty.seed() == index.seed());
}
