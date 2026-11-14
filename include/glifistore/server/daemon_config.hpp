/**
 * @file
 * @brief Daemon configuration parsing, validation, and materialization.
 */
#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/core/worker_routing.hpp"
#include "glifistore/index/index_hash_seed.hpp"
#include "glifistore/server/daemon_log.hpp"
#include "glifistore/server/reactor.hpp"
#include "glifistore/store/store.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace glifistore::cli {
struct OptionSpec;
}

namespace glifistore::server {

/**
 * @brief Parsed daemon, Store, logging, transport, and sandbox configuration before resource creation.
 */
struct DaemonOptions {
    ReactorConfig server{};
    StoreConfig store{
        .maintenance = {.mode = MaintenanceMode::background},
    };
    /// Empty when no named deployment profile was selected.
    std::string deployment_profile{};
    /// Path echoed by --dump-config when --authz-map was set (policy lives on server.authz).
    std::filesystem::path authz_map_path{};
    /// Effective Index mix seed after parse (ADR 0026). Applied via set_index_hash_seed.
    std::uint64_t index_hash_seed{kDefaultIndexHashSeed};
    bool index_hash_seed_explicit{};
    WorkerRoutingState worker_routing{};
    bool worker_hash_seed_explicit{};
    bool secure_profile{};
    bool show_help{};
    bool show_version{};
    bool show_dump_config{};
    bool quiet{};
    DaemonLogFormat log_format{DaemonLogFormat::human};
};

/// Optional environment lookup for tests. Returning nullopt means unset.
using DaemonEnvironmentLookup = std::function<std::optional<std::string>(std::string_view name)>;

[[nodiscard]] auto daemon_option_specs() noexcept -> std::span<const cli::OptionSpec>;

[[nodiscard]] auto storage_mode_name(StorageMode mode) noexcept -> std::string_view;

/// Stable ASCII dump of the fully resolved effective configuration (paths only for TLS files).
[[nodiscard]] auto format_daemon_config_dump(const DaemonOptions& options) -> std::string;

/// Precedence: defaults < deployment profile < config file < environment < CLI.
/// --config / GLIFISTORE_CONFIG select the file; the file cannot set config=.
/// --profile / GLIFISTORE_PROFILE / profile= select dev, embedded, or production.
[[nodiscard]] auto parse_daemon_options(int argc, char* const argv[], DaemonEnvironmentLookup getenv_fn = {})
    -> Result<DaemonOptions>;

[[nodiscard]] auto load_daemon_config_file(const std::filesystem::path& path)
    -> Result<std::map<std::string, std::string, std::less<>>>;

[[nodiscard]] auto load_daemon_environment(const DaemonEnvironmentLookup& getenv_fn)
    -> Result<std::map<std::string, std::string, std::less<>>>;

[[nodiscard]] auto environment_name_for_option(std::string_view long_name) -> std::string;

} // namespace glifistore::server
