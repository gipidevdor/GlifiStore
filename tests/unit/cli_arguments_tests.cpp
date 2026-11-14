#include "cli/arguments.hpp"
#include "test.hpp"

#include <array>
#include <limits>
#include <string>

namespace {

enum OptionId : std::size_t { help, port, verbose };

constexpr std::array kOptions{
    glifistore::cli::OptionSpec{help, "help", 'h', glifistore::cli::OptionArity::none, {}, "Show help"},
    glifistore::cli::OptionSpec{port, "port", 'p', glifistore::cli::OptionArity::required, "PORT",
                                "Listen port"},
    glifistore::cli::OptionSpec{
        verbose, "verbose", 'v', glifistore::cli::OptionArity::none, {}, "Enable verbose output"},
};

template <std::size_t Size>
[[nodiscard]] auto parse(const std::array<const char*, Size>& input)
    -> glifistore::Result<glifistore::cli::ParsedArguments> {
    std::array<char*, Size> arguments{};
    for (std::size_t index = 0; index < Size; ++index) {
        arguments[index] = const_cast<char*>(input[index]);
    }
    return glifistore::cli::parse_arguments(static_cast<int>(arguments.size()), arguments.data(), kOptions);
}

} // namespace

GLIFI_TEST("cli parses long short inline and positional arguments") {
    const std::array arguments{"/usr/bin/tool", "--verbose", "--port=7379", "segment.gseg"};
    const auto parsed = parse(arguments);
    GLIFI_REQUIRE(parsed.has_value());
    GLIFI_REQUIRE(parsed->has(verbose));
    GLIFI_REQUIRE(parsed->value(port) == "7379");
    GLIFI_REQUIRE(parsed->positionals.size() == 1);
    GLIFI_REQUIRE(parsed->positionals.front() == "segment.gseg");

    const std::array short_arguments{"tool", "-p7379", "-h"};
    const auto short_parsed = parse(short_arguments);
    GLIFI_REQUIRE(short_parsed.has_value());
    GLIFI_REQUIRE(short_parsed->value(port) == "7379");
    GLIFI_REQUIRE(short_parsed->has(help));
}

GLIFI_TEST("cli option delimiter preserves dash-prefixed positional arguments") {
    const std::array arguments{"tool", "--", "--not-an-option", "-"};
    const auto parsed = parse(arguments);
    GLIFI_REQUIRE(parsed.has_value());
    GLIFI_REQUIRE(parsed->positionals.size() == 2);
    GLIFI_REQUIRE(parsed->positionals[0] == "--not-an-option");
    GLIFI_REQUIRE(parsed->positionals[1] == "-");
}

GLIFI_TEST("cli rejects duplicates unknown options and missing values") {
    const std::array duplicate{"tool", "--port", "1", "-p2"};
    GLIFI_REQUIRE(!parse(duplicate).has_value());

    const std::array unknown{"tool", "--porrt", "1"};
    const auto unknown_result = parse(unknown);
    GLIFI_REQUIRE(!unknown_result.has_value());
    GLIFI_REQUIRE(unknown_result.error().message.find("did you mean --port?") != std::string::npos);

    const std::array missing{"tool", "--port"};
    GLIFI_REQUIRE(!parse(missing).has_value());

    const std::array flag_value{"tool", "--verbose=yes"};
    GLIFI_REQUIRE(!parse(flag_value).has_value());
}

GLIFI_TEST("cli numeric parser enforces complete input and bounds") {
    const auto valid = glifistore::cli::parse_size("7379", "--port", 0, 65535);
    GLIFI_REQUIRE(valid.has_value());
    GLIFI_REQUIRE(*valid == 7379);
    GLIFI_REQUIRE(!glifistore::cli::parse_size("7379x", "--port", 0, 65535).has_value());
    GLIFI_REQUIRE(!glifistore::cli::parse_size("65536", "--port", 0, 65535).has_value());
    GLIFI_REQUIRE(!glifistore::cli::parse_size("0", "--workers", 1, std::numeric_limits<std::size_t>::max())
                       .has_value());
}

GLIFI_TEST("cli byte-size parser supports decimal and binary suffixes") {
    const auto binary = glifistore::cli::parse_byte_size("4MiB", "--max-input-bytes", 1, 1U << 30U);
    GLIFI_REQUIRE(binary.has_value());
    GLIFI_REQUIRE(*binary == 4U * 1024U * 1024U);
    const auto decimal = glifistore::cli::parse_byte_size("4MB", "--max-input-bytes", 1, 1U << 30U);
    GLIFI_REQUIRE(decimal.has_value());
    GLIFI_REQUIRE(*decimal == 4'000'000U);
    GLIFI_REQUIRE(!glifistore::cli::parse_byte_size("4XB", "--max-input-bytes", 1, 1U << 30U).has_value());
    GLIFI_REQUIRE(!glifistore::cli::parse_byte_size("999999999999GiB", "--max-input-bytes", 1,
                                                    std::numeric_limits<std::size_t>::max())
                       .has_value());
}

GLIFI_TEST("cli executable name strips unix and windows directories") {
    GLIFI_REQUIRE(glifistore::cli::executable_name("/usr/local/bin/glifistored") == "glifistored");
    GLIFI_REQUIRE(glifistore::cli::executable_name("C:\\bin\\glifistored.exe") == "glifistored.exe");
}
