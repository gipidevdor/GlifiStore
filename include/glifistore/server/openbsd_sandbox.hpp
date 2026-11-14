/**
 * @file
 * @brief OpenBSD pledge/unveil planning from resolved daemon resources.
 */
#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/server/daemon_config.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace glifistore::server {

// OpenBSD pledge(2)/unveil(2) confinement for glifistored (security roadmap
// Phase 6 / ADR 0020 follow-on). On non-OpenBSD builds these helpers are
// compile-time no-ops so Linux/macOS/FreeBSD stay unchanged.
//
// Promise set (reviewed): stdio rpath wpath cpath dpath inet unix flock fattr.
// Unveil targets: durable data directory (rwc) plus TLS cert/key/CA/CRL and
// authz-map paths (r) when configured; Unix socket parent directory (rwc) for
// bind/unlink when --unix-socket is set. Fail closed when applied.

/**
 * @brief Resolved path and unveil permission string to install before locking down the namespace.
 */
struct OpenbsdUnveilEntry {
    std::filesystem::path path{};
    std::string permissions{}; // unveil(2) permission string, e.g. "rwc" or "r"
};

/**
 * @brief Observable platform support plus the complete unveil/pledge plan.
 */
struct OpenbsdSandboxPlan {
    bool available{};
    std::vector<OpenbsdUnveilEntry> unveils{};
    std::string promises{};
};

[[nodiscard]] constexpr auto openbsd_sandbox_promises() noexcept -> std::string_view {
    // inet: TCP listeners/clients. unix: AF_UNIX (--unix-socket). flock/fattr:
    // durable namespace locks/attrs. No dns/proc/exec/tmppath: bind by address;
    // no fork/exec after listen; TLS material is loaded before apply.
    return "stdio rpath wpath cpath dpath inet unix flock fattr";
}

[[nodiscard]] auto openbsd_sandbox_supported() noexcept -> bool;

/// Builds the unveil plan from resolved daemon options. Paths are absolute;
/// missing optional paths are omitted. Does not call unveil/pledge.
[[nodiscard]] auto plan_openbsd_sandbox(const DaemonOptions& options) -> Result<OpenbsdSandboxPlan>;

/// Applies unveil then pledge. No-op success when !openbsd_sandbox_supported().
/// Fail closed on any unveil/pledge error (including empty durable path when
/// storage requires a data directory that was not opened yet).
[[nodiscard]] auto apply_openbsd_sandbox(const DaemonOptions& options) -> Status;

} // namespace glifistore::server
