#include "glifistore/server/security_audit.hpp"
#include "integration/server_reactor_test_support.hpp"
#include "test.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

using namespace glifistore::test::server_reactor_support;
using glifistore::server::SecurityAudit;

namespace {

// Each CTest scenario has a fresh process: the production emitter snapshots fd 2
// exactly once. Never replace a C++ streambuf across a background write.
class AuditPipe final {
  public:
    explicit AuditPipe(const bool fill) {
        int descriptors[2]{};
        GLIFI_REQUIRE(::pipe(descriptors) == 0);
        read_ = descriptors[0];
        write_ = descriptors[1];
        flags_ = ::fcntl(write_, F_GETFL);
        GLIFI_REQUIRE(flags_ >= 0);
        GLIFI_REQUIRE(::fcntl(read_, F_SETFL, O_NONBLOCK) == 0);
        if (fill) {
            GLIFI_REQUIRE(::fcntl(write_, F_SETFL, flags_ | O_NONBLOCK) == 0);
            const std::array<char, 512> bytes{};
            while (::write(write_, bytes.data(), bytes.size()) > 0) {
            }
            GLIFI_REQUIRE(errno == EAGAIN || errno == EWOULDBLOCK);
            GLIFI_REQUIRE(::fcntl(write_, F_SETFL, flags_) == 0);
        }
        saved_ = ::dup(STDERR_FILENO);
        GLIFI_REQUIRE(saved_ >= 0);
        GLIFI_REQUIRE(::dup2(write_, STDERR_FILENO) == STDERR_FILENO);
    }

    ~AuditPipe() {
        restore();
        if (read_ >= 0) {
            static_cast<void>(::close(read_));
        }
        static_cast<void>(::close(write_));
    }

    void restore() {
        if (saved_ >= 0) {
            static_cast<void>(::dup2(saved_, STDERR_FILENO));
            static_cast<void>(::close(saved_));
            saved_ = -1;
        }
    }

    void close_reader() {
        GLIFI_REQUIRE(::close(read_) == 0);
        read_ = -1;
    }

    void make_nonblocking() {
        flags_ |= O_NONBLOCK;
        GLIFI_REQUIRE(::fcntl(write_, F_SETFL, flags_) == 0);
    }

    void check_flags() const {
        // Darwin adds its internal FWASWRITTEN bit after an ordinary write.
        constexpr int kFlags = O_ACCMODE | O_NONBLOCK | O_APPEND | O_ASYNC | O_SYNC;
        GLIFI_REQUIRE((::fcntl(write_, F_GETFL) & kFlags) == (flags_ & kFlags));
        struct sigaction action{};
        GLIFI_REQUIRE(::sigaction(SIGPIPE, nullptr, &action) == 0);
        GLIFI_REQUIRE(action.sa_handler == SIG_DFL);
    }

