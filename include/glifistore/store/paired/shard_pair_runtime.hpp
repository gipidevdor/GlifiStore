/**
 * @file
 * @brief Official paired shard runtime with bounded lanes and immutable reader leases.
 */
#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/core/latency_histogram.hpp"
#include "glifistore/store/config.hpp"
#include "glifistore/store/paired/fail_closed_state.hpp"
#include "glifistore/store/paired/read_generation.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace glifistore {
class Store;
}

namespace glifistore::store::paired {

struct WriterAsyncBatchEnv;
struct WriterSyncDrainEnv;

/**
 * @brief Paired Writer mutation kind, separate from server adapter enum values.
 */
enum class MutationKind : std::uint8_t { put, erase };

/// Opaque per-request context. The runtime never interprets it; glifistored
/// carries its encoded ConnectionToken here.
struct MutationContext final {
    std::uint64_t value{};
};

/**
 * @brief Asynchronous completion context and publication epoch, with optional owning error.
 */
struct MutationOutcome final {
    MutationContext context{};
    std::uint64_t request_id{};
    std::size_t admission_bytes{};
    std::uint32_t payload_slot{};
    std::uint64_t writer_epoch{};
    std::optional<Error> error{};
};

/// Writer-side completion sink for asynchronous submissions. deliver() must be
/// non-blocking; returning false reports an internal FIFO/lifetime violation and
/// terminates the process because the mutation has already been linearized.
struct MutationSink final {
    void* completions{};
    void* wakeup{};
    bool (*deliver)(void* completions, MutationOutcome outcome) noexcept {};
    void (*notify)(void* wakeup) noexcept {};
};

/// Asynchronous submission. Key and value bytes are copied into the lane's
/// preallocated payload slot before try_submit() returns, so the producer may
/// recycle its input buffer immediately.
struct AsyncMutationRequest final {
    std::size_t shard{};
    MutationKind kind{};
    MutationContext context{};
    std::uint64_t request_id{};
    std::span<const std::byte> key{};
    std::uint64_t key_hash{};
    std::span<const std::byte> value{};
    std::uint64_t expire_at_ns{};
    MutationSink sink{};
};

/**
 * @brief Point-in-time per-shard lane, publication, reclamation, merge, and latency observations.
 */
struct ShardPairStats final {
    std::size_t worker_index{};
    std::uint64_t reader_safe_epoch{};
    std::uint64_t writer_epoch{};
    std::size_t queue_depth{};
    std::size_t queued_bytes{};
    std::size_t maximum_queue_depth{};
    std::size_t maximum_queued_bytes{};
    std::size_t payload_slot_capacity{};
    std::size_t payload_slots_in_use{};
    std::size_t maximum_payload_slots_in_use{};
    std::size_t payload_arena_capacity_bytes{};
    std::size_t payload_arena_storage_bytes{};
    std::size_t payload_arena_bytes_in_use{};
    std::size_t maximum_payload_arena_bytes_in_use{};
    std::size_t payload_admission_bytes_in_use{};
    std::size_t maximum_payload_admission_bytes_in_use{};
    std::uint64_t payload_slot_full_total{};
    std::uint64_t payload_arena_full_total{};
    std::uint64_t payload_too_large_total{};
    std::uint64_t admitted{};
    std::uint64_t rejected{};
    std::uint64_t expired_before_store{};
    std::uint64_t completed{};
    std::uint64_t conflict_retries{};
    std::uint64_t conflict_retry_commits{};
    std::uint64_t writer_batches{};
    std::uint64_t writer_batch_records{};
    std::size_t maximum_writer_batch_records{};
    std::uint64_t total_writer_batch_wait_ns{};
    std::uint64_t maximum_writer_batch_wait_ns{};
    std::uint64_t writer_batch_durability_deadline_closes{};
    std::uint64_t writer_batch_queue_deadline_closes{};
    std::uint64_t sync_drain_turns{};
    std::uint64_t sync_turn_splits{};
    std::uint64_t sync_async_fairness_turns{};
    std::uint64_t publications{};
    std::uint64_t publication_records{};
    std::uint64_t completion_notifications{};
    std::uint64_t total_queue_wait_ns{};
    std::uint64_t maximum_queue_wait_ns{};
    std::uint64_t total_service_ns{};
    std::uint64_t maximum_service_ns{};
    std::uint64_t read_catalog_revision{};
    std::uint64_t read_refresh_attempts{};
    std::uint64_t read_refresh_successes{};
    std::uint64_t read_refresh_failures{};
    std::uint64_t read_refresh_deferrals{};
    std::uint64_t generations_retired{};
    std::uint64_t shutdown_generations_reclaimed{};
    std::uint64_t generation_admission_backpressure_total{};
    bool reader_shutdown_finalized{};
    std::size_t retired_generation_count{};
    std::size_t delta_entries{};
    std::size_t delta_record_versions{};
    std::size_t delta_arena_record_bytes{};
    std::size_t delta_arena_key_bytes{};
    std::size_t delta_arena_key_storage_bytes{};
    ReadGenerationMemoryStats read_generation_memory{};
    bool read_merge_active{};
    std::size_t read_merge_post_entries{};
    std::uint64_t read_merge_starts{};
    std::uint64_t read_merge_completions{};
    std::uint64_t read_merge_failures{};
    std::uint64_t read_merge_backpressure{};
    std::uint64_t read_merge_slots_processed{};
    std::size_t read_merge_remaining_slots{};
    std::size_t read_merge_post_capacity_remaining{};
    std::uint64_t maximum_read_merge_quantum_slots{};
    /// Synchronous embedded put/erase handoffs executed by this Writer.
    std::uint64_t sync_admitted{};
    LatencyHistogram queue_wait_histogram{};
    LatencyHistogram service_histogram{};
};

/// Paired Reader/Writer runtime owned by the Store (ADR 0031, ADR 0032, ADR 0037).
/// Every Worker/shard owns one immutable published read generation and the lanes
/// that feed mutation ownership via an execution token:
///
///   * embedded (`async_lane_capacity == 0`): callers combine under the token
///     (no mandatory Writer thread); sync put/erase enqueue a stack-resident node
///     and either execute or wait for completion;
///   * daemon (`async_lane_capacity > 0`): a dedicated per-shard Writer thread
///     holds the token and drains sync + async lanes;
///   * glifistored hands off borrowed frame bytes through the bounded SPSC lane
///     and receives asynchronous completions on its Reader.
///
/// Generation authority lives here and nowhere else: hosts adopt a published
/// generation, they never publish one.
class ShardPairRuntime final {
  public:
    static constexpr std::size_t kMaximumRetiredReadGenerations = 64;
    static constexpr std::size_t kMaximumReaderLeaseEpochs = kMaximumRetiredReadGenerations + 1U;
    /// Stable cross-platform accounting charge. Queue cells and payload storage
    /// are preallocated, but preserving a metadata charge keeps the configured
    /// byte-admission contract independent of ABI-specific sizeof values.
    static constexpr std::size_t kMutationAdmissionOverheadBytes = 128;

