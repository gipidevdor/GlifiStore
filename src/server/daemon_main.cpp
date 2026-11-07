#include "cli/arguments.hpp"
#include "glifistore/core/worker_routing.hpp"
#include "glifistore/index/index_hash_seed.hpp"
#include "glifistore/server/crash_test_hooks.hpp"
#include "glifistore/server/daemon_config.hpp"
#include "glifistore/server/daemon_log.hpp"
#include "glifistore/server/openbsd_sandbox.hpp"
#include "glifistore/server/server.hpp"
#include "glifistore/server/tls.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace {

volatile std::sig_atomic_t g_stop_signal = 0;

extern "C" void request_stop(const int signal) {
    g_stop_signal = signal;
}

[[nodiscard]] auto install_signal_handler(const int signal, const char* name) -> glifistore::Status {
    struct sigaction action{};
    action.sa_handler = request_stop;
    if (sigemptyset(&action.sa_mask) != 0 || ::sigaction(signal, &action, nullptr) != 0) {
        const auto error_number = errno;
        return glifistore::fail(glifistore::ErrorCode::io_error,
                                std::string{"cannot install "} + name + " handler: " +
                                    std::error_code{error_number, std::system_category()}.message());
    }
    return {};
}

void print_help(const std::string_view program) {
    glifistore::cli::write_help(std::cout, program, "GlifiStore native binary key-value server.", "[OPTIONS]",
                                glifistore::server::daemon_option_specs());
}

void emit_human_listen(const std::string_view program, const glifistore::server::DaemonOptions& arguments,
                       const glifistore::server::Server& server) {
    std::cout << program << ": listening address=" << arguments.server.bind_address
              << " executors=" << server.executor_count()
              << " storage=" << glifistore::server::storage_mode_name(arguments.store.storage_mode);
    if (server.cleartext_port() != 0 && server.tls_port() != 0) {
        std::cout << " cleartext_port=" << server.cleartext_port() << " tls_port=" << server.tls_port()
                  << " transport=cleartext+tls1.3 backend=" << glifistore::server::tls_backend_name();
        if (arguments.server.tls.mtls_enabled()) {
            std::cout << " auth=mtls";
        } else {
            std::cout << " auth=none";
        }
    } else if (server.tls_port() != 0) {
        std::cout << " port=" << server.tls_port()
                  << " transport=tls1.3 backend=" << glifistore::server::tls_backend_name();
        if (arguments.server.tls.mtls_enabled()) {
            std::cout << " auth=mtls";
        } else {
            std::cout << " auth=none";
        }
    } else {
        std::cout << " port=" << server.port() << " (cleartext; no authentication)";
    }
    if (const auto unix_path = server.unix_socket_path(); !unix_path.empty()) {
        std::cout << " unix_socket=" << unix_path;
        if (arguments.server.unix_peercred) {
            std::cout << " auth_unix=peercred";
        }
    }
    // The listen line is also the machine-readable readiness handoff used by
    // launchers when stdout is redirected to a file. Flush it explicitly: BSD
    // stdio is fully buffered in that mode, so a bare newline is not sufficient.
    std::cout << '\n' << std::flush;
}

void observe_lifecycle(const glifistore::server::Server& server, glifistore::server::DaemonLog& log,
                       bool& was_ready, bool& was_emergency, bool& was_fault) {
    if (!log.structured()) {
        return;
    }
    const auto snapshot = server.maintenance_snapshot();
    const bool is_ready = server.ready();
    if (is_ready && !was_ready) {
        log.emit_ready(true);
    } else if (!is_ready && was_ready) {
        log.emit_ready(false, glifistore::server::classify_ready_loss(server));
    }
    was_ready = is_ready;

    if (snapshot.mutations_rejected && !was_emergency) {
        log.emit_maintenance_emergency(snapshot.pressure);
        was_emergency = true;
    } else if (!snapshot.mutations_rejected) {
        was_emergency = false;
    }

    const bool faulted =
        snapshot.state == glifistore::MaintenanceState::faulted && snapshot.last_error.has_value();
    if (faulted && !was_fault) {
        log.emit_maintenance_fault(
            snapshot.state,
            std::string{glifistore::server::daemon_error_code_name(snapshot.last_error->code)},
            snapshot.last_error->message);
        was_fault = true;
    } else if (!faulted) {
        was_fault = false;
    }
}

} // namespace