    auto drain() const -> std::string {
        std::string text;
        std::array<char, 4096> bytes{};
        for (;;) {
            const auto count = ::read(read_, bytes.data(), bytes.size());
            if (count > 0) {
                text.append(bytes.data(), static_cast<std::size_t>(count));
            } else if (count < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
        return text;
    }

  private:
    int read_{-1};
    int write_{-1};
    int saved_{-1};
    int flags_{};
};

auto settle(SecurityAudit& audit, const AuditPipe* pipe) -> std::string {
    std::string text;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (audit.stats().events_pending != 0 && std::chrono::steady_clock::now() < deadline) {
        if (pipe != nullptr) {
            text += pipe->drain();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    GLIFI_REQUIRE(audit.stats().events_pending == 0);
    if (pipe != nullptr) {
        text += pipe->drain();
    }
    return text;
}

void normal_output() {
    AuditPipe pipe{false};
    SecurityAudit audit{true, false};
    pipe.restore();
    // Best-effort try_lock contention can suppress even a solitary event. Retry
    // format probes rather than requiring one persistent line per decision.
    const auto capture_event = [&](auto emit) {
        for (int attempt = 0; attempt < 10; ++attempt) {
            emit();
            auto text = settle(audit, &pipe);
            if (!text.empty()) {
                return text;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return std::string{};
    };
    std::string output;
    output += capture_event([&] { audit.auth_accept("reader.example"); });
    output += capture_event([&] { audit.auth_deny("no_peer_cert", "bad.example"); });
    output += capture_event([&] { audit.authz_deny("reader.example", "put", "capability_denied"); });
    output += capture_event([&] { audit.tls_error("TLS handshake failed"); });
    GLIFI_REQUIRE(output.find("\"event\":\"auth\"") != std::string::npos);
    GLIFI_REQUIRE(output.find("\"decision\":\"accept\"") != std::string::npos);
    GLIFI_REQUIRE(output.find("\"decision\":\"deny\"") != std::string::npos);
    GLIFI_REQUIRE(output.find("\"principal\":\"reader.example\"") != std::string::npos);
    GLIFI_REQUIRE(output.find("\"event\":\"authz\"") != std::string::npos);
    GLIFI_REQUIRE(output.find("\"opcode\":\"put\"") != std::string::npos);
    GLIFI_REQUIRE(output.find("\"reason\":\"capability_denied\"") != std::string::npos);
    GLIFI_REQUIRE(output.find("\"event\":\"tls\"") != std::string::npos);
    GLIFI_REQUIRE(output.find("PRIVATE KEY") == std::string::npos);
    GLIFI_REQUIRE(output.find("BEGIN CERTIFICATE") == std::string::npos);
    const auto escaped = capture_event([&] { audit.auth_deny(std::string(512, '\x01')); });
    GLIFI_REQUIRE(escaped.find("\\u0001") != std::string::npos);
    GLIFI_REQUIRE(escaped.find("...") != std::string::npos);
    GLIFI_REQUIRE(escaped.size() < 8U * 1024U);
    SecurityAudit quiet{true, true};
    quiet.auth_accept("hidden.example");
    std::string quiet_output;
    for (int attempt = 0; attempt < 10 && quiet_output.empty(); ++attempt) {
        quiet.auth_deny("no_peer_cert");
        quiet_output += settle(quiet, &pipe);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    GLIFI_REQUIRE(quiet_output.find("accept") == std::string::npos);
    GLIFI_REQUIRE(quiet_output.find("deny") != std::string::npos);
    constexpr std::uint64_t kAttempts = 4096;
    for (std::uint64_t index = 0; index < kAttempts; ++index) {
        audit.authz_deny("reader.example", "put", "capability_denied");
    }
    static_cast<void>(settle(audit, &pipe));
    const auto stats = audit.stats();
    GLIFI_REQUIRE(stats.authz_denies >= kAttempts + 1U);
    GLIFI_REQUIRE(stats.events_suppressed > 0);
    GLIFI_REQUIRE(stats.events_written + stats.events_suppressed ==
                  stats.auth_accepts + stats.auth_denies + stats.authz_denies + stats.tls_errors);
    pipe.check_flags();
}

void closed_output() {
    AuditPipe pipe{false};
    pipe.close_reader();
    SecurityAudit audit{true, false};
    pipe.restore();
    for (int attempt = 0; attempt < 10 && audit.stats().events_write_failed == 0; ++attempt) {
        audit.auth_deny("closed_sink");
        static_cast<void>(settle(audit, nullptr));
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    GLIFI_REQUIRE(audit.stats().events_write_failed > 0);
    GLIFI_REQUIRE(audit.stats().events_suppressed == audit.stats().auth_denies);
    GLIFI_REQUIRE(audit.stats().events_written == 0);
    pipe.check_flags();
}

void nonblocking_output() {
    AuditPipe pipe{true};
    pipe.make_nonblocking();
    SecurityAudit audit{true, false};
    pipe.restore();
    // All three capped fields expand sixfold, exceeding portable PIPE_BUF.
    const std::string field(256, '\x01');
    for (int attempt = 0; attempt < 10 && audit.stats().events_pending == 0; ++attempt) {
        audit.authz_deny(field, field, field);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    GLIFI_REQUIRE(audit.stats().events_pending == 1);
    GLIFI_REQUIRE(audit.stats().events_write_failed == 0);
    const auto output = settle(audit, &pipe);
    const auto json = output.find('{');
    GLIFI_REQUIRE(json != std::string::npos);
    GLIFI_REQUIRE(output.find('{', json + 1U) == std::string::npos);
    GLIFI_REQUIRE(output.ends_with("}\n"));
    GLIFI_REQUIRE(std::count(output.begin(), output.end(), '\n') == 1);
    GLIFI_REQUIRE(audit.stats().events_written == 1);
    GLIFI_REQUIRE(audit.stats().events_suppressed + audit.stats().events_written ==
                  audit.stats().authz_denies);
    GLIFI_REQUIRE(audit.stats().events_write_failed == 0);
    pipe.check_flags();
}

void blocked_output() {
    AuditPipe pipe{true};
    SecurityAudit audit{true, false};
    pipe.restore();
    const auto started = std::chrono::steady_clock::now();
    for (int attempt = 0; attempt < 10 && audit.stats().events_pending == 0; ++attempt) {
        audit.authz_deny("reader.example", "put", "blocked_sink");
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    const auto initial_attempts = audit.stats().authz_denies;
    GLIFI_REQUIRE(std::chrono::steady_clock::now() - started < std::chrono::seconds{2});
    GLIFI_REQUIRE(initial_attempts > 0);
    GLIFI_REQUIRE(audit.stats().events_pending > 0);
    GLIFI_REQUIRE(audit.stats().events_pending <= 129);
    pipe.check_flags();

    glifistore::server::AuthzPolicy policy;
    policy.set_enabled(true);
    auto opened = glifistore::server::Server::create({
        .port = 0,
        .worker_count = 1,
        .shutdown_drain_ms = 100,
        .authz = policy,
        .security_audit_events = true,
    });
    GLIFI_REQUIRE(opened.has_value());
    auto server = std::move(*opened);
    GLIFI_REQUIRE(server->start().has_value());
    const auto socket = connect_to(server->port());
    GLIFI_REQUIRE(socket >= 0);
    const auto frame = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 1,
        .key = bytes("denied"),
    });
    GLIFI_REQUIRE(frame.has_value());
    GLIFI_REQUIRE(send_all(socket, *frame));
    const auto response = glifistore::server::decode_response(receive_response(socket));
    GLIFI_REQUIRE(response.has_value());
    GLIFI_REQUIRE(response->frame.status == glifistore::server::ResponseStatus::permission_denied);
    const auto probe = connect_to(server->port());
    GLIFI_REQUIRE(probe >= 0);
    const auto health = probe_lifecycle(probe, glifistore::server::RequestOpcode::health, 2);
    GLIFI_REQUIRE(health.has_value());
    GLIFI_REQUIRE(health->decoded.frame.status == glifistore::server::ResponseStatus::ok);
    const auto stats = server->stats_report();
    GLIFI_REQUIRE(stats.has_value());
    GLIFI_REQUIRE(stats->find("authz_denies=1\n") != std::string::npos);
    static_cast<void>(::close(socket));
    static_cast<void>(::close(probe));
    const auto stopped = std::chrono::steady_clock::now();
    server->request_stop();
    GLIFI_REQUIRE(server->join().has_value());
    server.reset(); // Destruction as well as join must remain bounded.
    GLIFI_REQUIRE(std::chrono::steady_clock::now() - stopped < std::chrono::seconds{2});
    // Destroy more producers while the worker is blocked, then release it. This
    // exercises ownership of queued counter state after its Audit has disappeared.
    bool retained_counter = false;
    for (int index = 0; index < 128; ++index) {
        SecurityAudit temporary{true, false};
        temporary.tls_error("temporary");
        retained_counter = retained_counter || temporary.stats().events_pending != 0;
    }
    GLIFI_REQUIRE(retained_counter);
    for (int index = 0; index < 4096; ++index) {
        audit.authz_deny("reader.example", "put", "blocked_sink");
    }
    GLIFI_REQUIRE(audit.stats().authz_denies == initial_attempts + 4096U);
    GLIFI_REQUIRE(audit.stats().events_suppressed > 0);
    static_cast<void>(settle(audit, &pipe));
    const auto final = audit.stats();
    GLIFI_REQUIRE(final.events_written + final.events_suppressed == final.authz_denies);
    pipe.check_flags();
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_DFL);
    try {
        GLIFI_REQUIRE(argc == 2);
        const std::string_view scenario{argv[1]};
        if (scenario == "normal") {
            normal_output();
        } else if (scenario == "closed") {
            closed_output();
        } else if (scenario == "nonblocking") {
            nonblocking_output();
        } else {
            GLIFI_REQUIRE(scenario == "blocked");
            blocked_output();
        }
        std::cout << "audit sink " << scenario << " passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