    [[nodiscard]] static auto create(Store& store, const PairedConcurrencyConfig& config)
        -> Result<std::unique_ptr<ShardPairRuntime>>;
    ~ShardPairRuntime();

    ShardPairRuntime(const ShardPairRuntime&) = delete;
    auto operator=(const ShardPairRuntime&) -> ShardPairRuntime& = delete;
    ShardPairRuntime(ShardPairRuntime&&) = delete;
    auto operator=(ShardPairRuntime&&) -> ShardPairRuntime& = delete;

    [[nodiscard]] auto shard_count() const noexcept -> std::size_t {
        return lanes_.size();
    }

    [[nodiscard]] auto async_lane_enabled() const noexcept -> bool;

    /// ADR 0037: embedded sync combines without Writer threads when async is off.
    [[nodiscard]] auto combining_enabled() const noexcept -> bool {
        return !async_lane_enabled();
    }

    /// True when a permanent per-shard Writer thread must run (async lane and/or
    /// durable_group Writer-batch coalesce). Embedded volatile and durable_sync
    /// with async off use the caller combiner only.
    [[nodiscard]] auto dedicated_writer_required() const noexcept -> bool;
    [[nodiscard]] auto start() -> Status;

    [[nodiscard]] auto healthy() const noexcept -> bool {
        return healthy_.load(std::memory_order_acquire);
    }

