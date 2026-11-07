/**
 * @file
 * @brief Immutable read generations and incremental merge of bounded mutation deltas.
 */
#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/core/worker_routing.hpp"
#include "glifistore/persistence/runtime_catalog.hpp"
#include "glifistore/segment/record.hpp"
#include "glifistore/segment/segment.hpp"
#include "glifistore/store/paired/generation_direct_storage.hpp"
#include "glifistore/store/value.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace glifistore::experimental {
struct PairReadGenerationShellAccess;
} // namespace glifistore::experimental

namespace glifistore::store::paired {

/// A Writer-owned mutation which has already been linearized in the Store.
/// The SegmentPtr is the generation pin: RecordRef must never escape alone.
struct ReadMutation final {
    HashedKey key{};
    RecordRef record{};
    SegmentPtr segment{};
    std::optional<DurableRuntimeCatalog::PublishedReadRecord> durable{};
    Opcode opcode{Opcode::put};
};

/// Allocation-payload census for the currently published immutable view. The
/// byte fields are lower bounds: they include every explicitly sized index,
/// arena and vector payload, but intentionally exclude allocator/control-block
/// overhead that is implementation-specific. They therefore remain stable and
/// comparable across supported platforms while process RSS is reported
/// separately by the benchmark harness.
struct ReadGenerationMemoryStats final {
    std::size_t base_entries{};
    std::size_t base_capacity{};
    std::size_t base_record_storage_bytes{};
    /// Portion of base_record_storage_bytes backed by a dedicated anonymous
    /// mapping rather than the process general-purpose allocator.
    std::size_t base_record_mapped_storage_bytes{};
    std::size_t base_lookup_storage_bytes{};
    std::size_t base_key_bytes{};
    std::size_t base_key_storage_bytes{};
    std::size_t base_pin_storage_bytes{};
    std::size_t base_allocated_lower_bound_bytes{};
    std::size_t delta_entries{};
    std::size_t delta_capacity{};
    std::size_t delta_record_versions{};
    std::size_t delta_arena_record_bytes{};
    std::size_t delta_arena_key_bytes{};
    std::size_t delta_arena_key_storage_bytes{};
    std::size_t delta_lookup_storage_bytes{};
    std::size_t delta_allocated_lower_bound_bytes{};
    std::size_t generation_shell_bytes{};
    std::size_t current_allocated_lower_bound_bytes{};
};

/// Process-wide payload capacity of the bounded per-lineage spare mappings
/// retained by immutable-base builders. Guard pages are excluded.
[[nodiscard]] auto immutable_base_spare_mapping_bytes() noexcept -> std::size_t;

class ImmutableReadIndex;
class DeltaState;
class PairReadMerge;
class GenerationSlotPool;
struct IncrementalPublicationAccess;

/// Immutable two-level read view published by one paired Writer and adopted by
/// its Reader. GET performs at most one delta lookup and one base lookup.
/// Not marked final: a private EnableShared derived type exists solely so
/// make_shared can co-allocate the object and control block.
class PairReadGeneration {
  public:
    static constexpr std::size_t kMaximumIncrementalDeltaEntries = 40'960;

    [[nodiscard]] static auto empty(WorkerRoutingState routing)
        -> Result<std::shared_ptr<const PairReadGeneration>>;

    [[nodiscard]] static auto
    from_durable_snapshot(WorkerRoutingState routing,
                          std::span<const DurableRuntimeCatalog::PublishedReadRecord> records)
        -> Result<std::shared_ptr<const PairReadGeneration>>;

    // Rebuilds the durable base from one catalog-consistent snapshot while
    // preserving the logical visibility watermark of erased records.
    [[nodiscard]] static auto
    replace_durable_snapshot(std::shared_ptr<const PairReadGeneration> previous,
                             std::span<const DurableRuntimeCatalog::PublishedReadRecord> records)
        -> Result<std::shared_ptr<const PairReadGeneration>>;

