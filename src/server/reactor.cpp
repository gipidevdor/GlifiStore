#include "glifistore/server/reactor.hpp"

#include "glifistore/core/fault_injection.hpp"
#include "glifistore/core/hot_path_phases.hpp"
#include "glifistore/core/worker_routing.hpp"
#include "glifistore/server/connection_lifecycle.hpp"
#include "glifistore/server/peercred.hpp"
#include "server/reactor_detail.hpp"
#include "store/store_internal.hpp"
#include "system_error.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/uio.h>
#include <utility>
#include <vector>

namespace glifistore::server {

Reactor::Reactor(ReactorConfig config, const std::size_t executor_id, TcpListener cleartext_listener,
                 TcpListener tls_listener, UnixListener unix_listener, Poller poller, Wakeup wakeup,
                 Store& store, ConnectionHandoffMesh& mesh, DiskReadExecutor& disk_reads,
                 PairWriterPool& pair_writers, ServerLifecycleProbes lifecycle_probes,
                 std::shared_ptr<TlsContext> tls, std::shared_ptr<AbuseController> abuse,
                 std::shared_ptr<SecurityAudit> security_audit)
    : config_(std::move(config)), executor_id_(executor_id), listener_(std::move(cleartext_listener)),
      tls_listener_(std::move(tls_listener)), unix_listener_(std::move(unix_listener)),
      poller_(std::move(poller)), wakeup_(std::move(wakeup)), store_(store),
      worker_routing_(detail::StoreAccess::worker_routing(store)), mesh_(mesh), disk_reads_(disk_reads),
      pair_writers_(pair_writers), lifecycle_probes_(lifecycle_probes), tls_(std::move(tls)),
      abuse_(std::move(abuse)), security_audit_(std::move(security_audit)),
      disk_read_completions_(config_.disk_read_queue_capacity),
      mutation_completions_(config_.durable_mutation_queue_capacity),
      read_cancellation_epochs_(std::make_unique<std::atomic_uint64_t[]>(config_.maximum_connections)),
      connections_(config_.maximum_connections), events_(config_.event_batch_size) {
    durable_store_ = detail::StoreAccess::is_durable(store_);
    local_read_generation_ = pair_writers_.adopt_read_generation(executor_id_, minimum_cold_read_epoch());
    free_slots_.reserve(config_.maximum_connections);
    for (std::size_t slot = config_.maximum_connections; slot > 0; --slot) {
        free_slots_.push_back(static_cast<std::uint32_t>(slot - 1U));
    }
}

auto Reactor::create(const ReactorConfig& config, const std::size_t executor_id,
                     TcpListener cleartext_listener, TcpListener tls_listener, UnixListener unix_listener,
                     Store& store, ConnectionHandoffMesh& mesh, DiskReadExecutor& disk_reads,
                     PairWriterPool& pair_writers, ServerLifecycleProbes lifecycle_probes,
                     std::shared_ptr<TlsContext> tls, std::shared_ptr<AbuseController> abuse,
                     std::shared_ptr<SecurityAudit> security_audit) -> Result<std::unique_ptr<Reactor>> {
    if (executor_id >= mesh.size() || executor_id >= store.worker_count()) {
        return fail(ErrorCode::invalid_argument, "reactor executor id is outside the Worker mesh");
    }
    if (tls_listener.descriptor() >= 0 && !tls) {
        return fail(ErrorCode::invalid_argument, "TLS listener requires a TLS context");
    }
    auto poller = Poller::create();
    if (!poller) {
        return unexpected(poller.error());
    }
    auto wakeup = Wakeup::create();
    if (!wakeup) {
        return unexpected(wakeup.error());
    }
    if (!abuse && config.abuse.any_enabled()) {
        abuse = std::make_shared<AbuseController>(config.abuse);
    }
    if (!security_audit && (config.security_audit_events || config.authz.enabled() ||
                            (tls && tls->mtls_enabled()) || config.unix_peercred)) {
        security_audit = std::make_shared<SecurityAudit>(config.security_audit_events, config.quiet);
    }
    auto reactor = std::unique_ptr<Reactor>(new Reactor(
        config, executor_id, std::move(cleartext_listener), std::move(tls_listener), std::move(unix_listener),
        std::move(*poller), std::move(*wakeup), store, mesh, disk_reads, pair_writers, lifecycle_probes,
        std::move(tls), std::move(abuse), std::move(security_audit)));
    if (reactor->listener_.descriptor() >= 0) {
        if (auto added =
                reactor->poller_.add(reactor->listener_.descriptor(), kListenerToken, IoInterest::read);
            !added) {
            return unexpected(added.error());
        }
    }
    if (reactor->tls_listener_.descriptor() >= 0) {
        if (auto added = reactor->poller_.add(reactor->tls_listener_.descriptor(), kTlsListenerToken,
                                              IoInterest::read);
            !added) {
            return unexpected(added.error());
        }
    }
    if (reactor->unix_listener_.descriptor() >= 0) {
        if (auto added = reactor->poller_.add(reactor->unix_listener_.descriptor(), kUnixListenerToken,
                                              IoInterest::read);
            !added) {
            return unexpected(added.error());
        }
    }
    if (auto added = reactor->poller_.add(reactor->wakeup_.descriptor(), kMessageToken, IoInterest::read);
        !added) {
        return unexpected(added.error());
    }
    mesh.register_wakeup(executor_id, reactor->wakeup_);
    return reactor;
}

auto Reactor::connection(const ConnectionToken token) noexcept -> Connection* {
    if (token.slot >= connections_.size()) {
        return nullptr;
    }
    auto& candidate = connections_[token.slot];
    if (!candidate.socket.valid() || candidate.generation != token.generation) {
        return nullptr;
    }
    return &candidate;
}

auto Reactor::has_pending_output(const Connection& connection) noexcept -> bool {
    return connection.output_offset < connection.output.size() || connection.output_lease.has_value();
}

auto Reactor::pending_output_bytes(const Connection& connection) noexcept -> std::size_t {
    DecidedOutput decided{
        .contiguous_bytes = connection.output.size() - connection.output_offset,
    };
    if (connection.output_lease) {
        decided.lease_header_remaining =
            connection.output_lease->header.size() - connection.output_lease->header_offset;
        decided.lease_value_remaining =
            connection.output_lease->value.bytes.size() - connection.output_lease->value_offset;
    }
    return decided.total_bytes();
}

auto Reactor::acquire_cold_read_lease(const std::uint64_t epoch) noexcept -> bool {
    ReadLeaseEpoch* free{};
    for (auto& lease : cold_read_leases_) {
        if (lease.uses != 0 && lease.epoch == epoch) {
            if (lease.uses == std::numeric_limits<std::size_t>::max()) {
                return false;
            }
            ++lease.uses;
            return true;
        }
        if (lease.uses == 0 && free == nullptr) {
            free = &lease;
        }
    }
    if (free == nullptr) {
        return false;
    }
    free->epoch = epoch;
    free->uses = 1;
    return true;
}

auto Reactor::release_cold_read_lease(const std::uint64_t epoch) noexcept -> bool {
    for (auto& lease : cold_read_leases_) {
        if (lease.uses == 0 || lease.epoch != epoch) {
            continue;
        }
        --lease.uses;
        if (lease.uses == 0) {
            lease.epoch = std::numeric_limits<std::uint64_t>::max();
        }
        return true;
    }
    return false;
}

auto Reactor::minimum_cold_read_epoch() const noexcept -> std::uint64_t {
    auto minimum = std::numeric_limits<std::uint64_t>::max();
    for (const auto& lease : cold_read_leases_) {
        if (lease.uses != 0) {
            minimum = std::min(minimum, lease.epoch);
        }
    }
    return minimum;
}

void Reactor::close_connection(const ConnectionToken token) noexcept {
    auto* current = connection(token);
    if (current == nullptr) {
        return;
    }
    bool poller_removed = false;
    try {
        poller_removed = poller_.remove(current->socket.descriptor()).has_value();
    } catch (...) {
        poller_removed = false;
    }
    static_cast<void>(poller_removed);
    if (current->cold_read_in_flight) {
        read_cancellation_epochs_[token.slot].fetch_add(1U, std::memory_order_release);
    }
    current->tls_handshake_pending = false;
    current->tls_handshake_since = {};
    current->tls.reset();
    current->socket.reset();
    current->principal.clear();
    current->capabilities = Capability::none;
    current->key_prefix.clear();
    current->input.clear();
    current->input_offset = 0;
    current->output.clear();
    current->output_offset = 0;
    current->output_lease.reset();
    current->bound_worker.reset();
    current->initialized = false;
    current->peer_read_closed = false;
    current->write_armed = false;
    current->pipelined_store_input_observed = false;
    current->request_in_flight = false;
    current->mutations_in_flight = 0;
    current->mutation_window_count = 0;
    current->mutation_visibility.clear();
    current->cold_read_in_flight = false;
    current->close_after_flush = false;
    current->last_activity = {};
    current->partial_request_since = {};
    current->in_flight_since = {};
    current->output_progress_since = {};
    current->connection_rate_window_start_ns = 0;
    current->connection_rate_used = 0;
    if (advance_connection_generation(current->generation)) {
        free_slots_.push_back(token.slot);
    }
    active_connections_.fetch_sub(1U, std::memory_order_relaxed);
}

void Reactor::stop_accepting() noexcept {
    shutting_down_ = true;
    tcp_accept_pending_ = false;
    tls_accept_pending_ = false;
    if (listener_.descriptor() >= 0) {
        static_cast<void>(poller_.remove(listener_.descriptor()));
        listener_ = TcpListener{};
    }
    if (tls_listener_.descriptor() >= 0) {
        static_cast<void>(poller_.remove(tls_listener_.descriptor()));
        tls_listener_ = TcpListener{};
    }
    if (unix_listener_.descriptor() >= 0) {
        static_cast<void>(poller_.remove(unix_listener_.descriptor()));
        unix_listener_ = UnixListener{};
    }
}

void Reactor::close_idle_connections() noexcept {
    for (std::uint32_t slot = 0; slot < connections_.size(); ++slot) {
        auto& candidate = connections_[slot];
        if (!candidate.socket.valid()) {
            continue;
        }
        if (candidate.request_in_flight || has_pending_output(candidate) ||
            candidate.input_offset < candidate.input.size()) {
            continue;
        }
        close_connection(ConnectionToken{.slot = slot, .generation = candidate.generation});
    }
}

void Reactor::close_all_connections() noexcept {
    for (std::uint32_t slot = 0; slot < connections_.size(); ++slot) {
        auto& candidate = connections_[slot];
        if (!candidate.socket.valid()) {
            continue;
        }
        const ConnectionToken token{.slot = slot, .generation = candidate.generation};
        // Shutdown-deadline hard close: best-effort flush of any decided response still
        // buffered (same posture as in-flight request timeout). Join still reports drain
        // timeout; this only avoids discarding ACKs the kernel can accept right now.
        if (has_pending_output(candidate)) {
            static_cast<void>(write_ready(token));
            if (connection(token) == nullptr) {
                continue;
            }
        }
        close_connection(token);
    }
    reject_pending_handoffs();
}

void Reactor::reject_pending_handoffs() noexcept {
    while (auto handoff = mesh_.try_pop(executor_id_)) {
        // Accept-flood fillers without BIND OK may silently close; BIND cells must
        // surface OVERLOADED (same contract as adopt-slot exhaustion).
        if (handoff->bound_worker.has_value() || !handoff->output.empty()) {
            reject_orphaned_handoff(std::move(*handoff));
        }
    }
}

auto Reactor::accept_ready(const bool tls_endpoint) -> Status {
    auto& listener = tls_endpoint ? tls_listener_ : listener_;
    auto& pending = tls_endpoint ? tls_accept_pending_ : tcp_accept_pending_;
    pending = true;
    const auto now = std::chrono::steady_clock::now();
    for (std::size_t attempt = 0; attempt < config_.event_batch_size; ++attempt) {
        auto accepted = listener.accept();
        if (!accepted) {
            // EMFILE/ENFILE/configure_* failures must not stop the executor.
            // EAGAIN/EINTR are already empty optionals from TcpListener::accept.
            pending = false;
            return {};
        }
        auto& accepted_socket = accepted.value();
        if (!accepted_socket) {
            pending = false;
            return {};
        }
        if (abuse_ && !abuse_->try_admit_accept(now)) {
            // Drop the peer without adopting; SocketHandle closes on destroy.
            continue;
        }
        ConnectionHandoff handoff{.socket = std::move(accepted_socket).value(), .last_activity = now};
        if (tls_endpoint) {
            // Reserve from the same bounded connection table before allocating
            // handshake state. No peer-controlled wait occurs on this thread.
            if (!tls_ || free_slots_.empty()) {
                continue;
            }
            handoff.tls_handshake_pending = true;
            handoff.tls_handshake_since = now;
        }
        if (!handoff.tls_handshake_pending && config_.authz.enabled()) {
            const auto grant = config_.authz.grant_for(handoff.principal);
            handoff.capabilities = grant.capabilities;
            handoff.key_prefix = grant.key_prefix;
        }
        if (auto adopted = adopt_connection(std::move(handoff)); !adopted) {
            return adopted;
        }
    }
    return {};
}

auto Reactor::accept_unix_ready() -> Status {
    const auto now = std::chrono::steady_clock::now();
    while (true) {
        auto accepted = unix_listener_.accept();
        if (!accepted) {
            // Same isolation as TCP accept: one peer/setup glitch must not stop the process.
            return {};
        }
        auto& accepted_socket = accepted.value();
        if (!accepted_socket) {
            return {};
        }
        if (abuse_ && !abuse_->try_admit_accept(now)) {
            continue;
        }
        ConnectionHandoff handoff{.socket = std::move(accepted_socket).value(), .last_activity = now};
        if (config_.unix_peercred) {
            auto credentials = peer_credentials(handoff.socket.descriptor());
            if (!credentials) {
                if (security_audit_) {
                    security_audit_->auth_deny(credentials.error().message);
                }
                if (config_.unix_peercred_required) {
                    continue;
                }
            } else {
                handoff.principal = peercred_principal(*credentials);
                if (security_audit_) {
                    security_audit_->auth_accept(handoff.principal);
                }
            }
        } else if (config_.unix_peercred_required) {
            if (security_audit_) {
                security_audit_->auth_deny("unix_peercred_required");
            }
            continue;
        }
        if (config_.authz.enabled()) {
            const auto grant = config_.authz.grant_for(handoff.principal);
            handoff.capabilities = grant.capabilities;
            handoff.key_prefix = grant.key_prefix;
        }
        if (auto adopted = adopt_connection(std::move(handoff)); !adopted) {
            return adopted;
        }
    }
}

auto Reactor::adopt_connection(ConnectionHandoff handoff) -> Status {
    if (handoff.peer_read_closed && handoff.input.empty() && handoff.output.empty()) {
        return {};
    }
    if (free_slots_.empty()) {
        // Accept-flood keeps silent drop. BIND handoffs carry buffered OK / bound_worker
        // and must not destroy a definitive status with the socket.
        if (handoff.bound_worker.has_value() || !handoff.output.empty()) {
            reject_orphaned_handoff(std::move(handoff));
        }
        return {};
    }
    const auto slot = free_slots_.back();
    free_slots_.pop_back();
    auto& current = connections_[slot];
    current.socket = std::move(handoff.socket);
    if (config_.accepted_socket_send_buffer_bytes != 0) {
        const auto bytes = static_cast<int>(config_.accepted_socket_send_buffer_bytes);
        if (::setsockopt(current.socket.descriptor(), SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes)) != 0) {
            const bool bind_handoff = handoff.bound_worker.has_value() || !handoff.output.empty();
            handoff.socket = std::move(current.socket);
            free_slots_.push_back(slot);
            if (bind_handoff) {
                reject_orphaned_handoff(std::move(handoff));
            }
            return {};
        }
    }
    current.tls = std::move(handoff.tls);
    current.tls_handshake_pending = handoff.tls_handshake_pending;
    current.tls_handshake_since = handoff.tls_handshake_since;
    current.principal = std::move(handoff.principal);
    current.capabilities = handoff.capabilities;
    current.key_prefix = std::move(handoff.key_prefix);
    current.input = std::move(handoff.input);
    current.input_offset = 0;
    current.output = std::move(handoff.output);
    current.output_offset = 0;
    current.output_lease.reset();
    current.bound_worker = handoff.bound_worker;
    current.initialized = handoff.initialized;
    current.peer_read_closed = handoff.peer_read_closed;
    current.write_armed = has_pending_output(current);
    current.pipelined_store_input_observed = false;
    current.request_in_flight = false;
    current.mutations_in_flight = 0;
    current.mutation_window_count = 0;
    current.mutation_visibility.clear();
    current.cold_read_in_flight = false;
    current.close_after_flush = false;
    current.last_activity = handoff.last_activity.time_since_epoch().count() == 0
                                ? std::chrono::steady_clock::now()
                                : handoff.last_activity;
    current.partial_request_since = handoff.partial_request_since;
    current.in_flight_since = {};
    current.output_progress_since = handoff.output_progress_since;
    if (has_pending_output(current) && current.output_progress_since.time_since_epoch().count() == 0) {
        current.output_progress_since = std::chrono::steady_clock::now();
    }
    current.connection_rate_window_start_ns = handoff.connection_rate_window_start_ns;
    current.connection_rate_used = handoff.connection_rate_used;
    const ConnectionToken token{.slot = slot, .generation = current.generation};
    auto interest = IoInterest::none;
    if (!current.peer_read_closed) {
        interest = interest | IoInterest::read;
    }
    if (has_pending_output(current)) {
        interest = interest | IoInterest::write;
    }
    if (auto added = poller_.add(current.socket.descriptor(), token.encode(), interest); !added) {
        ConnectionHandoff rejected{.socket = std::move(current.socket),
                                   .tls = std::move(current.tls),
                                   .principal = std::move(current.principal),
                                   .capabilities = current.capabilities,
                                   .key_prefix = std::move(current.key_prefix),
                                   .input = std::move(current.input),
                                   .output = std::move(current.output),
                                   .bound_worker = current.bound_worker,
                                   .initialized = current.initialized,
                                   .peer_read_closed = current.peer_read_closed,
                                   .last_activity = current.last_activity,
                                   .partial_request_since = current.partial_request_since,
                                   .output_progress_since = current.output_progress_since,
                                   .connection_rate_window_start_ns = current.connection_rate_window_start_ns,
                                   .connection_rate_used = current.connection_rate_used};
        current.capabilities = Capability::none;
        current.bound_worker.reset();
        current.initialized = false;
        current.peer_read_closed = false;
        current.write_armed = false;
        current.close_after_flush = false;
        free_slots_.push_back(slot);
        if (rejected.bound_worker.has_value() || !rejected.output.empty()) {
            reject_orphaned_handoff(std::move(rejected));
        }
        return {};
    }
    active_connections_.fetch_add(1U, std::memory_order_relaxed);
    ++adopted_connections_;
    if (current.tls_handshake_pending) {
        return advance_tls_handshake(token);
    }
    // write_ready flushes BIND OK then processes residual input only after the
    // decided bytes fully drain (EAGAIN-safe). Do not call process_frames while
    // output remains — a trailing decode error would close and discard BIND OK.
    if (auto* adopted = connection(token);
        adopted != nullptr &&
        (has_pending_output(*adopted) || adopted->input_offset < adopted->input.size())) {
        if (auto flushed = write_ready(token); !flushed) {
            close_connection(token);
        }
    }
    // TLS accept/handoff: application records may already sit in OpenSSL's buffer
    // after SSL_accept (TLS 1.3 coalescing) with no fresh edge-triggered readable
    // event. Drain once before returning to the poller — same class as WANT_READ
    // stranded-ACK. Connection-local failures must not fail the accept loop.
    if (auto* adopted = connection(token); adopted != nullptr && adopted->tls && !adopted->peer_read_closed) {
        if (auto drained = read_ready(token); !drained) {
            close_connection(token);
        }
    }
    return {};
}

