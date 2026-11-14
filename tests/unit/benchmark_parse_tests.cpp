#include "parse.hpp"
#include "test.hpp"

#include <limits>
#include <string>

GLIFI_TEST("benchmark decimal size parser accepts complete representable values") {
    const auto zero = glifistore::bench::parse_decimal_size("0");
    GLIFI_REQUIRE(zero.has_value());
    GLIFI_REQUIRE(*zero == 0U);

    const auto maximum_text = std::to_string(std::numeric_limits<std::size_t>::max());
    const auto maximum = glifistore::bench::parse_decimal_size(maximum_text);
    GLIFI_REQUIRE(maximum.has_value());
    GLIFI_REQUIRE(*maximum == std::numeric_limits<std::size_t>::max());
}

GLIFI_TEST("benchmark decimal size parser rejects partial signed and overflowing input") {
    GLIFI_REQUIRE(!glifistore::bench::parse_decimal_size("").has_value());
    GLIFI_REQUIRE(!glifistore::bench::parse_decimal_size("12x").has_value());
    GLIFI_REQUIRE(!glifistore::bench::parse_decimal_size("-1").has_value());
    GLIFI_REQUIRE(!glifistore::bench::parse_decimal_size("+1").has_value());
    GLIFI_REQUIRE(!glifistore::bench::parse_decimal_size(" 1").has_value());

    const auto overflow = std::to_string(std::numeric_limits<std::size_t>::max()) + "0";
    GLIFI_REQUIRE(!glifistore::bench::parse_decimal_size(overflow).has_value());
}
