#include "server/reactor_factory.hpp"

#include "glifistore/server/socket.hpp"

#include <utility>

namespace glifistore::server {

auto ReactorFactory::create_all(const ReactorConfig& config, Store& store, ConnectionHandoffMesh& mesh,
                                DiskReadExecutor& disk_reads, PairWriterPool& pair_writers,
                                const ServerLifecycleProbes lifecycle_probes,
                                const std::shared_ptr<TlsContext>& tls_context,
                                std::shared_ptr<AbuseController> abuse,
                                std::shared_ptr<SecurityAudit> security_audit)
    -> Result<std::vector<std::unique_ptr<Reactor>>> {
#if defined(__linux__)
    const bool kernel_distribution = config.reuse_port && store.worker_count() > 1;
#else
    const bool kernel_distribution = false;
#endif
    const bool dual_listen = config.tls.requested() && config.tls_port.has_value();
    const bool tls_only = config.tls.requested() && !dual_listen;
    const bool listen_cleartext = !tls_only;
    const bool listen_tls = tls_only || dual_listen;
    const bool listen_unix = !config.unix_socket_path.empty();
    std::uint16_t shared_cleartext_port = config.port;
    std::uint16_t shared_tls_port = dual_listen ? *config.tls_port : config.port;

    if (!abuse && config.abuse.any_enabled()) {
        abuse = std::make_shared<AbuseController>(config.abuse);
    }
    if (!security_audit && (config.security_audit_events || config.authz.enabled() ||
                            (tls_context && tls_context->mtls_enabled()) || config.unix_peercred)) {
        security_audit = std::make_shared<SecurityAudit>(config.security_audit_events, config.quiet);
    }

    std::vector<std::unique_ptr<Reactor>> reactors;
    reactors.reserve(store.worker_count());
    for (std::size_t executor = 0; executor < store.worker_count(); ++executor) {
        TcpListener cleartext_listener;
        TcpListener tls_listener;
        UnixListener unix_listener;
        if (executor == 0 || kernel_distribution) {
            if (listen_cleartext) {
                auto bound =
                    TcpListener::bind(config.bind_address, shared_cleartext_port, 512, kernel_distribution);
                if (!bound) {
                    return unexpected(bound.error());
                }
                cleartext_listener = std::move(*bound);
                if (executor == 0) {
                    shared_cleartext_port = cleartext_listener.port();
                }
            }
            if (listen_tls) {
                auto bound =
                    TcpListener::bind(config.bind_address, shared_tls_port, 512, kernel_distribution);
                if (!bound) {
                    return unexpected(bound.error());
                }
                tls_listener = std::move(*bound);
                if (executor == 0) {
                    shared_tls_port = tls_listener.port();
                }
            }
        }
        // AF_UNIX has no portable SO_REUSEPORT fan-out; bind once on executor 0 and hand off.
        if (listen_unix && executor == 0) {
            auto bound = UnixListener::bind(config.unix_socket_path);
            if (!bound) {
                return unexpected(bound.error());
            }
            unix_listener = std::move(*bound);
        }
        auto reactor =
            Reactor::create(config, executor, std::move(cleartext_listener), std::move(tls_listener),
                            std::move(unix_listener), store, mesh, disk_reads, pair_writers, lifecycle_probes,
                            tls_context, abuse, security_audit);
        if (!reactor) {
            return unexpected(reactor.error());
        }
        reactors.push_back(std::move(*reactor));
    }
    return reactors;
}

} // namespace glifistore::server
