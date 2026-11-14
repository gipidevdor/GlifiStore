#include "glifistore/server/daemon_log.hpp"

#include "glifistore/core/error.hpp"
#include "glifistore/core/text_encoding.hpp"

#include <cctype>
#include <chrono>
#include <iostream>
#include <string>

namespace glifistore::server {
namespace {

constexpr std::size_t kMaxLoggedFieldBytes = 256U;

[[nodiscard]] auto ascii_lower(std::string text) -> std::string {
    for (char& character : text) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return text;
}

[[nodiscard]] auto escape_log_field(const std::string_view text) -> std::string {
    return json_escape_bounded(text, kMaxLoggedFieldBytes);
}

[[nodiscard]] auto epoch_seconds() noexcept -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] auto maintenance_pressure_label(const MaintenancePressureLevel level) noexcept
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

[[nodiscard]] auto maintenance_state_label(const MaintenanceState state) noexcept -> std::string_view {
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

[[nodiscard]] auto error_code_label(const ErrorCode code) noexcept -> std::string_view {
    switch (code) {
    case ErrorCode::invalid_argument:
        return "invalid_argument";
    case ErrorCode::arithmetic_overflow:
        return "arithmetic_overflow";
    case ErrorCode::record_too_large:
        return "record_too_large";
    case ErrorCode::segment_full:
        return "segment_full";
    case ErrorCode::segment_sealed:
        return "segment_sealed";
    case ErrorCode::invalid_record:
        return "invalid_record";
    case ErrorCode::checksum_mismatch:
        return "checksum_mismatch";
    case ErrorCode::invalid_reference:
        return "invalid_reference";
    case ErrorCode::sequence_conflict:
        return "sequence_conflict";
    case ErrorCode::corrupted_data:
        return "corrupted_data";
    case ErrorCode::not_found:
        return "not_found";
    case ErrorCode::resource_exhausted:
        return "resource_exhausted";
    case ErrorCode::storage_exhausted:
        return "storage_exhausted";
    case ErrorCode::file_too_large:
        return "file_too_large";
    case ErrorCode::descriptor_exhausted:
        return "descriptor_exhausted";
    case ErrorCode::read_only_filesystem:
        return "read_only_filesystem";
    case ErrorCode::internal_error:
        return "internal_error";
    case ErrorCode::unavailable:
        return "unavailable";
    case ErrorCode::io_error:
        return "io_error";
    }
    return "unknown";
}

} // namespace

auto daemon_log_format_name(const DaemonLogFormat format) noexcept -> std::string_view {
    switch (format) {
    case DaemonLogFormat::human:
        return "human";
    case DaemonLogFormat::json:
        return "json";
    }
    return "unknown";
}

auto parse_daemon_log_format(const std::string_view text) -> Result<DaemonLogFormat> {
    const auto lowered = ascii_lower(std::string{text});
    if (lowered == "human") {
        return DaemonLogFormat::human;
    }
    if (lowered == "json") {
        return DaemonLogFormat::json;
    }
    return fail(ErrorCode::invalid_argument,
                "unknown --log-format: " + std::string{text} + " (expected human or json)");
}

auto daemon_error_code_name(const ErrorCode code) noexcept -> std::string_view {
    return error_code_label(code);
}

auto ready_loss_reason_name(const ReadyLossReason reason) noexcept -> std::string_view {
    switch (reason) {
    case ReadyLossReason::none:
        return "none";
    case ReadyLossReason::not_live:
        return "not_live";
    case ReadyLossReason::shutting_down:
        return "shutting_down";
    case ReadyLossReason::store_not_operational:
        return "store_not_operational";
    case ReadyLossReason::pair_fail_closed:
        return "pair_fail_closed";
    case ReadyLossReason::admission_fenced:
        return "admission_fenced";
    case ReadyLossReason::maintenance_emergency:
        return "maintenance_emergency";
    case ReadyLossReason::maintenance_fault:
        return "maintenance_fault";
    }
    return "unknown";
}

auto classify_ready_loss(const Server& server) noexcept -> ReadyLossReason {
    if (!server.live()) {
        return ReadyLossReason::not_live;
    }
    if (server.stop_requested()) {
        return ReadyLossReason::shutting_down;
    }
    if (!server.store_operational()) {
        return ReadyLossReason::store_not_operational;
    }
    // Mirror Server::ready(): pair sticky fails READY while HEALTH/live may stay OK.
    // Volatile sticky often leaves the Store catalog "operational" — without this
    // probe, structured ready.reason would dishonestly report none.
    if (!server.pair_writers_healthy()) {
        return ReadyLossReason::pair_fail_closed;
    }
    if (!server.admissions_open()) {
        return ReadyLossReason::admission_fenced;
    }
    const auto snapshot = server.maintenance_snapshot();
    if (snapshot.mutations_rejected) {
        return ReadyLossReason::maintenance_emergency;
    }
    if (snapshot.state == MaintenanceState::faulted && snapshot.last_error.has_value()) {
        return ReadyLossReason::maintenance_fault;
    }
    return ReadyLossReason::none;
}

DaemonLog::DaemonLog(const DaemonLogFormat format, const std::string_view program, const bool quiet) noexcept
    : format_{format}, program_{program}, quiet_{quiet} {}

auto DaemonLog::suppress_quiet_lifecycle() const noexcept -> bool {
    return quiet_;
}

void DaemonLog::write_json_prefix(std::string& line, const std::string_view event) const {
    line += "{\"ts\":";
    line += std::to_string(epoch_seconds());
    line += ",\"event\":\"";
    line += event;
    line += "\",\"program\":\"";
    line += escape_log_field(program_);
    line += '"';
}

void DaemonLog::emit_start() const {
    if (format_ != DaemonLogFormat::json || suppress_quiet_lifecycle()) {
        return;
    }
    std::string line;
    line.reserve(128U);
    write_json_prefix(line, "start");
    line += "}\n";
    std::cerr << line;
}

void DaemonLog::emit_listen(const std::string_view bind_address, const std::uint16_t cleartext_port,
                            const std::uint16_t tls_port, const std::size_t executors,
                            const std::string_view storage_mode,
                            const std::string_view unix_socket_path) const {
    if (format_ != DaemonLogFormat::json || suppress_quiet_lifecycle()) {
        return;
    }
    std::string line;
    line.reserve(256U);
    write_json_prefix(line, "listen");
    line += ",\"bind\":\"";
    line += escape_log_field(bind_address);
    line += '"';
    if (cleartext_port != 0) {
        line += ",\"cleartext_port\":";
        line += std::to_string(cleartext_port);
    }
    if (tls_port != 0) {
        line += ",\"tls_port\":";
        line += std::to_string(tls_port);
    }
    if (!unix_socket_path.empty()) {
        line += ",\"unix_socket\":\"";
        line += escape_log_field(unix_socket_path);
        line += '"';
    }
    line += ",\"executors\":";
    line += std::to_string(executors);
    line += ",\"storage\":\"";
    line += escape_log_field(storage_mode);
    line += "\"}\n";
    std::cerr << line;
}

void DaemonLog::emit_ready(const bool ready, const ReadyLossReason reason) const {
    if (format_ != DaemonLogFormat::json) {
        return;
    }
    std::string line;
    line.reserve(192U);
    write_json_prefix(line, "ready");
    line += ",\"ready\":";
    line += ready ? "1" : "0";
    if (!ready && reason != ReadyLossReason::none) {
        line += ",\"reason\":\"";
        line += ready_loss_reason_name(reason);
        line += '"';
    }
    line += "}\n";
    std::cerr << line;
}

void DaemonLog::emit_maintenance_emergency(const MaintenancePressureLevel pressure) const {
    if (format_ != DaemonLogFormat::json) {
        return;
    }
    std::string line;
    line.reserve(192U);
    write_json_prefix(line, "maintenance_emergency");
    line += ",\"maintenance_pressure\":\"";
    line += maintenance_pressure_label(pressure);
    line += "\"}\n";
    std::cerr << line;
}

void DaemonLog::emit_maintenance_fault(const MaintenanceState state, const std::string_view error_code,
                                       const std::string_view error_message) const {
    if (format_ != DaemonLogFormat::json) {
        return;
    }
    std::string line;
    line.reserve(256U);
    write_json_prefix(line, "maintenance_fault");
    line += ",\"maintenance_state\":\"";
    line += maintenance_state_label(state);
    line += "\",\"error_code\":\"";
    line += escape_log_field(error_code);
    line += "\",\"error_message\":\"";
    line += escape_log_field(error_message);
    line += "\"}\n";
    std::cerr << line;
}

void DaemonLog::emit_shutdown_begin(const int signal, const bool executor_failure) const {
    if (format_ != DaemonLogFormat::json) {
        return;
    }
    std::string line;
    line.reserve(192U);
    write_json_prefix(line, "shutdown_begin");
    if (signal != 0) {
        line += ",\"signal\":";
        line += std::to_string(signal);
    }
    if (executor_failure) {
        line += ",\"executor_failure\":1";
    }
    line += "}\n";
    std::cerr << line;
}

void DaemonLog::emit_shutdown_drain_begin(const std::uint32_t drain_ms) const {
    if (format_ != DaemonLogFormat::json) {
        return;
    }
    std::string line;
    line.reserve(192U);
    write_json_prefix(line, "shutdown_drain_begin");
    line += ",\"drain_ms\":";
    line += std::to_string(drain_ms);
    line += "}\n";
    std::cerr << line;
}

void DaemonLog::emit_shutdown_drain_end(const bool timed_out, const bool join_failed) const {
    if (format_ != DaemonLogFormat::json) {
        return;
    }
    std::string line;
    line.reserve(192U);
    write_json_prefix(line, "shutdown_drain_end");
    line += ",\"timed_out\":";
    line += timed_out ? "1" : "0";
    if (join_failed) {
        line += ",\"join_failed\":1";
    }
    line += "}\n";
    std::cerr << line;
}

void DaemonLog::emit_stopped(const int signal) const {
    if (format_ != DaemonLogFormat::json || suppress_quiet_lifecycle()) {
        return;
    }
    std::string line;
    line.reserve(192U);
    write_json_prefix(line, "stopped");
    if (signal != 0) {
        line += ",\"signal\":";
        line += std::to_string(signal);
    }
    line += "}\n";
    std::cerr << line;
}

void DaemonLog::emit_executor_failure(const std::string_view error_code,
                                      const std::string_view error_message) const {
    if (format_ != DaemonLogFormat::json) {
        return;
    }
    std::string line;
    line.reserve(256U);
    write_json_prefix(line, "executor_failure");
    line += ",\"error_code\":\"";
    line += escape_log_field(error_code);
    line += "\",\"error_message\":\"";
    line += escape_log_field(error_message);
    line += "\"}\n";
    std::cerr << line;
}

} // namespace glifistore::server