    [[nodiscard]] static auto publish(std::shared_ptr<const PairReadGeneration> previous,
                                      std::span<const ReadMutation> mutations,
                                      std::size_t merge_delta_entries)
        -> Result<std::shared_ptr<const PairReadGeneration>>;

    /// Production publication path. While merge is non-null the same immutable
    /// mutation handles are also appended to its bounded post-cut delta.
    [[nodiscard]] static auto publish_incremental(std::shared_ptr<const PairReadGeneration> previous,
                                                  std::span<const ReadMutation> mutations,
                                                  PairReadMerge* merge = nullptr)
        -> Result<std::shared_ptr<const PairReadGeneration>>;
    [[nodiscard]] static auto start_incremental_merge(std::shared_ptr<const PairReadGeneration> cut,
                                                      std::size_t maximum_post_entries)
        -> Result<std::unique_ptr<PairReadMerge>>;
    /**
     * @brief Perform at most maximum_slots of merge work; retain the cut generation and post-cut delta until
     * finish.
     */
    [[nodiscard]] static auto advance_incremental_merge(PairReadMerge& merge, std::size_t maximum_slots)
        -> Result<std::size_t>;
    [[nodiscard]] static auto finish_incremental_merge(std::shared_ptr<const PairReadGeneration> current,
                                                       PairReadMerge& merge)
        -> Result<std::shared_ptr<const PairReadGeneration>>;
    [[nodiscard]] static auto merge_ready(const PairReadMerge& merge) noexcept -> bool;
    [[nodiscard]] static auto merge_post_entries(const PairReadMerge& merge) noexcept -> std::size_t;
    /// Exact outstanding scan/initialization work and worst-case publication
    /// capacity of the post-cut delta. The Writer uses both to amortize merge
    /// debt before consuming bounded post-cut capacity instead of deferring it
    /// to a terminal burst.
    [[nodiscard]] static auto merge_remaining_slots(const PairReadMerge& merge) noexcept -> std::size_t;
    [[nodiscard]] static auto merge_post_capacity_remaining(const PairReadMerge& merge) noexcept
        -> std::size_t;
    [[nodiscard]] static auto merge_advance_budget(const PairReadMerge& merge,
                                                   std::size_t maximum_new_records,
                                                   std::size_t minimum_slots) noexcept -> std::size_t;
    [[nodiscard]] static auto can_publish_incremental(const PairReadGeneration& current,
                                                      const PairReadMerge* merge,
                                                      std::size_t maximum_new_entries) noexcept -> bool;

    /// Direct-object constructors for GenerationSlotPool (no shared control block).
    [[nodiscard]] static auto empty_direct(WorkerRoutingState routing, GenerationDirectStorage& storage)
        -> Result<const PairReadGeneration*>;
    [[nodiscard]] static auto
    from_durable_snapshot_direct(WorkerRoutingState routing,
                                 std::span<const DurableRuntimeCatalog::PublishedReadRecord> records,
                                 GenerationDirectStorage& storage) -> Result<const PairReadGeneration*>;
    [[nodiscard]] static auto
    replace_durable_snapshot_direct(const PairReadGeneration& previous,
                                    std::span<const DurableRuntimeCatalog::PublishedReadRecord> records,
                                    GenerationDirectStorage& storage) -> Result<const PairReadGeneration*>;
    [[nodiscard]] static auto finish_incremental_merge_direct(const PairReadGeneration& current,
                                                              PairReadMerge& merge,
                                                              GenerationDirectStorage& storage)
        -> Result<const PairReadGeneration*>;
    static void destroy_direct(const PairReadGeneration* generation,
                               GenerationDirectStorage& storage) noexcept;

    /**
     * @brief Read from this immutable generation and return independent bytes; expiration uses absolute Unix
     * now_ns.
     */
    [[nodiscard]] auto get(const HashedKey& key, std::uint64_t now_ns) const -> Result<OwnedValue>;
    /**
     * @brief Borrow the durable reference/pin view; retain this generation lease through preparation or copy
     * the pin into owning cold state.
     */
    [[nodiscard]] auto prepare_durable(const HashedKey& key) const
        -> Result<DurableRuntimeCatalog::PublishedReadView>;

