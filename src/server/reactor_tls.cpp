#include "glifistore/server/reactor.hpp"

#include <chrono>
#include <new>
#include <string>
#include <utility>

namespace glifistore::server {

auto Reactor::advance_tls_handshake(const ConnectionToken token) -> Status try {
    auto* current = connection(token);
    if (current == nullptr || !current->tls_handshake_pending) {
        return {};
    }
    if (!current->tls) {
        auto started = tls_->begin_accept_socket(current->socket.descriptor());
        if (!started) {
            close_connection(token);
            return {};
        }
        current->tls = std::move(*started);
    }
    auto advanced = current->tls->accept_handshake();
    if (!advanced || advanced->kind == TlsIoKind::closed) {
        if (security_audit_) {
            if (tls_->mtls_enabled()) {
                security_audit_->auth_deny("tls_handshake_failed");
            } else {
                security_audit_->tls_error("tls_handshake_failed");
            }
        }
        close_connection(token);
        return {};
    }
    if (tls_io_blocked(advanced->kind)) {
        const auto interest = tls_io_needs_read(advanced->kind) ? IoInterest::read : IoInterest::write;
        if (auto changed = poller_.modify(current->socket.descriptor(), token.encode(), interest); !changed) {
            close_connection(token);
        }
        return {};
    }
    current->tls_handshake_pending = false;
    current->tls_handshake_since = {};
    if (tls_->mtls_enabled()) {
        current->principal = std::string{current->tls->peer_principal()};
        if (current->principal.empty()) {
            close_connection(token);
            return {};
        }
        if (security_audit_) {
            security_audit_->auth_accept(current->principal);
        }
    }
    if (config_.authz.enabled()) {
        auto grant = config_.authz.grant_for(current->principal);
        current->capabilities = grant.capabilities;
        current->key_prefix = std::move(grant.key_prefix);
    }
    touch_activity(*current, std::chrono::steady_clock::now());
    if (auto changed = poller_.modify(current->socket.descriptor(), token.encode(), IoInterest::read);
        !changed) {
        close_connection(token);
        return {};
    }
    // Finished and INIT can share one TLS flight. Drain now: an edge-triggered
    // backend may not receive another readable notification for decrypted bytes.
    if (auto drained = read_ready(token); !drained) {
        close_connection(token);
    }
    return {};
} catch (const std::bad_alloc&) {
    close_connection(token);
    return {};
}

} // namespace glifistore::server
