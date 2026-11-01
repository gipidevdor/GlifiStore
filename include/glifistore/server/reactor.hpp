#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/server/abuse_limits.hpp"
#include "glifistore/server/authz.hpp"
#include "glifistore/server/bounded_spsc_queue.hpp"
#include "glifistore/server/connection_handoff.hpp"
#include "glifistore/server/connection_token.hpp"
#include "glifistore/server/disk_read_executor.hpp"
#include "glifistore/server/mutation_window.hpp"
#include "glifistore/server/pair_writer.hpp"
#include "glifistore/server/poller.hpp"
#include "glifistore/server/protocol.hpp"
#include "glifistore/server/security_audit.hpp"
#include "glifistore/server/socket.hpp"
#include "glifistore/server/tls.hpp"
#include "glifistore/server/wakeup.hpp"
#include "glifistore/store/store.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace glifistore::server {

struct ServerLifecycleProbes final {
    bool (*live)(const void* context) noexcept {};
    bool (*ready)(const void* context) noexcept {};
    // Optional read-only admin probe. Writes a bounded ASCII report into `out`.
    // Returns false when the process is not live or the report cannot be built.
    bool (*stats)(const void* context, std::string& out) noexcept {};
    // Optional online backup probe. `destination` is a filesystem path (UTF-8). Writes a bounded
    // ASCII report into `out` on success; returns false on failure (message may be in `out`).
    bool (*backup)(void* context, std::string_view destination, std::string& out) noexcept {};
    const void* context{};
};

struct ReactorConfig {
    std::string bind_address{"127.0.0.1"};
    std::uint16_t port{7379};
    std::size_t maximum_connections{4096};
    std::size_t worker_count{1};
    std::size_t event_batch_size{256};
    std::size_t maximum_input_bytes{std::size_t{4} * 1024U * 1024U};
    std::size_t maximum_output_bytes{std::size_t{4} * 1024U * 1024U};
    // Zero leaves the platform default. A non-zero bound is primarily useful
    // for deterministic short-write tests and controlled deployment tuning.
    std::size_t accepted_socket_send_buffer_bytes{};
    std::size_t connection_handoff_capacity{4096};
    bool reuse_port{true};
    bool executor_affinity{};
    // Zero selects min(worker_count, 4). Request and per-Reactor completion
    // rings are fixed-capacity and reject excess cold reads as overloaded.
    std::size_t disk_read_thread_count{};
    std::size_t disk_read_queue_capacity{256};
    // Per-pair mutation and completion-ring capacity. Every PUT/ERASE crosses
    // the Reader -> Writer SPSC lane, for volatile and durable Stores alike.
    std::size_t durable_mutation_queue_capacity{256};
    std::size_t durable_mutation_queue_bytes{std::size_t{16} * 1024U * 1024U};
    // Zero disables expiry. Once Store execution begins the mutation always
    // runs to a classified completion and is never cancelled by this limit.
    std::uint32_t durable_mutation_queue_wait_ms{};
    // Bound how long join() waits after stop for connection drain and durable
    // mutation lanes. Zero means wait unbounded. The same deadline starts when
    // request_stop() is first observed: listeners stop accepting, idle
    // connections close, in-flight responses may still flush, then remaining
    // queued (not yet in Store) mutations complete as unavailable. In-flight
    // Store work is never cancelled and still drains before Store::close().
    // A timed-out drain makes join() return unavailable (fail-closed).
    std::uint32_t shutdown_drain_ms{30'000};
    // When tls.requested() and tls_port is unset: TLS-only on `port` (no cleartext).
    // When tls.requested() and tls_port is set: cleartext on `port`, TLS on *tls_port
    // (ADR 0020 dual listeners; never opportunistic TLS on one endpoint).
    std::optional<std::uint16_t> tls_port{};
    TlsConfig tls{};
    // When enabled, data-plane opcodes require capabilities from --authz-map.
    AuthzPolicy authz{};
    // Phase 5 abuse / DoS controls. Zero disables each limit (trusted cleartext default).
    AbuseLimits abuse{};
    // Phase 6 security audit JSON-lines (auth/authz/tls). Daemon enables for json logs
    // and --secure-profile; counters still accumulate when the shared audit sink exists.
    bool security_audit_events{};
    bool quiet{};
    // Phase 8 / ADR 0029: optional AF_UNIX listener (same wire protocol). Empty = disabled.
    std::filesystem::path unix_socket_path{};
    // Derive authz principal from OS peer credentials (unix:uid=N). Complementary to mTLS.
    bool unix_peercred{};
    // Drop accepted UDS peers when peer credentials cannot be obtained (secure-profile default).
    bool unix_peercred_required{};
};

class Reactor final {
  public:
    // cleartext_listener and/or tls_listener may be unbound (descriptor < 0).
    // A bound TLS listener requires a non-null tls context. unix_listener is optional.
    [[nodiscard]] static auto
    create(const ReactorConfig& config, std::size_t executor_id, TcpListener cleartext_listener,
           TcpListener tls_listener, UnixListener unix_listener, Store& store, ConnectionHandoffMesh& mesh,
           DiskReadExecutor& disk_reads, PairWriterPool& pair_writers,
           ServerLifecycleProbes lifecycle_probes = {}, std::shared_ptr<TlsContext> tls = {},
           std::shared_ptr<AbuseController> abuse = {}, std::shared_ptr<SecurityAudit> security_audit = {})
        -> Result<std::unique_ptr<Reactor>>;

