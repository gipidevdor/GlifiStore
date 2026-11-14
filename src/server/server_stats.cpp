#include "server/server_stats.hpp"

#include "glifistore/server/latency_histogram.hpp"
#include "glifistore/store/paired/read_generation.hpp"

#include <string>
#include <utility>

namespace glifistore::server {

auto ServerStatsReporter::maintenance_state_name(const MaintenanceState state) noexcept -> std::string_view {
    switch (state) {
    case MaintenanceState::stopped:
        return "stopped";
    case MaintenanceState::idle:
        return "idle";
    case MaintenanceState::evaluating:
        return "evaluating";
    case MaintenanceState::compacting:
        return "compacting";
    case MaintenanceState::suspended:
        return "suspended";
    case MaintenanceState::draining:
        return "draining";
    case MaintenanceState::faulted:
        return "faulted";
    }
    return "unknown";
}

auto ServerStatsReporter::maintenance_pressure_name(const MaintenancePressureLevel level) noexcept
    -> std::string_view {
    switch (level) {
    case MaintenancePressureLevel::none:
        return "none";
    case MaintenancePressureLevel::normal:
        return "normal";
    case MaintenancePressureLevel::pressure:
        return "pressure";
    case MaintenancePressureLevel::emergency:
        return "emergency";
    }
    return "unknown";
}

auto ServerStatsReporter::maintenance_skip_reason_name(const MaintenanceSkipReason reason) noexcept
    -> std::string_view {
    switch (reason) {
    case MaintenanceSkipReason::none:
        return "none";
    case MaintenanceSkipReason::mode_disabled:
        return "mode_disabled";
    case MaintenanceSkipReason::mode_cooperative:
        return "mode_cooperative";
    case MaintenanceSkipReason::no_gain:
        return "no_gain";
    case MaintenanceSkipReason::no_candidate:
        return "no_candidate";
    case MaintenanceSkipReason::budget:
        return "budget";
    case MaintenanceSkipReason::store_closed:
        return "store_closed";
    case MaintenanceSkipReason::sequence_conflict:
        return "sequence_conflict";
    case MaintenanceSkipReason::policy_deferred:
        return "policy_deferred";
    case MaintenanceSkipReason::reclaim_threshold:
        return "reclaim_threshold";
    case MaintenanceSkipReason::copy_budget:
        return "copy_budget";
    case MaintenanceSkipReason::rate_budget:
        return "rate_budget";
    case MaintenanceSkipReason::latency_budget:
        return "latency_budget";
    }
    return "unknown";
}

auto ServerStatsReporter::maintenance_activation_reason_name(
    const MaintenanceActivationReason reason) noexcept -> std::string_view {
    switch (reason) {
    case MaintenanceActivationReason::none:
        return "none";
    case MaintenanceActivationReason::scheduled:
        return "scheduled";
    case MaintenanceActivationReason::sealed_history:
        return "sealed_history";
    case MaintenanceActivationReason::segment_pressure:
        return "segment_pressure";
    case MaintenanceActivationReason::free_space_pressure:
        return "free_space_pressure";
    case MaintenanceActivationReason::emergency_capacity:
        return "emergency_capacity";
    case MaintenanceActivationReason::budget_backoff:
        return "budget_backoff";
    case MaintenanceActivationReason::policy_deferred:
        return "policy_deferred";
    case MaintenanceActivationReason::no_candidate:
        return "no_candidate";
    case MaintenanceActivationReason::reclaim_threshold:
        return "reclaim_threshold";
    case MaintenanceActivationReason::copy_budget:
        return "copy_budget";
    case MaintenanceActivationReason::rate_budget:
        return "rate_budget";
    case MaintenanceActivationReason::latency_budget:
        return "latency_budget";
    case MaintenanceActivationReason::latency_debt_override:
        return "latency_debt_override";
    }
    return "unknown";
}

auto ServerStatsReporter::render(const ServerStatsSnapshot& snapshot, const std::size_t maximum_bytes)
    -> Result<std::string> {
    try {
        std::string out;
        out.reserve(4096);
        out += "GlifiStore/stats\n";
#ifndef GLIFISTORE_VERSION
#define GLIFISTORE_VERSION "dev"
#endif
        out += "version=";
        out += GLIFISTORE_VERSION;
        out += '\n';
        out += snapshot.live ? "live=1\n" : "live=0\n";
        out += snapshot.ready ? "ready=1\n" : "ready=0\n";
        out += "executors=";
        out += std::to_string(snapshot.executors.size());
        out += '\n';
        out += "read_generation_spare_mapping_bytes=";
        out += std::to_string(store::paired::immutable_base_spare_mapping_bytes());
        out += '\n';

        std::uint64_t connections_active{};
        std::uint64_t connections_adopted{};
        std::uint64_t output_scatter_responses{};
        std::uint64_t output_scatter_bytes{};
        std::uint64_t output_scatter_partial_writes{};
        std::uint64_t output_scatter_completions{};
        std::uint64_t input_buffer_compactions{};
        std::uint64_t input_buffer_bytes_moved{};
        std::uint64_t output_buffer_compactions{};
        std::uint64_t output_buffer_bytes_moved{};
        for (const auto& executor : snapshot.executors) {
            connections_active += executor.active_connections;
            connections_adopted += executor.adopted_connections;
            output_scatter_responses += executor.output_scatter_responses;
            output_scatter_bytes += executor.output_scatter_bytes;
            output_scatter_partial_writes += executor.output_scatter_partial_writes;
            output_scatter_completions += executor.output_scatter_completions;
            input_buffer_compactions += executor.input_buffer_compactions;
            input_buffer_bytes_moved += executor.input_buffer_bytes_moved;
            output_buffer_compactions += executor.output_buffer_compactions;
            output_buffer_bytes_moved += executor.output_buffer_bytes_moved;
        }
        out += "connections_active=";
        out += std::to_string(connections_active);
        out += '\n';
        out += "connections_adopted=";
        out += std::to_string(connections_adopted);
        out += '\n';
        out += "output_scatter_responses=";
        out += std::to_string(output_scatter_responses);
        out += '\n';
        out += "output_scatter_bytes=";
        out += std::to_string(output_scatter_bytes);
        out += '\n';
        out += "output_scatter_partial_writes=";
        out += std::to_string(output_scatter_partial_writes);
        out += '\n';
        out += "output_scatter_completions=";
        out += std::to_string(output_scatter_completions);
        out += '\n';
        out += "input_buffer_compactions=";
        out += std::to_string(input_buffer_compactions);
        out += '\n';
        out += "input_buffer_bytes_moved=";
        out += std::to_string(input_buffer_bytes_moved);
        out += '\n';
        out += "output_buffer_compactions=";
        out += std::to_string(output_buffer_compactions);
        out += '\n';
        out += "output_buffer_bytes_moved=";
        out += std::to_string(output_buffer_bytes_moved);
        out += '\n';
        out += "abuse_accepts_rejected=";
        out += std::to_string(snapshot.abuse.accepts_rejected);
        out += '\n';
        out += "abuse_idle_closed=";
        out += std::to_string(snapshot.abuse.idle_closed);
        out += '\n';
        out += "abuse_request_timeout_closed=";
        out += std::to_string(snapshot.abuse.request_timeout_closed);
        out += '\n';
        out += "abuse_connection_rate_rejected=";
        out += std::to_string(snapshot.abuse.connection_rate_rejected);
        out += '\n';
        out += "abuse_principal_request_rejected=";
        out += std::to_string(snapshot.abuse.principal_request_rejected);
        out += '\n';
        out += "abuse_principal_bandwidth_rejected=";
        out += std::to_string(snapshot.abuse.principal_bandwidth_rejected);
        out += '\n';
        out += "tls_enabled=";
        out += snapshot.tls_enabled ? "1\n" : "0\n";
        out += "tls_mtls=";
        out += snapshot.tls_mtls ? "1\n" : "0\n";
        out += "tls_crl=";
        out += snapshot.tls_crl ? "1\n" : "0\n";
        out += "tls_ocsp_fail_closed=";
        out += snapshot.tls_ocsp_fail_closed ? "1\n" : "0\n";
        out += "authz_enabled=";
        out += snapshot.authz_enabled ? "1\n" : "0\n";
        out += "authz_principals=";
        out += std::to_string(snapshot.authz_principals);
        out += '\n';
        out += "auth_accepts=";
        out += std::to_string(snapshot.security_audit.auth_accepts);
        out += '\n';
        out += "auth_denies=";
        out += std::to_string(snapshot.security_audit.auth_denies);
        out += '\n';
        out += "authz_denies=";
        out += std::to_string(snapshot.security_audit.authz_denies);
        out += '\n';
        out += "tls_errors=";
        out += std::to_string(snapshot.security_audit.tls_errors);
        out += '\n';
        out += "audit_events_suppressed=";
        out += std::to_string(snapshot.security_audit.events_suppressed);
        out += '\n';
        out += "audit_events_written=";
        out += std::to_string(snapshot.security_audit.events_written);
        out += '\n';
        out += "audit_events_pending=";
        out += std::to_string(snapshot.security_audit.events_pending);
        out += '\n';
        out += "audit_events_write_failed=";
        out += std::to_string(snapshot.security_audit.events_write_failed);
        out += '\n';

        const auto& maintenance = snapshot.maintenance;
        out += "maintenance_state=";
        out += maintenance_state_name(maintenance.state);
        out += '\n';
        out += "maintenance_pressure=";
        out += maintenance_pressure_name(maintenance.pressure);
        out += '\n';
        out += "mutations_rejected=";
        out += maintenance.mutations_rejected ? "1\n" : "0\n";
        out += "compact_attempts=";
        out += std::to_string(maintenance.compact_attempts);
        out += '\n';
        out += "compact_completed=";
        out += std::to_string(maintenance.compact_completed);
        out += '\n';
        out += "useful_compactions=";
        out += std::to_string(maintenance.useful_compactions);
        out += '\n';
        out += "maintenance_last_compaction_pacing_delay_ns=";
        out += std::to_string(maintenance.last_compaction_pacing_delay_ns);
        out += '\n';
        out += "maintenance_total_compaction_pacing_delay_ns=";
        out += std::to_string(maintenance.total_compaction_pacing_delay_ns);
        out += '\n';
        out += "maintenance_last_compaction_pacing_sleep_count=";
        out += std::to_string(maintenance.last_compaction_pacing_sleep_count);
        out += '\n';
        out += "maintenance_last_compaction_pacing_burst_bytes=";
        out += std::to_string(maintenance.last_compaction_pacing_burst_bytes);
        out += '\n';
        out += "maintenance_skips=";
        out += std::to_string(maintenance.skips);
        out += '\n';
        out += "maintenance_consecutive_no_gain=";
        out += std::to_string(maintenance.consecutive_no_gain);
        out += '\n';
        out += "maintenance_last_skip_reason=";
        out += maintenance_skip_reason_name(maintenance.last_skip_reason);
        out += "\nmaintenance_last_activation_reason=";
        out += maintenance_activation_reason_name(maintenance.last_activation_reason);
        out += '\n';
        out += "maintenance_last_no_gain_source_records_verified=";
        out += std::to_string(maintenance.last_no_gain_source_records_verified);
        out += '\n';
        out += "maintenance_last_no_gain_source_bytes_verified=";
        out += std::to_string(maintenance.last_no_gain_source_bytes_verified);
        out += '\n';
        out += "maintenance_last_no_gain_expired_records_dropped=";
        out += std::to_string(maintenance.last_no_gain_expired_records_dropped);
        out += '\n';
        out += "maintenance_total_no_gain_source_records_verified=";
        out += std::to_string(maintenance.total_no_gain_source_records_verified);
        out += '\n';
        out += "maintenance_total_no_gain_source_bytes_verified=";
        out += std::to_string(maintenance.total_no_gain_source_bytes_verified);
        out += '\n';
        out += "maintenance_total_no_gain_expired_records_dropped=";
        out += std::to_string(maintenance.total_no_gain_expired_records_dropped);
        out += '\n';
        out += "maintenance_no_gain_scans_suppressed=";
        out += std::to_string(maintenance.no_gain_scans_suppressed);
        out += '\n';
        out += "maintenance_no_gain_retry_after_ns=";
        out += std::to_string(maintenance.no_gain_retry_after_ns);
        out += '\n';
        out += "maintenance_sequence_conflicts=";
        out += std::to_string(maintenance.sequence_conflicts);
        out += '\n';
        out += "durable_rotation_attempts=";
        out += std::to_string(maintenance.rotation.attempts);
        out += "\ndurable_rotations_committed=";
        out += std::to_string(maintenance.rotation.committed);
        out += "\ndurable_rotation_compaction_waits=";
        out += std::to_string(maintenance.rotation.compaction_waits);
        out += "\ndurable_rotation_final_record_commit_attempts=";
        out += std::to_string(maintenance.rotation.final_record_commit_attempts);
        out += "\ndurable_rotation_final_record_commits=";
        out += std::to_string(maintenance.rotation.final_record_commits);
        out += "\ndurable_rotation_last_publication_wait_ns=";
        out += std::to_string(maintenance.rotation.last_publication_wait_duration_ns);
        out += "\ndurable_rotation_total_publication_wait_ns=";
        out += std::to_string(maintenance.rotation.total_publication_wait_duration_ns);
        out += "\ndurable_rotation_maximum_publication_wait_ns=";
        out += std::to_string(maintenance.rotation.maximum_publication_wait_duration_ns);
        out += "\ndurable_rotation_last_seal_ns=";
        out += std::to_string(maintenance.rotation.last_seal_duration_ns);
        out += "\ndurable_rotation_total_seal_ns=";
        out += std::to_string(maintenance.rotation.total_seal_duration_ns);
        out += "\ndurable_rotation_maximum_seal_ns=";
        out += std::to_string(maintenance.rotation.maximum_seal_duration_ns);
        out += "\ndurable_rotation_last_create_ns=";
        out += std::to_string(maintenance.rotation.last_create_duration_ns);
        out += "\ndurable_rotation_total_create_ns=";
        out += std::to_string(maintenance.rotation.total_create_duration_ns);
        out += "\ndurable_rotation_maximum_create_ns=";
        out += std::to_string(maintenance.rotation.maximum_create_duration_ns);
        out += "\ndurable_rotation_last_manifest_publication_ns=";
        out += std::to_string(maintenance.rotation.last_manifest_publication_duration_ns);
        out += "\ndurable_rotation_total_manifest_publication_ns=";
        out += std::to_string(maintenance.rotation.total_manifest_publication_duration_ns);
        out += "\ndurable_rotation_maximum_manifest_publication_ns=";
        out += std::to_string(maintenance.rotation.maximum_manifest_publication_duration_ns);
        out += "\ndurable_rotation_last_execution_ns=";
        out += std::to_string(maintenance.rotation.last_execution_duration_ns);
        out += "\ndurable_rotation_total_execution_ns=";
        out += std::to_string(maintenance.rotation.total_execution_duration_ns);
        out += "\ndurable_rotation_maximum_execution_ns=";
        out += std::to_string(maintenance.rotation.maximum_execution_duration_ns);
        out += "\ndurable_rotation_last_total_ns=";
        out += std::to_string(maintenance.rotation.last_total_duration_ns);
        out += "\ndurable_rotation_total_ns=";
        out += std::to_string(maintenance.rotation.total_duration_ns);
        out += "\ndurable_rotation_maximum_total_ns=";
        out += std::to_string(maintenance.rotation.maximum_total_duration_ns);
        out += "\ndurable_rotation_last_final_record_commit_ns=";
        out += std::to_string(maintenance.rotation.last_final_record_commit_duration_ns);
        out += "\ndurable_rotation_total_final_record_commit_ns=";
        out += std::to_string(maintenance.rotation.total_final_record_commit_duration_ns);
        out += "\ndurable_rotation_maximum_final_record_commit_ns=";
        out += std::to_string(maintenance.rotation.maximum_final_record_commit_duration_ns);
        out += '\n';
        out += "maintenance_candidate_worker=";
        if (maintenance.last_observation.compaction_candidate_worker) {
            out += std::to_string(*maintenance.last_observation.compaction_candidate_worker);
        } else {
            out += "none";
        }
        out += "\nmaintenance_candidate_sealed_record_bytes=";
        out += std::to_string(maintenance.last_observation.candidate_sealed_record_bytes);
        out += "\nmaintenance_candidate_live_record_bytes=";
        out += std::to_string(maintenance.last_observation.candidate_live_record_bytes);
        out += "\nmaintenance_candidate_dead_record_bytes=";
        out += std::to_string(maintenance.last_observation.candidate_dead_record_bytes);
        out += "\nmaintenance_candidate_dead_byte_ratio_bp=";
        if (maintenance.last_observation.candidate_dead_byte_ratio_bp) {
            out += std::to_string(*maintenance.last_observation.candidate_dead_byte_ratio_bp);
        } else {
            out += "none";
        }
        out += "\nmaintenance_candidate_scheduling_dead_byte_ratio_bp=";
        if (maintenance.last_observation.candidate_scheduling_dead_byte_ratio_bp) {
            out += std::to_string(*maintenance.last_observation.candidate_scheduling_dead_byte_ratio_bp);
        } else {
            out += "none";
        }
        out += "\nmaintenance_unread_ttl_probe_performed=";
        out += maintenance.last_observation.unread_ttl_probe_performed ? "1" : "0";
        out += "\nmaintenance_candidate_unread_expired_sealed_record_count=";
        out += std::to_string(maintenance.last_observation.candidate_unread_expired_sealed_record_count);
        out += "\nmaintenance_candidate_unread_expired_sealed_record_bytes=";
        out += std::to_string(maintenance.last_observation.candidate_unread_expired_sealed_record_bytes);
        out += "\nmaintenance_rate_window_bytes_copied=";
        out += std::to_string(maintenance.rate_window_bytes_copied);
        out += "\nmaintenance_rate_window_cpu_ns=";
        out += std::to_string(maintenance.rate_window_cpu_ns);
        out += "\nmaintenance_foreground_latency_samples=";
        out += std::to_string(maintenance.foreground_latency_samples);
        out += "\nmaintenance_last_foreground_p99_ns=";
        out += std::to_string(maintenance.last_foreground_p99_ns);
        out += "\nmaintenance_latency_suspends=";
        out += std::to_string(maintenance.latency_suspends);
        out += "\nmaintenance_latency_guard_active=";
        out += maintenance.latency_guard_active ? "1" : "0";
        out += "\nmaintenance_latency_deferral_age_ns=";
        out += std::to_string(maintenance.latency_deferral_age_ns);
        out += "\nmaintenance_latency_debt_overrides=";
        out += std::to_string(maintenance.latency_debt_overrides);
        out += '\n';

        for (const auto& lane : snapshot.mutations) {
            out += "lane[";
            out += std::to_string(lane.worker_index);
            out += "].reader_safe_epoch=";
            out += std::to_string(lane.reader_safe_epoch);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].writer_epoch=";
            out += std::to_string(lane.writer_epoch);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].queue_depth=";
            out += std::to_string(lane.queue_depth);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].queued_bytes=";
            out += std::to_string(lane.queued_bytes);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].maximum_queue_depth=";
            out += std::to_string(lane.maximum_queue_depth);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].maximum_queued_bytes=";
            out += std::to_string(lane.maximum_queued_bytes);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].payload_slot_capacity=";
            out += std::to_string(lane.payload_slot_capacity);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].payload_slots_in_use=";
            out += std::to_string(lane.payload_slots_in_use);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].maximum_payload_slots_in_use=";
            out += std::to_string(lane.maximum_payload_slots_in_use);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].payload_arena_capacity_bytes=";
            out += std::to_string(lane.payload_arena_capacity_bytes);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].payload_arena_storage_bytes=";
            out += std::to_string(lane.payload_arena_storage_bytes);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].payload_arena_bytes_in_use=";
            out += std::to_string(lane.payload_arena_bytes_in_use);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].maximum_payload_arena_bytes_in_use=";
            out += std::to_string(lane.maximum_payload_arena_bytes_in_use);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].payload_admission_bytes_in_use=";
            out += std::to_string(lane.payload_admission_bytes_in_use);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].maximum_payload_admission_bytes_in_use=";
            out += std::to_string(lane.maximum_payload_admission_bytes_in_use);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].payload_slot_full_total=";
            out += std::to_string(lane.payload_slot_full_total);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].payload_arena_full_total=";
            out += std::to_string(lane.payload_arena_full_total);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].payload_too_large_total=";
            out += std::to_string(lane.payload_too_large_total);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].admitted=";
            out += std::to_string(lane.admitted);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].rejected=";
            out += std::to_string(lane.rejected);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].expired_before_store=";
            out += std::to_string(lane.expired_before_store);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].completed=";
            out += std::to_string(lane.completed);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].conflict_retries=";
            out += std::to_string(lane.conflict_retries);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].conflict_retry_commits=";
            out += std::to_string(lane.conflict_retry_commits);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].writer_batches=";
            out += std::to_string(lane.writer_batches);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].writer_batch_records=";
            out += std::to_string(lane.writer_batch_records);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].maximum_writer_batch_records=";
            out += std::to_string(lane.maximum_writer_batch_records);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].total_writer_batch_wait_ns=";
            out += std::to_string(lane.total_writer_batch_wait_ns);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].maximum_writer_batch_wait_ns=";
            out += std::to_string(lane.maximum_writer_batch_wait_ns);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].writer_batch_durability_deadline_closes=";
            out += std::to_string(lane.writer_batch_durability_deadline_closes);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].writer_batch_queue_deadline_closes=";
            out += std::to_string(lane.writer_batch_queue_deadline_closes);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].sync_drain_turns=";
            out += std::to_string(lane.sync_drain_turns);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].sync_turn_splits=";
            out += std::to_string(lane.sync_turn_splits);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].sync_async_fairness_turns=";
            out += std::to_string(lane.sync_async_fairness_turns);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].publications=";
            out += std::to_string(lane.publications);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].publication_records=";
            out += std::to_string(lane.publication_records);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].completion_notifications=";
            out += std::to_string(lane.completion_notifications);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].read_catalog_revision=";
            out += std::to_string(lane.read_catalog_revision);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].read_refresh_attempts=";
            out += std::to_string(lane.read_refresh_attempts);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].read_refresh_successes=";
            out += std::to_string(lane.read_refresh_successes);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].read_refresh_failures=";
            out += std::to_string(lane.read_refresh_failures);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].read_refresh_deferrals=";
            out += std::to_string(lane.read_refresh_deferrals);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].generations_retired=";
            out += std::to_string(lane.generations_retired);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].shutdown_generations_reclaimed=";
            out += std::to_string(lane.shutdown_generations_reclaimed);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].generation_admission_backpressure_total=";
            out += std::to_string(lane.generation_admission_backpressure_total);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].reader_shutdown_finalized=";
            out += lane.reader_shutdown_finalized ? "1" : "0";
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].retired_generation_count=";
            out += std::to_string(lane.retired_generation_count);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].delta_entries=";
            out += std::to_string(lane.delta_entries);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].delta_record_versions=";
            out += std::to_string(lane.delta_record_versions);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].delta_arena_record_bytes=";
            out += std::to_string(lane.delta_arena_record_bytes);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].delta_arena_key_bytes=";
            out += std::to_string(lane.delta_arena_key_bytes);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].delta_arena_key_storage_bytes=";
            out += std::to_string(lane.delta_arena_key_storage_bytes);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].read_generation_base_entries=";
            out += std::to_string(lane.read_generation_memory.base_entries);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].read_generation_base_capacity=";
            out += std::to_string(lane.read_generation_memory.base_capacity);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].read_generation_base_record_storage_bytes=";
            out += std::to_string(lane.read_generation_memory.base_record_storage_bytes);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].read_generation_base_record_mapped_storage_bytes=";
            out += std::to_string(lane.read_generation_memory.base_record_mapped_storage_bytes);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].read_generation_base_lookup_storage_bytes=";
            out += std::to_string(lane.read_generation_memory.base_lookup_storage_bytes);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].read_generation_base_key_bytes=";
            out += std::to_string(lane.read_generation_memory.base_key_bytes);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].read_generation_base_key_storage_bytes=";
            out += std::to_string(lane.read_generation_memory.base_key_storage_bytes);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].read_generation_base_pin_storage_bytes=";
            out += std::to_string(lane.read_generation_memory.base_pin_storage_bytes);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].read_generation_base_allocated_lower_bound_bytes=";
            out += std::to_string(lane.read_generation_memory.base_allocated_lower_bound_bytes);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].read_generation_delta_capacity=";
            out += std::to_string(lane.read_generation_memory.delta_capacity);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].read_generation_delta_lookup_storage_bytes=";
            out += std::to_string(lane.read_generation_memory.delta_lookup_storage_bytes);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].read_generation_delta_allocated_lower_bound_bytes=";
            out += std::to_string(lane.read_generation_memory.delta_allocated_lower_bound_bytes);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].read_generation_shell_bytes=";
            out += std::to_string(lane.read_generation_memory.generation_shell_bytes);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].read_generation_current_allocated_lower_bound_bytes=";
            out += std::to_string(lane.read_generation_memory.current_allocated_lower_bound_bytes);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].read_merge_active=";
            out += lane.read_merge_active ? "1" : "0";
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].read_merge_post_entries=";
            out += std::to_string(lane.read_merge_post_entries);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].read_merge_starts=";
            out += std::to_string(lane.read_merge_starts);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].read_merge_completions=";
            out += std::to_string(lane.read_merge_completions);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].read_merge_failures=";
            out += std::to_string(lane.read_merge_failures);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].read_merge_backpressure=";
            out += std::to_string(lane.read_merge_backpressure);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].read_merge_slots_processed=";
            out += std::to_string(lane.read_merge_slots_processed);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].read_merge_remaining_slots=";
            out += std::to_string(lane.read_merge_remaining_slots);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].read_merge_post_capacity_remaining=";
            out += std::to_string(lane.read_merge_post_capacity_remaining);
            out += "\nlane[";
            out += std::to_string(lane.worker_index);
            out += "].maximum_read_merge_quantum_slots=";
            out += std::to_string(lane.maximum_read_merge_quantum_slots);
            out += '\n';
            append_latency_histogram(out, "lane[" + std::to_string(lane.worker_index) + "].queue_wait_ns",
                                     lane.queue_wait_histogram);
            append_latency_histogram(out, "lane[" + std::to_string(lane.worker_index) + "].service_ns",
                                     lane.service_histogram);
            if (out.size() > maximum_bytes) {
                return fail(ErrorCode::resource_exhausted, "stats report exceeds the bounded size budget");
            }
        }
        for (const auto& batch : snapshot.batches) {
            out += "batch[";
            out += std::to_string(batch.worker_id.value);
            out += "].enabled=";
            out += batch.enabled ? "1" : "0";
            out += "\nbatch[";
            out += std::to_string(batch.worker_id.value);
            out += "].pending_records=";
            out += std::to_string(batch.pending_records);
            out += "\nbatch[";
            out += std::to_string(batch.worker_id.value);
            out += "].committed_batches=";
            out += std::to_string(batch.committed_batches);
            out += "\nbatch[";
            out += std::to_string(batch.worker_id.value);
            out += "].failed_batches=";
            out += std::to_string(batch.failed_batches);
            out += '\n';
            if (out.size() > maximum_bytes) {
                return fail(ErrorCode::resource_exhausted, "stats report exceeds the bounded size budget");
            }
        }
        return out;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::resource_exhausted, {});
    }
}

} // namespace glifistore::server
