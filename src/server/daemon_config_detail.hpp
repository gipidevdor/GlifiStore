#pragma once

#include "cli/arguments.hpp"
#include "glifistore/core/types.hpp"
#include "glifistore/core/worker_routing.hpp"
#include "glifistore/index/index_hash_seed.hpp"
#include "glifistore/server/authz.hpp"
#include "glifistore/server/daemon_config.hpp"
#include "glifistore/server/peercred.hpp"
#include "glifistore/server/tls.hpp"

#include <array>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace glifistore::server::daemon_config_detail {

enum OptionId : std::size_t {
    help,
    version,
    config,
    profile,
    dump_config,
    bind,
    port,
    shard_pairs,
    maximum_connections,
    workers,
    handoff_capacity,
    event_batch_size,
    maximum_input_bytes,
    maximum_output_bytes,
    durable_mutation_queue_capacity,
    durable_mutation_queue_bytes,
    durable_mutation_queue_wait,
    shutdown_drain,
    close_drain_deadline,
    reuse_port,
    no_reuse_port,
    executor_affinity,
    storage_mode,
    data_directory,
    durable_open_mode,
    maintenance_mode,
    maintenance_max_copy_bytes_per_cycle,
    maintenance_max_copy_bytes_per_sec,
    maintenance_max_cpu_ms_per_window,
    maintenance_suspend_on_p99_latency_ms,
    maintenance_suspend_on_p99_min_samples,
    maintenance_max_latency_deferral_ms,
    maintenance_unread_ttl_pressure_probe,
    maintenance_unread_ttl_normal_scheduling,
    sync_interval_ms,
    group_max_records,
    group_max_bytes,
    group_max_wait_ms,
    max_store_bytes,
    reserved_free_bytes,
    max_segments,
    max_hot_cache_bytes,
    max_hot_cache_value_bytes,
    disable_hot_cache,
    max_temporary_compaction_bytes,
    quiet,
    log_format,
    tls_cert,
    tls_key,
    tls_client_ca,
    tls_crl,
    tls_ocsp_fail_closed,
    tls_port,
    authz_map,
    secure_profile,
    unix_socket,
    unix_peercred,
    max_accepts_per_sec,
    idle_timeout_ms,
    request_timeout_ms,
    connection_max_requests_per_sec,
    principal_max_requests_per_sec,
    principal_max_bytes_per_sec,
    index_hash_seed,
    worker_hash_seed,
};

