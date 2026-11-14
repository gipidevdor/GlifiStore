#include "glifistore/server/daemon_config.hpp"

#include <string>
#include <string_view>

namespace glifistore::server {

namespace {

[[nodiscard]] auto open_mode_name(const DurableOpenMode mode) noexcept -> std::string_view {
    switch (mode) {
    case DurableOpenMode::open_or_create:
        return "open-or-create";
    case DurableOpenMode::create_new:
        return "create-new";
    case DurableOpenMode::open_existing:
        return "open-existing";
    }
    return "unknown";
}

[[nodiscard]] auto maintenance_mode_name(const MaintenanceMode mode) noexcept -> std::string_view {
    switch (mode) {
    case MaintenanceMode::cooperative:
        return "cooperative";
    case MaintenanceMode::background:
        return "background";
    case MaintenanceMode::disabled:
        return "disabled";
    }
    return "unknown";
}

} // namespace

auto format_daemon_config_dump(const DaemonOptions& options) -> std::string {
    const auto& group = options.store.storage_mode == StorageMode::durable_periodic &&
                                options.store.durable_periodic.batch.has_value()
                            ? *options.store.durable_periodic.batch
                            : options.store.durable_group;
    std::string out;
    out.reserve(2048);
    out += "GlifiStore/config\n";
    out += "profile=";
    out += options.deployment_profile;
    out += "\nbind=";
    out += options.server.bind_address;
    out += "\nport=";
    out += std::to_string(options.server.port);
    out += "\nshard-pairs=";
    out += std::to_string(options.server.worker_count);
    out += "\nmax-connections=";
    out += std::to_string(options.server.maximum_connections);
    out += "\nhandoff-capacity=";
    out += std::to_string(options.server.connection_handoff_capacity);
    out += "\nevent-batch-size=";
    out += std::to_string(options.server.event_batch_size);
    out += "\nmax-input-bytes=";
    out += std::to_string(options.server.maximum_input_bytes);
    out += "\nmax-output-bytes=";
    out += std::to_string(options.server.maximum_output_bytes);
    out += "\ndurable-mutation-queue-capacity=";
    out += std::to_string(options.server.durable_mutation_queue_capacity);
    out += "\ndurable-mutation-queue-bytes=";
    out += std::to_string(options.server.durable_mutation_queue_bytes);
    out += "\ndurable-mutation-queue-wait-ms=";
    out += std::to_string(options.server.durable_mutation_queue_wait_ms);
    out += "\nshutdown-drain-ms=";
    out += std::to_string(options.server.shutdown_drain_ms);
    out += "\nclose-drain-deadline-ms=";
    if (options.store.close_drain_deadline_ms.has_value()) {
        out += std::to_string(*options.store.close_drain_deadline_ms);
    }
    out += "\nreuse-port=";
    out += options.server.reuse_port ? "true" : "false";
    out += "\nexecutor-affinity=";
    out += options.server.executor_affinity ? "true" : "false";
    out += "\nstorage-mode=";
    out += storage_mode_name(options.store.storage_mode);
    out += "\ndata-dir=";
    if (options.store.data_directory) {
        out += options.store.data_directory->string();
    }
    out += "\nopen-mode=";
    out += open_mode_name(options.store.durable_open_mode);
    out += "\nmaintenance-mode=";
    out += maintenance_mode_name(options.store.maintenance.mode);
    out += "\nmaintenance-max-copy-bytes-per-cycle=";
    out += std::to_string(options.store.maintenance.max_copy_bytes_per_cycle);
    out += "\nmaintenance-max-copy-bytes-per-sec=";
    out += std::to_string(options.store.maintenance.max_copy_bytes_per_sec);
    out += "\nmaintenance-max-cpu-ms-per-window=";
    out += std::to_string(options.store.maintenance.max_cpu_ms_per_window);
    out += "\nmaintenance-suspend-on-p99-latency-ms=";
    out += std::to_string(options.store.maintenance.suspend_on_p99_latency_ms);
    out += "\nmaintenance-suspend-on-p99-min-samples=";
    out += std::to_string(options.store.maintenance.suspend_on_p99_min_samples);
    out += "\nmaintenance-max-latency-deferral-ms=";
    out += std::to_string(options.store.maintenance.max_latency_deferral_ms);
    out += "\nmaintenance-unread-ttl-pressure-probe=";
    out += options.store.maintenance.unread_ttl_pressure_probe ? "true" : "false";
    out += "\nmaintenance-unread-ttl-normal-scheduling=";
    out += options.store.maintenance.unread_ttl_normal_scheduling ? "true" : "false";
    out += "\nmaintenance-min-eval-interval-ms=";
    out += std::to_string(options.store.maintenance.min_eval_interval_ms);
    out += "\nmaintenance-max-eval-interval-ms=";
    out += std::to_string(options.store.maintenance.max_eval_interval_ms);
    out += "\nmaintenance-dead-byte-ratio-bp-normal=";
    out += std::to_string(options.store.maintenance.dead_byte_ratio_bp_normal);
    out += "\nmaintenance-segment-count-pressure-pct=";
    out += std::to_string(options.store.maintenance.segment_count_pressure_pct);
    out += "\nmaintenance-max-no-gain-attempts=";
    out += std::to_string(options.store.maintenance.max_no_gain_attempts);
    out += "\nmaintenance-free-bytes-pressure-margin=";
    out += std::to_string(options.store.maintenance.free_bytes_pressure_margin);
    out += "\ndisk-read-thread-count=";
    out += std::to_string(options.server.disk_read_thread_count);
    out += "\ndisk-read-queue-capacity=";
    out += std::to_string(options.server.disk_read_queue_capacity);
    out += "\nsync-interval-ms=";
    out += std::to_string(options.store.durable_periodic.sync_interval_ms);
    out += "\ngroup-max-records=";
    out += std::to_string(group.max_records);
    out += "\ngroup-max-bytes=";
    out += std::to_string(group.max_bytes);
    out += "\ngroup-max-wait-ms=";
    out += std::to_string(group.max_wait_ms);
    out += "\ngroup-min-records=";
    out += std::to_string(group.min_records);
    out += "\nmax-store-bytes=";
    out += std::to_string(options.store.durable_limits.max_store_bytes);
    out += "\nreserved-free-bytes=";
    out += std::to_string(options.store.durable_limits.reserved_free_bytes);
    out += "\nmax-segments=";
    out += std::to_string(options.store.durable_limits.max_segment_count);
    out += "\nmax-manifest-bytes=";
    out += std::to_string(options.store.durable_limits.max_manifest_bytes);
    out += "\nmax-open-files=";
    out += std::to_string(options.store.durable_limits.max_open_files);
    out += "\nmax-recovery-memory-bytes=";
    out += std::to_string(options.store.durable_limits.max_recovery_memory_bytes);
    out += "\nmax-live-keys=";
    out += std::to_string(options.store.durable_limits.max_live_keys);
    out += "\nmax-write-amplification=";
    out += std::to_string(options.store.durable_limits.max_write_amplification);
    out += "\nmax-hot-cache-bytes=";
    out += std::to_string(options.store.durable_limits.max_hot_cache_bytes);
    out += "\nmax-hot-cache-bytes-per-worker=";
    out += std::to_string(options.store.durable_limits.max_hot_cache_bytes_per_worker);
    out += "\nmax-hot-cache-staging-bytes-per-worker=";
    out += std::to_string(options.store.durable_limits.max_hot_cache_staging_bytes_per_worker);
    out += "\nmax-hot-cache-entries-per-worker=";
    out += std::to_string(options.store.durable_limits.max_hot_cache_entries_per_worker);
    out += "\nmax-hot-cache-value-bytes=";
    out += std::to_string(options.store.durable_limits.max_hot_cache_value_bytes);
    out += "\nhot-cache-enabled=";
    out += options.store.durable_limits.hot_cache_enabled ? "true" : "false";
    out += "\nmax-temporary-compaction-bytes=";
    out += std::to_string(options.store.durable_limits.max_temporary_compaction_bytes);
    out += "\nquiet=";
    out += options.quiet ? "true" : "false";
    out += "\nlog-format=";
    out += daemon_log_format_name(options.log_format);
    out += "\ntls-cert=";
    out += options.server.tls.certificate_file.string();
    out += "\ntls-key=";
    out += options.server.tls.private_key_file.string();
    out += "\ntls-client-ca=";
    out += options.server.tls.client_ca_file.string();
    out += "\ntls-crl=";
    out += options.server.tls.crl_file.string();
    out += "\ntls-ocsp-fail-closed=";
    out += options.server.tls.ocsp_fail_closed ? "true" : "false";
    out += "\ntls-port=";
    if (options.server.tls_port.has_value()) {
        out += std::to_string(*options.server.tls_port);
    }
    out += "\nauthz-map=";
    out += options.authz_map_path.string();
    out += "\nauthz-enabled=";
    out += options.server.authz.enabled() ? "true" : "false";
    out += "\nauthz-principals=";
    out += std::to_string(options.server.authz.size());
    out += "\nauthz-prefix-scoped=";
    out += std::to_string(options.server.authz.prefix_scoped_count());
    out += "\nindex-hash-seed=";
    out += std::to_string(options.index_hash_seed);
    out += "\nindex-hash-seed-explicit=";
    out += options.index_hash_seed_explicit ? "true" : "false";
    out += "\nworker-hash-seed=";
    out += std::to_string(options.worker_routing.seed);
    out += "\nworker-hash-seed-explicit=";
    out += options.worker_hash_seed_explicit ? "true" : "false";
    out += "\nworker-routing-algorithm=";
    out += options.worker_routing.keyed() ? "siphash24-v1" : "fnv1a64-v1";
    out += "\nsecure-profile=";
    out += options.secure_profile ? "true" : "false";
    out += "\nunix-socket=";
    out += options.server.unix_socket_path.string();
    out += "\nunix-peercred=";
    out += options.server.unix_peercred ? "true" : "false";
    out += "\nunix-peercred-required=";
    out += options.server.unix_peercred_required ? "true" : "false";
    out += "\nsecurity-audit-events=";
    out += options.server.security_audit_events ? "true" : "false";
    out += "\nmax-accepts-per-sec=";
    out += std::to_string(options.server.abuse.max_accepts_per_sec);
    out += "\nidle-timeout-ms=";
    out += std::to_string(options.server.abuse.idle_timeout_ms);
    out += "\nrequest-timeout-ms=";
    out += std::to_string(options.server.abuse.request_timeout_ms);
    out += "\nconnection-max-requests-per-sec=";
    out += std::to_string(options.server.abuse.connection_max_requests_per_sec);
    out += "\nprincipal-max-requests-per-sec=";
    out += std::to_string(options.server.abuse.principal_max_requests_per_sec);
    out += "\nprincipal-max-bytes-per-sec=";
    out += std::to_string(options.server.abuse.principal_max_bytes_per_sec);
    out += '\n';
    return out;
}

} // namespace glifistore::server