    Reactor(const Reactor&) = delete;
    auto operator=(const Reactor&) -> Reactor& = delete;
    Reactor(Reactor&&) = delete;
    auto operator=(Reactor&&) -> Reactor& = delete;

    [[nodiscard]] auto run_once(int timeout_ms) -> Status;
    // Remove listeners from the poller and close them. Existing connections stay
    // open until drained or force-closed. Safe only on the owning executor thread.
    void stop_accepting() noexcept;
    // Close connections with no in-flight request and no pending output. Used
    // while shutting down so idle clients cannot hold the process open.
    void close_idle_connections() noexcept;
    // Force-close every connection. Outstanding async completions are still
    // accounted when they arrive (stale generation discards the response).
    void close_all_connections() noexcept;
    // Best-effort OVERLOADED for BIND handoffs still sitting only in the mesh
    // (shutdown force-close / late race). Must not destroy buffered OK silently.
    void reject_pending_handoffs() noexcept;
    [[nodiscard]] auto idle_for_shutdown() const noexcept -> bool {
        return active_connections_.load(std::memory_order_relaxed) == 0 && disk_reads_outstanding_ == 0 &&
               mutations_outstanding_ == 0 && !mesh_.has_pending(executor_id_);
    }
    // Cleartext listen port, or 0 when this reactor has no cleartext listener.
    [[nodiscard]] auto cleartext_port() const noexcept -> std::uint16_t {
        return listener_.descriptor() >= 0 ? listener_.port() : 0;
    }
    // TLS listen port, or 0 when this reactor has no TLS listener.
    [[nodiscard]] auto tls_port() const noexcept -> std::uint16_t {
        return tls_listener_.descriptor() >= 0 ? tls_listener_.port() : 0;
    }
    // Backward-compatible primary port: cleartext if present, otherwise TLS.
    [[nodiscard]] auto port() const noexcept -> std::uint16_t {
        const auto cleartext = cleartext_port();
        return cleartext != 0 ? cleartext : tls_port();
    }
    [[nodiscard]] auto unix_socket_path() const noexcept -> std::string_view {
        return unix_listener_.descriptor() >= 0 ? std::string_view{unix_listener_.path().native()}
                                                : std::string_view{};
    }
    [[nodiscard]] auto executor_id() const noexcept -> std::size_t {
        return executor_id_;
    }
    [[nodiscard]] auto active_connections() const noexcept -> std::size_t {
        return active_connections_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] auto adopted_connections() const noexcept -> std::size_t {
        return adopted_connections_;
    }
    [[nodiscard]] auto output_scatter_responses() const noexcept -> std::uint64_t {
        return output_scatter_responses_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] auto output_scatter_bytes() const noexcept -> std::uint64_t {
        return output_scatter_bytes_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] auto output_scatter_partial_writes() const noexcept -> std::uint64_t {
        return output_scatter_partial_writes_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] auto output_scatter_completions() const noexcept -> std::uint64_t {
        return output_scatter_completions_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] auto input_buffer_compactions() const noexcept -> std::uint64_t {
        return input_buffer_compactions_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] auto input_buffer_bytes_moved() const noexcept -> std::uint64_t {
        return input_buffer_bytes_moved_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] auto output_buffer_compactions() const noexcept -> std::uint64_t {
        return output_buffer_compactions_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] auto output_buffer_bytes_moved() const noexcept -> std::uint64_t {
        return output_buffer_bytes_moved_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] auto abuse_stats() const noexcept -> AbuseStats {
        return abuse_ ? abuse_->stats() : AbuseStats{};
    }
    [[nodiscard]] auto security_audit_stats() const noexcept -> SecurityAuditStats {
        return security_audit_ ? security_audit_->stats() : SecurityAuditStats{};
    }

  private:
    struct ReadLeaseEpoch final {
        std::uint64_t epoch{std::numeric_limits<std::uint64_t>::max()};
        std::size_t uses{};
    };