void Reactor::reject_orphaned_handoff(ConnectionHandoff handoff,
                                      const std::optional<std::uint64_t> request_id_override) noexcept try {
    if (!handoff.socket.valid()) {
        return;
    }
    std::uint64_t request_id = request_id_override.value_or(0);
    if (!request_id_override.has_value() && !handoff.output.empty()) {
        if (auto decoded = decode_response(handoff.output)) {
            request_id = decoded->frame.request_id;
        }
    }
    const ResponseView overloaded{
        .status = ResponseStatus::overloaded,
        .request_id = request_id,
        .owner_worker = handoff.bound_worker.value_or(kNoWorker),
        .worker_count = static_cast<std::uint32_t>(mesh_.size()),
        .routing_epoch = kRoutingEpoch,
    };
    auto encoded = encode_response(overloaded);
    if (!encoded) {
        return;
    }
    std::size_t offset = 0;
    while (offset < encoded->size()) {
        if (handoff.tls) {
            auto written = handoff.tls->write(encoded->data() + offset, encoded->size() - offset);
            if (!written || written->kind != TlsIoKind::ok || written->bytes == 0) {
                break;
            }
            offset += written->bytes;
            continue;
        }
        const auto sent = ::send(handoff.socket.descriptor(), encoded->data() + offset,
                                 encoded->size() - offset, reactor_detail::send_flags());
        if (sent <= 0) {
            break;
        }
        offset += static_cast<std::size_t>(sent);
    }
} catch (...) {
    return;
}

