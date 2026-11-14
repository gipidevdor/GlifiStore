#include "glifistore/server/connection_handoff.hpp"

#include <utility>

namespace glifistore::server {

ConnectionHandoffMesh::ConnectionHandoffMesh(const std::size_t executor_count,
                                             const std::size_t queue_capacity) {
    endpoints_.reserve(executor_count);
    for (std::size_t index = 0; index < executor_count; ++index) {
        endpoints_.push_back(std::make_unique<Endpoint>(queue_capacity));
    }
}

void ConnectionHandoffMesh::register_wakeup(const std::size_t executor, Wakeup& wakeup) noexcept {
    endpoints_[executor]->wakeup = &wakeup;
}

void ConnectionHandoffMesh::stop_accepting() noexcept {
    accepting_.store(false, std::memory_order_release);
}

auto ConnectionHandoffMesh::try_handoff(const std::size_t target_executor, ConnectionHandoff& connection)
    -> bool {
    if (!accepting_.load(std::memory_order_acquire)) {
        return false;
    }
    auto& endpoint = *endpoints_[target_executor];
    if (!endpoint.connections.try_push(std::move(connection))) {
        return false;
    }
    endpoint.pending.fetch_add(1U, std::memory_order_release);
    if (endpoint.wakeup != nullptr) {
        static_cast<void>(endpoint.wakeup->notify());
    }
    return true;
}

auto ConnectionHandoffMesh::try_handoff(const std::size_t target_executor, ConnectionHandoff&& connection)
    -> bool {
    return try_handoff(target_executor, connection);
}

auto ConnectionHandoffMesh::try_pop(const std::size_t executor) -> std::optional<ConnectionHandoff> {
    auto& endpoint = *endpoints_[executor];
    auto handoff = endpoint.connections.try_pop();
    if (handoff.has_value()) {
        endpoint.pending.fetch_sub(1U, std::memory_order_release);
    }
    return handoff;
}

auto ConnectionHandoffMesh::has_pending(const std::size_t executor) const noexcept -> bool {
    return endpoints_[executor]->pending.load(std::memory_order_acquire) != 0;
}

} // namespace glifistore::server
