#include "glifistore/server/authz.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

namespace glifistore::server {
namespace {

[[nodiscard]] auto trim(std::string_view text) noexcept -> std::string_view {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] auto split_capabilities(std::string_view text) -> Result<Capability> {
    Capability set = Capability::none;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const auto comma = text.find(',', begin);
        const auto piece = trim(
            text.substr(begin, comma == std::string_view::npos ? std::string_view::npos : comma - begin));
        if (!piece.empty()) {
            auto parsed = parse_capability_token(piece);
            if (!parsed) {
                return unexpected(parsed.error());
            }
            set = set | *parsed;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        begin = comma + 1;
    }
    if (set == Capability::none) {
        return fail(ErrorCode::invalid_argument, "authz map entry requires at least one capability");
    }
    return normalize_capabilities(set);
}

[[nodiscard]] auto parse_optional_prefix(std::string_view rest, const std::string_view where,
                                         const std::size_t line_number) -> Result<std::string> {
    rest = trim(rest);
    if (rest.empty()) {
        return std::string{};
    }
    constexpr std::string_view kPrefixToken = "prefix=";
    if (!rest.starts_with(kPrefixToken)) {
        return fail(ErrorCode::invalid_argument,
                    std::string{where} + " line " + std::to_string(line_number) +
                        ": expected optional 'prefix=<bytes>' after capabilities, got '" + std::string{rest} +
                        "'");
    }
    const auto value = rest.substr(kPrefixToken.size());
    if (value.empty()) {
        return fail(ErrorCode::invalid_argument,
                    std::string{where} + " line " + std::to_string(line_number) +
                        ": prefix= must not be empty (omit prefix for unrestricted keyspace)");
    }
    if (value.find_first_of(" \t") != std::string_view::npos) {
        return fail(ErrorCode::invalid_argument, std::string{where} + " line " + std::to_string(line_number) +
                                                     ": prefix value must not contain whitespace");
    }
    return std::string{value};
}

} // namespace

auto parse_capability_token(const std::string_view token) -> Result<Capability> {
    if (token == "read") {
        return Capability::read;
    }
    if (token == "write") {
        return Capability::write;
    }
    if (token == "admin") {
        return Capability::admin;
    }
    return fail(ErrorCode::invalid_argument,
                "unknown capability '" + std::string{token} + "' (expected read, write, or admin)");
}

auto capability_name(const Capability capability) noexcept -> std::string_view {
    switch (capability) {
    case Capability::read:
        return "read";
    case Capability::write:
        return "write";
    case Capability::admin:
        return "admin";
    case Capability::none:
        return "none";
    }
    return "unknown";
}

auto AuthzPolicy::parse(const std::string_view text, const std::string_view where) -> Result<AuthzPolicy> {
    AuthzPolicy policy;
    policy.enabled_ = true;
    std::size_t line_number = 0;
    std::size_t offset = 0;
    while (offset <= text.size()) {
        const auto end = text.find('\n', offset);
        auto line =
            text.substr(offset, end == std::string_view::npos ? std::string_view::npos : end - offset);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        ++line_number;
        offset = end == std::string_view::npos ? text.size() + 1 : end + 1;

        const auto trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }
        const auto space = trimmed.find_first_of(" \t");
        if (space == std::string_view::npos) {
            return fail(ErrorCode::invalid_argument,
                        std::string{where} + " line " + std::to_string(line_number) +
                            ": expected 'principal capability[,capability...] [prefix=<bytes>]'");
        }
        const auto principal = trim(trimmed.substr(0, space));
        auto rest = trim(trimmed.substr(space + 1));
        if (principal.empty()) {
            return fail(ErrorCode::invalid_argument, std::string{where} + " line " +
                                                         std::to_string(line_number) +
                                                         ": principal must not be empty");
        }
        const auto caps_end = rest.find_first_of(" \t");
        const auto caps_text = trim(caps_end == std::string_view::npos ? rest : rest.substr(0, caps_end));
        const auto after_caps =
            caps_end == std::string_view::npos ? std::string_view{} : trim(rest.substr(caps_end));
        auto caps = split_capabilities(caps_text);
        if (!caps) {
            return fail(ErrorCode::invalid_argument, std::string{where} + " line " +
                                                         std::to_string(line_number) + ": " +
                                                         caps.error().message);
        }
        auto prefix = parse_optional_prefix(after_caps, where, line_number);
        if (!prefix) {
            return unexpected(prefix.error());
        }
        AuthzGrant grant{.capabilities = *caps, .key_prefix = std::move(*prefix)};
        if (!policy.principals_.emplace(std::string{principal}, std::move(grant)).second) {
            return fail(ErrorCode::invalid_argument,
                        std::string{where} + " line " + std::to_string(line_number) +
                            ": duplicate principal '" + std::string{principal} + "'");
        }
    }
    return policy;
}