auto Reactor::queue_response(const ConnectionToken token, const ResponseView& response) -> Status {
    GS_PHASE_TCP(encode);
    auto* current = connection(token);
    if (current == nullptr) {
        return fail(ErrorCode::not_found, "response targets a stale connection");
    }
    auto encoded_size = encoded_response_size(response);
    if (!encoded_size) {
        return unexpected(encoded_size.error());
    }
    if (current->output_lease) {
        return fail(ErrorCode::corrupted_data,
                    "contiguous response cannot overtake an active scatter output lease");
    }
    const auto pending = pending_output_bytes(*current);
    if (*encoded_size > config_.maximum_output_bytes ||
        pending > config_.maximum_output_bytes - *encoded_size) {
        return fail(ErrorCode::record_too_large, "connection output high watermark exceeded");
    }
    try {
        if (glifistore::fault::consume_fail(glifistore::fault::Site::response_queue)) {
            throw std::bad_alloc{};
        }
        prepare_output_append(*current, *encoded_size);
        const auto output_offset = current->output.size();
        current->output.resize(output_offset + *encoded_size);
        auto destination = std::span<std::byte>{current->output}.subspan(output_offset, *encoded_size);
        if (auto encoded = encode_response(destination, response); !encoded) {
            current->output.resize(output_offset);
            return unexpected(encoded.error());
        }
        if (pending == 0) {
            current->output_progress_since = std::chrono::steady_clock::now();
        }
        if (abuse_ && !current->principal.empty() && !response.value.empty()) {
            abuse_->record_principal_response_bytes(current->principal, response.value.size(),
                                                    std::chrono::steady_clock::now());
        }
        return {};
    } catch (const std::bad_alloc&) {
        // Per-connection isolation: do not escalate to executor fail-stop. Mutation
        // completions already close without inventing OVERLOADED (may be committed).
        return fail(ErrorCode::resource_exhausted, "response queue allocation failed");
    }
}

