#pragma once

#include "glifistore/store/maintenance.hpp"
#include "glifistore/store/store.hpp"
#include "test.hpp"

#include <chrono>
#include <thread>

namespace maintenance_controller_test_support {

[[nodiscard]] inline auto wait_for_initial_idle(glifistore::Store& store) -> glifistore::MaintenanceSnapshot {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    glifistore::MaintenanceSnapshot snapshot{};
    while (std::chrono::steady_clock::now() < deadline) {
        snapshot = store.maintenance_snapshot();
        if (snapshot.evaluation_cycles > 0 && snapshot.state == glifistore::MaintenanceState::idle) {
            return snapshot;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(snapshot.evaluation_cycles > 0);
    GLIFI_REQUIRE(snapshot.state == glifistore::MaintenanceState::idle);
    return snapshot;
}

} // namespace maintenance_controller_test_support

using maintenance_controller_test_support::wait_for_initial_idle;
