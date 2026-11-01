#include "glifistore/core/fault_injection.hpp"
#include "glifistore/core/hot_path_phases.hpp"
#include "glifistore/server/connection_lifecycle.hpp"
#include "glifistore/server/reactor.hpp"
#include "server/reactor_detail.hpp"
#include "system_error.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <new>
#include <sys/socket.h>
#include <sys/uio.h>
#include <utility>

namespace glifistore::server {

auto Reactor::read_ready(const ConnectionToken token) -> Status {
    auto* current = connection(token);
    if (current == nullptr) {
        return {};
    }
    if (current->close_after_flush) {
        return write_ready(token);
    }
    std::array<std::byte, std::size_t{16} * 1024U> buffer{};
    while (true) {
        std::size_t received_size = 0;
        if (current->tls) {
            auto received = current->tls->read(buffer.data(), buffer.size());
            if (!received) {
                return unexpected(received.error());
            }
            if (received->kind == TlsIoKind::want_read || received->kind == TlsIoKind::want_write ||
                received->kind == TlsIoKind::would_block) {
                // SSL_write may be waiting on a readable TLS record. Retry decided
                // output before returning to the edge-triggered poller.
                if (has_pending_output(*current)) {
                    return write_ready(token);
                }
                return {};
            }
            if (received->kind == TlsIoKind::closed) {
                current->peer_read_closed = true;
                if (reactor_detail::connection_action_for(
                        current->peer_read_closed, current->close_after_flush, current->request_in_flight,
                        has_pending_output(*current),
                        current->input_offset < current->input.size()) == ConnectionAction::close_now) {
                    close_connection(token);
                    return {};
                }
                return write_ready(token);
            }
            received_size = received->bytes;
        } else {
            const auto received = ::recv(current->socket.descriptor(), buffer.data(), buffer.size(), 0);
            if (received > 0) {
                received_size = static_cast<std::size_t>(received);
            } else if (received == 0) {
                current->peer_read_closed = true;
                if (reactor_detail::connection_action_for(
                        current->peer_read_closed, current->close_after_flush, current->request_in_flight,
                        has_pending_output(*current),
                        current->input_offset < current->input.size()) == ConnectionAction::close_now) {
                    close_connection(token);
                    return {};
                }
                return write_ready(token);
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return {};
            } else if (errno == EINTR) {
                continue;
            } else {
                return system_error("recv");
            }
        }
        const auto buffered = current->input.size() - current->input_offset;
        // Isolate input-buffer failures to this connection. Returning Status error
        // lets run_once hard-close and discard any already-decided output; throwing
        // fail-stops the executor. Drain decided bytes first when present.
        const auto isolate_input_failure = [&]() -> Status {
            current = connection(token);
            if (current == nullptr) {
                return {};
            }
            if (has_pending_output(*current)) {
                current->close_after_flush = true;
                current->input.clear();
                current->input_offset = 0;
                return write_ready(token);
            }
            close_connection(token);
            return {};
        };
        if (received_size > config_.maximum_input_bytes ||
            buffered > config_.maximum_input_bytes - received_size) {
            return isolate_input_failure();
        }
        try {
            if (glifistore::fault::consume_fail(glifistore::fault::Site::input_buffer)) {
                throw std::bad_alloc{};
            }
            prepare_input_append(*current, received_size);
            current->input.insert(current->input.end(), buffer.begin(),
                                  buffer.begin() + static_cast<std::ptrdiff_t>(received_size));
        } catch (const std::bad_alloc&) {
            return isolate_input_failure();
        }
        touch_activity(*current, std::chrono::steady_clock::now());
        if (auto processed = process_frames(token); !processed) {
            // Mirror write_ready: a decided response (abuse OVERLOADED / authz deny)
            // may already be queued before a trailing decode failure. Closing here
            // discarded that signal as silent EOF and undermined rate-limit feedback.
            current = connection(token);
            if (current != nullptr && has_pending_output(*current)) {
                current->close_after_flush = true;
                current->input.clear();
                current->input_offset = 0;
                return write_ready(token);
            }
            return processed;
        }
        current = connection(token);
        if (current == nullptr) {
            return {};
        }
        if (has_pending_output(*current)) {
            if (auto flushed = write_ready(token); !flushed) {
                return flushed;
            }
            current = connection(token);
            if (current == nullptr) {
                return {};
            }
        }
        // A rejected stream must yield even if the peer keeps sending junk.
        // Otherwise recv/process_frames/discard could starve the output timer.
        if (current->close_after_flush) {
            // TLS may have armed read interest solely to complete SSL_write.
            return current->tls ? Status{} : update_connection_interest(token);
        }
    }
}

void Reactor::prepare_input_append(Connection& current, const std::size_t additional_bytes) {
    if (current.input_offset == 0 || additional_bytes == 0) {
        return;
    }
    const auto physical_room = current.input.capacity() - current.input.size();
    const bool would_reallocate = additional_bytes > physical_room;
    const bool would_cross_limit = current.input.size() > config_.maximum_input_bytes - additional_bytes;
    if (!would_reallocate && !would_cross_limit) {
        return;
    }
    const auto remaining = current.input.size() - current.input_offset;
    current.input.erase(current.input.begin(),
                        current.input.begin() + static_cast<std::ptrdiff_t>(current.input_offset));
    current.input_offset = 0;
    input_buffer_compactions_.fetch_add(1U, std::memory_order_relaxed);
    input_buffer_bytes_moved_.fetch_add(remaining, std::memory_order_relaxed);
}

void Reactor::prepare_output_append(Connection& current, const std::size_t additional_bytes) {
    if (current.output_offset == 0 || additional_bytes == 0) {
        return;
    }
    const auto physical_room = current.output.capacity() - current.output.size();
    const bool would_reallocate = additional_bytes > physical_room;
    const bool would_cross_limit = current.output.size() > config_.maximum_output_bytes - additional_bytes;
    if (!would_reallocate && !would_cross_limit) {
        return;
    }
    const auto remaining = current.output.size() - current.output_offset;
    current.output.erase(current.output.begin(),
                         current.output.begin() + static_cast<std::ptrdiff_t>(current.output_offset));
    current.output_offset = 0;
    output_buffer_compactions_.fetch_add(1U, std::memory_order_relaxed);
    output_buffer_bytes_moved_.fetch_add(remaining, std::memory_order_relaxed);
}

auto Reactor::write_ready(const ConnectionToken token) -> Status {
    auto* current = connection(token);
    if (current == nullptr) {
        return {};
    }
    for (;;) {
        while (has_pending_output(*current)) {
            std::size_t written_size{};
            std::size_t requested_size{};
            bool would_block{};
            const bool scatter = current->output_lease.has_value();
            if (current->tls) {
                if (scatter) {
                    return fail(ErrorCode::corrupted_data, "TLS connection owns an invalid scatter lease");
                }
                const auto* data = current->output.data() + current->output_offset;
                requested_size = current->output.size() - current->output_offset;
                auto written = [&] {
                    GS_PHASE_TCP(socket_write);
                    return current->tls->write(data, requested_size);
                }();
                if (!written) {
                    return unexpected(written.error());
                }
                if (written->kind == TlsIoKind::want_read) {
                    // Opportunistic SSL_read: OpenSSL may need to consume a control
                    // record before SSL_write can proceed. Do not rely solely on an
                    // edge-triggered readable notification (especially after half-close
                    // previously armed write-only interest).
                    std::array<std::byte, std::size_t{16} * 1024U> tls_scratch{};
                    auto received = current->tls->read(tls_scratch.data(), tls_scratch.size());
                    if (received && received->kind == TlsIoKind::ok && received->bytes > 0) {
                        if (current->close_after_flush) {
                            // Consume TLS control progress without admitting new plaintext.
                            continue;
                        }
                        const auto buffered = current->input.size() - current->input_offset;
                        if (received->bytes <= config_.maximum_input_bytes &&
                            buffered <= config_.maximum_input_bytes - received->bytes) {
                            try {
                                prepare_input_append(*current, received->bytes);
                                current->input.insert(current->input.end(), tls_scratch.begin(),
                                                      tls_scratch.begin() +
                                                          static_cast<std::ptrdiff_t>(received->bytes));
                            } catch (const std::bad_alloc&) {
                                current->close_after_flush = true;
                                current->input.clear();
                                current->input_offset = 0;
                            }
                        }
                        continue;
                    }
                    if (received && received->kind == TlsIoKind::closed) {
                        current->peer_read_closed = true;
                    }
                    // Always keep read interest for WANT_READ, even after half-close.
                    const auto interest = IoInterest::read | IoInterest::write;
                    auto modified = [&] {
                        GS_PHASE_TCP(poller_update);
                        return poller_.modify(current->socket.descriptor(), token.encode(), interest);
                    }();
                    if (!modified) {
                        return modified;
                    }
                    current->write_armed = true;
                    // One immediate retry covers fail-once injection and sockets that
                    // are already readable/writable without a fresh ET edge. A second
                    // WANT_READ with no drain progress returns to the poller below.
                    written = [&] {
                        GS_PHASE_TCP(socket_write);
                        return current->tls->write(data, requested_size);
                    }();
                    if (!written) {
                        return unexpected(written.error());
                    }
                    if (written->kind == TlsIoKind::ok) {
                        written_size = written->bytes;
                    } else if (written->kind == TlsIoKind::closed) {
                        return fail(ErrorCode::io_error, "TLS write closed by peer");
                    } else if (tls_io_blocked(written->kind)) {
                        would_block = true;
                    } else {
                        return fail(ErrorCode::io_error, "TLS write returned an unexpected status");
                    }
                } else if (tls_io_blocked(written->kind)) {
                    would_block = true;
                } else if (written->kind == TlsIoKind::closed) {
                    return fail(ErrorCode::io_error, "TLS write closed by peer");
                } else {
                    written_size = written->bytes;
                }
                if (would_block) {
                    GS_EVENT_TCP(socket_write_would_block);
                    // want_write / would_block / persistent want_read: keep write armed;
                    // keep read unless half-closed *and* OpenSSL only asked for write.
                    const bool needs_read =
                        tls_io_needs_read(written->kind) ||
                        (!current->peer_read_closed && !current->close_after_flush && !scatter);
                    if (current->write_armed && !needs_read) {
                        return {};
                    }
                    const auto interest =
                        needs_read ? (IoInterest::read | IoInterest::write) : IoInterest::write;
                    auto modified = [&] {
                        GS_PHASE_TCP(poller_update);
                        return poller_.modify(current->socket.descriptor(), token.encode(), interest);
                    }();
                    if (!modified) {
                        return modified;
                    }
                    current->write_armed = true;
                    return {};
                }
                GS_EVENT_TCP(socket_write_progress);
                if (written_size < requested_size) {
                    GS_EVENT_TCP(socket_write_partial);
                }
            } else if (!scatter) {
                const auto* data = current->output.data() + current->output_offset;
                requested_size = current->output.size() - current->output_offset;
                const auto written = [&] {
                    GS_PHASE_TCP(socket_write);
                    return ::send(current->socket.descriptor(), data, requested_size,
                                  reactor_detail::send_flags());
                }();
                if (written > 0) {
                    written_size = static_cast<std::size_t>(written);
                } else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    would_block = true;
                } else if (written < 0 && errno == EINTR) {
                    GS_EVENT_TCP(socket_write_interrupted);
                    continue;
                } else if (written == 0) {
                    return fail(ErrorCode::io_error, "socket send made no progress");
                } else {
                    return system_error("send");
                }
            } else {
                std::array<iovec, 3> vectors{};
                std::size_t count{};
                const auto append = [&](const std::byte* data, const std::size_t size) {
                    if (size == 0) {
                        return;
                    }
                    vectors[count++] = {.iov_base = const_cast<std::byte*>(data), .iov_len = size};
                    requested_size += size;
                };
                if (current->output_offset < current->output.size()) {
                    append(current->output.data() + current->output_offset,
                           current->output.size() - current->output_offset);
                }
                auto& lease = *current->output_lease;
                append(lease.header.data() + lease.header_offset, lease.header.size() - lease.header_offset);
                append(lease.value.bytes.data() + lease.value_offset,
                       lease.value.bytes.size() - lease.value_offset);
                msghdr message{};
                message.msg_iov = vectors.data();
                message.msg_iovlen = static_cast<decltype(message.msg_iovlen)>(count);
                const auto written = [&] {
                    GS_PHASE_TCP(socket_write);
                    return ::sendmsg(current->socket.descriptor(), &message, reactor_detail::send_flags());
                }();
                if (written > 0) {
                    written_size = static_cast<std::size_t>(written);
                } else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    would_block = true;
                } else if (written < 0 && errno == EINTR) {
                    GS_EVENT_TCP(socket_write_interrupted);
                    continue;
                } else if (written == 0) {
                    return fail(ErrorCode::io_error, "socket sendmsg made no progress");
                } else {
                    return system_error("sendmsg");
                }
            }
            if (would_block) {
                GS_EVENT_TCP(socket_write_would_block);
                if (scatter) {
                    output_scatter_partial_writes_.fetch_add(1U, std::memory_order_relaxed);
                }
                if (current->write_armed) {
                    return {};
                }
                const auto interest = current->peer_read_closed || current->close_after_flush || scatter
                                          ? IoInterest::write
                                          : IoInterest::read | IoInterest::write;
                auto modified = [&] {
                    GS_PHASE_TCP(poller_update);
                    return poller_.modify(current->socket.descriptor(), token.encode(), interest);
                }();
                if (!modified) {
                    return modified;
                }
                current->write_armed = true;
                return {};
            }
            GS_EVENT_TCP(socket_write_progress);
            if (written_size < requested_size) {
                GS_EVENT_TCP(socket_write_partial);
            }
            if (scatter && written_size < requested_size) {
                output_scatter_partial_writes_.fetch_add(1U, std::memory_order_relaxed);
            }

            if (written_size != 0) {
                current->output_progress_since = std::chrono::steady_clock::now();
            }
            auto remaining = written_size;
            const auto consume = [&](std::size_t& offset, const std::size_t size) {
                const auto available = size - offset;
                const auto consumed = std::min(available, remaining);
                offset += consumed;
                remaining -= consumed;
            };
            consume(current->output_offset, current->output.size());
            if (current->output_offset == current->output.size()) {
                current->output.clear();
                current->output_offset = 0;
            }
            if (current->output_lease) {
                consume(current->output_lease->header_offset, current->output_lease->header.size());
                consume(current->output_lease->value_offset, current->output_lease->value.bytes.size());
                if (current->output_lease->header_offset == current->output_lease->header.size() &&
                    current->output_lease->value_offset == current->output_lease->value.bytes.size()) {
                    current->output_lease.reset();
                    output_scatter_completions_.fetch_add(1U, std::memory_order_relaxed);
                }
            }
            if (remaining != 0) {
                return fail(ErrorCode::corrupted_data, "socket write exceeded queued output extents");
            }
        }