constexpr std::array kOptionSpecs{
    cli::OptionSpec{help, "help", 'h', cli::OptionArity::none, {}, "Show this help message and exit"},
    cli::OptionSpec{version, "version", 'V', cli::OptionArity::none, {}, "Show version information and exit"},
    cli::OptionSpec{config, "config", '\0', cli::OptionArity::required, "PATH",
                    "Load settings from PATH (defaults < profile < file < env < CLI)"},
    cli::OptionSpec{profile, "profile", '\0', cli::OptionArity::required, "NAME",
                    "Apply a deployment profile preset: dev, embedded, or production"},
    cli::OptionSpec{dump_config,
                    "dump-config",
                    '\0',
                    cli::OptionArity::none,
                    {},
                    "Print the resolved effective configuration and exit without listening"},
    cli::OptionSpec{bind, "bind", 'b', cli::OptionArity::required, "IPv4",
                    "Bind to an IPv4 address (default: 127.0.0.1; non-loopback is "
                    "cleartext with no authentication — trusted networks only)"},
    cli::OptionSpec{port, "port", 'p', cli::OptionArity::required, "PORT",
                    "Listen on PORT; 0 selects an ephemeral port (default: 7379)"},
    cli::OptionSpec{shard_pairs, "shard-pairs", '\0', cli::OptionArity::required, "COUNT",
                    "Run COUNT paired Reader/Writer shards (default: 1)"},
    cli::OptionSpec{workers, "workers", 'w', cli::OptionArity::required, "COUNT",
                    "Deprecated alias for --shard-pairs"},
    cli::OptionSpec{maximum_connections, "max-connections", '\0', cli::OptionArity::required, "COUNT",
                    "Limit concurrent connections (default: 4096)"},
    cli::OptionSpec{handoff_capacity, "handoff-capacity", '\0', cli::OptionArity::required, "COUNT",
                    "Bound each connection handoff queue (default: 4096)"},
    cli::OptionSpec{event_batch_size, "event-batch-size", '\0', cli::OptionArity::required, "COUNT",
                    "Process at most COUNT readiness events per wait (default: 256)"},
    cli::OptionSpec{maximum_input_bytes, "max-input-bytes", '\0', cli::OptionArity::required, "BYTES",
                    "Limit buffered input per connection (default: 4MiB)"},
    cli::OptionSpec{maximum_output_bytes, "max-output-bytes", '\0', cli::OptionArity::required, "BYTES",
                    "Limit buffered output per connection (default: 4MiB)"},
    cli::OptionSpec{durable_mutation_queue_capacity, "durable-mutation-queue-capacity", '\0',
                    cli::OptionArity::required, "COUNT",
                    "Bound admitted mutations per ShardPair (default: 256)"},
    cli::OptionSpec{durable_mutation_queue_bytes, "durable-mutation-queue-bytes", '\0',
                    cli::OptionArity::required, "BYTES",
                    "Bound owned mutation bytes per ShardPair (default: 16MiB)"},
    cli::OptionSpec{durable_mutation_queue_wait, "durable-mutation-queue-wait-ms", '\0',
                    cli::OptionArity::required, "MILLISECONDS",
                    "Expire queued mutations after this wait; 0 disables (default: 0)"},
    cli::OptionSpec{shutdown_drain, "shutdown-drain-ms", '\0', cli::OptionArity::required, "MILLISECONDS",
                    "Bound connection and durable mutation drain after stop (default: 30000; 0 "
                    "unbounded). Stop accepting, close idle connections, expire queued pre-Store "
                    "work as unavailable on timeout"},
    cli::OptionSpec{close_drain_deadline, "close-drain-deadline-ms", '\0', cli::OptionArity::required,
                    "MILLISECONDS",
                    "Bound Store Writer and admission drain during close (default: unbounded; set to "
                    "make close return unavailable if drain has not finished)"},
    cli::OptionSpec{reuse_port,
                    "reuse-port",
                    '\0',
                    cli::OptionArity::none,
                    {},
                    "Enable per-executor SO_REUSEPORT listeners where supported"},
    cli::OptionSpec{
        no_reuse_port, "no-reuse-port", '\0', cli::OptionArity::none, {}, "Disable SO_REUSEPORT listeners"},
    cli::OptionSpec{executor_affinity,
                    "executor-affinity",
                    '\0',
                    cli::OptionArity::none,
                    {},
                    "Request executor CPU affinity where supported"},
    cli::OptionSpec{storage_mode, "storage-mode", '\0', cli::OptionArity::required, "MODE",
                    "Use volatile, durable-sync, durable-periodic, or durable-group storage"},
    cli::OptionSpec{data_directory, "data-dir", '\0', cli::OptionArity::required, "PATH",
                    "Store durable v1 files below PATH (required for durable storage)"},
    cli::OptionSpec{durable_open_mode, "open-mode", '\0', cli::OptionArity::required, "MODE",
                    "Use open-or-create, create-new, or open-existing (default: open-or-create)"},
    cli::OptionSpec{maintenance_mode, "maintenance-mode", '\0', cli::OptionArity::required, "MODE",
                    "Use cooperative, background (default), or disabled Store maintenance scheduling"},
    cli::OptionSpec{maintenance_max_copy_bytes_per_cycle, "maintenance-max-copy-bytes-per-cycle", '\0',
                    cli::OptionArity::required, "BYTES",
                    "Limit one normal maintenance compaction (default: 128MiB; 0 disables)"},
    cli::OptionSpec{maintenance_max_copy_bytes_per_sec, "maintenance-max-copy-bytes-per-sec", '\0',
                    cli::OptionArity::required, "BYTES",
                    "Pace private normal-mode compaction writes (default: 0=unlimited; embedded profile "
                    "64MiB/s; production profile 128MiB/s; pressure/emergency bypass)"},
    cli::OptionSpec{maintenance_max_cpu_ms_per_window, "maintenance-max-cpu-ms-per-window", '\0',
                    cli::OptionArity::required, "MILLISECONDS",
                    "Limit normal-mode compaction wall time per one-second window (default: 0=unlimited; "
                    "pressure/emergency bypass)"},
    cli::OptionSpec{maintenance_suspend_on_p99_latency_ms, "maintenance-suspend-on-p99-latency-ms", '\0',
                    cli::OptionArity::required, "MILLISECONDS",
                    "Suspend normal compaction when the latest foreground mutation p99 reaches this "
                    "threshold (default: 0=disabled; pressure/emergency bypass)"},
    cli::OptionSpec{maintenance_suspend_on_p99_min_samples, "maintenance-suspend-on-p99-min-samples", '\0',
                    cli::OptionArity::required, "COUNT",
                    "Require COUNT foreground samples before p99 suspension (default: 32)"},
    cli::OptionSpec{maintenance_max_latency_deferral_ms, "maintenance-max-latency-deferral-ms", '\0',
                    cli::OptionArity::required, "MILLISECONDS",
                    "Admit one normal reclaim after this continuous latency deferral (default: 30000; "
                    "0 defers until pressure)"},
    cli::OptionSpec{maintenance_unread_ttl_pressure_probe, "maintenance-unread-ttl-pressure-probe", '\0',
                    cli::OptionArity::required, "BOOL",
                    "Probe unread expired sealed puts under pressure/emergency (default: true)"},
    cli::OptionSpec{maintenance_unread_ttl_normal_scheduling, "maintenance-unread-ttl-normal-scheduling",
                    '\0', cli::OptionArity::required, "BOOL",
                    "Include unread expired sealed puts in normal dead-byte threshold (default: false)"},
    cli::OptionSpec{sync_interval_ms, "sync-interval-ms", '\0', cli::OptionArity::required, "MILLISECONDS",
                    "Durable-periodic flush interval (default: 1000)"},
    cli::OptionSpec{group_max_records, "group-max-records", '\0', cli::OptionArity::required, "COUNT",
                    "Strict-group / periodic batch record limit (default: 32 / 4096)"},
    cli::OptionSpec{group_max_bytes, "group-max-bytes", '\0', cli::OptionArity::required, "BYTES",
                    "Strict-group / periodic batch byte limit (default: 64KiB / 4MiB)"},
    cli::OptionSpec{group_max_wait_ms, "group-max-wait-ms", '\0', cli::OptionArity::required, "MILLISECONDS",
                    "Strict-group / periodic batch close wait (default: 10 / 1000)"},
    cli::OptionSpec{max_store_bytes, "max-store-bytes", '\0', cli::OptionArity::required, "BYTES",
                    "Cap durable Store Segment bytes (default: 8GiB)"},
    cli::OptionSpec{reserved_free_bytes, "reserved-free-bytes", '\0', cli::OptionArity::required, "BYTES",
                    "Keep this much free space unused (default: 256MiB)"},
    cli::OptionSpec{max_segments, "max-segments", '\0', cli::OptionArity::required, "COUNT",
                    "Cap durable Segment count (default: 127)"},
    cli::OptionSpec{max_hot_cache_bytes, "max-hot-cache-bytes", '\0', cli::OptionArity::required, "BYTES",
                    "Retained 0.1.x durable-limit compatibility setting; paired daemon reads use "
                    "ReadGeneration and disable the legacy hot cache (default: 256MiB)"},
    cli::OptionSpec{max_hot_cache_value_bytes, "max-hot-cache-value-bytes", '\0', cli::OptionArity::required,
                    "BYTES",
                    "Retained 0.1.x durable-limit compatibility setting; paired daemon reads disable "
                    "the legacy hot cache (default: 64KiB)"},
    cli::OptionSpec{disable_hot_cache,
                    "disable-hot-cache",
                    '\0',
                    cli::OptionArity::none,
                    {},
                    "Explicitly disable the legacy durable hot cache (already disabled by paired daemon)"},
    cli::OptionSpec{max_temporary_compaction_bytes, "max-temporary-compaction-bytes", '\0',
                    cli::OptionArity::required, "BYTES",
                    "Cap temporary durable compaction peak bytes (default: 1GiB)"},
    cli::OptionSpec{
        quiet, "quiet", 'q', cli::OptionArity::none, {}, "Suppress normal startup and shutdown messages"},
    cli::OptionSpec{log_format, "log-format", '\0', cli::OptionArity::required, "FORMAT",
                    "Emit lifecycle logs as human stderr/stdout (default) or JSON-lines on stderr "
                    "(human|json)"},
    cli::OptionSpec{tls_cert, "tls-cert", '\0', cli::OptionArity::required, "PATH",
                    "PEM certificate chain for TLS 1.3 (requires --tls-key; TLS-only on --port unless "
                    "--tls-port)"},
    cli::OptionSpec{tls_key, "tls-key", '\0', cli::OptionArity::required, "PATH",
                    "PEM private key for TLS (requires --tls-cert)"},
    cli::OptionSpec{tls_client_ca, "tls-client-ca", '\0', cli::OptionArity::required, "PATH",
                    "PEM CA for client certificates (enables mTLS; requires --tls-cert/--tls-key)"},
    cli::OptionSpec{tls_crl, "tls-crl", '\0', cli::OptionArity::required, "PATH",
                    "PEM CRL for mTLS peer revocation (fail closed; requires --tls-client-ca)"},
    cli::OptionSpec{tls_ocsp_fail_closed,
                    "tls-ocsp-fail-closed",
                    '\0',
                    cli::OptionArity::none,
                    {},
                    "Require fail-closed revocation via --tls-crl (live AIA OCSP HTTP unsupported)"},
    cli::OptionSpec{tls_port, "tls-port", '\0', cli::OptionArity::required, "PORT",
                    "Listen for TLS on PORT while --port stays cleartext (requires --tls-cert/--tls-key; "
                    "0=ephemeral; incompatible with --secure-profile)"},
    cli::OptionSpec{
        authz_map, "authz-map", '\0', cli::OptionArity::required, "PATH",
        "Static principal capability map (read/write/admin; optional prefix=; default-deny authz)"},
    cli::OptionSpec{secure_profile,
                    "secure-profile",
                    '\0',
                    cli::OptionArity::none,
                    {},
                    "Fail-closed secure profile: require TLS+mTLS+--authz-map; refuse --tls-port dual "
                    "cleartext; apply Phase 5 abuse-limit defaults (explicit 0 refused); UDS requires "
                    "--unix-peercred"},
    cli::OptionSpec{unix_socket, "unix-socket", '\0', cli::OptionArity::required, "PATH",
                    "Also listen on AF_UNIX PATH (same wire protocol; complementary to TCP/TLS; "
                    "owner-only socket mode)"},
    cli::OptionSpec{unix_peercred,
                    "unix-peercred",
                    '\0',
                    cli::OptionArity::none,
                    {},
                    "Map UDS peers to authz principal unix:uid=<uid> via SO_PEERCRED/getpeereid "
                    "(requires --unix-socket; not a TLS replacement)"},
    cli::OptionSpec{max_accepts_per_sec, "max-accepts-per-sec", '\0', cli::OptionArity::required, "COUNT",
                    "Limit accepted connections/handshakes per second (default: 0=unlimited; "
                    "secure-profile default 128)"},
    cli::OptionSpec{idle_timeout_ms, "idle-timeout-ms", '\0', cli::OptionArity::required, "MILLISECONDS",
                    "Close idle connections after this wait (default: 0=disabled; secure-profile default "
                    "60000)"},
    cli::OptionSpec{request_timeout_ms, "request-timeout-ms", '\0', cli::OptionArity::required,
                    "MILLISECONDS",
                    "Bound partial-frame assembly and in-flight response wait (default: 0=disabled; "
                    "secure-profile default 30000). Store mutations already executing are not cancelled"},
    cli::OptionSpec{connection_max_requests_per_sec, "connection-max-requests-per-sec", '\0',
                    cli::OptionArity::required, "COUNT",
                    "Per-connection request admission per second (default: 0=unlimited; secure-profile "
                    "default 256)"},
    cli::OptionSpec{principal_max_requests_per_sec, "principal-max-requests-per-sec", '\0',
                    cli::OptionArity::required, "COUNT",
                    "Per-principal request admission per second (default: 0=unlimited; secure-profile "
                    "default 1024)"},
    cli::OptionSpec{principal_max_bytes_per_sec, "principal-max-bytes-per-sec", '\0',
                    cli::OptionArity::required, "BYTES",
                    "Per-principal request+response byte budget per second (default: 0=unlimited; "
                    "secure-profile default 32MiB)"},
    cli::OptionSpec{index_hash_seed, "index-hash-seed", '\0', cli::OptionArity::required, "U64",
                    "Process-lifetime SwissTable Index mix seed (ADR 0026). Default: published "
                    "Index v1 constant. --secure-profile randomizes unless this flag is set. "
                    "Does not change Worker routing (ADR 0030)"},
    cli::OptionSpec{worker_hash_seed, "worker-hash-seed", '\0', cli::OptionArity::required, "U64",
                    "Worker ownership SipHash-2-4 seed (ADR 0030). Default: FNV-1a-v1 (no seed). "
                    "Setting this flag selects siphash24-v1 and persists the seed in the Manifest "
                    "for durable Stores. --secure-profile randomizes unless this flag is set. "
                    "Reopen refuses a mismatched explicit seed"},
};

