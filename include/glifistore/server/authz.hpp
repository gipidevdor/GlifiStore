#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/server/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace glifistore::server {

enum class Capability : std::uint8_t {
    none = 0,
    read = 1 << 0,
    write = 1 << 1,
    admin = 1 << 2,
};

[[nodiscard]] constexpr auto operator|(Capability left, Capability right) noexcept -> Capability {
    return static_cast<Capability>(static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
}

[[nodiscard]] constexpr auto operator&(Capability left, Capability right) noexcept -> Capability {
    return static_cast<Capability>(static_cast<std::uint8_t>(left) & static_cast<std::uint8_t>(right));
}

[[nodiscard]] constexpr auto has_capability(Capability set, Capability needle) noexcept -> bool {
    return (set & needle) == needle;
}

[[nodiscard]] constexpr auto normalize_capabilities(Capability set) noexcept -> Capability {
    if (has_capability(set, Capability::admin)) {
        set = set | Capability::write | Capability::read;
    } else if (has_capability(set, Capability::write)) {
        set = set | Capability::read;
    }
    return set;
}

[[nodiscard]] auto parse_capability_token(std::string_view token) -> Result<Capability>;
[[nodiscard]] auto capability_name(Capability capability) noexcept -> std::string_view;

// Phase 8 first slice: optional key-prefix scope per principal (ADR 0022 amendment / ADR 0025).
// Empty key_prefix means unrestricted keyspace for that principal's capabilities.
struct AuthzGrant final {
    Capability capabilities{Capability::none};
    std::string key_prefix{};
};

class AuthzPolicy final {
  public:
    AuthzPolicy() = default;

    [[nodiscard]] static auto load_file(const std::filesystem::path& path) -> Result<AuthzPolicy>;
    [[nodiscard]] static auto parse(std::string_view text, std::string_view where = "authz map")
        -> Result<AuthzPolicy>;

    [[nodiscard]] auto enabled() const noexcept -> bool {
        return enabled_;
    }

    [[nodiscard]] auto empty() const noexcept -> bool {
        return principals_.empty();
    }

    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return principals_.size();
    }

    [[nodiscard]] auto prefix_scoped_count() const noexcept -> std::size_t;
    [[nodiscard]] auto capabilities_for(std::string_view principal) const noexcept -> Capability;
    [[nodiscard]] auto grant_for(std::string_view principal) const -> AuthzGrant;
    [[nodiscard]] auto key_prefix_for(std::string_view principal) const -> std::string;

    void set_enabled(bool enabled) noexcept {
        enabled_ = enabled;
    }

    void bind(std::string principal, Capability capabilities, std::string key_prefix = {});

  private:
    struct TransparentStringHash final {
        using is_transparent = void;

        [[nodiscard]] auto operator()(const std::string_view value) const noexcept -> std::size_t {
            return std::hash<std::string_view>{}(value);
        }
    };

    bool enabled_{};
    std::unordered_map<std::string, AuthzGrant, TransparentStringHash, std::equal_to<>> principals_{};
};

[[nodiscard]] auto required_capability(RequestOpcode opcode) noexcept -> Capability;
// When key_prefix is non-empty (prefix-scoped principal), STATS requires admin
// (ADR 0027 / Phase 8 STATS isolation). Pass empty prefix for unrestricted principals.
[[nodiscard]] auto required_capability(RequestOpcode opcode, std::string_view key_prefix) noexcept
    -> Capability;
[[nodiscard]] auto opcode_requires_key_prefix_check(RequestOpcode opcode) noexcept -> bool;
[[nodiscard]] auto key_matches_prefix(std::string_view prefix, std::span<const std::byte> key) noexcept
    -> bool;
[[nodiscard]] auto authorize_opcode(const AuthzPolicy& policy, Capability granted,
                                    RequestOpcode opcode) noexcept -> bool;
[[nodiscard]] auto authorize_opcode(const AuthzPolicy& policy, Capability granted, RequestOpcode opcode,
                                    std::string_view key_prefix) noexcept -> bool;
[[nodiscard]] auto authorize_request(const AuthzPolicy& policy, Capability granted,
                                     std::string_view key_prefix, RequestOpcode opcode,
                                     std::span<const std::byte> key) noexcept -> bool;

} // namespace glifistore::server
