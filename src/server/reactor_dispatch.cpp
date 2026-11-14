#include "glifistore/core/fault_injection.hpp"
#include "glifistore/core/hot_path_phases.hpp"
#include "glifistore/core/worker_routing.hpp"
#include "glifistore/server/reactor.hpp"
#include "server/reactor_detail.hpp"
#include "store/store_internal.hpp"
#include "system_error.hpp"

#include <chrono>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace glifistore::server {

auto Reactor::process_frames(const ConnectionToken token, const std::uint32_t new_mutation_admission_budget)
    -> Status {
    auto* current = connection(token);
    if (current == nullptr) {
        return {};
    }
    if (current->close_after_flush) {
        // Handoff-reject path: drain OVERLOADED only — do not accept BIND/Store frames.
        current->input.clear();
        current->input_offset = 0;
        return {};
    }
    std::uint64_t cached_now_ns{};
    std::uint32_t new_mutations_admitted{};
    // Sample the reactor clock once per process_frames turn (not per request).
    const auto now = std::chrono::steady_clock::now();
    while (!current->output_lease && current->input_offset < current->input.size()) {
        // Cold reads and non-mutation in-flight work still serialize the connection.
        if (current->cold_read_in_flight) {
            break;
        }
        if (current->request_in_flight && current->mutations_in_flight == 0) {
            break;
        }
        // ADR 0037 Phase C: mutation windows are capped at the Writer publication chunk.
        if (current->mutations_in_flight >= kMaximumMutationWindow) {
            break;
        }
        const std::span<const std::byte> available{current->input.data() + current->input_offset,
                                                   current->input.size() - current->input_offset};
        DecodedFrame<RequestView> decoded{};
        {
            GS_PHASE_TCP(decode);
            auto parsed = decode_request(available);
            if (!parsed) {
                return unexpected(parsed.error());
            }
            decoded = *parsed;
        }
        if (!decoded.complete) {
            if (current->partial_request_since.time_since_epoch().count() == 0) {
                current->partial_request_since = now;
            }
            break;
        }
        current->partial_request_since = {};
        // An open mutation window may absorb only adjacent mutations. Any
        // other opcode is an ordering/backpressure boundary and must wait for
        // the preceding publication completions. In particular, do not let a
        // large immediate response leap into the output buffer while the
        // Writer lane is still consuming an earlier PUT/ERASE.
        if (current->mutations_in_flight > 0 && decoded.frame.opcode != RequestOpcode::put &&
            decoded.frame.opcode != RequestOpcode::erase) {
            break;
        }
        if (shutting_down_ && decoded.frame.opcode != RequestOpcode::health &&
            decoded.frame.opcode != RequestOpcode::ready && decoded.frame.opcode != RequestOpcode::stats) {
            // Connection drain refuses new work; lifecycle probes may still observe live/ready/stats.
            ResponseView refused{.status = ResponseStatus::overloaded,
                                 .request_id = decoded.frame.request_id,
                                 .owner_worker = static_cast<std::uint32_t>(executor_id_),
                                 .worker_count = static_cast<std::uint32_t>(mesh_.size()),
                                 .routing_epoch = kRoutingEpoch};
            if (auto queued = queue_response(token, refused); !queued) {
                return queued;
            }
            current->input_offset += decoded.consumed;
            continue;
        }
        const bool authorized = authorize_request(config_.authz, current->capabilities, current->key_prefix,
                                                  decoded.frame.opcode, decoded.frame.key);
        // Only authorized probes are exempt. Denied requests consume attempt
        // budgets before audit or response side effects (ADR 0041).
        const bool lifecycle_probe = authorized && (decoded.frame.opcode == RequestOpcode::health ||
                                                    decoded.frame.opcode == RequestOpcode::ready ||
                                                    decoded.frame.opcode == RequestOpcode::stats);
        const auto refuse_denied_flood = [&] {
            if (authorized) {
                return false;
            }
            current->close_after_flush = true;
            current->input.clear();
            current->input_offset = 0;
            current->partial_request_since = {};
            return true;
        };
        if (!lifecycle_probe && abuse_) {
            if (!abuse_->try_admit_connection_request(current->connection_rate_window_start_ns,
                                                      current->connection_rate_used, now)) {
                ResponseView refused{.status = ResponseStatus::overloaded,
                                     .request_id = decoded.frame.request_id,
                                     .owner_worker = static_cast<std::uint32_t>(executor_id_),
                                     .worker_count = static_cast<std::uint32_t>(mesh_.size()),
                                     .routing_epoch = kRoutingEpoch};
                if (auto queued = queue_response(token, refused); !queued) {
                    return queued;
                }
                if (refuse_denied_flood()) {
                    return {};
                }
                current->input_offset += decoded.consumed;
                continue;
            }
            const auto request_bytes = decoded.frame.key.size() + decoded.frame.value.size();
            if (!abuse_->try_admit_principal(current->principal, request_bytes, now)) {
                ResponseView refused{.status = ResponseStatus::overloaded,
                                     .request_id = decoded.frame.request_id,
                                     .owner_worker = static_cast<std::uint32_t>(executor_id_),
                                     .worker_count = static_cast<std::uint32_t>(mesh_.size()),
                                     .routing_epoch = kRoutingEpoch};
                if (auto queued = queue_response(token, refused); !queued) {
                    return queued;
                }
                if (refuse_denied_flood()) {
                    return {};
                }
                current->input_offset += decoded.consumed;
                continue;
            }
        }
        if (!authorized) {
            if (security_audit_) {
                const auto reason = authorize_opcode(config_.authz, current->capabilities,
                                                     decoded.frame.opcode, current->key_prefix)
                                        ? "prefix_denied"
                                        : "capability_denied";
                security_audit_->authz_deny(current->principal, request_opcode_name(decoded.frame.opcode),
                                            reason);
            }
            ResponseView denied{.status = ResponseStatus::permission_denied,
                                .request_id = decoded.frame.request_id,
                                .owner_worker = static_cast<std::uint32_t>(executor_id_),
                                .worker_count = static_cast<std::uint32_t>(mesh_.size()),
                                .routing_epoch = kRoutingEpoch};
            if (auto queued = queue_response(token, denied); !queued) {
                return queued;
            }
            current->input_offset += decoded.consumed;
            continue;
        }
        if (decoded.frame.opcode == RequestOpcode::bind_worker) {
            current->input_offset += decoded.consumed;
            touch_activity(*current, now);
            return bind_connection(token, decoded.frame);
        }

        ResponseView response{.status = ResponseStatus::ok,
                              .request_id = decoded.frame.request_id,
                              .owner_worker = static_cast<std::uint32_t>(executor_id_),
                              .worker_count = static_cast<std::uint32_t>(mesh_.size()),
                              .routing_epoch = kRoutingEpoch};
        bool immediate_response = true;
        std::vector<std::byte> init_identity;
        switch (decoded.frame.opcode) {
        case RequestOpcode::init:
            // Do not mark initialized until identity bytes exist — a throw after
            // the flag left the connection half-initialized and fail-stopped the
            // executor under memory pressure.
            try {
                if (glifistore::fault::consume_fail(glifistore::fault::Site::init_identity)) {
                    throw std::bad_alloc{};
                }
                init_identity = encode_init_identity_value(get_worker_routing());
                response.value = init_identity;
                current->initialized = true;
            } catch (const std::bad_alloc&) {
                response.status = ResponseStatus::overloaded;
            }
            break;
        case RequestOpcode::ping:
            response.value = decoded.frame.value;
            break;
        case RequestOpcode::health:
            if (lifecycle_probes_.live != nullptr && lifecycle_probes_.live(lifecycle_probes_.context)) {
                response.value = reactor_detail::bytes("GlifiStore/live");
            } else {
                response.status = ResponseStatus::internal_error;
            }
            break;
        case RequestOpcode::ready:
            if (lifecycle_probes_.ready != nullptr && lifecycle_probes_.ready(lifecycle_probes_.context)) {
                response.value = reactor_detail::bytes("GlifiStore/ready");
            } else {
                response.status = ResponseStatus::internal_error;
            }
            break;
        case RequestOpcode::stats: {
            if (lifecycle_probes_.stats == nullptr) {
                response.status = ResponseStatus::internal_error;
                break;
            }
            try {
                std::string report;
                if (!lifecycle_probes_.stats(lifecycle_probes_.context, report)) {
                    response.status = ResponseStatus::internal_error;
                    break;
                }
                const auto value_budget = config_.maximum_output_bytes > kResponseHeaderBytes
                                              ? config_.maximum_output_bytes - kResponseHeaderBytes
                                              : std::size_t{0};
                if (report.size() > value_budget || report.size() + kResponseHeaderBytes > kMaxFrameBytes) {
                    response.status = ResponseStatus::overloaded;
                    break;
                }
                response.value = std::as_bytes(std::span<const char>{report.data(), report.size()});
                if (auto queued = queue_response(token, response); !queued) {
                    return queued;
                }
                immediate_response = false;
            } catch (const std::bad_alloc&) {
                response.status = ResponseStatus::overloaded;
            }
            break;
        }
        case RequestOpcode::backup: {
            // Wire v2: BACKUP is Bound-state only (not a pre-INIT lifecycle probe).
            // Unbound BACKUP used to run the synchronous fenced path on any fresh
            // connection when authz was disabled.
            if (!current->initialized || !current->bound_worker.has_value()) {
                response.status = ResponseStatus::not_bound;
                break;
            }
            if (lifecycle_probes_.backup == nullptr) {
                response.status = ResponseStatus::unsupported;
                break;
            }
            try {
                const auto destination = std::string_view{
                    reinterpret_cast<const char*>(decoded.frame.key.data()), decoded.frame.key.size()};
                const auto value_budget = config_.maximum_output_bytes > kResponseHeaderBytes
                                              ? config_.maximum_output_bytes - kResponseHeaderBytes
                                              : std::size_t{0};
                // Refuse before the fenced copy when the OK report cannot fit. OVERLOADED
                // after a successful backup would falsely claim known-not-committed.
                const auto estimated = reactor_detail::backup_ok_report_max_bytes(destination.size());
                if (estimated > value_budget || estimated + kResponseHeaderBytes > kMaxFrameBytes) {
                    response.status = ResponseStatus::overloaded;
                    break;
                }
                std::string report;
                if (!lifecycle_probes_.backup(const_cast<void*>(lifecycle_probes_.context), destination,
                                              report)) {
                    response.status = ResponseStatus::internal_error;
                    response.value = std::as_bytes(std::span<const char>{report.data(), report.size()});
                    if (auto queued = queue_response(token, response); !queued) {
                        return queued;
                    }
                    immediate_response = false;
                    break;
                }
                if (report.size() > value_budget || report.size() + kResponseHeaderBytes > kMaxFrameBytes) {
                    // Backup already committed. Never OVERLOADED (rejected / not-committed).
                    // Prefer a minimal OK that fits; otherwise INTERNAL_ERROR (reconcile).
                    constexpr std::string_view kMinimalOk = "status=ok\n";
                    if (kMinimalOk.size() <= value_budget &&
                        kMinimalOk.size() + kResponseHeaderBytes <= kMaxFrameBytes) {
                        response.value = reactor_detail::bytes(kMinimalOk);
                    } else {
                        response.status = ResponseStatus::internal_error;
                    }
                } else {
                    response.value = std::as_bytes(std::span<const char>{report.data(), report.size()});
                }
                if (auto queued = queue_response(token, response); !queued) {
                    return queued;
                }
                immediate_response = false;
            } catch (const std::bad_alloc&) {
                // May have crossed the backup fence; do not claim known-not-committed.
                response.status = ResponseStatus::internal_error;
            }
            break;
        }
        case RequestOpcode::get:
        case RequestOpcode::put:
        case RequestOpcode::erase: {
            if (available.size() > decoded.consumed) {
                current->pipelined_store_input_observed = true;
            }
            // GET is a visibility barrier: drain the open mutation window first.
            if (decoded.frame.opcode == RequestOpcode::get && current->mutations_in_flight > 0) {
                return {};
            }
            immediate_response = false;
            const auto mutations_before_dispatch = current->mutations_in_flight;
            if (auto dispatched = dispatch_request(token, decoded.frame, cached_now_ns); !dispatched) {
                return dispatched;
            }
            if (current->mutations_in_flight > mutations_before_dispatch) {
                new_mutations_admitted += current->mutations_in_flight - mutations_before_dispatch;
            }
            break;
        }
        case RequestOpcode::bind_worker:
            break;
        }
        if (immediate_response) {
            if (auto queued = queue_response(token, response); !queued) {
                return queued;
            }
        }
        current->input_offset += decoded.consumed;
        touch_activity(*current, now);
        if (current->request_in_flight) {
            current->in_flight_since = now;
            // Completion-driven parsing deliberately admits only one follow-up
            // mutation while decided output drains. Normal read/write turns use
            // the full publication-window budget. Keeping the budget local to
            // this turn prevents slow clients from reopening a 32-request
            // Writer window behind an EAGAIN-blocked response.
            if (new_mutations_admitted >= new_mutation_admission_budget) {
                break;
            }
            // ADR 0037 Phase C: keep parsing PUT/ERASE into the open mutation window.
            if (current->mutations_in_flight > 0 && current->mutations_in_flight < kMaximumMutationWindow &&
                current->input_offset < current->input.size()) {
                continue;
            }
            break;
        }
    }
    if (current->input_offset == current->input.size()) {
        current->input.clear();
        current->input_offset = 0;
        if (!current->request_in_flight) {
            current->partial_request_since = {};
        }
    }
    return {};
}