using SettingMap = std::map<std::string, std::string, std::less<>>;

[[nodiscard]] inline auto take_profile_name(SettingMap& settings) -> std::optional<std::string> {
    const auto found = settings.find("profile");
    if (found == settings.end()) {
        return std::nullopt;
    }
    auto name = found->second;
    settings.erase(found);
    return name;
}

[[nodiscard]] inline auto find_spec(const std::string_view long_name) -> const cli::OptionSpec* {
    for (const auto& spec : kOptionSpecs) {
        if (spec.long_name == long_name) {
            return &spec;
        }
    }
    return nullptr;
}

[[nodiscard]] inline auto find_spec(const std::size_t id) -> const cli::OptionSpec* {
    for (const auto& spec : kOptionSpecs) {
        if (spec.id == id) {
            return &spec;
        }
    }
    return nullptr;
}

[[nodiscard]] inline auto trim(std::string_view text) -> std::string_view {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] inline auto ascii_lower(std::string text) -> std::string {
    for (char& character : text) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return text;
}

[[nodiscard]] inline auto deployment_profile_settings(const std::string_view name) -> Result<SettingMap> {
    if (name.empty()) {
        return fail(ErrorCode::invalid_argument, "deployment profile must not be empty");
    }
    const auto lowered = ascii_lower(std::string{name});
    if (lowered == "dev") {
        return SettingMap{
            {"storage-mode", "volatile"},
            {"maintenance-mode", "disabled"},
        };
    }
    if (lowered == "embedded") {
        return SettingMap{
            {"storage-mode", "durable-periodic"},
            {"maintenance-mode", "background"},
            {"shard-pairs", "1"},
            {"max-store-bytes", "1073741824"},
            {"reserved-free-bytes", "67108864"},
            {"max-segments", "32"},
            {"max-hot-cache-bytes", "67108864"},
            {"max-temporary-compaction-bytes", "268435456"},
            // Conservative latency-first starting point. Operators should
            // replace it with a device-specific value from the maintenance A/B.
            {"maintenance-max-copy-bytes-per-sec", "67108864"},
        };
    }
    if (lowered == "production") {
        return SettingMap{
            {"storage-mode", "durable-periodic"},
            {"maintenance-mode", "background"},
            // Bounded starting point, deliberately overrideable by config/env/CLI.
            {"maintenance-max-copy-bytes-per-sec", "134217728"},
        };
    }
    return fail(ErrorCode::invalid_argument, "unknown deployment profile: " + std::string{name} +
                                                 " (expected dev, embedded, or production)");
}

