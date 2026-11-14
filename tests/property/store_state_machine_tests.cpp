#include "support/stateful_store_model.hpp"
#include "test.hpp"

#include <cstdint>
#include <random>
#include <vector>

GLIFI_TEST("durable Store state machine matches reference model across reopen") {
    constexpr std::uint64_t seed_count = 8;
    constexpr std::size_t bytes_per_seed = 192;
    for (std::uint64_t seed = 0; seed < seed_count; ++seed) {
        std::mt19937_64 random{0x475359504841ULL ^ seed};
        std::vector<std::uint8_t> input(bytes_per_seed);
        for (auto& byte : input) {
            byte = static_cast<std::uint8_t>(random());
        }
        glifistore::test::run_stateful_store_model(input, seed);
    }
}
