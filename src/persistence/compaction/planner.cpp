#include "glifistore/persistence/compaction.hpp"
#include "glifistore/persistence/resource_limits.hpp"
#include "persistence/compaction/resource_estimator.hpp"

#include <limits>
#include <utility>

namespace glifistore {

auto plan_durable_worker_compaction(const Manifest& current, const WorkerId worker_id,
                                    const std::size_t output_segment_count,
                                    const DurableResourceLimits& limits,
                                    const std::uint64_t blob_reclaim_bytes) -> Result<DurableCompactionPlan> {
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
    for (const auto& entry : current.segments) {
        if (entry.owner_worker == worker_id && entry.role == ManifestSegmentRole::sealed) {
            plan.sources.push_back(entry);
        }
    }
    if (plan.sources.empty()) {
        return fail(ErrorCode::not_found, "durable Worker has no sealed Segments to compact");
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

    plan.next_manifest.segments.clear();
    plan.next_manifest.segments.reserve(current.segments.size() - plan.sources.size() + output_segment_count);
    std::size_t replacements_remaining = output_segment_count;
    for (const auto& entry : current.segments) {
        if (entry.owner_worker != worker_id || entry.role != ManifestSegmentRole::sealed) {
            plan.next_manifest.segments.push_back(entry);
            continue;
        }
        if (replacements_remaining == 0) {
            continue;
        }
        if (entry.generation.value == std::numeric_limits<std::uint32_t>::max()) {
            return fail(ErrorCode::arithmetic_overflow, "durable compaction Segment generation is exhausted");
        }
        auto replacement = entry;
        ++replacement.generation.value;
        plan.replacements.push_back(replacement);
        plan.next_manifest.segments.push_back(replacement);
        --replacements_remaining;
    }
    ++plan.next_manifest.manifest_generation;

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
    const auto transition = validate_durable_compaction_transition(current, plan.next_manifest, worker_id);
    if (!transition) {
        return unexpected(transition.error());
    }
    if (*transition != output_segment_count) {
        return fail(ErrorCode::internal_error,
                    "durable compaction planner produced the wrong replacement count");
    }
    return plan;
}

} // namespace glifistore
