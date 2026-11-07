#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/server/bounded_spsc_queue.hpp"
#include "glifistore/server/connection_token.hpp"
#include "glifistore/server/latency_histogram.hpp"
#include "glifistore/server/pair_read_generation.hpp"
#include "glifistore/server/wakeup.hpp"
#include "glifistore/store/paired/shard_pair_runtime.hpp"
#include "glifistore/store/store.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace glifistore::server {

enum class MutationKind : std::uint8_t { put = 0, erase = 1 };

struct MutationCompletion final {
    ConnectionToken connection{};
    std::uint64_t request_id{};
    std::size_t admission_bytes{};
    std::uint32_t payload_slot{};
    std::uint64_t writer_epoch{};
    std::optional<Error> error{};
};

struct PairWriterStats final {
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
    store::paired::ReadGenerationMemoryStats read_generation_memory{};
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
    LatencyHistogram queue_wait_histogram{};
    LatencyHistogram service_histogram{};
};

struct MutationRequest final {
    ConnectionToken connection{};
    std::uint64_t request_id{};
    std::size_t worker_index{};
    MutationKind kind{};
    std::span<const std::byte> key{};
    std::uint64_t key_hash{};
    std::span<const std::byte> value{};
    std::uint64_t expire_at_ns{};
    BoundedSpscQueue<MutationCompletion>* completions{};
    Wakeup* wakeup{};
};

struct PairReadMergeConfig final {
    std::size_t delta_entries{8'192};
    std::size_t maximum_post_entries{32'736};
    std::size_t quantum_slots{4'096};
};

// Thin daemon adapter over Store-owned ShardPairRuntime (ADR 0032). Generation
// authority and Writer threads live in the Store; this type only maps Reactor
// ConnectionToken/Wakeup completions onto the shared runtime.
class PairWriterPool final {
  public:
    static constexpr std::size_t kMaximumRetiredReadGenerations =
        store::paired::ShardPairRuntime::kMaximumRetiredReadGenerations;
    static constexpr std::size_t kMaximumReaderLeaseEpochs =
        store::paired::ShardPairRuntime::kMaximumReaderLeaseEpochs;
    static constexpr std::size_t kMutationAdmissionOverheadBytes =
        store::paired::ShardPairRuntime::kMutationAdmissionOverheadBytes;

    [[nodiscard]] static auto create(Store& store, std::size_t worker_count, std::size_t capacity_per_worker,
                                     std::size_t payload_bytes_per_worker,
                                     std::chrono::milliseconds maximum_queue_wait,
                                     PairReadMergeConfig read_merge = {})
        -> Result<std::unique_ptr<PairWriterPool>>;
    ~PairWriterPool() = default;

    PairWriterPool(const PairWriterPool&) = delete;
    auto operator=(const PairWriterPool&) -> PairWriterPool& = delete;
    PairWriterPool(PairWriterPool&&) = delete;
    auto operator=(PairWriterPool&&) -> PairWriterPool& = delete;

    [[nodiscard]] auto start() -> Status;
    [[nodiscard]] auto try_submit(const MutationRequest& request) noexcept -> std::optional<std::size_t>;
    [[nodiscard]] auto release_payload(std::size_t worker_index, std::uint32_t payload_slot) noexcept -> bool;
    [[nodiscard]] static auto mutation_admission_bytes(std::size_t key_bytes,
                                                       std::size_t value_bytes) noexcept
        -> std::optional<std::size_t>;
    void note_rejected(std::size_t worker_index) noexcept;
    [[nodiscard]] auto stats() const -> std::vector<PairWriterStats>;
    [[nodiscard]] auto adopt_read_generation(
        std::size_t worker_index,
        std::uint64_t minimum_leased_epoch = std::numeric_limits<std::uint64_t>::max()) const noexcept
        -> const PairReadGeneration*;
    void request_read_refresh(std::size_t worker_index) noexcept;
    [[nodiscard]] auto healthy() const noexcept -> bool;
    // Drain-deadline path: abandon pre-Store queued work as resource_exhausted so
    // Reactors can flush OVERLOADED before close_all_connections.
    void abandon_queued_mutations() noexcept;

    [[nodiscard]] auto expire_remaining_armed() const noexcept -> bool {
        return runtime_.expire_remaining_armed();
    }

    [[nodiscard]] auto stop_and_drain(std::optional<std::chrono::milliseconds> deadline = std::nullopt)
        -> Status;

  private:
    explicit PairWriterPool(store::paired::ShardPairRuntime& runtime) noexcept;

    store::paired::ShardPairRuntime& runtime_;
};

} // namespace glifistore::server
