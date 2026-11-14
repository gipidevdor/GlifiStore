/**
 * @file
 * @brief Pure connection-drain decisions that preserve pending mutation output.
 */
#pragma once

// Connection / input lifecycle decisions (behavior-neutral extraction).
// Normative: docs/spec/connection-drain-state-machine.md

#include <cstddef>
#include <cstdint>

namespace glifistore::server {

/**
 * @brief Connection output/drain lifecycle, independent of input EOF state.
 */
enum class ConnectionLifecycle : std::uint8_t {
    open,
    peer_half_closed,
    close_after_flush,
    draining_decided_output,
    forced_close,
    closed,
};

/**
 * @brief Whether further input is accepted, incomplete, stopped, or ended.
 */
enum class InputLifecycle : std::uint8_t {
    accepting,
    partial_frame,
    stopped,
    eof,
};

/**
 * @brief Pure close/drain/admission decision derived from outstanding work.
 */
enum class ConnectionAction : std::uint8_t {
    none,
    close_now,
    drain_then_close,
    refuse_new_frames,
};

/// Snapshot of the flags / occupancy predicates used by Reactor close paths.
struct ConnectionDrainSnapshot final {
    bool peer_read_closed{};
    bool close_after_flush{};
    bool request_in_flight{};
    bool has_pending_output{};
    bool residual_input{};
    bool forced_close{};
};

[[nodiscard]] constexpr auto connection_lifecycle_of(const ConnectionDrainSnapshot s) noexcept
    -> ConnectionLifecycle {
    if (s.forced_close) {
        return ConnectionLifecycle::forced_close;
    }
    if (s.close_after_flush) {
        return s.has_pending_output || s.request_in_flight ? ConnectionLifecycle::draining_decided_output
                                                           : ConnectionLifecycle::close_after_flush;
    }
    if (s.peer_read_closed) {
        return s.has_pending_output || s.request_in_flight || s.residual_input
                   ? ConnectionLifecycle::draining_decided_output
                   : ConnectionLifecycle::peer_half_closed;
    }
    return ConnectionLifecycle::open;
}

[[nodiscard]] constexpr auto input_lifecycle_of(const ConnectionDrainSnapshot s,
                                                const bool partial_frame) noexcept -> InputLifecycle {
    if (s.peer_read_closed) {
        return InputLifecycle::eof;
    }
    if (s.close_after_flush || s.forced_close) {
        return InputLifecycle::stopped;
    }
    if (partial_frame) {
        return InputLifecycle::partial_frame;
    }
    return InputLifecycle::accepting;
}

/// Single close/drain decision used by read_ready / write_ready / hangup / timeouts.
[[nodiscard]] constexpr auto decide_connection_action(const ConnectionDrainSnapshot s) noexcept
    -> ConnectionAction {
    if (s.forced_close && !s.has_pending_output && !s.request_in_flight) {
        return ConnectionAction::close_now;
    }
    if (s.close_after_flush) {
        if (!s.has_pending_output && !s.request_in_flight) {
            return ConnectionAction::close_now;
        }
        return ConnectionAction::refuse_new_frames;
    }
    if (s.peer_read_closed) {
        if (!s.has_pending_output && !s.request_in_flight && !s.residual_input) {
            return ConnectionAction::close_now;
        }
        return ConnectionAction::drain_then_close;
    }
    return ConnectionAction::none;
}

/// Typed view of decided bytes still owed to the peer (contiguous + optional lease).
struct DecidedOutput final {
    std::size_t contiguous_bytes{};
    std::size_t lease_header_remaining{};
    std::size_t lease_value_remaining{};

    [[nodiscard]] constexpr auto empty() const noexcept -> bool {
        return contiguous_bytes == 0 && lease_header_remaining == 0 && lease_value_remaining == 0;
    }

    [[nodiscard]] constexpr auto total_bytes() const noexcept -> std::size_t {
        return contiguous_bytes + lease_header_remaining + lease_value_remaining;
    }
};

} // namespace glifistore::server
