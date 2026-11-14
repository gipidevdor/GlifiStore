#include "cli/arguments.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <ostream>
#include <string>

namespace glifistore::cli {
namespace {

[[nodiscard]] auto find_long(const std::span<const OptionSpec> specs, const std::string_view name)
    -> std::optional<std::size_t> {
    for (std::size_t index = 0; index < specs.size(); ++index) {
        if (specs[index].long_name == name) {
            return index;
        }
    }
    return std::nullopt;
}

[[nodiscard]] auto find_short(const std::span<const OptionSpec> specs, const char name)
    -> std::optional<std::size_t> {
    for (std::size_t index = 0; index < specs.size(); ++index) {
        if (specs[index].short_name != '\0' && specs[index].short_name == name) {
            return index;
        }
    }
    return std::nullopt;
}

[[nodiscard]] auto option_label(const OptionSpec& spec) -> std::string {
    return "--" + std::string{spec.long_name};
}

[[nodiscard]] auto edit_distance(const std::string_view left, const std::string_view right) -> std::size_t {
    std::vector<std::size_t> previous(right.size() + 1U);
    std::vector<std::size_t> current(right.size() + 1U);
    for (std::size_t index = 0; index <= right.size(); ++index) {
        previous[index] = index;
    }
    for (std::size_t left_index = 1; left_index <= left.size(); ++left_index) {
        current[0] = left_index;
        for (std::size_t right_index = 1; right_index <= right.size(); ++right_index) {
            const auto substitution =
                previous[right_index - 1U] + (left[left_index - 1U] == right[right_index - 1U] ? 0U : 1U);
            current[right_index] =
                std::min({previous[right_index] + 1U, current[right_index - 1U] + 1U, substitution});
        }
        std::swap(previous, current);
    }
    return previous.back();
}

[[nodiscard]] auto unknown_long_option(const std::span<const OptionSpec> specs, const std::string_view name)
    -> Unexpected {
    const OptionSpec* closest = nullptr;
    auto closest_distance = std::numeric_limits<std::size_t>::max();
    for (const auto& spec : specs) {
        const auto distance = edit_distance(name, spec.long_name);
        if (distance < closest_distance) {
            closest = &spec;
            closest_distance = distance;
        }
    }
    std::string message = "unknown option: --" + std::string{name};
    const auto suggestion_limit = std::max<std::size_t>(1, name.size() / 3U);
    if (closest != nullptr && closest_distance <= suggestion_limit) {
        message += "; did you mean --";
        message += closest->long_name;
        message += '?';
    }
    return fail(ErrorCode::invalid_argument, std::move(message));
}

[[nodiscard]] auto record_option(ParsedArguments& parsed, std::vector<bool>& seen,
                                 const std::span<const OptionSpec> specs, const std::size_t spec_index,
                                 const std::string_view value) -> Status {
    const auto& spec = specs[spec_index];
    if (seen[spec_index] && !spec.repeatable) {
        return fail(ErrorCode::invalid_argument, "option specified more than once: " + option_label(spec));
    }
    seen[spec_index] = true;
    parsed.options.push_back({.id = spec.id, .value = value});
    return {};
}

[[nodiscard]] auto parse_long_option(int& index, const int argc, char* const argv[],
                                     const std::span<const OptionSpec> specs, ParsedArguments& parsed,
                                     std::vector<bool>& seen) -> Status {
    const std::string_view argument{argv[index]};
    const auto equals = argument.find('=');
    const auto name = argument.substr(2, equals == std::string_view::npos ? equals : equals - 2U);
    const auto found = find_long(specs, name);
    if (!found) {
        return unknown_long_option(specs, name);
    }
    const auto& spec = specs[*found];
    const bool inline_value = equals != std::string_view::npos;
    if (spec.arity == OptionArity::none) {
        if (inline_value) {
            return fail(ErrorCode::invalid_argument, option_label(spec) + " does not accept a value");
        }
        return record_option(parsed, seen, specs, *found, {});
    }
    if (inline_value) {
        const auto value = argument.substr(equals + 1U);
        if (value.empty()) {
            return fail(ErrorCode::invalid_argument, "missing value for " + option_label(spec));
        }
        return record_option(parsed, seen, specs, *found, value);
    }
    if (index + 1 >= argc) {
        return fail(ErrorCode::invalid_argument, "missing value for " + option_label(spec));
    }
    ++index;
    return record_option(parsed, seen, specs, *found, argv[index]);
}

[[nodiscard]] auto parse_short_option(int& index, const int argc, char* const argv[],
                                      const std::span<const OptionSpec> specs, ParsedArguments& parsed,
                                      std::vector<bool>& seen) -> Status {
    const std::string_view argument{argv[index]};
    const auto found = find_short(specs, argument[1]);
    if (!found) {
        return fail(ErrorCode::invalid_argument, "unknown option: -" + std::string{1, argument[1]});
    }
    const auto& spec = specs[*found];
    if (spec.arity == OptionArity::none) {
        if (argument.size() != 2) {
            return fail(ErrorCode::invalid_argument,
                        "combined short options are not supported: " + std::string{argument});
        }
        return record_option(parsed, seen, specs, *found, {});
    }
    if (argument.size() > 2) {
        return record_option(parsed, seen, specs, *found, argument.substr(2));
    }
    if (index + 1 >= argc) {
        return fail(ErrorCode::invalid_argument, "missing value for " + option_label(spec));
    }
    ++index;
    return record_option(parsed, seen, specs, *found, argv[index]);
}

[[nodiscard]] auto rendered_option(const OptionSpec& spec) -> std::string {
    std::string rendered{"  "};
    if (spec.short_name != '\0') {
        rendered += '-';
        rendered += spec.short_name;
        rendered += ", ";
    } else {
        rendered += "    ";
    }
    rendered += "--";
    rendered += spec.long_name;
    if (spec.arity == OptionArity::required) {
        rendered += " <";
        rendered += spec.value_name;
        rendered += '>';
    }
    return rendered;
}

} // namespace

auto ParsedArguments::has(const std::size_t id) const noexcept -> bool {
    return std::ranges::any_of(options, [id](const auto& option) { return option.id == id; });
}

auto ParsedArguments::value(const std::size_t id) const noexcept -> std::optional<std::string_view> {
    const auto found = std::ranges::find_if(options, [id](const auto& option) { return option.id == id; });
    return found == options.end() ? std::nullopt : std::optional{found->value};
}

auto executable_name(const std::string_view argument_zero) noexcept -> std::string_view {
    const auto slash = argument_zero.find_last_of("/\\");
    auto name = argument_zero;
    if (slash != std::string_view::npos) {
        name.remove_prefix(slash + 1U);
    }
    return name;
}

auto parse_arguments(const int argc, char* const argv[], const std::span<const OptionSpec> specs)
    -> Result<ParsedArguments> {
    ParsedArguments parsed;
    std::vector<bool> seen(specs.size());
    bool positional_only = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (!positional_only && argument == "--") {
            positional_only = true;
            continue;
        }
        if (!positional_only && argument.starts_with("--") && argument.size() > 2) {
            if (auto status = parse_long_option(index, argc, argv, specs, parsed, seen); !status) {
                return unexpected(status.error());
            }
            continue;
        }
        if (!positional_only && argument.starts_with('-') && argument.size() > 1) {
            if (auto status = parse_short_option(index, argc, argv, specs, parsed, seen); !status) {
                return unexpected(status.error());
            }
            continue;
        }
        parsed.positionals.push_back(argument);
    }
    return parsed;
}

