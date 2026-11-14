#include "glifistore/persistence/compaction.hpp"
#include "glifistore/persistence/resource_limits.hpp"
#include "persistence/compaction/resource_estimator.hpp"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace glifistore {

auto select_durable_compaction_prefix(const Manifest& current, const WorkerId worker_id,
                                      const std::span<const IndexEntry> entries,
                                      const std::uint64_t max_copy_bytes)
    -> Result<DurableCompactionPrefixChoice> {
    std::vector<ManifestSegmentEntry> sealed;
    for (const auto& entry : current.segments) {
        if (entry.owner_worker == worker_id && entry.role == ManifestSegmentRole::sealed) {
            sealed.push_back(entry);
        }
    }
    // Manifest entries are ordered by Segment ID, so `sealed` is too.
    std::vector<std::uint64_t> live_bytes(sealed.size());
    for (const auto& entry : entries) {
        const auto found = std::ranges::lower_bound(
            sealed, entry.record.segment_id.value, {},
            [](const ManifestSegmentEntry& segment) { return segment.segment_id.value; });
        if (found == sealed.end() || found->segment_id != entry.record.segment_id ||
            found->generation != entry.record.generation) {
            continue;
        }
        auto& bytes = live_bytes[static_cast<std::size_t>(found - sealed.begin())];
        if (entry.record.size.value > std::numeric_limits<std::uint64_t>::max() - bytes) {
            return fail(ErrorCode::arithmetic_overflow, "durable compaction live byte estimate overflow");
        }
        bytes += entry.record.size.value;
    }

    // A v2 blob profile and an unbounded budget both admit the whole set; the
    // blob profile may reclaim blob bytes without a Segment gain (ADR 0042).
    if (current.format_version == 2) {
        std::uint64_t total{};
        for (const auto bytes : live_bytes) {
            if (bytes > std::numeric_limits<std::uint64_t>::max() - total) {
                return fail(ErrorCode::arithmetic_overflow, "durable compaction live byte estimate overflow");
            }
            total += bytes;
        }
        if (sealed.empty() || (max_copy_bytes != 0 && total > max_copy_bytes)) {
            return DurableCompactionPrefixChoice{};
        }
        return DurableCompactionPrefixChoice{.source_count = sealed.size(), .live_bytes = total};
    }

    DurableCompactionPrefixChoice best{};
    DurableCompactionPrefixChoice longest_within_budget{};
    std::uint64_t cumulative{};
    for (std::size_t index = 0; index < sealed.size(); ++index) {
        if (live_bytes[index] > std::numeric_limits<std::uint64_t>::max() - cumulative) {
            return fail(ErrorCode::arithmetic_overflow, "durable compaction live byte estimate overflow");
        }
        cumulative += live_bytes[index];
        if (max_copy_bytes != 0 && cumulative > max_copy_bytes) {
            break;
        }
        longest_within_budget = {.source_count = index + 1U, .live_bytes = cumulative};
        const auto outputs = durable_compaction_output_segments(cumulative);
        if (!outputs) {
            return unexpected(outputs.error());
        }
        const auto prefix_length = index + 1U;
        if (*outputs >= prefix_length) {
            continue;
        }
        const auto reclaimed = prefix_length - *outputs;
        if (reclaimed > best.estimated_reclaimed_segments) {
            best = {.source_count = prefix_length,
                    .live_bytes = cumulative,
                    .estimated_reclaimed_segments = reclaimed};
        }
    }
    // The Index carries no expiry, so the estimate cannot see expired puts that
    // the exact layout drops. Without an estimated gain, let the exact layout of
    // the longest admissible prefix decide (it reports an exact no-gain).
    return best.source_count != 0 ? best : longest_within_budget;
}

auto plan_durable_worker_compaction(const Manifest& current, const WorkerId worker_id,
                                    const std::size_t output_segment_count,
                                    const DurableResourceLimits& limits,
                                    const std::uint64_t blob_reclaim_bytes, const std::size_t source_count)
    -> Result<DurableCompactionPlan> {
    if (auto valid = validate_durable_manifest_resources(current, limits); !valid) {
        return unexpected(valid.error());
    }
    if (worker_id.value >= current.worker_count) {
        return fail(ErrorCode::invalid_argument,
                    "durable compaction Worker is outside the manifest Worker set");
    }
    if (current.manifest_generation == std::numeric_limits<std::uint64_t>::max()) {
        return fail(ErrorCode::arithmetic_overflow, "durable compaction manifest generation is exhausted");
    }

    DurableCompactionPlan plan{.worker_id = worker_id, .next_manifest = current};
    std::size_t sealed_count{};
    for (const auto& entry : current.segments) {
        if (entry.owner_worker == worker_id && entry.role == ManifestSegmentRole::sealed) {
            ++sealed_count;
        }
    }
    if (sealed_count == 0) {
        return fail(ErrorCode::not_found, "durable Worker has no sealed Segments to compact");
    }
    const auto selected = source_count == 0 ? sealed_count : source_count;
    if (selected > sealed_count) {
        return fail(ErrorCode::invalid_argument,
                    "durable compaction prefix exceeds the Worker's sealed Segment set");
    }
    if (current.format_version == 2 && selected != sealed_count) {
        return fail(ErrorCode::invalid_argument,
                    "blob-profile durable compaction must replace the complete sealed set");
    }
    for (const auto& entry : current.segments) {
        if (plan.sources.size() < selected && entry.owner_worker == worker_id &&
            entry.role == ManifestSegmentRole::sealed) {
            plan.sources.push_back(entry);
        }
    }
    if (output_segment_count > plan.sources.size()) {
        return fail(ErrorCode::invalid_argument, "durable compaction output exceeds its sealed source set");
    }
    if (auto amplification =
            validate_compaction_write_amplification(plan.sources.size(), output_segment_count, limits,
                                                    current.format_version == 2 ? blob_reclaim_bytes : 0);
        !amplification) {
        return unexpected(amplification.error());
    }

    const DurableCompactionShape shape{.source_count = selected, .replacement_count = output_segment_count};
    auto next = canonical_durable_compaction_manifest(current, worker_id, shape);
    if (!next) {
        return unexpected(next.error());
    }
    plan.next_manifest = std::move(*next);
    for (std::size_t index = 0; index < output_segment_count; ++index) {
        auto replacement = plan.sources[index];
        ++replacement.generation.value;
        plan.replacements.push_back(replacement);
    }

    auto estimate =
        estimate_compaction_resources(current, plan.next_manifest, plan.sources.size(), output_segment_count);
    if (!estimate) {
        return unexpected(estimate.error());
    }
    if (auto valid = validate_compaction_resources(*estimate, limits); !valid) {
        return unexpected(valid.error());
    }
    plan.temporary_bytes = estimate->temporary_bytes;
    if (blob_reclaim_bytes > std::numeric_limits<std::uint64_t>::max() - estimate->reclaimed_bytes) {
        return fail(ErrorCode::arithmetic_overflow, "compaction total reclaim overflow");
    }
    plan.reclaimed_bytes = estimate->reclaimed_bytes + (current.format_version == 2 ? blob_reclaim_bytes : 0);

    if (auto encoded = encoded_manifest_size(plan.next_manifest); !encoded) {
        return unexpected(encoded.error());
    }
    const auto transition = derive_durable_compaction_shape(current, plan.next_manifest, worker_id);
    if (!transition) {
        return unexpected(transition.error());
    }
    if (*transition != shape) {
        return fail(ErrorCode::internal_error,
                    "durable compaction planner produced the wrong replacement count");
    }
    return plan;
}

} // namespace glifistore
