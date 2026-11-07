#include "glifistore/store/value.hpp"
#include "test.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

GLIFI_TEST("OwnedBytes assigns aliased subranges across inline and heap storage") {
    for (const auto initial_size : {std::size_t{64}, std::size_t{128}}) {
        for (const auto target_size : {std::size_t{0}, std::size_t{32}, std::size_t{64}}) {
            std::vector<std::byte> original(initial_size);
            for (std::size_t index = 0; index < original.size(); ++index) {
                original[index] = static_cast<std::byte>(index);
            }
            glifistore::OwnedBytes value{original};
            const auto offset = target_size == initial_size ? 0U : 1U;
            const auto expected_bytes = std::span<const std::byte>{original}.subspan(offset, target_size);
            const std::vector<std::byte> expected(expected_bytes.begin(), expected_bytes.end());
            value.assign(value.data() + offset, target_size);
            GLIFI_REQUIRE(value.as_vector() == expected);
        }
    }
}

GLIFI_TEST("OwnedBytes assigns an aliased heap subrange without losing its bytes") {
    std::vector<std::byte> original(256);
    for (std::size_t index = 0; index < original.size(); ++index) {
        original[index] = static_cast<std::byte>(index);
    }
    glifistore::OwnedBytes value{original};
    const std::vector<std::byte> expected(original.begin() + 7, original.begin() + 135);
    value.assign(value.data() + 7, std::size_t{128});
    GLIFI_REQUIRE(value.as_vector() == expected);
}