auto Reactor::queue_owned_response(const ConnectionToken token, ResponseView response, OwnedValue value,
                                   const bool allow_pipelined_scatter) -> Status {
    auto* current = connection(token);
    if (current == nullptr) {
        return fail(ErrorCode::not_found, "owned response targets a stale connection");
    }
    response.value = value.view();
    // Hot in-memory reads can choose one bounded lease even with buffered input:
    // process_frames stops at the lease and resumes after sendmsg releases the
    // exact value pin. Cold reads retain the contiguous pipeline path so the
    // disk executor can overlap the following materialization.
    const auto has_buffered_follow_up = current->input_offset < current->input.size();
    if (current->tls || value.bytes.size() < reactor_detail::kMinimumScatterValueBytes ||
        (!allow_pipelined_scatter && (has_buffered_follow_up || current->pipelined_store_input_observed))) {
        return queue_response(token, response);
    }
    if (current->output_lease) {
        return fail(ErrorCode::corrupted_data, "connection already owns a scatter output lease");
    }
    auto encoded_size = encoded_response_size(response);
    if (!encoded_size) {
        return unexpected(encoded_size.error());
    }
    const auto pending = pending_output_bytes(*current);
    if (*encoded_size > config_.maximum_output_bytes ||
        pending > config_.maximum_output_bytes - *encoded_size) {
        return fail(ErrorCode::record_too_large, "connection output high watermark exceeded");
    }
    try {
        if (glifistore::fault::consume_fail(glifistore::fault::Site::response_queue)) {
            throw std::bad_alloc{};
        }
        LeasedOutput leased;
        if (auto encoded = encode_response_header(leased.header, response); !encoded) {
            return unexpected(encoded.error());
        }
        const auto value_bytes = value.bytes.size();
        leased.value = std::move(value);
        current->output_lease.emplace(std::move(leased));
        if (pending == 0) {
            current->output_progress_since = std::chrono::steady_clock::now();
        }
        output_scatter_responses_.fetch_add(1U, std::memory_order_relaxed);
        output_scatter_bytes_.fetch_add(value_bytes, std::memory_order_relaxed);
        if (abuse_ && !current->principal.empty()) {
            abuse_->record_principal_response_bytes(current->principal, value_bytes,
                                                    std::chrono::steady_clock::now());
        }
        return {};
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::resource_exhausted, "owned response queue allocation failed");
    }
}