auto AuthzPolicy::load_file(const std::filesystem::path& path) -> Result<AuthzPolicy> {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return fail(ErrorCode::invalid_argument,
                    "authz map is missing or not a regular file: " + path.string());
    }
    std::ifstream input{path};
    if (!input) {
        return fail(ErrorCode::io_error, "cannot open authz map: " + path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input && !input.eof()) {
        return fail(ErrorCode::io_error, "cannot read authz map: " + path.string());
    }
    return parse(buffer.str(), path.string());
}

auto AuthzPolicy::prefix_scoped_count() const noexcept -> std::size_t {
    std::size_t count = 0;
    for (const auto& [_, grant] : principals_) {
        if (!grant.key_prefix.empty()) {
            ++count;
        }
    }
    return count;
}

auto AuthzPolicy::capabilities_for(const std::string_view principal) const noexcept -> Capability {
    if (!enabled_ || principal.empty()) {
        return Capability::none;
    }
    const auto found = principals_.find(principal);
    if (found == principals_.end()) {
        return Capability::none;
    }
    return found->second.capabilities;
}

auto AuthzPolicy::grant_for(const std::string_view principal) const -> AuthzGrant {
    if (!enabled_ || principal.empty()) {
        return {};
    }
    const auto found = principals_.find(principal);
    if (found == principals_.end()) {
        return {};
    }
    return found->second;
}

auto AuthzPolicy::key_prefix_for(const std::string_view principal) const -> std::string {
    return grant_for(principal).key_prefix;
}

void AuthzPolicy::bind(std::string principal, const Capability capabilities, std::string key_prefix) {
    enabled_ = true;
    principals_[std::move(principal)] =
        AuthzGrant{.capabilities = normalize_capabilities(capabilities), .key_prefix = std::move(key_prefix)};
}

auto required_capability(const RequestOpcode opcode) noexcept -> Capability {
    return required_capability(opcode, {});
}

auto required_capability(const RequestOpcode opcode, const std::string_view key_prefix) noexcept
    -> Capability {
    switch (opcode) {
    case RequestOpcode::init:
    case RequestOpcode::bind_worker:
    case RequestOpcode::health:
    case RequestOpcode::ready:
        return Capability::none;
    case RequestOpcode::ping:
    case RequestOpcode::get:
        return Capability::read;
    case RequestOpcode::stats:
        // Phase 8: prefix-scoped principals must not observe daemon-wide STATS
        // (fail closed). Unscoped principals keep read; admin always implies read.
        return key_prefix.empty() ? Capability::read : Capability::admin;
    case RequestOpcode::backup:
        return Capability::admin;
    case RequestOpcode::put:
    case RequestOpcode::erase:
        return Capability::write;
    }
    return Capability::admin;
}

auto opcode_requires_key_prefix_check(const RequestOpcode opcode) noexcept -> bool {
    switch (opcode) {
    case RequestOpcode::get:
    case RequestOpcode::put:
    case RequestOpcode::erase:
        return true;
    case RequestOpcode::init:
    case RequestOpcode::ping:
    case RequestOpcode::bind_worker:
    case RequestOpcode::health:
    case RequestOpcode::ready:
    case RequestOpcode::stats:
    case RequestOpcode::backup:
        return false;
    }
    return true;
}

auto key_matches_prefix(const std::string_view prefix, const std::span<const std::byte> key) noexcept
    -> bool {
    if (prefix.empty()) {
        return true;
    }
    if (key.size() < prefix.size()) {
        return false;
    }
    const auto* key_bytes = reinterpret_cast<const char*>(key.data());
    return std::string_view{key_bytes, prefix.size()} == prefix;
}

auto authorize_opcode(const AuthzPolicy& policy, const Capability granted,
                      const RequestOpcode opcode) noexcept -> bool {
    return authorize_opcode(policy, granted, opcode, {});
}

auto authorize_opcode(const AuthzPolicy& policy, const Capability granted, const RequestOpcode opcode,
                      const std::string_view key_prefix) noexcept -> bool {
    if (!policy.enabled()) {
        return true;
    }
    const auto needed = required_capability(opcode, key_prefix);
    if (needed == Capability::none) {
        return true;
    }
    return has_capability(granted, needed);
}

auto authorize_request(const AuthzPolicy& policy, const Capability granted, const std::string_view key_prefix,
                       const RequestOpcode opcode, const std::span<const std::byte> key) noexcept -> bool {
    if (!authorize_opcode(policy, granted, opcode, key_prefix)) {
        return false;
    }
    if (!policy.enabled() || !opcode_requires_key_prefix_check(opcode)) {
        return true;
    }
    return key_matches_prefix(key_prefix, key);
}

} // namespace glifistore::server
