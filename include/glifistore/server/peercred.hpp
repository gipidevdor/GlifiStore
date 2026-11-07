/**
 * @file
 * @brief Platform-local Unix-socket peer identity extraction.
 */
#pragma once

#include "glifistore/core/error.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace glifistore::server {

/// OS peer identity for an accepted Unix-domain connection (Phase 8 / ADR 0029).
/// pid may be unavailable on some platforms; uid/gid are always set on success.
struct PeerCredentials {
    std::uint32_t uid{};
    std::uint32_t gid{};
    /// Zero means "unknown / not reported" (never a real init pid in this API).
    std::uint32_t pid{};
};

/// True when this build can obtain peer credentials on AF_UNIX sockets.
[[nodiscard]] auto peercred_supported() noexcept -> bool;

/// Platform notes (compile-time):
/// - Linux: SO_PEERCRED (struct ucred)
/// - macOS / FreeBSD: getpeereid (+ LOCAL_PEEREPID for pid when available)
/// - OpenBSD: getpeereid (pid not reported)
[[nodiscard]] auto peer_credentials(int descriptor) -> Result<PeerCredentials>;

/// Canonical authz principal: "unix:uid=<decimal>". Exact-match for --authz-map.
[[nodiscard]] auto peercred_principal(const PeerCredentials& credentials) -> std::string;

[[nodiscard]] constexpr auto peercred_principal_prefix() noexcept -> std::string_view {
    return "unix:uid=";
}

} // namespace glifistore::server