    // Cleartext-only owning output lease. Header and materialized value remain
    // in separate stable extents until sendmsg drains both. TLS deliberately
    // stays on the contiguous output buffer because its write API does not
    // retain caller iovecs across calls.
    struct LeasedOutput final {
        std::array<std::byte, kResponseHeaderBytes> header{};
        std::size_t header_offset{};
        OwnedValue value;
        std::size_t value_offset{};
    };

    struct Connection {
        SocketHandle socket;
        std::unique_ptr<TlsSession> tls;
        bool tls_handshake_pending{};
        std::chrono::steady_clock::time_point tls_handshake_since{};
        std::string principal{};
        Capability capabilities{Capability::none};
        std::string key_prefix{};
        std::uint32_t generation{1};
        std::vector<std::byte> input;
        std::size_t input_offset{};
        std::vector<std::byte> output;
        std::size_t output_offset{};
        std::optional<LeasedOutput> output_lease;
        std::optional<std::uint32_t> bound_worker;
        bool initialized{};
        bool peer_read_closed{};
        bool write_armed{};
        // Sticky workload classification: once this owner-bound connection
        // pipelines Store requests, preserve overlap with the next cold read
        // instead of serializing it behind a single scatter lease.
        bool pipelined_store_input_observed{};
        // At most one asynchronous Store request per connection preserves wire
        // response order unless an ADR 0037 mutation window is open (≤32 PUTs/ERASEs
        // before a GET barrier). GET and other barriers wait for the window to drain.
        bool request_in_flight{};
        std::uint32_t mutations_in_flight{};
        bool cold_read_in_flight{};
        // After BIND handoff rejection, keep the socket until OVERLOADED drains.
        bool close_after_flush{};
        std::chrono::steady_clock::time_point last_activity{};
        // Set while a partial request frame is buffered; cleared on complete frame.
        std::chrono::steady_clock::time_point partial_request_since{};
        // Set when request_in_flight becomes true; cleared when the response is queued.
        std::chrono::steady_clock::time_point in_flight_since{};
        std::chrono::steady_clock::time_point output_progress_since{};
        std::uint64_t connection_rate_window_start_ns{};
        std::uint32_t connection_rate_used{};
        // ADR 0037 Phase C: pipelined GET waits until published epoch covers prior mutations.
        MutationVisibilityBarrier mutation_visibility{};
        std::uint32_t mutation_window_count{};
    };

    Reactor(ReactorConfig config, std::size_t executor_id, TcpListener cleartext_listener,
            TcpListener tls_listener, UnixListener unix_listener, Poller poller, Wakeup wakeup, Store& store,
            ConnectionHandoffMesh& mesh, DiskReadExecutor& disk_reads, PairWriterPool& pair_writers,
            ServerLifecycleProbes lifecycle_probes, std::shared_ptr<TlsContext> tls,
            std::shared_ptr<AbuseController> abuse, std::shared_ptr<SecurityAudit> security_audit);

    [[nodiscard]] auto accept_ready(bool tls_endpoint) -> Status;
    [[nodiscard]] auto advance_tls_handshake(ConnectionToken token) -> Status;
    [[nodiscard]] auto accept_unix_ready() -> Status;
    [[nodiscard]] auto adopt_connection(ConnectionHandoff handoff) -> Status;
    // Best-effort OVERLOADED on a handoff that cannot be adopted (full table /
    // poller failure). Isolates the peer; must not fail the executor.
    // `request_id_override` is used when the buffered BIND OK was already cleared.
    void reject_orphaned_handoff(ConnectionHandoff handoff,
                                 std::optional<std::uint64_t> request_id_override = {}) noexcept;
    [[nodiscard]] auto read_ready(ConnectionToken token) -> Status;
    [[nodiscard]] auto write_ready(ConnectionToken token) -> Status;
    [[nodiscard]] auto process_frames(ConnectionToken token,
                                      std::uint32_t new_mutation_admission_budget = kMaximumMutationWindow)
        -> Status;
    void prepare_input_append(Connection& current, std::size_t additional_bytes);
    void prepare_output_append(Connection& current, std::size_t additional_bytes);
    [[nodiscard]] auto bind_connection(ConnectionToken token, const RequestView& request) -> Status;
    [[nodiscard]] auto transfer_connection(ConnectionToken token, std::size_t target_worker,
                                           std::uint64_t request_id) -> Status;
    [[nodiscard]] auto dispatch_request(ConnectionToken token, const RequestView& request,
                                        std::uint64_t& cached_now_ns) -> Status;
    [[nodiscard]] auto execute_local(ConnectionToken token, const RequestView& request,
                                     std::uint64_t key_hash, std::uint64_t& cached_now_ns) -> Status;
    [[nodiscard]] auto process_messages() -> Status;
    [[nodiscard]] auto process_handoffs() -> Status;
    [[nodiscard]] auto process_disk_read_completions() -> Status;
    [[nodiscard]] auto process_mutation_completions() -> Status;
    void flush_deferred_mutation_payloads() noexcept;
    [[nodiscard]] auto acquire_cold_read_lease(std::uint64_t epoch) noexcept -> bool;
    [[nodiscard]] auto release_cold_read_lease(std::uint64_t epoch) noexcept -> bool;
    [[nodiscard]] auto minimum_cold_read_epoch() const noexcept -> std::uint64_t;
    [[nodiscard]] auto update_connection_interest(ConnectionToken token) -> Status;
    [[nodiscard]] auto queue_response(ConnectionToken token, const ResponseView& response) -> Status;
    [[nodiscard]] auto queue_owned_response(ConnectionToken token, ResponseView response, OwnedValue value,
                                            bool allow_pipelined_scatter = false) -> Status;
    [[nodiscard]] static auto has_pending_output(const Connection& connection) noexcept -> bool;
    [[nodiscard]] static auto pending_output_bytes(const Connection& connection) noexcept -> std::size_t;
    [[nodiscard]] auto connection(ConnectionToken token) noexcept -> Connection*;
    void close_connection(ConnectionToken token) noexcept;
    void touch_activity(Connection& current, std::chrono::steady_clock::time_point now) noexcept;
    void enforce_timeouts(std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] auto next_timeout_ms(std::chrono::steady_clock::time_point now) const noexcept -> int;

