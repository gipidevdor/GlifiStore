#pragma once

#include "glifistore/core/error.hpp"

#include <cstddef>
#include <iosfwd>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace glifistore::cli {

enum class OptionArity { none, required };

struct OptionSpec {
    std::size_t id{};
    std::string_view long_name;
    char short_name{};
    OptionArity arity{OptionArity::none};
    std::string_view value_name;
    std::string_view description;
    bool repeatable{};
};

struct ParsedOption {
    std::size_t id{};
    std::string_view value;
};

struct ParsedArguments {
    std::vector<ParsedOption> options;
    std::vector<std::string_view> positionals;

    [[nodiscard]] auto has(std::size_t id) const noexcept -> bool;
    [[nodiscard]] auto value(std::size_t id) const noexcept -> std::optional<std::string_view>;
};

[[nodiscard]] auto executable_name(std::string_view argument_zero) noexcept -> std::string_view;
[[nodiscard]] auto parse_arguments(int argc, char* const argv[], std::span<const OptionSpec> specs)
    -> Result<ParsedArguments>;
[[nodiscard]] auto parse_size(std::string_view text, std::string_view option_name, std::size_t minimum,
                              std::size_t maximum) -> Result<std::size_t>;
[[nodiscard]] auto parse_byte_size(std::string_view text, std::string_view option_name, std::size_t minimum,
                                   std::size_t maximum) -> Result<std::size_t>;

void write_help(std::ostream& output, std::string_view program, std::string_view summary,
                std::string_view usage_suffix, std::span<const OptionSpec> specs);

} // namespace glifistore::cli
