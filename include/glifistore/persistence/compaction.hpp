/**
 * @file
 * @brief Whole-Worker sealed-history planning and exact replacement layout.
 */
#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/persistence/manifest.hpp"
#include "glifistore/store/config.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace glifistore {

/// A v1 compaction replaces every sealed Segment owned by one Worker in one
/// manifest generation. Replacements reuse the earliest source Segment IDs
/// with incremented generations so their sequence ranges remain before the
/// Worker's active Segment without changing the v1 manifest format.
struct DurableCompactionPlan {
    WorkerId worker_id{};
    Manifest next_manifest;
    std::vector<ManifestSegmentEntry> sources{};
    std::vector<ManifestSegmentEntry> replacements{};
    std::uint64_t temporary_bytes{};
    std::uint64_t reclaimed_bytes{};
};

/**
 * @brief Exact destination placement assigned to one source Record during layout.
 */
struct DurableCompactionPlacement {
    std::size_t segment_index{};
    RecordOffset offset{};
};

/// Exact, allocation-free next-fit layout for sequence-ordered v1 Records.
/// Aggregate byte division is only a lower bound because a Record cannot span
/// two Segment payloads.
class DurableCompactionLayout final {
  public:
    [[nodiscard]] auto add_record(std::uint32_t encoded_size) -> Result<DurableCompactionPlacement>;

    [[nodiscard]] auto segment_count() const noexcept -> std::size_t {
        return segment_count_;
    }

    [[nodiscard]] auto encoded_bytes() const noexcept -> std::uint64_t {
        return encoded_bytes_;
    }

  private:
    std::size_t segment_count_{};
    std::uint64_t encoded_bytes_{};
    std::uint32_t current_payload_bytes_{};
};

/// Aggregate-byte lower bound. Use DurableCompactionLayout when Record sizes
/// are known and an exact replacement count is required.
[[nodiscard]] auto durable_compaction_output_segments(std::uint64_t live_encoded_bytes)
    -> Result<std::size_t>;

/// Returns the deterministic replacement count when next is exactly the v1
/// whole-Worker transition derived from current.
[[nodiscard]] auto validate_durable_compaction_transition(const Manifest& current, const Manifest& next,
                                                          WorkerId worker_id) -> Result<std::size_t>;

/**
 * @brief Plan a whole-Worker rewrite and validate identity, temporary-space, and resource bounds before
 * intent publication.
 */
[[nodiscard]] auto
plan_durable_worker_compaction(const Manifest& current, WorkerId worker_id, std::size_t output_segment_count,
                               const DurableResourceLimits& limits, std::uint64_t blob_reclaim_bytes = 0)
    -> Result<DurableCompactionPlan>;

} // namespace glifistore