auto parse_size(const std::string_view text, const std::string_view option_name, const std::size_t minimum,
                const std::size_t maximum) -> Result<std::size_t> {
    std::size_t parsed{};
    const auto converted = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (text.empty() || converted.ec != std::errc{} || converted.ptr != text.data() + text.size() ||
        parsed < minimum || parsed > maximum) {
        return fail(ErrorCode::invalid_argument, std::string{option_name} + " must be an integer between " +
                                                     std::to_string(minimum) + " and " +
                                                     std::to_string(maximum) + ": " + std::string{text});
    }
    return parsed;
}

auto parse_byte_size(const std::string_view text, const std::string_view option_name,
                     const std::size_t minimum, const std::size_t maximum) -> Result<std::size_t> {
    const auto suffix_begin = text.find_first_not_of("0123456789");
    const auto digits = text.substr(0, suffix_begin);
    auto suffix =
        suffix_begin == std::string_view::npos ? std::string{} : std::string{text.substr(suffix_begin)};
    for (auto& character : suffix) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }

    std::size_t multiplier = 1;
    if (suffix.empty() || suffix == "b") {
        multiplier = 1;
    } else if (suffix == "kb") {
        multiplier = 1'000;
    } else if (suffix == "kib") {
        multiplier = 1U << 10U;
    } else if (suffix == "mb") {
        multiplier = 1'000'000;
    } else if (suffix == "mib") {
        multiplier = 1U << 20U;
    } else if (suffix == "gb") {
        multiplier = 1'000'000'000;
    } else if (suffix == "gib") {
        multiplier = 1U << 30U;
    } else {
        return fail(ErrorCode::invalid_argument,
                    std::string{option_name} + " has an unsupported byte-size suffix: " + std::string{text});
    }

    std::size_t amount{};
    const auto converted = std::from_chars(digits.data(), digits.data() + digits.size(), amount);
    if (digits.empty() || converted.ec != std::errc{} || converted.ptr != digits.data() + digits.size() ||
        amount > maximum / multiplier) {
        return fail(ErrorCode::invalid_argument,
                    std::string{option_name} +
                        " is outside the supported byte-size range: " + std::string{text});
    }
    const auto bytes = amount * multiplier;
    if (bytes < minimum || bytes > maximum) {
        return fail(ErrorCode::invalid_argument,
                    std::string{option_name} + " must be between " + std::to_string(minimum) + " and " +
                        std::to_string(maximum) + " bytes: " + std::string{text});
    }
    return bytes;
}

void write_help(std::ostream& output, const std::string_view program, const std::string_view summary,
                const std::string_view usage_suffix, const std::span<const OptionSpec> specs) {
    output << summary << "\n\nUsage:\n  " << program;
    if (!usage_suffix.empty()) {
        output << ' ' << usage_suffix;
    }
    output << "\n\nOptions:\n";
    std::size_t width = 0;
    for (const auto& spec : specs) {
        width = std::max(width, rendered_option(spec).size());
    }
    for (const auto& spec : specs) {
        const auto rendered = rendered_option(spec);
        output << rendered << std::string(width - rendered.size() + 2U, ' ') << spec.description << '\n';
    }
}

} // namespace glifistore::cli