    /// Stops admission and drains every admitted mutation before returning.
    /// nullopt waits unbounded. A set deadline expires remaining queued (pre-Store)
    /// work as resource_exhausted once it elapses (including a zero deadline, which
    /// arms expiry immediately); in-flight Store mutations are never cancelled.
    /// Returns unavailable if the deadline expired before Writers finished.
    [[nodiscard]] auto stop_and_drain(std::optional<std::chrono::milliseconds> deadline = std::nullopt)
        -> Status;
    /// Daemon lifecycle only: call after every Reader/Reactor, Writer and cold-I/O
    /// thread has joined. Clears the adoptable raw pointer and deterministically
    /// releases all generation ownership before Store file handles close.
    /// Fails without partial finalization if a counted embedded ReadLease remains.
    [[nodiscard]] auto finalize_reader_shutdown() -> Status;
    /// Arms expire_remaining_ and completes every still-queued (pre-Store) async
    /// mutation as resource_exhausted so Reactors can flush wire OVERLOADED before
    /// hard-closing sockets. Safe while a Writer is blocked in Store: queue pops are
    /// serialized with the Writer consumer. Idempotent.
    void abandon_queued_mutations() noexcept;

    [[nodiscard]] auto expire_remaining_armed() const noexcept -> bool {
        return expire_remaining_.load(std::memory_order_acquire);
    }

    /// Synchronous embedded mutation. Callable from any thread: the caller's key
    /// and value stay borrowed for the whole call, the owning Writer linearizes
    /// and publishes, and only then is the caller released.
    [[nodiscard]] auto mutate(std::size_t shard, MutationKind kind, const HashedKey& key,
                              std::span<const std::byte> value, std::uint64_t expire_at_ns,
                              ValueType type = ValueType::bytes) -> Status;

    /// Synchronous same-shard batch. Items are linearized FIFO on the owning Writer
    /// and published in groups of at most 32 (same bound as async). A dedicated
    /// Writer may service already-admitted async work between groups; this batch's
    /// own FIFO order is unchanged. Each item gets its own Status; ACK is only after
    /// the publication that includes that item. Key/value spans must remain live
    /// for the duration of the call.
    struct SyncBatchItem final {
        MutationKind kind{};
        const HashedKey* key{};
        std::span<const std::byte> value{};
        std::uint64_t expire_at_ns{};
    };

    [[nodiscard]] auto mutate_batch(std::size_t shard, std::span<const SyncBatchItem> items,
                                    std::span<Status> statuses) -> Status;

    /// Counted read lease for embedded readers. The lease keeps the adopted
    /// generation alive: the Writer retires generations eagerly and frees them
    /// only at an observed quiescent point with no lease outstanding.
    class ReadLease final {
      public:
        ReadLease() noexcept = default;
        ReadLease(const ShardPairRuntime& runtime, std::size_t shard) noexcept;
        ~ReadLease();

        ReadLease(const ReadLease&) = delete;
        auto operator=(const ReadLease&) -> ReadLease& = delete;
        ReadLease(ReadLease&&) = delete;
        auto operator=(ReadLease&&) -> ReadLease& = delete;

        explicit operator bool() const noexcept {
            return generation_ != nullptr;
        }

        [[nodiscard]] auto generation() const noexcept -> const PairReadGeneration* {
            return generation_;
        }

      private:
        const ShardPairRuntime* runtime_{};
        std::size_t shard_{};
        const PairReadGeneration* generation_{};
    };

