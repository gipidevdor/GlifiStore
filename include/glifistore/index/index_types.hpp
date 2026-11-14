/**
 * @file
 * @brief Owning Index snapshots, mutation results, and memory/probe statistics.
 */
#pragma once

#include "glifistore/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace glifistore {

/**
 * @brief Owning binary key and its current physical Record reference.
 */
struct IndexEntry {
    std::string key;
    RecordRef record;
};

/**
 * @brief Whether insertion created a key and the displaced reference, if any.
 */
struct IndexMutationResult {
    bool inserted{};
    std::optional<RecordRef> previous;
};

/**
 * @brief Point-in-time table occupancy, probe, and allocated/live storage observations.
 */
struct IndexStats {
    std::size_t size{};
    std::size_t bucket_count{};
    std::size_t deleted_count{};
    float load_factor{};
    float effective_load_factor{};
    std::size_t arena_allocated_bytes{};
    std::size_t arena_live_bytes{};
    std::size_t slot_bytes{};
    std::size_t table_allocated_bytes{};
    std::size_t maximum_probe_groups{};
    std::uint64_t rehash_count{};
    std::uint64_t tombstone_rebuild_count{};
};

/**
 * @brief Record scan, visible-state, tombstone, and expiration counts from Index reconstruction.
 */
struct RebuildStats {
    std::uint64_t records_scanned{};
    std::uint64_t records_visible{};
    std::uint64_t tombstones{};
    std::uint64_t expired{};
};

} // namespace glifistore
