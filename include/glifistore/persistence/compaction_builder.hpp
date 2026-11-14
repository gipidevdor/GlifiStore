/**
 * @file
 * @brief Private replacement construction, intent gating, and bounded copy pacing.
 */
#pragma once

#include "glifistore/index/index.hpp"
#include "glifistore/persistence/compaction.hpp"
#include "glifistore/persistence/filesystem.hpp"
#include "glifistore/persistence/segment_image.hpp"
#include "glifistore/segment/segment_header.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace glifistore {

/**
 * @brief Distinguish a private/prepared rewrite from policy no-gain and recovery-required authority
 * uncertainty.
 */
enum class DurableCompactionBuildOutcome { prepared, not_started, not_beneficial, recovery_required };

/**
 * @brief Per-attempt verified/copied bytes, timing, pacing, prefix and remap observations.
 */
struct DurableCompactionCopyStats {
    std::uint64_t source_index_records_verified{};
    std::uint64_t source_bytes_verified{};
    std::uint64_t records_copied{};
    std::uint64_t bytes_copied{};
    std::uint64_t expired_records_dropped{};
    /// Wall time spent before requesting the global Manifest publication
    /// lease. This includes the exact scan, output copy, seal and verification.
    std::uint64_t pre_intent_duration_ns{};
    /// Wall time for the recovery-sensitive intent -> promotion -> Manifest ->
    /// retirement -> intent removal window. Filled by the runtime owner.
    std::uint64_t publication_lease_duration_ns{};
    /// Delay deliberately injected before private staged Record writes by the
    /// normal-mode byte-rate limiter. Pacing never runs after intent authority.
    std::uint64_t pacing_delay_ns{};
    std::uint64_t pacing_sleep_count{};
    std::uint64_t pacing_burst_bytes{};
    /// Conservative lower bound for peak transient compaction metadata. It
    /// covers fixed vector storage, output Index tables/arena and Record scratch;
    /// allocator metadata and heap storage owned by copied std::strings are not
    /// included and must be measured separately by an allocation census.
    std::uint64_t transient_metadata_lower_bound_bytes{};
    std::uint64_t snapshot_duration_ns{};
    std::uint64_t commit_gate_duration_ns{};
    /// Retired with ADR 0044 (no concurrent journal exists); always zero.
    std::uint64_t delta_replay_duration_ns{};
    std::uint64_t delta_records{};
    std::uint64_t delta_key_bytes{};
    std::uint64_t delta_overflow_aborts{};
    /// Sealed Segments selected as the oldest prefix, and those left after it.
    std::uint64_t source_segments{};
    std::uint64_t retained_sealed_segments{};
    /// Commit-time conditional remap (ADR 0044): Index entries retargeted to
    /// their copy, copies already superseded by a foreground mutation (now dead
    /// output bytes), and expired source puts erased from the Index.
    std::uint64_t remapped_records{};
    std::uint64_t superseded_copies{};
    std::uint64_t superseded_copy_bytes{};
    std::uint64_t expired_index_erasures{};
    std::uint64_t remap_duration_ns{};
    /// 1 when the intent was rebased onto an authority newer than the snapshot.
    std::uint64_t manifest_rebases{};
    std::uint64_t manifest_conflict_aborts{};
    std::uint64_t abandoned_copy_bytes{};
};

/// One sealed source Record selected by the Index snapshot. `output` names its
/// staged copy; an empty `output` marks an expired put that was not copied.
/// After the next Manifest is durable, the runtime retargets (or erases) the
/// Index entry only if it still equals `source` (ADR 0044 conditional remap).
struct DurableCompactionRemap {
    std::string key;
    std::uint64_t key_hash{};
    RecordRef source{};
    std::optional<RecordRef> output;
};

