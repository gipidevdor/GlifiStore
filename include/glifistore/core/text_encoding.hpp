/**
 * @file
 * @brief Lowercase hexadecimal and JSON string-content encoders shared by logs, reports and tools.
 *
 * Internal utility header (not installed). Output is ASCII and deterministic: operator reports,
 * audit records and migration checkpoints compare these strings byte for byte.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace glifistore {

inline constexpr std::string_view kLowerHexDigits{"0123456789abcdef"};

/**
 * @brief Append two lowercase hexadecimal digits for one octet.
 */
inline void append_lower_hex_octet(std::string& out, const unsigned char octet) {
    out.push_back(kLowerHexDigits[(octet >> 4U) & 0xFU]);
    out.push_back(kLowerHexDigits[octet & 0xFU]);
}

/**
 * @brief Encode bytes as lowercase hexadecimal, two digits per byte, with no separators.
 */
[[nodiscard]] inline auto to_lower_hex(const std::span<const std::byte> bytes) -> std::string {
    std::string out;
    out.reserve(bytes.size() * 2U);
    for (const auto byte : bytes) {
        append_lower_hex_octet(out, static_cast<unsigned char>(byte));
    }
    return out;
}

/**
 * @brief Encode the octets of a byte string (for example a Key) as lowercase hexadecimal.
 */
[[nodiscard]] inline auto to_lower_hex(const std::string_view text) -> std::string {
    return to_lower_hex(std::as_bytes(std::span{text.data(), text.size()}));
}

/**
 * @brief Append @p text escaped as JSON string content (without surrounding quotes).
 *
 * Escapes backslash, quote, `\n`, `\r`, `\t` and other control octets below 0x20 as `\u00XX`.
 * Octets at or above 0x20 are copied unchanged, so non-ASCII input is passed through as bytes.
 */
inline void append_json_escaped(std::string& out, const std::string_view text) {
    for (const char ch : text) {
        switch (ch) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20U) {
                out += "\\u00";
                append_lower_hex_octet(out, static_cast<unsigned char>(ch));
            } else {
                out.push_back(ch);
            }
            break;
        }
    }
}

/**
 * @brief Escape all of @p text as JSON string content.
 */
[[nodiscard]] inline auto json_escape(const std::string_view text) -> std::string {
    std::string out;
    out.reserve(text.size() + 8U);
    append_json_escaped(out, text);
    return out;
}

/**
 * @brief Escape at most @p max_input_bytes of @p text; append `...` when input was truncated.
 *
 * Bounds log and audit field size against client-controlled input.
 */
[[nodiscard]] inline auto json_escape_bounded(const std::string_view text, const std::size_t max_input_bytes)
    -> std::string {
    const auto retained = text.substr(0, std::min(text.size(), max_input_bytes));
    std::string out;
    out.reserve(retained.size() + 16U);
    append_json_escaped(out, retained);
    if (text.size() > max_input_bytes) {
        out += "...";
    }
    return out;
}

} // namespace glifistore