int main(const int argc, char** argv) try {
    const auto program = glifistore::cli::executable_name(argc > 0 ? argv[0] : "glifistored");
    auto arguments = glifistore::server::parse_daemon_options(argc, argv);
    if (!arguments) {
        std::cerr << program << ": error: " << arguments.error().message << "\nTry '" << program
                  << " --help' for more information.\n";
        return 2;
    }
    if (arguments->show_help) {
        print_help(program);
        return 0;
    }
    if (arguments->show_version) {
        std::cout << program << ' ' << GLIFISTORE_VERSION << '\n';
        return 0;
    }
    if (arguments->show_dump_config) {
        std::cout << glifistore::server::format_daemon_config_dump(*arguments);
        return 0;
    }

    glifistore::server::DaemonLog log{arguments->log_format, program, arguments->quiet};
    log.emit_start();

    // Writing to a closed peer must surface as EPIPE, not process death (TLS/handshake
    // denial paths included). BSD raises SIGPIPE by default unless ignored.
    if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        std::cerr << program << ": error: failed to ignore SIGPIPE\n";
        return 2;
    }

    // ADR 0026: apply Index mix seed before any Store/Index construction.
    glifistore::set_index_hash_seed(arguments->index_hash_seed);
    glifistore::set_worker_routing(arguments->worker_routing);

    if (auto crash_hooks = glifistore::server::maybe_install_crash_test_hooks(arguments->store);
        !crash_hooks) {
        std::cerr << program << ": error: " << crash_hooks.error().message << '\n';
        return 2;
    }

    auto server = glifistore::server::Server::create(arguments->server, arguments->store);
    if (!server) {
        std::cerr << program << ": error: " << server.error().message << '\n';
        return 1;
    }
    // OpenBSD: unveil data/TLS/authz paths then pledge. Fail closed (ADR 0020 /
    // security roadmap Phase 6.5). No-op on Linux/macOS/FreeBSD.
    if (auto sandboxed = glifistore::server::apply_openbsd_sandbox(*arguments); !sandboxed) {
        std::cerr << program << ": error: " << sandboxed.error().message << '\n';
        return 1;
    }
    if (glifistore::server::openbsd_sandbox_supported() && !arguments->quiet) {
        std::cerr << program << ": openbsd-sandbox=pledge+unveil promises="
                  << glifistore::server::openbsd_sandbox_promises() << '\n';
    }
    if (auto installed = install_signal_handler(SIGINT, "SIGINT"); !installed) {
        std::cerr << program << ": error: " << installed.error().message << '\n';
        return 1;
    }
    if (auto installed = install_signal_handler(SIGTERM, "SIGTERM"); !installed) {
        std::cerr << program << ": error: " << installed.error().message << '\n';
        return 1;
    }
    if (auto started = (*server)->start(); !started) {
        std::cerr << program << ": error: " << started.error().message << '\n';
        return 1;
    }
    if (arguments->server.bind_address != "127.0.0.1" && !arguments->secure_profile &&
        ((*server)->cleartext_port() != 0 || !arguments->server.tls.requested())) {
        std::cerr << program << ": warning: bind address " << arguments->server.bind_address
                  << " exposes cleartext TCP with no authentication; restrict to a trusted network "
                     "or use --secure-profile (TLS-only + mTLS + --authz-map; OpenBSD uses LibreSSL; "
                     "docs/security/roadmap.md)\n";
    }
    if (log.structured()) {
        log.emit_listen(arguments->server.bind_address, (*server)->cleartext_port(), (*server)->tls_port(),
                        (*server)->executor_count(),
                        glifistore::server::storage_mode_name(arguments->store.storage_mode),
                        (*server)->unix_socket_path());
    } else if (!arguments->quiet) {
        emit_human_listen(program, *arguments, **server);
    }

    bool was_ready = (*server)->ready();
    bool was_emergency = (*server)->maintenance_snapshot().mutations_rejected;
    bool was_fault = (*server)->maintenance_snapshot().state == glifistore::MaintenanceState::faulted &&
                     (*server)->maintenance_snapshot().last_error.has_value();
    if (log.structured() && was_ready) {
        log.emit_ready(true);
    }

    while (g_stop_signal == 0 && (*server)->live()) {
        observe_lifecycle(**server, log, was_ready, was_emergency, was_fault);
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }

    // Sticky pair fail-closed keeps live() true; only executor `failed_` ends the loop.
    const bool executor_failure = !(*server)->live() && g_stop_signal == 0;
    if (executor_failure) {
        if (const auto failure = (*server)->first_failure(); failure.has_value()) {
            if (log.structured()) {
                log.emit_executor_failure(glifistore::server::daemon_error_code_name(failure->code),
                                          failure->message);
            }
        }
    }

    log.emit_shutdown_begin(static_cast<int>(g_stop_signal), executor_failure);
    (*server)->request_stop();
    log.emit_shutdown_drain_begin(arguments->server.shutdown_drain_ms);
    if (auto stopped = (*server)->join(); !stopped) {
        log.emit_shutdown_drain_end((*server)->shutdown_drain_timed_out(), true);
        std::cerr << program << ": error: reactor failure: " << stopped.error().message << '\n';
        return 1;
    }
    log.emit_shutdown_drain_end((*server)->shutdown_drain_timed_out(), false);
    if (log.structured()) {
        log.emit_stopped(static_cast<int>(g_stop_signal));
    } else if (!arguments->quiet) {
        std::cout << program << ": stopped";
        if (g_stop_signal != 0) {
            std::cout << " signal=" << g_stop_signal;
        }
        std::cout << '\n';
    }
    return 0;
} catch (const std::exception& exception) {

    const auto program = glifistore::cli::executable_name(argc > 0 ? argv[0] : "glifistored");
    std::cerr << program << ": fatal: " << exception.what() << '\n';
    return 1;
} catch (...) {
    const auto program = glifistore::cli::executable_name(argc > 0 ? argv[0] : "glifistored");
    std::cerr << program << ": fatal: unknown non-standard exception\n";
    return 1;
}
