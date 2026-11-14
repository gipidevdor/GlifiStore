#include "server/server_health.hpp"

#include "glifistore/server/server.hpp"
#include "store/store_internal.hpp"

namespace glifistore::server {

auto ServerHealth::live(const bool started, const bool healthy) noexcept -> bool {
    return started && healthy;
}

auto ServerHealth::ready(const bool is_live, const bool stop_requested, const bool store_operational,
                         const MaintenanceSnapshot& maintenance) noexcept -> bool {
    if (!is_live) {
        return false;
    }
    if (stop_requested) {
        return false;
    }
    if (!store_operational) {
        return false;
    }
    if (maintenance.mutations_rejected) {
        return false;
    }
    if (maintenance.state == MaintenanceState::faulted && maintenance.last_error.has_value()) {
        return false;
    }
    return true;
}

auto Server::live() const noexcept -> bool {
    // Process/executor liveness only. Pair sticky fail-closed must not fail HEALTH
    // (wire v2 / durable-tcp-daemon: sticky fails READY, HEALTH may remain OK).
    return started_.load(std::memory_order_acquire) && !failed_.load(std::memory_order_acquire);
}

auto Server::ready() const noexcept -> bool {
    if (!pair_writers_healthy()) {
        return false;
    }
    if (!admissions_open()) {
        return false;
    }
    return ServerHealth::ready(live(), stop_requested(), store_operational(), maintenance_snapshot());
}

auto Server::store_operational() const noexcept -> bool {
    return detail::StoreAccess::operational(*store_);
}

auto Server::admissions_open() const noexcept -> bool {
    return detail::StoreAccess::admissions_open(*store_);
}

auto Server::maintenance_snapshot() const -> MaintenanceSnapshot {
    return store_->maintenance_snapshot();
}

} // namespace glifistore::server
