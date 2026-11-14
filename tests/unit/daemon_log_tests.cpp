#include "glifistore/server/daemon_log.hpp"
#include "test.hpp"

#include <sstream>
#include <string>

namespace {

class CapturedStderr final {
  public:
    CapturedStderr() {
        old_ = std::cerr.rdbuf(capture_.rdbuf());
    }

    ~CapturedStderr() {
        std::cerr.rdbuf(old_);
    }

    [[nodiscard]] auto text() const -> std::string {
        return capture_.str();
    }

  private:
    std::stringstream capture_{};
    std::streambuf* old_{nullptr};
};

} // namespace

GLIFI_TEST("daemon log format parsing rejects unknown values") {
    const auto parsed = glifistore::server::parse_daemon_log_format("yaml");
    GLIFI_REQUIRE(!parsed.has_value());
    GLIFI_REQUIRE(parsed.error().message.find("human or json") != std::string::npos);
}

GLIFI_TEST("daemon log format parsing accepts human and json") {
    const auto human = glifistore::server::parse_daemon_log_format("human");
    GLIFI_REQUIRE(human.has_value());
    GLIFI_REQUIRE(*human == glifistore::server::DaemonLogFormat::human);

    const auto json = glifistore::server::parse_daemon_log_format("JSON");
    GLIFI_REQUIRE(json.has_value());
    GLIFI_REQUIRE(*json == glifistore::server::DaemonLogFormat::json);
}

GLIFI_TEST("daemon log json emits bounded lifecycle events") {
    CapturedStderr capture;
    const glifistore::server::DaemonLog log{glifistore::server::DaemonLogFormat::json, "glifistored", false};
    log.emit_start();
    log.emit_listen("127.0.0.1", 7379, 0, 2, "volatile");
    log.emit_ready(true);
    log.emit_ready(false, glifistore::server::ReadyLossReason::shutting_down);
    log.emit_maintenance_emergency(glifistore::MaintenancePressureLevel::emergency);
    log.emit_maintenance_fault(glifistore::MaintenanceState::faulted, "internal_error",
                               "maintenance scheduler failed");
    log.emit_shutdown_begin(15, false);
    log.emit_shutdown_drain_begin(30000);
    log.emit_shutdown_drain_end(false, false);
    log.emit_stopped(15);

    const auto output = capture.text();
    GLIFI_REQUIRE(output.find("\"event\":\"start\"") != std::string::npos);
    GLIFI_REQUIRE(output.find("\"event\":\"listen\"") != std::string::npos);
    GLIFI_REQUIRE(output.find("\"bind\":\"127.0.0.1\"") != std::string::npos);
    GLIFI_REQUIRE(output.find("\"cleartext_port\":7379") != std::string::npos);
    GLIFI_REQUIRE(output.find("\"event\":\"ready\"") != std::string::npos);
    GLIFI_REQUIRE(output.find("\"reason\":\"shutting_down\"") != std::string::npos);
    GLIFI_REQUIRE(output.find("\"event\":\"maintenance_emergency\"") != std::string::npos);
    GLIFI_REQUIRE(output.find("\"maintenance_pressure\":\"emergency\"") != std::string::npos);
    GLIFI_REQUIRE(output.find("\"event\":\"maintenance_fault\"") != std::string::npos);
    GLIFI_REQUIRE(output.find("\"event\":\"shutdown_drain_begin\"") != std::string::npos);
    GLIFI_REQUIRE(output.find("\"drain_ms\":30000") != std::string::npos);
    GLIFI_REQUIRE(output.find("\"event\":\"stopped\"") != std::string::npos);
    GLIFI_REQUIRE(output.find("tls-cert") == std::string::npos);
}

GLIFI_TEST("daemon log quiet suppresses start listen and stopped json events") {
    CapturedStderr capture;
    const glifistore::server::DaemonLog log{glifistore::server::DaemonLogFormat::json, "glifistored", true};
    log.emit_start();
    log.emit_listen("127.0.0.1", 7379, 0, 1, "volatile");
    log.emit_ready(false, glifistore::server::ReadyLossReason::maintenance_emergency);
    log.emit_stopped(0);

    const auto output = capture.text();
    GLIFI_REQUIRE(output.find("\"event\":\"start\"") == std::string::npos);
    GLIFI_REQUIRE(output.find("\"event\":\"listen\"") == std::string::npos);
    GLIFI_REQUIRE(output.find("\"event\":\"stopped\"") == std::string::npos);
    GLIFI_REQUIRE(output.find("\"event\":\"ready\"") != std::string::npos);
}

GLIFI_TEST("daemon log human format emits no structured lines") {
    CapturedStderr capture;
    const glifistore::server::DaemonLog log{glifistore::server::DaemonLogFormat::human, "glifistored", false};
    log.emit_listen("127.0.0.1", 7379, 0, 1, "volatile");
    GLIFI_REQUIRE(capture.text().empty());
}

GLIFI_TEST("daemon log json escapes control characters and truncates long fields") {
    CapturedStderr capture;
    const glifistore::server::DaemonLog log{glifistore::server::DaemonLogFormat::json, "glifistored", false};
    std::string message(300U, 'x');
    message[10] = '\n';
    log.emit_executor_failure("io_error", message);

    const auto output = capture.text();
    GLIFI_REQUIRE(output.find("\\n") != std::string::npos);
    GLIFI_REQUIRE(output.find("...") != std::string::npos);
}

GLIFI_TEST("daemon ready loss classification names are stable") {
    GLIFI_REQUIRE(glifistore::server::ready_loss_reason_name(
                      glifistore::server::ReadyLossReason::maintenance_emergency) == "maintenance_emergency");
    GLIFI_REQUIRE(glifistore::server::ready_loss_reason_name(
                      glifistore::server::ReadyLossReason::maintenance_fault) == "maintenance_fault");
    GLIFI_REQUIRE(glifistore::server::ready_loss_reason_name(
                      glifistore::server::ReadyLossReason::admission_fenced) == "admission_fenced");
    GLIFI_REQUIRE(glifistore::server::ready_loss_reason_name(
                      glifistore::server::ReadyLossReason::pair_fail_closed) == "pair_fail_closed");
}