    /// Asynchronous host lane (glifistored). Copies the borrowed request bytes
    /// into the lane's preallocated slot pool before returning; nullopt means
    /// bounded admission rejected the request.
    [[nodiscard]] auto try_submit(const AsyncMutationRequest& request) noexcept -> std::optional<std::size_t>;
    /// Host-only after acquiring the matching completion. False is an internal
    /// FIFO/lifetime violation and must fail the host closed.
    [[nodiscard]] auto release_payload(std::size_t shard, std::uint32_t payload_slot) noexcept -> bool;
    [[nodiscard]] static auto mutation_admission_bytes(std::size_t key_bytes,
                                                       std::size_t value_bytes) noexcept
        -> std::optional<std::size_t>;
    void note_rejected(std::size_t shard) noexcept;
    [[nodiscard]] auto stats() const -> std::vector<ShardPairStats>;
    /// Acquire one immutable view per Reader event-loop turn and report the
    /// adopted epoch. The paired Writer retains that epoch until a later turn
    /// reports quiescence; GET performs no refcount or lock operation.
    /// minimum_leased_epoch is the oldest generation still borrowed by an
    /// asynchronous Reader operation, or UINT64_MAX when none exists. The
    /// Writer may reclaim only generations older than the resulting safe epoch.
    /// Requires PairedConcurrencyConfig::reader_epoch_lease.
    [[nodiscard]] auto adopt_read_generation(
        std::size_t shard,
        std::uint64_t minimum_leased_epoch = std::numeric_limits<std::uint64_t>::max()) const noexcept
        -> const PairReadGeneration*;
    /// Reader-side atomic poll. A stale durable catalog wakes only the paired
    /// Writer; snapshot construction and publication never execute on Reader.
    void request_read_refresh(std::size_t shard) noexcept;

  private:
    friend struct WriterAsyncBatchEnv;
    /// WriterSyncDrainEnv names private nested Lane/SyncMutation in its fields.
    friend struct WriterSyncDrainEnv;

    struct Lane;
    struct SyncMutation;

    ShardPairRuntime(Store& store, PairedConcurrencyConfig config,
                     std::vector<std::shared_ptr<const PairReadGeneration>> initial_generations,
                     std::vector<std::uint64_t> initial_catalog_revisions);

    void run(std::size_t shard) noexcept;
    void run_writer_async_batch(WriterAsyncBatchEnv& env) noexcept;
    /// Drain one dedicated-Writer sync turn (≤32 records, Writer-local continuation).
    /// Returns true when at least one sync node was taken under the execution token.
    [[nodiscard]] auto run_writer_sync_drain(WriterSyncDrainEnv& env) noexcept -> bool;
    /// Drain already-queued sync mutations for `shard` (FIFO after LIFO admission,
    /// ≤32 publication chunks). Caller must hold the execution token (combiner or
    /// dedicated Writer). ACK polarity matches the historical run() sync path.
    void process_sync_lane(std::size_t shard) noexcept;
    /// Combining-mode mutate path: acquire token, drain, release with lost-wakeup CAS.
    void combine_sync_lane(std::size_t shard) noexcept;
    /// Writerless housekeeping under the execution token. publication_records
    /// lets merge work track the bounded post-cut capacity consumed by the next
    /// publication; zero performs one ordinary maintenance quantum.
    void combiner_housekeeping(std::size_t shard, std::size_t publication_records = 0U) noexcept;
    void note_writer_exit() noexcept;
    [[nodiscard]] auto begin_submission() noexcept -> bool;
    void finish_submission() noexcept;
    void wake(Lane& lane) noexcept;
    void wait_sync_done(SyncMutation& node) noexcept;
    void trip_generation_slot_fail_closed() noexcept;

    static constexpr auto kAdmissionClosed = std::size_t{1}
                                             << (std::numeric_limits<std::size_t>::digits - 1U);
    static constexpr auto kAdmissionCountMask = kAdmissionClosed - 1U;

    Store& store_;
    const PairedConcurrencyConfig config_;
    const std::chrono::milliseconds maximum_queue_wait_;
    std::vector<std::unique_ptr<Lane>> lanes_;
    /// Immutable after construction. Failure is rare, but building this fan-out
    /// view in every embedded mutation made successful PUTs pay one allocation.
    std::vector<FailClosedLaneWake> fail_closed_wakes_;
    std::atomic_size_t active_writers_{};
    std::atomic_size_t admission_state_{};
    std::atomic_bool started_{};
    std::atomic_bool stopping_{};
    std::atomic_bool expire_remaining_{};
    std::atomic_bool healthy_{true};
    std::mutex reader_shutdown_mutex_{};
};

} // namespace glifistore::store::paired