    [[nodiscard]] auto epoch() const noexcept -> std::uint64_t {
        return epoch_;
    }

    [[nodiscard]] auto visible_through() const noexcept -> std::uint64_t {
        return visible_through_;
    }

    [[nodiscard]] auto delta_entries() const noexcept -> std::size_t;
    [[nodiscard]] auto delta_record_versions() const noexcept -> std::size_t;
    [[nodiscard]] auto delta_arena_record_bytes() const noexcept -> std::size_t;
    [[nodiscard]] auto delta_arena_key_bytes() const noexcept -> std::size_t;
    [[nodiscard]] auto delta_arena_key_storage_bytes() const noexcept -> std::size_t;
    [[nodiscard]] auto base_entries() const noexcept -> std::size_t;
    [[nodiscard]] auto memory_stats() const noexcept -> ReadGenerationMemoryStats;

  private:
    [[nodiscard]] static auto
    build_durable_snapshot(WorkerRoutingState routing,
                           std::span<const DurableRuntimeCatalog::PublishedReadRecord> records,
                           std::uint64_t epoch, std::uint64_t visible_floor)
        -> Result<std::shared_ptr<const PairReadGeneration>>;
    [[nodiscard]] static auto
    build_durable_snapshot_direct(WorkerRoutingState routing,
                                  std::span<const DurableRuntimeCatalog::PublishedReadRecord> records,
                                  std::uint64_t epoch, std::uint64_t visible_floor,
                                  GenerationDirectStorage& storage) -> Result<const PairReadGeneration*>;
    /// Shared/direct publication only. ADR 0036 lab shell allocators live in
    /// experimental/pair_read_generation_shell_access.cpp via ShellAccess.
    [[nodiscard]] static auto publish_incremental_construct(
        const PairReadGeneration& previous, std::shared_ptr<const PairReadGeneration> previous_owner,
        std::span<const ReadMutation> mutations, PairReadMerge* merge,
        GenerationDirectStorage* direct_storage, const PairReadGeneration** direct_result)
        -> Result<std::shared_ptr<const PairReadGeneration>>;
    [[nodiscard]] static auto publish_incremental_direct(const PairReadGeneration& previous,
                                                         std::span<const ReadMutation> mutations,
                                                         GenerationDirectStorage& storage)
        -> Result<const PairReadGeneration*>;
    /// Generation + embedded DeltaState are co-allocated in the .cpp via a private
    /// derived helper. delta_ points into that storage for the object's lifetime.
    PairReadGeneration(WorkerRoutingState routing, std::shared_ptr<const ImmutableReadIndex> base,
                       const DeltaState* delta, std::uint64_t epoch, std::uint64_t visible_through) noexcept;

    void bind_delta(const DeltaState* delta) noexcept {
        delta_ = delta;
    }

    WorkerRoutingState routing_{};
    std::shared_ptr<const ImmutableReadIndex> base_;
    const DeltaState* delta_{};
    std::uint64_t epoch_{};
    std::uint64_t visible_through_{};

    /// Allow the .cpp co-allocation helper to construct and bind embedded delta storage.
    friend struct PairReadGenerationEnableShared;
    friend struct IncrementalPublicationAccess;
    friend struct experimental::PairReadGenerationShellAccess;
    friend class GenerationSlotPool;
};

/// Opaque Writer-owned state. It is never published to or touched by Reader.
class PairReadMerge final {
  public:
    ~PairReadMerge();
    PairReadMerge(const PairReadMerge&) = delete;
    auto operator=(const PairReadMerge&) -> PairReadMerge& = delete;
    PairReadMerge(PairReadMerge&&) noexcept;
    auto operator=(PairReadMerge&&) noexcept -> PairReadMerge&;

  private:
    struct State;
    explicit PairReadMerge(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;

    friend class PairReadGeneration;
    friend struct IncrementalPublicationAccess;
};

} // namespace glifistore::store::paired
