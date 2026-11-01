#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>

namespace glifistore::server {

struct SecurityAuditSink;
struct SecurityAuditOutputCounters;

// Phase 6 security audit trail: authn/authz decisions and TLS handshake failures.
// Emits JSON-lines on stderr (no key/value payloads, no PEM bytes). Counters feed STATS.
struct SecurityAuditStats final {
    std::uint64_t auth_accepts{};
    std::uint64_t auth_denies{};
    std::uint64_t authz_denies{};
    std::uint64_t tls_errors{};
    std::uint64_t events_suppressed{};
    std::uint64_t events_written{};
    std::uint64_t events_pending{};
    std::uint64_t events_write_failed{};
};

class SecurityAudit final {
  public:
    // emit_events: write JSON-lines (typically --log-format json or --secure-profile).
    // quiet: suppress auth_accept only. A process-wide bounded emitter snapshots
    // stderr on first use. Producers and destruction never wait for sink I/O.
    // Accepted events are best effort; pending events may be lost at process exit.
    SecurityAudit(bool emit_events, bool quiet);

    void auth_accept(std::string_view principal) noexcept;
    void auth_deny(std::string_view reason, std::string_view principal = {}) noexcept;
    void authz_deny(std::string_view principal, std::string_view opcode, std::string_view reason) noexcept;
    void tls_error(std::string_view reason) noexcept;

    [[nodiscard]] auto stats() const noexcept -> SecurityAuditStats;
    [[nodiscard]] auto emit_events() const noexcept -> bool {
        return emit_events_;
    }

  private:
    void write_event(std::string_view event, std::string_view principal, std::string_view opcode,
                     std::string_view reason, std::string_view decision) noexcept;

    bool emit_events_{};
    bool quiet_{};
    std::shared_ptr<SecurityAuditOutputCounters> output_;
    std::shared_ptr<SecurityAuditSink> sink_;
    std::atomic<std::uint64_t> auth_accepts_{};
    std::atomic<std::uint64_t> auth_denies_{};
    std::atomic<std::uint64_t> authz_denies_{};
    std::atomic<std::uint64_t> tls_errors_{};
};

} // namespace glifistore::server
