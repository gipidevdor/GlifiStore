#pragma once

#include <charconv>
#include <cstddef>
#include <optional>
#include <string_view>
#include <system_error>

namespace glifistore::bench {

[[nodiscard]] inline auto parse_decimal_size(const std::string_view text) noexcept
    -> std::optional<std::size_t> {
    if (text.empty()) {
        return std::nullopt;
    }
    std::size_t parsed{};
    const auto converted = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return parsed;
}

} // namespace glifistore::bench
