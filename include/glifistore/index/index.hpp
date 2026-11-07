/**
 * @file
 * @brief Worker-owned logical Index and reconstruction from Segment histories.
 */
#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/core/types.hpp"
#include "glifistore/index/index_types.hpp"
#include "glifistore/index/swiss_table.hpp"
#include "glifistore/segment/segment.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace glifistore {

/// Logical Index partition owned by one Worker. Backed by SwissTableIndex.
class Index final {
  public:
    Index();
    explicit Index(std::uint64_t seed);
    explicit Index(WorkerRoutingState routing);
    Index(WorkerRoutingState routing, std::uint64_t seed);
    ~Index();
    Index(Index&&) noexcept;
    auto operator=(Index&&) noexcept -> Index&;
    Index(const Index&) = delete;
    auto operator=(const Index&) -> Index& = delete;

    [[nodiscard]] auto seed() const noexcept -> std::uint64_t;

    /**
     * @brief Look up an exact binary key without taking a lock; the owner supplies serialization.
     */
    [[nodiscard]] auto find(std::string_view key) const -> std::optional<RecordRef>;
    /**
     * @brief Look up an exact binary key without taking a lock; the owner supplies serialization.
     */
    [[nodiscard]] auto find(const HashedKey& key) const -> std::optional<RecordRef>;
    /**
     * @brief Publish a physical reference and return any displaced reference; no Segment pin is created.
     */
    [[nodiscard]] auto insert_or_assign(std::string_view key, RecordRef ref) -> Result<IndexMutationResult>;
    /**
     * @brief Publish a physical reference and return any displaced reference; no Segment pin is created.
     */
    [[nodiscard]] auto insert_or_assign(const HashedKey& key, RecordRef ref) -> Result<IndexMutationResult>;
    /**
     * @brief Remove an Index entry and return its former reference, if present.
     */
    auto erase(std::string_view key) -> IndexMutationResult;
    /**
     * @brief Remove an Index entry and return its former reference, if present.
     */
    auto erase(const HashedKey& key) -> IndexMutationResult;
    /**
     * @brief Erase without rebuilding table/arena storage, for allocation-prepared replay.
     */
    auto erase_no_compact(const HashedKey& key) -> IndexMutationResult;
    /**
     * @brief Reserve table capacity for count entries; capacity/allocation errors remain explicit.
     */
    [[nodiscard]] auto reserve(std::size_t count) -> Status;
    [[nodiscard]] auto prepare_insert(const HashedKey& key) -> Status;
    [[nodiscard]] auto prepare_batch_insert(std::size_t additional_entries,
                                            std::size_t additional_heap_key_bytes) -> Status;
    /**
     * @brief Allocate an owning key/reference snapshot; references still require independent Segment lifetime
     * ownership.
     */
    [[nodiscard]] auto entries() const -> std::vector<IndexEntry>;
    [[nodiscard]] auto stats() const noexcept -> IndexStats;
    [[nodiscard]] auto make_empty() const -> Index;

  private:
    explicit Index(SwissTableIndex table);

    SwissTableIndex table_;
};

/**
 * @brief Reconstructed Index ownership and the scan counters that produced it.
 */
struct RebuildResult {
    Index index;
    RebuildStats stats;
};

[[nodiscard]] auto rebuild_index_from_segments(std::span<const SegmentPtr> segments, std::uint64_t now_ns = 0,
                                               WorkerRoutingState routing = get_worker_routing())
    -> Result<RebuildResult>;

} // namespace glifistore
