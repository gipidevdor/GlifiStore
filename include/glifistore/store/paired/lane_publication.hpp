/**
 * @file
 * @brief Shared mutation-publication decisions for synchronous and asynchronous lanes.
 */
#pragma once

// Shared Writer-side read-generation publication helpers (behavior-neutral extraction).
// Normative: docs/spec/mutation-lifecycle.md

#include "glifistore/core/error.hpp"
#include "glifistore/store/paired/generation_slot_pool.hpp"
#include "glifistore/store/paired/lane_state.hpp"
#include "glifistore/store/paired/publication_coordinator.hpp"
#include "glifistore/store/paired/read_generation.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>

namespace glifistore {
class Store;
}

namespace glifistore::store::paired {

/**
 * @brief Borrowed Writer-side generation/merge/Store state for one shard publication.
 */
struct LanePublicationContext final {
    GenerationState& generation;
    MergeState& merge;
    Store& store;
    std::size_t shard{};
    std::size_t maximum_retired_generations{};
};

void mirror_slot_pool_publication(GenerationState& generation) noexcept;

void store_generation_memory_stats(GenerationState& destination,
                                   const ReadGenerationMemoryStats& source) noexcept;

void store_merge_progress(MergeState& destination) noexcept;

[[nodiscard]] auto publish_slot_incremental(LanePublicationContext& context,
                                            GenerationSlotPool::Reservation& reservation,
                                            std::span<const ReadMutation> mutations) -> bool;

[[nodiscard]] auto publish_slot_direct(LanePublicationContext& context,
                                       GenerationSlotPool::Reservation& reservation,
                                       const PairReadGeneration* generation) -> bool;

[[nodiscard]] auto try_drain_durable_snapshot(LanePublicationContext& context, const bool allow_fail_closed,
                                              const std::function<void()>* after_drain) noexcept -> bool;

/// ADR 0036 dual-path containment (default Alternative A; opt-in slot pool behind flag).
/// Call sites must not branch on uses_slot_pool() for these operations; polarity of
/// deferred vs fail-closed remains caller-owned via DualPathPublishOutcome.
enum class DualPathPublishStatus : std::uint8_t {
    published,
    deferred,      // slot reservation / quiescence; no authority change
    build_failed,  // generation construction failed; may clear merge when applicable
    publish_failed // construction succeeded; release-store failed
};

/**
 * @brief Publication result shared by pointer and slot-pool paths; the caller classifies deferred/failure
 * polarity.
 */
struct DualPathPublishOutcome final {
    DualPathPublishStatus status{DualPathPublishStatus::published};
    ErrorCode build_error{ErrorCode::internal_error};
};

void reclaim_quiescent_generations(GenerationState& generation, ReclamationState& reclaim,
                                   bool reader_epoch_lease) noexcept;

[[nodiscard]] auto start_incremental_merge(GenerationState& generation,
                                           std::size_t merge_maximum_post_entries)
    -> Result<std::unique_ptr<PairReadMerge>>;

[[nodiscard]] auto try_reserve_publication_slot(GenerationState& generation) noexcept
    -> std::optional<GenerationSlotPool::Reservation>;

[[nodiscard]] auto
replace_durable_snapshot_and_publish(LanePublicationContext& context,
                                     std::span<const DurableRuntimeCatalog::PublishedReadRecord> records)
    -> DualPathPublishOutcome;

void note_catalog_snapshot_installed(LanePublicationContext& context,
                                     std::uint64_t catalog_revision) noexcept;

[[nodiscard]] auto finish_incremental_merge_and_publish(LanePublicationContext& context)
    -> DualPathPublishOutcome;

[[nodiscard]] auto publish_incremental_read_mutations(
    LanePublicationContext& context, std::span<const ReadMutation> mutations,
    std::optional<GenerationSlotPool::Reservation>& slot_reservation,
    const std::function<void(std::size_t publication_count)>* prepare_publish_retry = nullptr) -> bool;

[[nodiscard]] inline auto load_published_generation(const GenerationState& generation) noexcept
    -> const PairReadGeneration* {
    if (generation.uses_slot_pool()) {
        const auto token =
            GenerationPublicationToken{.raw = generation.published_token.load(std::memory_order_acquire)};
        const auto* decoded = generation.slot_pool->decode_published(token);
        if (decoded != nullptr) {
            return decoded;
        }
        return generation.published_generation.load(std::memory_order_relaxed);
    }
    return generation.published_generation.load(std::memory_order_acquire);
}

/// Dual-path Reader shutdown finalization for one lane generation (ADR 0036 opt-in).
[[nodiscard]] auto finalize_generation_reader_shutdown(GenerationState& generation) -> Status;

} // namespace glifistore::store::paired