        current->output_progress_since = {};
        touch_activity(*current, std::chrono::steady_clock::now());
        if (reactor_detail::connection_action_for(current->peer_read_closed, current->close_after_flush,
                                                  current->request_in_flight, has_pending_output(*current),
                                                  current->input_offset < current->input.size()) ==
            ConnectionAction::close_now) {
            close_connection(token);
            return {};
        }
        // Drain pipelined input before half-close teardown. Client SHUT_WR means
        // "done sending" — responses already decided (or still buffered) must flush,
        // and frames already received must still execute. Closing here used to drop
        // residual input and, via callers that process_frames on EAGAIN, could also
        // discard a buffered ACK when a trailing decode failed.
        if (!current->request_in_flight && current->input_offset < current->input.size()) {
            if (auto processed = process_frames(token); !processed) {
                // A decided response may have been queued earlier in this turn
                // (e.g. abuse OVERLOADED) before a trailing decode failed. Drain it.
                current = connection(token);
                if (current != nullptr && has_pending_output(*current)) {
                    current->close_after_flush = true;
                    current->input.clear();
                    current->input_offset = 0;
                    continue;
                }
                return processed;
            }
            current = connection(token);
            if (current == nullptr) {
                return {};
            }
            if (has_pending_output(*current)) {
                continue;
            }
        }
        if (current->peer_read_closed &&
            reactor_detail::connection_action_for(current->peer_read_closed, current->close_after_flush,
                                                  current->request_in_flight, has_pending_output(*current),
                                                  current->input_offset < current->input.size()) ==
                ConnectionAction::close_now) {
            close_connection(token);
            return {};
        }
        if (current->request_in_flight) {
            // A synchronous flush entered from read_ready can drain output
            // without ever arming writable interest. The socket is already
            // registered readable, so a read -> read modify would be a pure
            // syscall. Half-closed or write-armed connections still require
            // the full reconciliation below.
            if (!current->write_armed && !current->peer_read_closed) {
                return {};
            }
            return update_connection_interest(token);
        }
        if (!current->write_armed) {
            return {};
        }
        auto modified = [&] {
            GS_PHASE_TCP(poller_update);
            return poller_.modify(current->socket.descriptor(), token.encode(), IoInterest::read);
        }();
        if (!modified) {
            return modified;
        }
        current->write_armed = false;
        return {};
    }
}

} // namespace glifistore::server