auto Reactor::update_connection_interest(const ConnectionToken token) -> Status {
    auto* current = connection(token);
    if (current == nullptr) {
        return {};
    }
    auto interest = IoInterest::none;
    if (!current->peer_read_closed && !current->close_after_flush && !current->output_lease) {
        interest = interest | IoInterest::read;
    }
    if (has_pending_output(*current)) {
        interest = interest | IoInterest::write;
    }
    auto modified = [&] {
        GS_PHASE_TCP(poller_update);
        return poller_.modify(current->socket.descriptor(), token.encode(), interest);
    }();
    if (!modified) {
        return modified;
    }
    current->write_armed = has_interest(interest, IoInterest::write);
    return {};
}

auto Reactor::process_handoffs() -> Status {
    while (auto handoff = mesh_.try_pop(executor_id_)) {
        if (auto adopted = adopt_connection(std::move(*handoff)); !adopted) {
            return adopted;
        }
    }
    return {};
}

auto Reactor::run_once(const int timeout_ms) -> Status {
    pair_writers_.request_read_refresh(executor_id_);
    local_read_generation_ = pair_writers_.adopt_read_generation(executor_id_, minimum_cold_read_epoch());
    if (!local_read_generation_) {
        return fail(ErrorCode::unavailable, "paired read generation is unavailable");
    }
    const auto now = std::chrono::steady_clock::now();
    enforce_timeouts(now);
    if (auto messages = process_messages(); !messages) {
        return messages;
    }
    // A bounded accept turn may leave an edge-triggered listener readable.
    // Explicit continuation avoids waiting for a fresh edge while yielding to
    // ordinary I/O and completion handling between batches.
    if (tcp_accept_pending_) {
        if (auto accepted = accept_ready(false); !accepted) {
            return accepted;
        }
    }
    if (tls_accept_pending_) {
        if (auto accepted = accept_ready(true); !accepted) {
            return accepted;
        }
    }
    const auto wait_ms = next_timeout_ms(now);
    const auto effective_timeout =
        tcp_accept_pending_ || tls_accept_pending_
            ? 0
            : (timeout_ms < 0 ? wait_ms : (wait_ms < 0 ? timeout_ms : std::min(timeout_ms, wait_ms)));
    auto ready = poller_.wait(events_, effective_timeout);
    if (!ready) {
        return unexpected(ready.error());
    }
    for (std::size_t index = 0; index < *ready; ++index) {
        const auto event = events_[index];
        if (event.token == kMessageToken) {
            if (auto messages = process_messages(); !messages) {
                return messages;
            }
            continue;
        }
        if (event.token == kListenerToken) {
            if (auto accepted = accept_ready(false); !accepted) {
                return accepted;
            }
            continue;
        }
        if (event.token == kTlsListenerToken) {
            if (auto accepted = accept_ready(true); !accepted) {
                return accepted;
            }
            continue;
        }
        if (event.token == kUnixListenerToken) {
            if (auto accepted = accept_unix_ready(); !accepted) {
                return accepted;
            }
            continue;
        }
        const auto token = ConnectionToken::decode(event.token);
        if (auto* pending = connection(token); pending != nullptr && pending->tls_handshake_pending) {
            if (has_flag(event.flags, IoFlags::error) || has_flag(event.flags, IoFlags::hangup)) {
                close_connection(token);
            } else if (auto advanced = advance_tls_handshake(token); !advanced) {
                return advanced;
            }
            continue;
        }
        if (has_flag(event.flags, IoFlags::readable)) {
            if (auto read = read_ready(token); !read) {
                close_connection(token);
                continue;
            }
        }
        if (connection(token) != nullptr && has_flag(event.flags, IoFlags::writable)) {
            if (auto written = write_ready(token); !written) {
                close_connection(token);
                continue;
            }
        }
        // Hangup before raw error: EPOLLERR|EPOLLRDHUP and kqueue EV_EOF(+EV_ERROR)
        // co-report. The old error-first hard-close skipped hangup drain and could
        // discard decided bytes left after a partial writable flush (EAGAIN).
        if (auto* current = connection(token); current != nullptr && has_flag(event.flags, IoFlags::hangup)) {
            current->peer_read_closed = true;
            const auto action = reactor_detail::connection_action_for(
                current->peer_read_closed, current->close_after_flush, current->request_in_flight,
                has_pending_output(*current), current->input_offset < current->input.size());
            if (action == ConnectionAction::close_now || !write_ready(token)) {
                close_connection(token);
            }
            continue;
        }
        if (connection(token) != nullptr && has_flag(event.flags, IoFlags::error)) {
            auto* current = connection(token);
            int so_error = 0;
            if (current != nullptr && current->socket.valid()) {
                socklen_t length = sizeof(so_error);
                if (::getsockopt(current->socket.descriptor(), SOL_SOCKET, SO_ERROR, &so_error, &length) !=
                    0) {
                    so_error = errno;
                }
            }
            if (so_error == 0) {
                // Spurious co-report without a sticky socket error — do not discard
                // decided output; wait for writable/hangup.
                continue;
            }
            // Terminal SO_ERROR: best-effort one more drain, then release the slot.
            if (current != nullptr &&
                (has_pending_output(*current) || current->input_offset < current->input.size())) {
                static_cast<void>(write_ready(token));
            }
            if (connection(token) != nullptr) {
                close_connection(token);
            }
            continue;
        }
    }
    enforce_timeouts(std::chrono::steady_clock::now());
    return {};
}

void Reactor::touch_activity(Connection& current, const std::chrono::steady_clock::time_point now) noexcept {
    current.last_activity = now;
}

} // namespace glifistore::server
