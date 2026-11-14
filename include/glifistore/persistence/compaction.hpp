/**
 * @file
 * @brief Oldest sealed-prefix compaction planning and exact replacement layout (ADR 0015, 0044).
 */
#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/index/index_types.hpp"
#include "glifistore/persistence/manifest.hpp"
#include "glifistore/store/config.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace glifistore {

/// Shape of one canonical transition: the oldest `source_count` sealed Segments
/// of a Worker are replaced by `replacement_count <= source_count` outputs.
/// A `source_count` equal to the complete sealed set is the ADR 0015 transaction.
struct DurableCompactionShape {
    std::size_t source_count{};
    std::size_t replacement_count{};

    friend auto operator==(const DurableCompactionShape&, const DurableCompactionShape&) -> bool = default;
};

/// A v1 compaction replaces the oldest sealed prefix owned by one Worker in one
/// manifest generation (ADR 0044). Every Record of that Worker outside the
/// prefix is newer than every Record inside it, so dropping obsolete prefix
/// Records cannot resurrect older state. Replacements reuse the earliest source
/// Segment IDs with incremented generations and take the sources' Manifest
/// positions, so sequence ranges stay ordered without changing the v1 format.
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

/// Builds the canonical next Manifest for `shape` without resource checks.
[[nodiscard]] auto canonical_durable_compaction_manifest(const Manifest& current, WorkerId worker_id,
                                                         DurableCompactionShape shape) -> Result<Manifest>;

/// Derives the unique prefix shape when next is exactly a canonical v1
/// transition of current. The blob profile (format 2) admits only the complete
/// sealed set.
[[nodiscard]] auto derive_durable_compaction_shape(const Manifest& current, const Manifest& next,
                                                   WorkerId worker_id) -> Result<DurableCompactionShape>;

/// Returns the replacement count of a canonical transition.
[[nodiscard]] auto validate_durable_compaction_transition(const Manifest& current, const Manifest& next,
                                                          WorkerId worker_id) -> Result<std::size_t>;

/// ADR 0044 prefix choice. For every prefix length `k` of the Worker's sealed
/// Segments it attributes the snapshot's live Record bytes, estimates the
/// output count with the aggregate lower bound, and keeps the `k` reclaiming
/// the most Segments with live bytes within `max_copy_bytes` (0 = unbounded).
/// Ties prefer the shorter prefix. The Index carries no expiry, so when no
/// prefix promises a gain it returns the longest admissible prefix with zero
/// estimated reclaim and lets the exact layout decide. `source_count == 0`
/// means even the oldest Segment exceeds the budget.
struct DurableCompactionPrefixChoice {
    std::size_t source_count{};
    std::uint64_t live_bytes{};
    std::size_t estimated_reclaimed_segments{};
};

[[nodiscard]] auto select_durable_compaction_prefix(const Manifest& current, WorkerId worker_id,
                                                    std::span<const IndexEntry> entries,
                                                    std::uint64_t max_copy_bytes)
    -> Result<DurableCompactionPrefixChoice>;

/**
 * @brief Plan a sealed-prefix rewrite and validate identity, temporary-space, and resource bounds before
 * intent publication. `source_count == 0` selects the complete sealed set.
 */
[[nodiscard]] auto
plan_durable_worker_compaction(const Manifest& current, WorkerId worker_id, std::size_t output_segment_count,
                               const DurableResourceLimits& limits, std::uint64_t blob_reclaim_bytes = 0,
                               std::size_t source_count = 0) -> Result<DurableCompactionPlan>;

} // namespace glifistore