auto Reactor::bind_connection(const ConnectionToken token, const RequestView& request) -> Status {
    auto* current = connection(token);
    if (current == nullptr) {
        return {};
    }
    ResponseView response{.status = ResponseStatus::ok,
                          .request_id = request.request_id,
                          .owner_worker = request.target_worker,
                          .worker_count = static_cast<std::uint32_t>(mesh_.size()),
                          .routing_epoch = kRoutingEpoch};
    if (!current->initialized || current->bound_worker.has_value() || request.target_worker >= mesh_.size()) {
        response.status = ResponseStatus::invalid_request;
        response.owner_worker = current->bound_worker.value_or(kNoWorker);
        return queue_response(token, response);
    }
    current->bound_worker = request.target_worker;
    if (auto queued = queue_response(token, response); !queued) {
        current->bound_worker.reset();
        return queued;
    }
    if (request.target_worker == executor_id_) {
        // write_ready flushes BIND OK then parses residual input only after drain.
        // Calling process_frames while EAGAIN still holds decided bytes lets a
        // trailing decode error close the socket and discard BIND OK.
        return write_ready(token);
    }
    return transfer_connection(token, request.target_worker, request.request_id);
}

auto Reactor::transfer_connection(const ConnectionToken token, const std::size_t target_worker,
                                  const std::uint64_t request_id) -> Status {
    auto* current = connection(token);
    if (current == nullptr) {
        return {};
    }
    if (current->output_lease) {
        return fail(ErrorCode::corrupted_data, "connection handoff cannot transfer a scatter output lease");
    }
    if (auto removed = poller_.remove(current->socket.descriptor()); !removed) {
        // Must not move a live socket that is still registered here — dual-poller
        // ownership spins the source executor on stale tokens after handoff.
        current->bound_worker.reset();
        current->output.clear();
        current->output_offset = 0;
        current->output_lease.reset();
        const ResponseView overloaded{.status = ResponseStatus::overloaded,
                                      .request_id = request_id,
                                      .owner_worker = static_cast<std::uint32_t>(target_worker),
                                      .worker_count = static_cast<std::uint32_t>(mesh_.size()),
                                      .routing_epoch = kRoutingEpoch};
        if (auto queued = queue_response(token, overloaded); queued) {
            current->close_after_flush = true;
            current->input.clear();
            current->input_offset = 0;
            if (auto flushed = write_ready(token); !flushed) {
                close_connection(token);
            }
            return {};
        }
        close_connection(token);
        return {};
    }
    if (current->input_offset > 0) {
        current->input.erase(current->input.begin(),
                             current->input.begin() + static_cast<std::ptrdiff_t>(current->input_offset));
        current->input_offset = 0;
    }
    if (current->output_offset > 0) {
        current->output.erase(current->output.begin(),
                              current->output.begin() + static_cast<std::ptrdiff_t>(current->output_offset));
        current->output_offset = 0;
    }
    ConnectionHandoff handoff{.socket = std::move(current->socket),
                              .tls = std::move(current->tls),
                              .principal = std::move(current->principal),
                              .capabilities = current->capabilities,
                              .key_prefix = std::move(current->key_prefix),
                              .input = std::move(current->input),
                              .output = std::move(current->output),
                              .bound_worker = static_cast<std::uint32_t>(target_worker),
                              .initialized = current->initialized,
                              .peer_read_closed = current->peer_read_closed,
                              .last_activity = current->last_activity,
                              .partial_request_since = current->partial_request_since,
                              .output_progress_since = current->output_progress_since,
                              .connection_rate_window_start_ns = current->connection_rate_window_start_ns,
                              .connection_rate_used = current->connection_rate_used};
    current->principal.clear();
    current->capabilities = Capability::none;
    current->key_prefix.clear();
    current->bound_worker.reset();
    current->initialized = false;
    current->peer_read_closed = false;
    current->write_armed = false;
    current->pipelined_store_input_observed = false;
    current->request_in_flight = false;
    current->cold_read_in_flight = false;
    current->last_activity = {};
    current->partial_request_since = {};
    current->in_flight_since = {};
    current->output_progress_since = {};
    current->connection_rate_window_start_ns = 0;
    current->connection_rate_used = 0;
    // Slot stays occupied until enqueue succeeds. A full handoff queue must not
    // destroy the socket after BIND already buffered OK — restore, replace with
    // OVERLOADED, flush best-effort, then close (spec: close on enqueue failure).
    if (!mesh_.try_handoff(target_worker, handoff)) {
        current->socket = std::move(handoff.socket);
        current->tls = std::move(handoff.tls);
        current->principal = std::move(handoff.principal);
        current->capabilities = handoff.capabilities;
        current->key_prefix = std::move(handoff.key_prefix);
        current->input = std::move(handoff.input);
        current->output.clear();
        current->output_offset = 0;
        current->output_lease.reset();
        current->bound_worker.reset();
        current->initialized = handoff.initialized;
        current->peer_read_closed = handoff.peer_read_closed;
        current->last_activity = handoff.last_activity;
        current->partial_request_since = handoff.partial_request_since;
        current->output_progress_since = handoff.output_progress_since;
        current->connection_rate_window_start_ns = handoff.connection_rate_window_start_ns;
        current->connection_rate_used = handoff.connection_rate_used;
        if (auto added = poller_.add(current->socket.descriptor(), token.encode(), IoInterest::read);
            !added) {
            reject_orphaned_handoff(
                ConnectionHandoff{.socket = std::move(current->socket),
                                  .tls = std::move(current->tls),
                                  .bound_worker = static_cast<std::uint32_t>(target_worker),
                                  .initialized = current->initialized},
                request_id);
            // Socket is no longer on the connection / poller; finish slot teardown.
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
            current->cold_read_in_flight = false;
            current->close_after_flush = false;
            current->last_activity = {};
            current->partial_request_since = {};
            current->in_flight_since = {};
            current->connection_rate_window_start_ns = 0;
            current->connection_rate_used = 0;
            if (advance_connection_generation(current->generation)) {
                free_slots_.push_back(token.slot);
            }
            active_connections_.fetch_sub(1U, std::memory_order_relaxed);
            return {};
        }
        const ResponseView overloaded{.status = ResponseStatus::overloaded,
                                      .request_id = request_id,
                                      .owner_worker = static_cast<std::uint32_t>(target_worker),
                                      .worker_count = static_cast<std::uint32_t>(mesh_.size()),
                                      .routing_epoch = kRoutingEpoch};
        if (auto queued = queue_response(token, overloaded); queued) {
            current->close_after_flush = true;
            current->input.clear();
            current->input_offset = 0;
            if (auto flushed = write_ready(token); !flushed) {
                close_connection(token);
            }
            // Pending OVERLOADED stays until writable drain; do not fail the
            // read path (that would close and clear the buffered status).
            return {};
        }
        close_connection(token);
        return {};
    }
    if (advance_connection_generation(current->generation)) {
        free_slots_.push_back(token.slot);
    }
    active_connections_.fetch_sub(1U, std::memory_order_relaxed);
    return {};
}

