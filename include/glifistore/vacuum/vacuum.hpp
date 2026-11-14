/**
 * @file
 * @brief Volatile Segment candidate selection and private Index/Segment rebuilding.
 */
#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/index/index.hpp"
#include "glifistore/segment/segment.hpp"

#include <cstddef>
#include <functional>
#include <span>
#include <vector>

namespace glifistore {

/**
 * @brief Volatile sealed-Segment selection threshold and per-run candidate count bound.
 */
struct VacuumPolicy {
    double maximum_live_ratio{0.25};
    std::size_t maximum_segments_per_run{4};
};

/**
 * @brief Source verification, live copy, expiration, and physical Segment gain observations.
 */
struct VacuumStats {
    std::uint64_t source_records_verified{};
    std::uint64_t source_bytes_verified{};
    std::uint64_t records_copied{};
    std::uint64_t bytes_copied{};
    std::uint64_t expired_records_dropped{};
    std::uint64_t old_segments{};
    std::uint64_t new_segments{};
};

/**
 * @brief Private rebuilt Index/Segment ownership and counters; the caller controls publication.
 */
struct VacuumResult {
    Index index;
    std::vector<SegmentPtr> segments;
    VacuumStats stats;
};

/**
 * @brief Select sparse sealed candidates without publishing a replacement history.
 */
class VacuumPlanner final {
  public:
    explicit VacuumPlanner(VacuumPolicy policy = {}) : policy_(policy) {}

    [[nodiscard]] auto candidates(std::span<const SegmentPtr> segments) const -> std::vector<SegmentId>;

  private:
    VacuumPolicy policy_;
};

/**
 * @brief Copy-build replacement storage and Index state before caller-owned publication.
 */
class VacuumBuilder final {
  public:
    using SegmentAllocator = std::function<Result<SegmentPtr>()>;

    [[nodiscard]] auto rebuild(const Index& current_index, std::span<const SegmentPtr> current_segments,
                               SegmentId first_new_segment, WorkerId owner = {}) const
        -> Result<VacuumResult>;
    [[nodiscard]] auto rebuild(const Index& current_index, std::span<const SegmentPtr> current_segments,
                               std::span<const SegmentId> candidates, SegmentAllocator allocate_segment,
                               std::uint64_t now_ns = 0) const -> Result<VacuumResult>;
};

} // namespace glifistore
