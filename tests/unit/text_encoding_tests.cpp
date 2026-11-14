#include "glifistore/core/text_encoding.hpp"
#include "test.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

GLIFI_TEST("lowercase hex encodes every octet as two digits") {
    constexpr std::array<std::byte, 4> kBytes{std::byte{0x00}, std::byte{0x0f}, std::byte{0xa5},
                                              std::byte{0xff}};
    GLIFI_REQUIRE(glifistore::to_lower_hex(std::span<const std::byte>{kBytes}) == "000fa5ff");
    GLIFI_REQUIRE(glifistore::to_lower_hex(std::string_view{"\x01k\xfe", 3}) == "016bfe");
    GLIFI_REQUIRE(glifistore::to_lower_hex(std::string_view{}).empty());
}

GLIFI_TEST("json escape covers quotes, backslash and control octets") {
    const std::string input{"a\"b\\c\n\r\t\x01\x1f\x7f", 11};
    GLIFI_REQUIRE(glifistore::json_escape(input) == "a\\\"b\\\\c\\n\\r\\t\\u0001\\u001f\x7f");
    GLIFI_REQUIRE(glifistore::json_escape(std::string_view{"\0", 1}) == "\\u0000");
}

GLIFI_TEST("bounded json escape truncates input and marks the truncation") {
    GLIFI_REQUIRE(glifistore::json_escape_bounded("abcd", 4) == "abcd");
    GLIFI_REQUIRE(glifistore::json_escape_bounded("abcde", 4) == "abcd...");
    GLIFI_REQUIRE(glifistore::json_escape_bounded("\"\"\"", 2) == "\\\"\\\"...");
    GLIFI_REQUIRE(glifistore::json_escape_bounded("", 0).empty());
}
