#include "glifistore/server/connection_lifecycle.hpp"
#include "glifistore/server/connection_token.hpp"
#include "test.hpp"

#include <cstdint>
#include <limits>

using glifistore::server::advance_connection_generation;
using glifistore::server::connection_lifecycle_of;
using glifistore::server::ConnectionAction;
using glifistore::server::ConnectionDrainSnapshot;
using glifistore::server::ConnectionLifecycle;
using glifistore::server::decide_connection_action;
using glifistore::server::DecidedOutput;
using glifistore::server::input_lifecycle_of;
using glifistore::server::InputLifecycle;

GLIFI_TEST("connection_lifecycle decide close_now only when drained") {
    ConnectionDrainSnapshot open{};
    GLIFI_REQUIRE(decide_connection_action(open) == ConnectionAction::none);
    GLIFI_REQUIRE(connection_lifecycle_of(open) == ConnectionLifecycle::open);

    ConnectionDrainSnapshot half{.peer_read_closed = true, .residual_input = true};
    GLIFI_REQUIRE(decide_connection_action(half) == ConnectionAction::drain_then_close);
    GLIFI_REQUIRE(connection_lifecycle_of(half) == ConnectionLifecycle::draining_decided_output);

    ConnectionDrainSnapshot half_drained{.peer_read_closed = true};
    GLIFI_REQUIRE(decide_connection_action(half_drained) == ConnectionAction::close_now);
    GLIFI_REQUIRE(connection_lifecycle_of(half_drained) == ConnectionLifecycle::peer_half_closed);

    ConnectionDrainSnapshot soft{.close_after_flush = true, .has_pending_output = true};
    GLIFI_REQUIRE(decide_connection_action(soft) == ConnectionAction::refuse_new_frames);
    GLIFI_REQUIRE(input_lifecycle_of(soft, false) == InputLifecycle::stopped);

    ConnectionDrainSnapshot soft_drained{.close_after_flush = true};
    GLIFI_REQUIRE(decide_connection_action(soft_drained) == ConnectionAction::close_now);

    DecidedOutput decided{.contiguous_bytes = 4, .lease_header_remaining = 1};
    GLIFI_REQUIRE(!decided.empty());
    GLIFI_REQUIRE(decided.total_bytes() == 5);
}

GLIFI_TEST("connection token generation retires a slot instead of wrapping into an ABA identity") {
    std::uint32_t generation = std::numeric_limits<std::uint32_t>::max() - 1U;
    GLIFI_REQUIRE(advance_connection_generation(generation));
    GLIFI_REQUIRE(generation == std::numeric_limits<std::uint32_t>::max());

    GLIFI_REQUIRE(!advance_connection_generation(generation));
    GLIFI_REQUIRE(generation == 0U);
    GLIFI_REQUIRE(!advance_connection_generation(generation));
    GLIFI_REQUIRE(generation == 0U);
}
