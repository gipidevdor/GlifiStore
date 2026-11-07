/**
 * @file
 * @brief Structured daemon lifecycle logging and readiness-loss reasons.
 */
#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/server/server.hpp"
#include "glifistore/store/maintenance_types.hpp"

#include <cstdint>
#include <iosfwd>
#include <string_view>

namespace glifistore::server {

/**
 * @brief Human or structured JSON lifecycle output.
 */
enum class DaemonLogFormat : std::uint8_t {
    human,
    json,
};

[[nodiscard]] auto daemon_log_format_name(DaemonLogFormat format) noexcept -> std::string_view;

[[nodiscard]] auto parse_daemon_log_format(std::string_view text) -> Result<DaemonLogFormat>;

[[nodiscard]] auto daemon_error_code_name(ErrorCode code) noexcept -> std::string_view;

/**
 * @brief Programmatic reason for readiness loss in lifecycle reports.
 */
enum class ReadyLossReason : std::uint8_t {
    none,
    not_live,
    shutting_down,
    store_not_operational,
    pair_fail_closed,
    admission_fenced,
    maintenance_emergency,
    maintenance_fault,
};

[[nodiscard]] auto ready_loss_reason_name(ReadyLossReason reason) noexcept -> std::string_view;

[[nodiscard]] auto classify_ready_loss(const Server& server) noexcept -> ReadyLossReason;

/// Fail-closed structured daemon lifecycle logging. Human format is a no-op here;
/// daemon_main keeps legacy stdout/stderr lines for human mode.
class DaemonLog final {
  public:
    DaemonLog(DaemonLogFormat format, std::string_view program, bool quiet) noexcept;

    [[nodiscard]] auto structured() const noexcept -> bool {
        return format_ == DaemonLogFormat::json;
    }

    void emit_start() const;
    void emit_listen(std::string_view bind_address, std::uint16_t cleartext_port, std::uint16_t tls_port,
                     std::size_t executors, std::string_view storage_mode,
                     std::string_view unix_socket_path = {}) const;
    void emit_ready(bool ready, ReadyLossReason reason = ReadyLossReason::none) const;
    void emit_maintenance_emergency(MaintenancePressureLevel pressure) const;
    void emit_maintenance_fault(MaintenanceState state, std::string_view error_code,
                                std::string_view error_message) const;
    void emit_shutdown_begin(int signal, bool executor_failure) const;
    void emit_shutdown_drain_begin(std::uint32_t drain_ms) const;
    void emit_shutdown_drain_end(bool timed_out, bool join_failed) const;
    void emit_stopped(int signal) const;
    void emit_executor_failure(std::string_view error_code, std::string_view error_message) const;

  private:
    [[nodiscard]] auto suppress_quiet_lifecycle() const noexcept -> bool;
    void write_json_prefix(std::string& line, std::string_view event) const;

    DaemonLogFormat format_{DaemonLogFormat::human};
    std::string_view program_{};
    bool quiet_{};
};

} // namespace glifistore::server
