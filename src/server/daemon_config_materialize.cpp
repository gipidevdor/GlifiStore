#include "daemon_config_detail.hpp"
#include "glifistore/server/authz.hpp"
#include "glifistore/server/tls.hpp"

#include <string>
#include <utility>
#include <vector>

namespace glifistore::server::daemon_config_detail {

auto materialize_from_settings(SettingMap settings, const bool show_help, const bool show_version,
                               std::string deployment_profile) -> Result<DaemonOptions> {
    std::vector<std::string> storage;
    storage.reserve(settings.size() * 2U + 1U);
    storage.emplace_back("glifistored");
    for (const auto& [key, value] : settings) {
        const auto* spec = find_spec(key);
        if (spec == nullptr) {
            return fail(ErrorCode::invalid_argument, "unknown daemon setting: " + key);
        }
        if (spec->arity == cli::OptionArity::none) {
            auto enabled = parse_bool_token(value, "--" + key);
            if (!enabled) {
                return unexpected(enabled.error());
            }
            if (*enabled) {
                storage.push_back("--" + key);
            }
        } else {
            storage.push_back("--" + key);
            storage.push_back(value);
        }
    }

    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (auto& entry : storage) {
        argv.push_back(entry.data());
    }
    auto parsed = cli::parse_arguments(static_cast<int>(argv.size()), argv.data(), kOptionSpecs);
    if (!parsed) {
        return unexpected(parsed.error());
    }

    DaemonOptions options;
    options.deployment_profile = std::move(deployment_profile);
    options.show_help = show_help;
    options.show_version = show_version;
    options.quiet = parsed->has(quiet);
    if (const auto format = parsed->value(log_format)) {
        auto resolved = parse_daemon_log_format(*format);
        if (!resolved) {
            return unexpected(resolved.error());
        }
        options.log_format = *resolved;
    }
    if (const auto address = parsed->value(bind)) {
        options.server.bind_address = *address;
    }

    const auto set_size_option = [&](const OptionId id, const std::string_view name,
                                     const std::size_t minimum, const std::size_t maximum,
                                     std::size_t& destination) -> Status {
        const auto text = parsed->value(id);
        if (!text) {
            return {};
        }
        auto value = cli::parse_size(*text, name, minimum, maximum);
        if (!value) {
            return unexpected(value.error());
        }
        destination = *value;
        return {};
    };
    const auto set_byte_size_option = [&](const OptionId id, const std::string_view name,
                                          const std::size_t minimum, const std::size_t maximum,
                                          std::size_t& destination) -> Status {
        const auto text = parsed->value(id);
        if (!text) {
            return {};
        }
        auto value = cli::parse_byte_size(*text, name, minimum, maximum);
        if (!value) {
            return unexpected(value.error());
        }
        destination = *value;
        return {};
    };

    std::size_t parsed_port = options.server.port;
    if (auto status =
            set_size_option(port, "--port", 0, std::numeric_limits<std::uint16_t>::max(), parsed_port);
        !status) {
        return unexpected(status.error());
    }
    options.server.port = static_cast<std::uint16_t>(parsed_port);

    constexpr auto maximum_size = std::numeric_limits<std::size_t>::max();
    if (auto status = set_size_option(shard_pairs, "--shard-pairs", 1, kMaximumWorkerCount,
                                      options.server.worker_count);
        !status) {
        return unexpected(status.error());
    }
    if (auto status =
            set_size_option(maximum_connections, "--max-connections", 1,
                            std::numeric_limits<std::uint32_t>::max(), options.server.maximum_connections);
        !status) {
        return unexpected(status.error());
    }
    if (auto status = set_size_option(handoff_capacity, "--handoff-capacity", 1, std::size_t{1} << 30U,
                                      options.server.connection_handoff_capacity);
        !status) {
        return unexpected(status.error());
    }
    if (auto status = set_size_option(event_batch_size, "--event-batch-size", 1, maximum_size,
                                      options.server.event_batch_size);
        !status) {
        return unexpected(status.error());
    }
    if (auto status = set_byte_size_option(maximum_input_bytes, "--max-input-bytes", kRequestHeaderBytes,
                                           maximum_size, options.server.maximum_input_bytes);
        !status) {
        return unexpected(status.error());
    }
    if (auto status = set_byte_size_option(maximum_output_bytes, "--max-output-bytes", kResponseHeaderBytes,
                                           maximum_size, options.server.maximum_output_bytes);
        !status) {
        return unexpected(status.error());
    }
    if (auto status = set_size_option(durable_mutation_queue_capacity, "--durable-mutation-queue-capacity", 1,
                                      std::size_t{1} << 30U, options.server.durable_mutation_queue_capacity);
        !status) {
        return unexpected(status.error());
    }
    if (auto status = set_byte_size_option(durable_mutation_queue_bytes, "--durable-mutation-queue-bytes", 1,
                                           maximum_size, options.server.durable_mutation_queue_bytes);
        !status) {
        return unexpected(status.error());
    }
    std::size_t mutation_queue_wait_ms = options.server.durable_mutation_queue_wait_ms;
    if (auto status = set_size_option(durable_mutation_queue_wait, "--durable-mutation-queue-wait-ms", 0,
                                      std::numeric_limits<std::uint32_t>::max(), mutation_queue_wait_ms);
        !status) {
        return unexpected(status.error());
    }
    options.server.durable_mutation_queue_wait_ms = static_cast<std::uint32_t>(mutation_queue_wait_ms);
    std::size_t shutdown_drain_ms = options.server.shutdown_drain_ms;
    if (auto status = set_size_option(shutdown_drain, "--shutdown-drain-ms", 0,
                                      std::numeric_limits<std::uint32_t>::max(), shutdown_drain_ms);
        !status) {
        return unexpected(status.error());
    }
    options.server.shutdown_drain_ms = static_cast<std::uint32_t>(shutdown_drain_ms);
    if (const auto text = parsed->value(close_drain_deadline)) {
        auto value =
            cli::parse_size(*text, "--close-drain-deadline-ms", 0, std::numeric_limits<std::uint32_t>::max());
        if (!value) {
            return unexpected(value.error());
        }
        options.store.close_drain_deadline_ms = static_cast<std::uint32_t>(*value);
    }
    std::size_t max_accepts = options.server.abuse.max_accepts_per_sec;
    if (auto status = set_size_option(max_accepts_per_sec, "--max-accepts-per-sec", 0,
                                      std::numeric_limits<std::uint32_t>::max(), max_accepts);
        !status) {
        return unexpected(status.error());
    }
    options.server.abuse.max_accepts_per_sec = static_cast<std::uint32_t>(max_accepts);
    std::size_t idle_timeout = options.server.abuse.idle_timeout_ms;
    if (auto status = set_size_option(idle_timeout_ms, "--idle-timeout-ms", 0,
                                      std::numeric_limits<std::uint32_t>::max(), idle_timeout);
        !status) {
        return unexpected(status.error());
    }
    options.server.abuse.idle_timeout_ms = static_cast<std::uint32_t>(idle_timeout);
    std::size_t request_timeout = options.server.abuse.request_timeout_ms;
    if (auto status = set_size_option(request_timeout_ms, "--request-timeout-ms", 0,
                                      std::numeric_limits<std::uint32_t>::max(), request_timeout);
        !status) {
        return unexpected(status.error());
    }
    options.server.abuse.request_timeout_ms = static_cast<std::uint32_t>(request_timeout);
    std::size_t connection_requests = options.server.abuse.connection_max_requests_per_sec;
    if (auto status = set_size_option(connection_max_requests_per_sec, "--connection-max-requests-per-sec", 0,
                                      std::numeric_limits<std::uint32_t>::max(), connection_requests);
        !status) {
        return unexpected(status.error());
    }
    options.server.abuse.connection_max_requests_per_sec = static_cast<std::uint32_t>(connection_requests);
    std::size_t principal_requests = options.server.abuse.principal_max_requests_per_sec;
    if (auto status = set_size_option(principal_max_requests_per_sec, "--principal-max-requests-per-sec", 0,
                                      std::numeric_limits<std::uint32_t>::max(), principal_requests);
        !status) {
        return unexpected(status.error());
    }
    options.server.abuse.principal_max_requests_per_sec = static_cast<std::uint32_t>(principal_requests);
    std::size_t principal_bytes = options.server.abuse.principal_max_bytes_per_sec;
    if (auto status = set_byte_size_option(principal_max_bytes_per_sec, "--principal-max-bytes-per-sec", 0,
                                           maximum_size, principal_bytes);
        !status) {
        return unexpected(status.error());
    }
    options.server.abuse.principal_max_bytes_per_sec = principal_bytes;
    if (parsed->has(reuse_port) && parsed->has(no_reuse_port)) {
        return fail(ErrorCode::invalid_argument, "--reuse-port and --no-reuse-port are mutually exclusive");
    }
    if (parsed->has(reuse_port)) {
        options.server.reuse_port = true;
    }
    if (parsed->has(no_reuse_port)) {
        options.server.reuse_port = false;
    }
    options.server.executor_affinity = parsed->has(executor_affinity);

    if (const auto mode = parsed->value(storage_mode)) {
        if (*mode == "volatile") {
            options.store.storage_mode = StorageMode::volatile_memory;
        } else if (*mode == "durable-sync") {
            options.store.storage_mode = StorageMode::durable_sync;
        } else if (*mode == "durable-periodic") {
            options.store.storage_mode = StorageMode::durable_periodic;
        } else if (*mode == "durable-group") {
            options.store.storage_mode = StorageMode::durable_group;
        } else {
            return fail(ErrorCode::invalid_argument, "unknown --storage-mode: " + std::string{*mode});
        }
    }
    if (const auto path = parsed->value(data_directory)) {
        if (path->empty()) {
            return fail(ErrorCode::invalid_argument, "--data-dir must not be empty");
        }
        options.store.data_directory = std::filesystem::path{*path};
    }
    if (const auto mode = parsed->value(durable_open_mode)) {
        if (*mode == "open-or-create") {
            options.store.durable_open_mode = DurableOpenMode::open_or_create;
        } else if (*mode == "create-new") {
            options.store.durable_open_mode = DurableOpenMode::create_new;
        } else if (*mode == "open-existing") {
            options.store.durable_open_mode = DurableOpenMode::open_existing;
        } else {
            return fail(ErrorCode::invalid_argument, "unknown --open-mode: " + std::string{*mode});
        }
    }
    if (const auto mode = parsed->value(maintenance_mode)) {
        if (*mode == "cooperative") {
            options.store.maintenance.mode = MaintenanceMode::cooperative;
        } else if (*mode == "background") {
            options.store.maintenance.mode = MaintenanceMode::background;
        } else if (*mode == "disabled") {
            options.store.maintenance.mode = MaintenanceMode::disabled;
        } else {
            return fail(ErrorCode::invalid_argument, "unknown --maintenance-mode: " + std::string{*mode});
        }
    }
    std::size_t maintenance_copy_bytes =
        static_cast<std::size_t>(options.store.maintenance.max_copy_bytes_per_cycle);
    if (auto status = set_byte_size_option(maintenance_max_copy_bytes_per_cycle,
                                           "--maintenance-max-copy-bytes-per-cycle", 0, maximum_size,
                                           maintenance_copy_bytes);
        !status) {
        return unexpected(status.error());
    }
    options.store.maintenance.max_copy_bytes_per_cycle = maintenance_copy_bytes;

    std::size_t maintenance_copy_bytes_per_sec =
        static_cast<std::size_t>(options.store.maintenance.max_copy_bytes_per_sec);
    if (auto status =
            set_byte_size_option(maintenance_max_copy_bytes_per_sec, "--maintenance-max-copy-bytes-per-sec",
                                 0, maximum_size, maintenance_copy_bytes_per_sec);
        !status) {
        return unexpected(status.error());
    }
    options.store.maintenance.max_copy_bytes_per_sec = maintenance_copy_bytes_per_sec;

    std::size_t maintenance_cpu_ms = options.store.maintenance.max_cpu_ms_per_window;
    if (auto status =
            set_size_option(maintenance_max_cpu_ms_per_window, "--maintenance-max-cpu-ms-per-window", 0,
                            std::numeric_limits<std::uint32_t>::max(), maintenance_cpu_ms);
        !status) {
        return unexpected(status.error());
    }
    options.store.maintenance.max_cpu_ms_per_window = static_cast<std::uint32_t>(maintenance_cpu_ms);

    std::size_t maintenance_p99_ms = options.store.maintenance.suspend_on_p99_latency_ms;
    if (auto status =
            set_size_option(maintenance_suspend_on_p99_latency_ms, "--maintenance-suspend-on-p99-latency-ms",
                            0, std::numeric_limits<std::uint32_t>::max(), maintenance_p99_ms);
        !status) {
        return unexpected(status.error());
    }
    options.store.maintenance.suspend_on_p99_latency_ms = static_cast<std::uint32_t>(maintenance_p99_ms);

    std::size_t maintenance_p99_min_samples = options.store.maintenance.suspend_on_p99_min_samples;
    if (auto status = set_size_option(maintenance_suspend_on_p99_min_samples,
                                      "--maintenance-suspend-on-p99-min-samples", 1,
                                      std::numeric_limits<std::uint32_t>::max(), maintenance_p99_min_samples);
        !status) {
        return unexpected(status.error());
    }
    options.store.maintenance.suspend_on_p99_min_samples =
        static_cast<std::uint32_t>(maintenance_p99_min_samples);

    std::size_t maintenance_max_deferral_ms = options.store.maintenance.max_latency_deferral_ms;
    if (auto status =
            set_size_option(maintenance_max_latency_deferral_ms, "--maintenance-max-latency-deferral-ms", 0,
                            std::numeric_limits<std::uint32_t>::max(), maintenance_max_deferral_ms);
        !status) {
        return unexpected(status.error());
    }
    options.store.maintenance.max_latency_deferral_ms =
        static_cast<std::uint32_t>(maintenance_max_deferral_ms);

    if (parsed->has(maintenance_unread_ttl_pressure_probe)) {
        if (const auto value = parsed->value(maintenance_unread_ttl_pressure_probe)) {
            if (auto enabled = parse_bool_token(*value, "--maintenance-unread-ttl-pressure-probe"); enabled) {
                options.store.maintenance.unread_ttl_pressure_probe = *enabled;
            } else {
                return unexpected(enabled.error());
            }
        }
    }
    if (parsed->has(maintenance_unread_ttl_normal_scheduling)) {
        if (const auto value = parsed->value(maintenance_unread_ttl_normal_scheduling)) {
            if (auto enabled = parse_bool_token(*value, "--maintenance-unread-ttl-normal-scheduling");
                enabled) {
                options.store.maintenance.unread_ttl_normal_scheduling = *enabled;
            } else {
                return unexpected(enabled.error());
            }
        }
    }

    const bool has_batch_or_resource =
        parsed->has(sync_interval_ms) || parsed->has(group_max_records) || parsed->has(group_max_bytes) ||
        parsed->has(group_max_wait_ms) || parsed->has(max_store_bytes) || parsed->has(reserved_free_bytes) ||
        parsed->has(max_segments) || parsed->has(max_hot_cache_bytes) ||
        parsed->has(max_hot_cache_value_bytes) || parsed->has(disable_hot_cache) ||
        parsed->has(max_temporary_compaction_bytes);
    const bool durable = options.store.storage_mode != StorageMode::volatile_memory;
    if (durable && !options.store.data_directory) {
        return fail(ErrorCode::invalid_argument, "--data-dir is required for durable storage");
    }
    if (!durable &&
        (parsed->has(data_directory) || parsed->has(durable_open_mode) || has_batch_or_resource)) {
        return fail(ErrorCode::invalid_argument,
                    "durable data-dir, open-mode, batch, and resource flags require a durable "
                    "--storage-mode");
    }

    std::size_t sync_interval = options.store.durable_periodic.sync_interval_ms;
    if (auto status = set_size_option(sync_interval_ms, "--sync-interval-ms", 1,
                                      std::numeric_limits<std::uint32_t>::max(), sync_interval);
        !status) {
        return unexpected(status.error());
    }
    options.store.durable_periodic.sync_interval_ms = static_cast<std::uint32_t>(sync_interval);

    const auto apply_group_field = [&](DurableGroupConfig& batch) -> Status {
        std::size_t max_records = batch.max_records;
        if (auto status = set_size_option(group_max_records, "--group-max-records", 1,
                                          std::numeric_limits<std::uint32_t>::max(), max_records);
            !status) {
            return status;
        }
        batch.max_records = static_cast<std::uint32_t>(max_records);
        std::size_t max_bytes = batch.max_bytes;
        if (auto status =
                set_byte_size_option(group_max_bytes, "--group-max-bytes", 1, maximum_size, max_bytes);
            !status) {
            return status;
        }
        if (max_bytes > std::numeric_limits<std::uint32_t>::max()) {
            return fail(ErrorCode::invalid_argument,
                        "--group-max-bytes exceeds the supported u32 batch limit");
        }
        batch.max_bytes = static_cast<std::uint32_t>(max_bytes);
        std::size_t max_wait = batch.max_wait_ms;
        if (auto status = set_size_option(group_max_wait_ms, "--group-max-wait-ms", 1,
                                          std::numeric_limits<std::uint32_t>::max(), max_wait);
            !status) {
            return status;
        }
        batch.max_wait_ms = static_cast<std::uint32_t>(max_wait);
        return {};
    };
    if (auto status = apply_group_field(options.store.durable_group); !status) {
        return unexpected(status.error());
    }
    if (!options.store.durable_periodic.batch.has_value()) {
        options.store.durable_periodic.batch = DurableGroupConfig{};
    }
    if (auto status = apply_group_field(*options.store.durable_periodic.batch); !status) {
        return unexpected(status.error());
    }

    std::size_t store_bytes = static_cast<std::size_t>(options.store.durable_limits.max_store_bytes);
    if (auto status =
            set_byte_size_option(max_store_bytes, "--max-store-bytes", 1, maximum_size, store_bytes);
        !status) {
        return unexpected(status.error());
    }
    options.store.durable_limits.max_store_bytes = store_bytes;
    std::size_t reserved_bytes = static_cast<std::size_t>(options.store.durable_limits.reserved_free_bytes);
    if (auto status = set_byte_size_option(reserved_free_bytes, "--reserved-free-bytes", 0, maximum_size,
                                           reserved_bytes);
        !status) {
        return unexpected(status.error());
    }
    options.store.durable_limits.reserved_free_bytes = reserved_bytes;
    if (auto status = set_size_option(max_segments, "--max-segments", 1, maximum_size,
                                      options.store.durable_limits.max_segment_count);
        !status) {
        return unexpected(status.error());
    }
    std::size_t hot_cache_bytes = static_cast<std::size_t>(options.store.durable_limits.max_hot_cache_bytes);
    if (auto status = set_byte_size_option(max_hot_cache_bytes, "--max-hot-cache-bytes", 0, maximum_size,
                                           hot_cache_bytes);
        !status) {
        return unexpected(status.error());
    }
    options.store.durable_limits.max_hot_cache_bytes = hot_cache_bytes;
    std::size_t hot_cache_value_bytes =
        static_cast<std::size_t>(options.store.durable_limits.max_hot_cache_value_bytes);
    if (auto status = set_byte_size_option(max_hot_cache_value_bytes, "--max-hot-cache-value-bytes", 0,
                                           kMaxNormalRecordSize, hot_cache_value_bytes);
        !status) {
        return unexpected(status.error());
    }
    options.store.durable_limits.max_hot_cache_value_bytes = hot_cache_value_bytes;
    if (parsed->has(disable_hot_cache)) {
        options.store.durable_limits.hot_cache_enabled = false;
    }
    std::size_t temporary_bytes =
        static_cast<std::size_t>(options.store.durable_limits.max_temporary_compaction_bytes);
    if (auto status = set_byte_size_option(max_temporary_compaction_bytes, "--max-temporary-compaction-bytes",
                                           1, maximum_size, temporary_bytes);
        !status) {
        return unexpected(status.error());
    }
    options.store.durable_limits.max_temporary_compaction_bytes = temporary_bytes;

    if (const auto path = parsed->value(tls_cert)) {
        options.server.tls.certificate_file = std::filesystem::path{*path};
    }
    if (const auto path = parsed->value(tls_key)) {
        options.server.tls.private_key_file = std::filesystem::path{*path};
    }
    if (const auto path = parsed->value(tls_client_ca)) {
        options.server.tls.client_ca_file = std::filesystem::path{*path};
    }
    if (const auto path = parsed->value(tls_crl)) {
        options.server.tls.crl_file = std::filesystem::path{*path};
    }
    options.server.tls.ocsp_fail_closed = parsed->has(tls_ocsp_fail_closed);
    if (parsed->has(tls_port)) {
        std::size_t parsed_tls_port = 0;
        if (auto status = set_size_option(tls_port, "--tls-port", 0,
                                          std::numeric_limits<std::uint16_t>::max(), parsed_tls_port);
            !status) {
            return unexpected(status.error());
        }
        options.server.tls_port = static_cast<std::uint16_t>(parsed_tls_port);
    }
    if (const auto path = parsed->value(authz_map)) {
        if (path->empty()) {
            return fail(ErrorCode::invalid_argument, "--authz-map must not be empty");
        }
        options.authz_map_path = std::filesystem::path{*path};
        auto policy = AuthzPolicy::load_file(options.authz_map_path);
        if (!policy) {
            return unexpected(policy.error());
        }
        options.server.authz = std::move(*policy);
    }
    options.secure_profile = parsed->has(secure_profile);
    if (const auto path = parsed->value(unix_socket)) {
        if (path->empty()) {
            return fail(ErrorCode::invalid_argument, "--unix-socket must not be empty");
        }
        options.server.unix_socket_path = std::filesystem::path{*path};
    }
    options.server.unix_peercred = parsed->has(unix_peercred);
    if (options.server.unix_peercred && options.server.unix_socket_path.empty()) {
        return fail(ErrorCode::invalid_argument, "--unix-peercred requires --unix-socket");
    }
    if (!options.server.unix_socket_path.empty() && options.server.unix_peercred && !peercred_supported()) {
        return fail(ErrorCode::unavailable,
                    "--unix-peercred is unsupported on this platform (need SO_PEERCRED or getpeereid)");
    }
    // Explicit --unix-peercred fails closed on accept when credentials cannot be read.
    options.server.unix_peercred_required = options.server.unix_peercred;
    options.index_hash_seed = kDefaultIndexHashSeed;
    options.index_hash_seed_explicit = parsed->has(index_hash_seed);
    if (options.index_hash_seed_explicit) {
        std::size_t seed_value = 0;
        if (auto status = set_size_option(index_hash_seed, "--index-hash-seed", 0,
                                          std::numeric_limits<std::size_t>::max(), seed_value);
            !status) {
            return unexpected(status.error());
        }
        options.index_hash_seed = static_cast<std::uint64_t>(seed_value);
    }
    options.worker_routing = {};
    options.worker_hash_seed_explicit = parsed->has(worker_hash_seed);
    if (options.worker_hash_seed_explicit) {
        std::size_t seed_value = 0;
        if (auto status = set_size_option(worker_hash_seed, "--worker-hash-seed", 0,
                                          std::numeric_limits<std::size_t>::max(), seed_value);
            !status) {
            return unexpected(status.error());
        }
        options.worker_routing = {.algorithm = RoutingAlgorithm::siphash24_v1,
                                  .seed = static_cast<std::uint64_t>(seed_value)};
    }
    if (options.secure_profile) {
        if (!options.server.tls.requested() || !options.server.tls.mtls_enabled()) {
            return fail(ErrorCode::invalid_argument,
                        "--secure-profile requires --tls-cert/--tls-key and --tls-client-ca");
        }
        if (options.authz_map_path.empty() || !options.server.authz.enabled()) {
            return fail(ErrorCode::invalid_argument, "--secure-profile requires --authz-map");
        }
        if (options.server.tls_port.has_value()) {
            return fail(ErrorCode::invalid_argument,
                        "--secure-profile refuses dual cleartext (--tls-port); use TLS-only on --port");
        }
        if (!options.server.unix_socket_path.empty()) {
            // UDS is complementary local transport; fail closed without peercred principals.
            if (!options.server.unix_peercred) {
                return fail(ErrorCode::invalid_argument,
                            "--secure-profile with --unix-socket requires --unix-peercred");
            }
            options.server.unix_peercred_required = true;
        }
        if (!options.index_hash_seed_explicit) {
            // Phase 8: secret Index mix seed so tenants cannot precompute bucket floods.
            options.index_hash_seed = generate_index_hash_seed();
        }
        if (!options.worker_hash_seed_explicit) {
            options.worker_routing = {.algorithm = RoutingAlgorithm::siphash24_v1,
                                      .seed = generate_worker_hash_seed()};
        }
        // Phase 5: apply secure defaults for unset (zero) knobs; refuse explicit disable.
        const auto defaults = secure_profile_abuse_defaults();
        const bool accepts_set = parsed->has(max_accepts_per_sec);
        const bool idle_set = parsed->has(idle_timeout_ms);
        const bool request_set = parsed->has(request_timeout_ms);
        const bool connection_set = parsed->has(connection_max_requests_per_sec);
        const bool principal_req_set = parsed->has(principal_max_requests_per_sec);
        const bool principal_bytes_set = parsed->has(principal_max_bytes_per_sec);
        if ((accepts_set && options.server.abuse.max_accepts_per_sec == 0) ||
            (idle_set && options.server.abuse.idle_timeout_ms == 0) ||
            (request_set && options.server.abuse.request_timeout_ms == 0) ||
            (connection_set && options.server.abuse.connection_max_requests_per_sec == 0) ||
            (principal_req_set && options.server.abuse.principal_max_requests_per_sec == 0) ||
            (principal_bytes_set && options.server.abuse.principal_max_bytes_per_sec == 0)) {
            return fail(ErrorCode::invalid_argument,
                        "--secure-profile refuses disabling Phase 5 abuse limits (explicit 0)");
        }
        if (!accepts_set) {
            options.server.abuse.max_accepts_per_sec = defaults.max_accepts_per_sec;
        }
        if (!idle_set) {
            options.server.abuse.idle_timeout_ms = defaults.idle_timeout_ms;
        }
        if (!request_set) {
            options.server.abuse.request_timeout_ms = defaults.request_timeout_ms;
        }
        if (!connection_set) {
            options.server.abuse.connection_max_requests_per_sec = defaults.connection_max_requests_per_sec;
        }
        if (!principal_req_set) {
            options.server.abuse.principal_max_requests_per_sec = defaults.principal_max_requests_per_sec;
        }
        if (!principal_bytes_set) {
            options.server.abuse.principal_max_bytes_per_sec = defaults.principal_max_bytes_per_sec;
        }
    }
    // Phase 6: emit auth/authz/tls audit JSON-lines for secure profile or structured logs.
    options.server.quiet = options.quiet;
    options.server.security_audit_events =
        options.secure_profile || options.log_format == DaemonLogFormat::json;
    options.store.worker_routing = {.algorithm = options.worker_routing.algorithm,
                                    .seed = options.worker_routing.seed,
                                    .seed_explicit = options.worker_hash_seed_explicit};
    if (auto tls = validate_tls_config(options.server.tls); !tls) {
        return unexpected(tls.error());
    }
    return options;
}

} // namespace glifistore::server::daemon_config_detail