auto Reactor::dispatch_request(const ConnectionToken token, const RequestView& request,
                               std::uint64_t& cached_now_ns) -> Status {
    auto* current = connection(token);
    if (current == nullptr) {
        return {};
    }
    const auto key = reactor_detail::key_text(request.key);
    std::uint64_t key_hash{};
    std::size_t owner{};
    {
        GS_PHASE_TCP(route);
        key_hash = hash_key_routing(key, worker_routing_);
        owner = route_worker(key_hash, mesh_.size());
    }
    if (!current->bound_worker.has_value()) {
        return queue_response(token, {.status = ResponseStatus::not_bound,
                                      .request_id = request.request_id,
                                      .owner_worker = static_cast<std::uint32_t>(owner),
                                      .worker_count = static_cast<std::uint32_t>(mesh_.size()),
                                      .routing_epoch = kRoutingEpoch});
    }
    if (*current->bound_worker != executor_id_) {
        return fail(ErrorCode::corrupted_data, "bound connection is owned by the wrong Reactor");
    }
    if (owner != executor_id_) {
        return queue_response(token, {.status = ResponseStatus::wrong_owner,
                                      .request_id = request.request_id,
                                      .owner_worker = static_cast<std::uint32_t>(owner),
                                      .worker_count = static_cast<std::uint32_t>(mesh_.size()),
                                      .routing_epoch = kRoutingEpoch});
    }
    return execute_local(token, request, key_hash, cached_now_ns);
}

} // namespace glifistore::server