[[nodiscard]] inline auto parse_bool_token(const std::string_view text, const std::string_view where)
    -> Result<bool> {
    const auto lowered = ascii_lower(std::string{text});
    if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") {
        return true;
    }
    if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") {
        return false;
    }
    return fail(ErrorCode::invalid_argument,
                std::string{where} + " expects a boolean (true/false), got '" + std::string{text} + "'");
}

[[nodiscard]] inline auto default_getenv(const std::string_view name) -> std::optional<std::string> {
    const auto* value = std::getenv(std::string{name}.c_str());
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string{value};
}

[[nodiscard]] inline auto settings_from_parsed(const cli::ParsedArguments& parsed) -> Result<SettingMap> {
    SettingMap settings;
    for (const auto& option : parsed.options) {
        const auto* spec = find_spec(option.id);
        if (spec == nullptr) {
            return fail(ErrorCode::internal_error, "parsed daemon option has no matching specification");
        }
        if (option.id == help || option.id == version || option.id == config || option.id == dump_config) {
            continue;
        }
        const auto key = option.id == workers ? std::string{"shard-pairs"} : std::string{spec->long_name};
        const auto value =
            spec->arity == cli::OptionArity::none ? std::string{"true"} : std::string{option.value};
        if (!settings.emplace(key, value).second) {
            return fail(ErrorCode::invalid_argument, "--workers and --shard-pairs name the same setting");
        }
    }
    if (settings.contains("reuse-port") && settings.contains("no-reuse-port")) {
        return fail(ErrorCode::invalid_argument, "--reuse-port and --no-reuse-port are mutually exclusive");
    }
    return settings;
}

