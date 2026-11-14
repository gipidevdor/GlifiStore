#pragma once

#include "test.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace glifistore::test {

inline auto hex_value(char character) -> std::optional<std::uint8_t> {
    if (character >= '0' && character <= '9') {
        return static_cast<std::uint8_t>(character - '0');
    }
    if (character >= 'a' && character <= 'f') {
        return static_cast<std::uint8_t>(character - 'a' + 10);
    }
    if (character >= 'A' && character <= 'F') {
        return static_cast<std::uint8_t>(character - 'A' + 10);
    }
    return std::nullopt;
}

inline auto read_hex_fixture(const std::filesystem::path& path) -> std::vector<std::byte> {
    std::ifstream stream(path);
    require(stream.is_open(), "fixture file is open", __FILE__, __LINE__);
    const std::string contents{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    std::vector<std::uint8_t> digits;
    for (char character : contents) {
        if (std::isspace(static_cast<unsigned char>(character)) != 0) {
            continue;
        }
        const auto value = hex_value(character);
        require(value.has_value(), "fixture contains only hexadecimal digits", __FILE__, __LINE__);
        digits.push_back(*value);
    }
    require(digits.size() % 2 == 0, "fixture has complete hexadecimal byte pairs", __FILE__, __LINE__);

    std::vector<std::byte> bytes;
    bytes.reserve(digits.size() / 2);
    for (std::size_t index = 0; index < digits.size(); index += 2) {
        bytes.push_back(static_cast<std::byte>((digits[index] << 4U) | digits[index + 1]));
    }
    return bytes;
}

} // namespace glifistore::test
