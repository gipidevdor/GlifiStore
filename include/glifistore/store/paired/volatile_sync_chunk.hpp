/**
 * @file
 * @brief Bounded volatile synchronous mutation chunks and publication fallback policy.
 */
#pragma once

// Shared volatile sync publication chunk (behavior-neutral extraction).
// Normative: docs/spec/mutation-lifecycle.md

#include "glifistore/core/error.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/store/paired/generation_slot_pool.hpp"
#include "glifistore/store/paired/lane_publication.hpp"
#include "glifistore/store/paired/mutation_batch.hpp"
#include "glifistore/store/paired/read_generation.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>

namespace glifistore {
class Store;
}

namespace glifistore::store::paired {

/**
 * @brief Select the bounded volatile synchronous execution/publication path.
 */
enum class VolatileSyncChunkMode : std::uint8_t {
    // Dedicated Writer loop in run(): mid-chunk fail-closed, publish retry, litmus faults.
    dedicated_writer,
    // Combiner process_sync_lane(): simplified exception polarity.
    combiner,
};

/**
 * @brief Borrowed synchronous mutation inputs for one bounded volatile publication chunk.
 */
struct VolatileSyncMutationView final {
    /**
     * @brief Mutation operation carried by one borrowed volatile chunk item.
     */
    enum class Kind : std::uint8_t { put, erase };

    Kind kind{};
    const HashedKey* key{};
    std::span<const std::byte> value{};
    std::uint64_t expire_at_ns{};
    Status status{};
};

/// Applies Store volatile mutations and publishes one incremental generation for
/// the chunk. Mutates `views[*].status` and invokes `reclaim_after_publish` on
/// success. Caller completes `done` notification.
/// Hooks are non-owning pointers to caller-stable functors. Passing temporaries
/// or assigning std::function under process_fail_at is unsafe (noexcept drains).
void apply_volatile_sync_publication_chunk(
    Store& store, std::size_t shard, LanePublicationContext& publication,
    std::span<VolatileSyncMutationView> views,
    std::optional<GenerationSlotPool::Reservation>& slot_reservation, VolatileSyncChunkMode mode,
    std::atomic_bool& healthy, const std::function<void()>* publish_fail_closed,
    const std::function<void()>* reclaim_after_publish,
    const std::function<void(std::size_t publication_count)>* prepare_publish_retry);

} // namespace glifistore::store::paired