inline void apply_layer(SettingMap& destination, const SettingMap& layer) {
    for (const auto& [key, value] : layer) {
        const auto* spec = find_spec(key);
        if (spec != nullptr && spec->arity == cli::OptionArity::none && value == "false") {
            destination.erase(key);
            continue;
        }
        if (key == "reuse-port") {
            destination.erase("no-reuse-port");
        } else if (key == "no-reuse-port") {
            destination.erase("reuse-port");
        }
        destination.insert_or_assign(key, value);
    }
}

[[nodiscard]] auto materialize_from_settings(SettingMap settings, bool show_help, bool show_version,
                                             std::string deployment_profile) -> Result<DaemonOptions>;

[[nodiscard]] inline auto normalize_setting_value(const cli::OptionSpec& spec, std::string value,
                                                  const std::string_view where) -> Result<std::string> {
    if (spec.arity == cli::OptionArity::none) {
        auto enabled = parse_bool_token(value, where);
        if (!enabled) {
            return unexpected(enabled.error());
        }
        return *enabled ? std::string{"true"} : std::string{"false"};
    }
    if (value.empty()) {
        return fail(ErrorCode::invalid_argument, std::string{where} + " must not be empty");
    }
    return value;
}

[[nodiscard]] inline auto ingest_setting(SettingMap& settings, const std::string_view key, std::string value,
                                         const std::string_view where) -> Status {
    if (key == "help" || key == "version" || key == "config" || key == "dump-config") {
        return fail(ErrorCode::invalid_argument,
                    std::string{where} + " cannot set '" + std::string{key} + "'");
    }
    const auto* spec = find_spec(key);
    if (spec == nullptr) {
        return fail(ErrorCode::invalid_argument,
                    std::string{where} + " has unknown setting '" + std::string{key} + "'");
    }
    auto normalized = normalize_setting_value(*spec, std::move(value), where);
    if (!normalized) {
        return unexpected(normalized.error());
    }
    const auto canonical_key = key == "workers" ? std::string{"shard-pairs"} : std::string{key};
    if (!settings.emplace(canonical_key, std::move(*normalized)).second) {
        return fail(ErrorCode::invalid_argument,
                    std::string{where} + " duplicates setting '" + canonical_key + "'");
    }
    return {};
}

} // namespace glifistore::server::daemon_config_detail
