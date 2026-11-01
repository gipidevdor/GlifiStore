#include "glifistore/server/reactor.hpp"

#include <chrono>
#include <limits>
#include <optional>

namespace glifistore::server {

void Reactor::enforce_timeouts(const std::chrono::steady_clock::time_point now) noexcept {
    const auto idle_ms = abuse_ ? abuse_->limits().idle_timeout_ms : 0U;
    const auto request_ms = abuse_ ? abuse_->limits().request_timeout_ms : 0U;
    const auto output_ms = request_ms != 0 ? request_ms : idle_ms;
    const auto handshake_ms =
        config_.tls.handshake_timeout_ms == 0 ? 10'000U : config_.tls.handshake_timeout_ms;
    if (!tls_ && idle_ms == 0 && request_ms == 0) {
        return;
    }
    for (std::uint32_t slot = 0; slot < connections_.size(); ++slot) {
        auto& candidate = connections_[slot];
        if (!candidate.socket.valid()) {
            continue;
        }
        const ConnectionToken token{.slot = slot, .generation = candidate.generation};
        if (candidate.tls_handshake_pending) {
            if (now - candidate.tls_handshake_since >= std::chrono::milliseconds{handshake_ms}) {
                if (security_audit_) {
                    security_audit_->tls_error("tls_handshake_timed_out");
                }
                close_connection(token);
            }
            continue;
        }
        if (output_ms != 0 && has_pending_output(candidate) &&
            candidate.output_progress_since.time_since_epoch().count() != 0 &&
            now - candidate.output_progress_since >= std::chrono::milliseconds{output_ms}) {
            // No new input/work may be admitted during the final bounded flush.
            // Existing Store mutations still finish; no replacement ACK is made.
            if (abuse_) {
                abuse_->note_request_timeout_closed();
            }
            candidate.close_after_flush = true;
            candidate.input.clear();
            candidate.input_offset = 0;
            static_cast<void>(write_ready(token));
            if (connection(token) != nullptr) {
                close_connection(token);
            }
            continue;
        }
        bool timed_out = false;
        bool request_timeout = false;
        if (request_ms != 0) {
            const auto budget = std::chrono::milliseconds{request_ms};
            const bool partial_request_timed_out =
                candidate.partial_request_since.time_since_epoch().count() != 0 &&
                now - candidate.partial_request_since >= budget;
            const bool in_flight_request_timed_out =
                !partial_request_timed_out && candidate.request_in_flight &&
                candidate.in_flight_since.time_since_epoch().count() != 0 &&
                now - candidate.in_flight_since >= budget;
            if (partial_request_timed_out || in_flight_request_timed_out) {
                timed_out = true;
                request_timeout = true;
            }
        }
        if (!timed_out && idle_ms != 0 && !candidate.request_in_flight && !has_pending_output(candidate) &&
            candidate.input_offset >= candidate.input.size() &&
            candidate.last_activity.time_since_epoch().count() != 0 &&
            now - candidate.last_activity >= std::chrono::milliseconds{idle_ms}) {
            timed_out = true;
        }
        if (!timed_out) {
            continue;
        }
        if (request_timeout) {
            abuse_->note_request_timeout_closed();
        } else {
            abuse_->note_idle_closed();
        }
        if (request_timeout && has_pending_output(candidate) && !candidate.request_in_flight) {
            // Partial-frame timeout must not discard a prior decided response still
            // blocked on EAGAIN (idle timeout already requires !has_pending_output).
            candidate.close_after_flush = true;
            candidate.input.clear();
            candidate.input_offset = 0;
            candidate.partial_request_since = {};
            static_cast<void>(write_ready(token));
            continue;
        }
        if (request_timeout && has_pending_output(candidate)) {
            // In-flight timeout: best-effort flush of any prior decided bytes, then hard
            // close (admitted work continues; stale completions are dropped).
            static_cast<void>(write_ready(token));
            if (connection(token) == nullptr) {
                continue;
            }
        }
        close_connection(token);
    }
}

auto Reactor::next_timeout_ms(const std::chrono::steady_clock::time_point now) const noexcept -> int {
    const auto idle_ms = abuse_ ? abuse_->limits().idle_timeout_ms : 0U;
    const auto request_ms = abuse_ ? abuse_->limits().request_timeout_ms : 0U;
    const auto output_ms = request_ms != 0 ? request_ms : idle_ms;
    const auto handshake_ms =
        config_.tls.handshake_timeout_ms == 0 ? 10'000U : config_.tls.handshake_timeout_ms;
    if (!tls_ && idle_ms == 0 && request_ms == 0) {
        return -1;
    }
    std::optional<std::chrono::steady_clock::duration> soonest;
    const auto consider = [&](const std::chrono::steady_clock::time_point since,
                              const std::uint32_t budget_ms) {
        if (budget_ms == 0 || since.time_since_epoch().count() == 0) {
            return;
        }
        const auto deadline = since + std::chrono::milliseconds{budget_ms};
        const auto remaining = deadline - now;
        if (!soonest.has_value() || remaining < *soonest) {
            soonest = remaining;
        }
    };
    for (const auto& candidate : connections_) {
        if (!candidate.socket.valid()) {
            continue;
        }
        if (candidate.tls_handshake_pending) {
            consider(candidate.tls_handshake_since, handshake_ms);
            continue;
        }
        if (has_pending_output(candidate)) {
            consider(candidate.output_progress_since, output_ms);
        }
        consider(candidate.partial_request_since, request_ms);
        if (candidate.request_in_flight) {
            consider(candidate.in_flight_since, request_ms);
        }
        if (!candidate.request_in_flight && !has_pending_output(candidate) &&
            candidate.input_offset >= candidate.input.size()) {
            consider(candidate.last_activity, idle_ms);
        }
    }
    if (!soonest.has_value()) {
        return -1;
    }
    if (*soonest <= std::chrono::steady_clock::duration::zero()) {
        return 0;
    }
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(*soonest).count();
    if (millis > std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(millis);
}

} // namespace glifistore::server
