#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/persistence/runtime_catalog.hpp"
#include "glifistore/persistence/store_backup.hpp"
#include "glifistore/server/connection_handoff.hpp"
#include "glifistore/server/reactor.hpp"
#include "glifistore/server/thread_affinity.hpp"
#include "glifistore/store/store.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace glifistore::server {

struct ServerRuntime;

struct ReactorBufferStats final {
    std::uint64_t input_compactions{};
    std::uint64_t input_bytes_moved{};
    std::uint64_t output_compactions{};
    std::uint64_t output_bytes_moved{};
};

class Server final {
  public:
    // A missing Store shard count inherits ReactorConfig::worker_count. An
    // explicit count must match because each ShardPair owns one Reader/Reactor
    // and one Writer.
    [[nodiscard]] static auto create(const ReactorConfig& config = {}, StoreConfig store_config = {})
        -> Result<std::unique_ptr<Server>>;
    ~Server();

    Server(const Server&) = delete;
    auto operator=(const Server&) -> Server& = delete;
    Server(Server&&) = delete;
    auto operator=(Server&&) -> Server& = delete;

    [[nodiscard]] auto start() -> Status;
    void request_stop() noexcept;
    [[nodiscard]] auto join() -> Status;

    // Primary port for callers: cleartext when listening cleartext, else TLS.
    [[nodiscard]] auto port() const noexcept -> std::uint16_t;
    [[nodiscard]] auto cleartext_port() const noexcept -> std::uint16_t;
    [[nodiscard]] auto tls_port() const noexcept -> std::uint16_t;
    [[nodiscard]] auto unix_socket_path() const -> std::string;

    [[nodiscard]] auto executor_count() const noexcept -> std::size_t {
        return reactors_.size();
    }

    [[nodiscard]] auto adopted_connections_per_executor() const -> std::vector<std::size_t>;
    [[nodiscard]] auto active_connections_per_executor() const -> std::vector<std::size_t>;
    [[nodiscard]] auto executor_affinity_results() const -> std::vector<ExecutorAffinityResult>;
    // Aggregate copy pressure from sliding input/output buffers across all Reactors.
    [[nodiscard]] auto reactor_buffer_stats() const noexcept -> ReactorBufferStats;
    [[nodiscard]] auto pair_writer_stats() const -> std::vector<PairWriterStats>;
    [[nodiscard]] auto durable_batch_stats() const -> std::vector<DurableBatchWorkerStats>;

    [[nodiscard]] auto healthy() const noexcept -> bool {
        return !failed_.load(std::memory_order_acquire) && pair_writers_healthy();
    }

    // Paired Writer pool sticky health (READY / classify_ready_loss). Independent of
    // process liveness (`live`) and of durable catalog `store_operational`.
    [[nodiscard]] auto pair_writers_healthy() const noexcept -> bool {
        return pair_writers_ != nullptr && pair_writers_->healthy();
    }

    [[nodiscard]] auto stop_requested() const noexcept -> bool {
        return stop_requested_.load(std::memory_order_acquire);
    }

    [[nodiscard]] auto shutdown_drain_timed_out() const noexcept -> bool {
        return shutdown_drain_timed_out_.load(std::memory_order_acquire);
    }

    // Process liveness: started and no executor failure has been recorded.
    [[nodiscard]] auto live() const noexcept -> bool;
    // Traffic readiness: live, not shutting down, pair Writers healthy, Store admission open,
    // durable catalog healthy, and maintenance is not in emergency or a sticky faulted state.
    [[nodiscard]] auto ready() const noexcept -> bool;
    [[nodiscard]] auto store_operational() const noexcept -> bool;
    // False while online backup (or close) has closed begin_operation admissions.
    [[nodiscard]] auto admissions_open() const noexcept -> bool;
    [[nodiscard]] auto maintenance_snapshot() const -> MaintenanceSnapshot;
    [[nodiscard]] auto first_failure() const -> std::optional<Error>;
    // Bounded ASCII admin report (version, live/ready, connections, lane/batch, maintenance).
    // Read-only; fails closed when not live or when the report would exceed the size budget.
    [[nodiscard]] auto stats_report() const -> Result<std::string>;
    // Online durable backup while this Server holds the Store. Fences Store admissions during the
    // copy window (see Store::backup_to). Destination must be an empty new data directory path.
    [[nodiscard]] auto backup_to(const std::filesystem::path& destination, bool scan_records = true)
        -> Result<DurableStoreBackupReport>;

  private:
    Server(ReactorConfig config, ServerRuntime&& runtime);
    void run(std::size_t executor_id) noexcept;

    ReactorConfig config_;
    std::unique_ptr<Store> store_;
    std::unique_ptr<DiskReadExecutor> disk_reads_;
    std::unique_ptr<PairWriterPool> pair_writers_;
    ConnectionHandoffMesh mesh_;
    std::vector<std::unique_ptr<Reactor>> reactors_;
    std::vector<std::thread> threads_;
    std::vector<ExecutorAffinityResult> affinity_results_;
    std::atomic<bool> stop_requested_{};
    std::atomic<bool> started_{};
    std::atomic<bool> failed_{};
    std::atomic<bool> shutdown_drain_timed_out_{};
    mutable std::mutex failure_mutex_;
    std::optional<Error> failure_;
    std::mutex shutdown_mutex_;
    std::optional<std::chrono::steady_clock::time_point> shutdown_deadline_{};
};

} // namespace glifistore::server
