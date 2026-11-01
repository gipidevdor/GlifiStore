#pragma once

#include "glifistore/server/authz.hpp"
#include "glifistore/server/bounded_mpsc_queue.hpp"
#include "glifistore/server/socket.hpp"
#include "glifistore/server/tls.hpp"
#include "glifistore/server/wakeup.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace glifistore::server {

struct ConnectionHandoff {
    SocketHandle socket{};
    std::unique_ptr<TlsSession> tls{};
    bool tls_handshake_pending{};
    std::chrono::steady_clock::time_point tls_handshake_since{};
    std::string principal{};
    Capability capabilities{Capability::none};
    // Empty = unrestricted keyspace; set from --authz-map prefix= (Phase 8 slice).
    std::string key_prefix{};
    std::vector<std::byte> input{};
    std::vector<std::byte> output{};
    std::optional<std::uint32_t> bound_worker{};
    bool initialized{};
    bool peer_read_closed{};
    std::chrono::steady_clock::time_point last_activity{};
    std::chrono::steady_clock::time_point partial_request_since{};
    std::chrono::steady_clock::time_point output_progress_since{};
    std::uint64_t connection_rate_window_start_ns{};
    std::uint32_t connection_rate_used{};
};

// The only cross-executor data structure in the server data path. It transfers
// ownership of a connection reference and its buffered state exactly once when
// BIND_WORKER selects the final Reactor.
class ConnectionHandoffMesh final {
  public:
    ConnectionHandoffMesh(std::size_t executor_count, std::size_t queue_capacity);

    ConnectionHandoffMesh(const ConnectionHandoffMesh&) = delete;
    auto operator=(const ConnectionHandoffMesh&) -> ConnectionHandoffMesh& = delete;
    ConnectionHandoffMesh(ConnectionHandoffMesh&& other) noexcept
        : endpoints_(std::move(other.endpoints_)),
          accepting_(other.accepting_.load(std::memory_order_relaxed)) {}
    auto operator=(ConnectionHandoffMesh&& other) noexcept -> ConnectionHandoffMesh& {
        if (this != &other) {
            endpoints_ = std::move(other.endpoints_);
            accepting_.store(other.accepting_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        return *this;
    }

    void register_wakeup(std::size_t executor, Wakeup& wakeup) noexcept;
    // Refuse new handoffs (process stop). Queued cells remain until the target pops
    // or reject_orphaned_handoff drains them — never silently destroy BIND OK.
    void stop_accepting() noexcept;
    [[nodiscard]] auto accepting() const noexcept -> bool {
        return accepting_.load(std::memory_order_acquire);
    }
    // Success consumes connection; rejection leaves it untouched so the
    // source Reactor can restore and close it through the wire contract.
    [[nodiscard]] auto try_handoff(std::size_t target_executor, ConnectionHandoff& connection) -> bool;
    [[nodiscard]] auto try_handoff(std::size_t target_executor, ConnectionHandoff&& connection) -> bool;
    [[nodiscard]] auto try_pop(std::size_t executor) -> std::optional<ConnectionHandoff>;
    [[nodiscard]] auto has_pending(std::size_t executor) const noexcept -> bool;
    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return endpoints_.size();
    }

  private:
    struct Endpoint {
        explicit Endpoint(std::size_t capacity) : connections(capacity) {}

        BoundedMpscQueue<ConnectionHandoff> connections;
        Wakeup* wakeup{};
        std::atomic<std::size_t> pending{};
    };

    std::vector<std::unique_ptr<Endpoint>> endpoints_;
    std::atomic<bool> accepting_{true};
};

} // namespace glifistore::server