/// A successful build leaves the old manifest authoritative, a durable
/// compaction intent installed, and every replacement sealed and revalidated.
/// `plan.next_manifest` may be rebased onto a newer old authority than the
/// snapshot when unrelated rotations happened during the copy.
struct PreparedDurableCompaction {
    DurableCompactionPlan plan;
    Manifest old_manifest;
    std::vector<DurableCompactionRemap> remaps;
    std::vector<SelectedSegmentCommit> replacement_commits;
    /// Resident images aligned with plan.replacements; null entries start cold (ADR 0046).
    std::vector<std::shared_ptr<SegmentImage>> replacement_images{};
    DurableCompactionCopyStats stats;
};

/**
 * @brief Build outcome, optional prepared transaction, and explicit failure context.
 */
struct DurableCompactionBuildResult {
    DurableCompactionBuildOutcome outcome{DurableCompactionBuildOutcome::not_started};
    std::optional<PreparedDurableCompaction> prepared;
    DurableCompactionCopyStats stats{};
    std::optional<Error> error;

    [[nodiscard]] auto succeeded() const noexcept -> bool {
        return outcome == DurableCompactionBuildOutcome::prepared && prepared.has_value();
    }
};

/// Optional runtime gate acquired after the complete staged output copy, seal
/// and verification, immediately before the durable intent. `acquire` returns
/// the current authoritative Manifest, which becomes the intent's old authority
/// when it still contains every source as the Worker's oldest sealed prefix.
/// The gate must remain owned until the caller has either rolled the prepared
/// transaction back or completed the v1 transaction. A rejected gate performs
/// no authoritative persistent namespace mutation; private temporary files are
/// discarded.
struct DurableCompactionIntentGate {
    void* context{};
    auto (*acquire)(void* context) -> Result<Manifest>{};
};

/**
 * @brief Rate bound applied only to private replacement writes before intent authority.
 */
struct DurableCompactionPacing final {
    /// Zero disables pacing. A nonzero value applies only to replacement
    /// Record writes under private staged names, before intent publication.
    std::uint64_t bytes_per_second{};
};

/// Request for one sealed-prefix transaction. `entries` is an owning Index
/// snapshot restricted to the selected prefix (other entries are rejected).
/// `source_count == 0` selects the complete sealed set.
struct DurableCompactionBuildRequest {
    std::size_t source_count{};
    std::uint64_t now_ns{};
    DurableCompactionIntentGate intent_gate{};
    DurableCompactionPacing pacing{};
    /// Reserve for output images mirrored during the copy; null builds every output cold.
    std::shared_ptr<MemoryReserve> image_reserve{};
    /// Image bytes held by the sources, which outputs may borrow until the sources retire; null
    /// admits outputs within the plain budget.
    std::shared_ptr<CompactionImageCredit> image_credit{};
};

/// Before the durable intent, failures are not_started and publish no
/// replacement name. not_beneficial is a successful policy decision made
/// before the intent when the exact output layout would reclaim no physical
/// Segment. Once intent publication may have occurred, failures are
/// recovery_required; the runtime must fail closed and let restart resolve the
/// old authority.
[[nodiscard]] auto build_durable_worker_compaction(DataDirectory& directory, const Manifest& current,
                                                   WorkerId worker_id, std::vector<IndexEntry> entries,
                                                   const DurableResourceLimits& limits,
                                                   const DurableCompactionBuildRequest& request)
    -> DurableCompactionBuildResult;

/// Whole-sealed-set convenience overload for an externally immutable Index.
[[nodiscard]] auto build_durable_worker_compaction(DataDirectory& directory, const Manifest& current,
                                                   WorkerId worker_id, const Index& current_index,
                                                   std::uint64_t now_ns,
                                                   const DurableResourceLimits& limits = {})
    -> DurableCompactionBuildResult;

/// Restricts an owning Index snapshot to references inside the oldest
/// `source_count` sealed Segments of the Worker (0 = complete sealed set).
[[nodiscard]] auto durable_compaction_prefix_entries(const Manifest& current, WorkerId worker_id,
                                                     std::size_t source_count,
                                                     std::vector<IndexEntry> entries)
    -> std::vector<IndexEntry>;

} // namespace glifistore
