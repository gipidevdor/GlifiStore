/**
 * @file
 * @brief Bounded reconstruction of Worker state from authoritative committed Segment extents.
 */
#pragma once

#include "glifistore/index/index.hpp"
#include "glifistore/persistence/filesystem.hpp"
#include "glifistore/persistence/manifest.hpp"
#include "glifistore/persistence/namespace_audit.hpp"
#include "glifistore/segment/segment_header.hpp"
#include "glifistore/store/config.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace glifistore {

/**
 * @brief Chosen valid commit slot for the corresponding Manifest Segment entry.
 */
struct RecoveredSegmentState {
    SelectedSegmentCommit selected;
};

/**
 * @brief Owning rebuilt partition state and next mutation/active Segment metadata.
 */
struct RecoveredWorkerState {
    WorkerId worker_id;
    Index index;
    SequenceNumber next_sequence;
    SegmentId active_segment;
    bool active_requires_rotation{};
    std::uint64_t active_live_record_bytes{};
    std::uint64_t sealed_live_record_bytes{};
};

/**
 * @brief Recovery scan totals and count of Workers needing active-Segment rotation.
 */
struct DurableRecoveryStats {
    RebuildStats rebuild;
    std::size_t segments_scanned{};
    std::size_t workers_requiring_rotation{};
};

/**
 * @brief Owning reconstructed state from the authoritative Manifest and committed extents.
 */
struct DurableRecoveryState {
    Manifest manifest;
    NamespaceAuditReport namespace_audit;
    /// Index-for-index aligned with manifest.segments; catalog identity is not duplicated.
    std::vector<RecoveredSegmentState> segments;
    std::vector<RecoveredWorkerState> workers;
    DurableRecoveryStats stats;
};

/// DataDirectory must remain alive while any recovered RecordRef is used to
/// reopen its backing Segment. Recovery itself keeps at most one Segment file
/// descriptor and one Worker's temporary latest-key map live at a time.
[[nodiscard]] auto recover_durable_state(DataDirectory& directory, std::uint64_t now_ns = 0,
                                         const DurableResourceLimits& limits = {},
                                         const DurableCompactionIntent* compaction_intent = nullptr)
    -> Result<DurableRecoveryState>;

} // namespace glifistore