    static constexpr std::uint64_t kListenerToken = std::numeric_limits<std::uint64_t>::max();
    static constexpr std::uint64_t kMessageToken = kListenerToken - 1U;
    static constexpr std::uint64_t kTlsListenerToken = kListenerToken - 2U;
    static constexpr std::uint64_t kUnixListenerToken = kListenerToken - 3U;
    static constexpr std::uint64_t kRoutingEpoch = 1;

    ReactorConfig config_;
    std::size_t executor_id_{};
    TcpListener listener_;
    TcpListener tls_listener_;
    UnixListener unix_listener_;
    Poller poller_;
    Wakeup wakeup_;
    Store& store_;
    WorkerRoutingState worker_routing_{};
    ConnectionHandoffMesh& mesh_;
    DiskReadExecutor& disk_reads_;
    PairWriterPool& pair_writers_;
    // One generation pin per event-loop turn, not one shared_ptr operation per
    // GET. Durable entries additionally retain their exact immutable file
    // generation while asynchronous materialization is in flight.
    const PairReadGeneration* local_read_generation_{};
    bool durable_store_{};
    ServerLifecycleProbes lifecycle_probes_{};
    std::shared_ptr<TlsContext> tls_;
    std::shared_ptr<AbuseController> abuse_;
    std::shared_ptr<SecurityAudit> security_audit_;
    BoundedSpscQueue<DiskReadCompletion> disk_read_completions_;
    BoundedSpscQueue<MutationCompletion> mutation_completions_;
    // Stable for the Reactor lifetime. Epochs make cancellation safe across
    // connection-slot reuse without allocating a shared control block per GET.
    std::unique_ptr<std::atomic_uint64_t[]> read_cancellation_epochs_;
    // Reader-private and allocation-free. Retire pressure bounds the number
    // of simultaneously reachable epochs to current + retired generations.
    std::array<ReadLeaseEpoch, PairWriterPool::kMaximumReaderLeaseEpochs> cold_read_leases_{};
    std::vector<Connection> connections_;
    std::vector<std::uint32_t> free_slots_;
    std::vector<IoEvent> events_;
    std::atomic<std::size_t> active_connections_{};
    std::size_t adopted_connections_{};
    std::atomic_uint64_t output_scatter_responses_{};
    std::atomic_uint64_t output_scatter_bytes_{};
    std::atomic_uint64_t output_scatter_partial_writes_{};
    std::atomic_uint64_t output_scatter_completions_{};
    std::atomic_uint64_t input_buffer_compactions_{};
    std::atomic_uint64_t input_buffer_bytes_moved_{};
    std::atomic_uint64_t output_buffer_compactions_{};
    std::atomic_uint64_t output_buffer_bytes_moved_{};
    std::size_t disk_reads_outstanding_{};
    std::size_t mutations_outstanding_{};
    std::size_t mutation_bytes_outstanding_{};
    // Drain-deadline abandon may complete a later FIFO payload while an earlier
    // Store-entered mutation still owns the slot-pool head. Defer those releases
    // until in-order release succeeds (still send OVERLOADED immediately).
    std::vector<std::uint32_t> deferred_mutation_payloads_;
    bool tcp_accept_pending_{};
    bool tls_accept_pending_{};
    bool shutting_down_{};
};

} // namespace glifistore::server
