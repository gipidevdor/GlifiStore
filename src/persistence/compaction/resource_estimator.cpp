#include "persistence/compaction/resource_estimator.hpp"

#include "glifistore/persistence/compaction_intent.hpp"
#include "glifistore/persistence/resource_limits.hpp"
#include "glifistore/segment/segment_header.hpp"

#include <limits>

namespace glifistore {
namespace {

[[nodiscard]] auto checked_add(const std::uint64_t left, const std::uint64_t right, const char* description)
    -> Result<std::uint64_t> {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return fail(ErrorCode::arithmetic_overflow, description);
    }
    return left + right;
}

[[nodiscard]] auto checked_multiply(const std::uint64_t left, const std::uint64_t right,
                                    const char* description) -> Result<std::uint64_t> {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return fail(ErrorCode::arithmetic_overflow, description);
    }
    return left * right;
}

[[nodiscard]] auto segment_bytes(const std::size_t count, const char* description) -> Result<std::uint64_t> {
    return checked_multiply(static_cast<std::uint64_t>(count), static_cast<std::uint64_t>(kSegmentSizeBytes),
                            description);
}

} // namespace

auto validate_compaction_write_amplification(const std::size_t source_count, const std::size_t output_count,
                                             const DurableResourceLimits& limits,
                                             const std::uint64_t blob_reclaim_bytes) -> Status {
    if (output_count > source_count) {
        return fail(ErrorCode::invalid_argument, "compaction output exceeds sources");
    }
    const auto temporary = segment_bytes(output_count, "compaction temporary byte count overflow");
    if (!temporary) {
        return unexpected(temporary.error());
    }
    const auto reclaimed =
        segment_bytes(source_count - output_count, "compaction reclaimed byte count overflow");
    if (!reclaimed) {
        return unexpected(reclaimed.error());
    }
    const auto total_reclaimed =
        checked_add(*reclaimed, blob_reclaim_bytes, "compaction blob reclaim overflow");
    if (!total_reclaimed) {
        return unexpected(total_reclaimed.error());
    }
    if (*temporary == 0) {
        return {};
    }
    if (*total_reclaimed == 0) {
        return fail(ErrorCode::storage_exhausted, "durable compaction would not reclaim a physical Segment");
    }
    if (*total_reclaimed <= std::numeric_limits<std::uint64_t>::max() / limits.max_write_amplification &&
        *temporary > *total_reclaimed * limits.max_write_amplification) {
        return fail(ErrorCode::storage_exhausted,
                    "durable compaction exceeds the configured write-amplification limit");
    }
    return {};
}

auto estimate_compaction_resources(const Manifest& current, const Manifest& next,
                                   const std::size_t source_count, const std::size_t output_count)
    -> Result<CompactionResourceEstimate> {
    const auto temporary = segment_bytes(output_count, "compaction temporary byte count overflow");
    const auto reclaimed =
        segment_bytes(source_count - output_count, "compaction reclaimed byte count overflow");
    if (!temporary || !reclaimed) {
        return unexpected((!temporary ? temporary.error() : reclaimed.error()));
    }

    const auto current_segments =
        segment_bytes(current.segments.size(), "compaction Store byte count overflow");
    const auto current_manifest = durable_manifest_bytes(current.segments.size());
    const auto next_manifest = durable_manifest_bytes(next.segments.size());
    if (!current_segments || !current_manifest || !next_manifest) {
        if (!current_segments) {
            return unexpected(current_segments.error());
        }
        return unexpected((!current_manifest ? current_manifest.error() : next_manifest.error()));
    }
    auto peak = checked_add(*current_segments, *temporary, "compaction peak Store byte count overflow");
    if (peak) {
        peak = checked_add(*peak, *current_manifest, "compaction peak Store byte count overflow");
    }
    if (peak) {
        peak = checked_add(*peak, *next_manifest, "compaction peak Store byte count overflow");
    }
    auto intent_bytes = checked_add(static_cast<std::uint64_t>(kCompactionIntentHeaderBytes),
                                    *current_manifest, "compaction intent byte count overflow");
    if (intent_bytes) {
        intent_bytes = checked_add(*intent_bytes, *next_manifest, "compaction intent byte count overflow");
    }
    if (peak && intent_bytes) {
        peak = checked_add(*peak, *intent_bytes, "compaction peak Store byte count overflow");
    }
    if (!intent_bytes) {
        return unexpected(intent_bytes.error());
    }
    if (!peak) {
        return unexpected(peak.error());
    }
    return CompactionResourceEstimate{
        .temporary_bytes = *temporary,
        .reclaimed_bytes = *reclaimed,
        .peak_store_bytes = *peak,
        .intent_bytes = *intent_bytes,
    };
}

auto validate_compaction_resources(const CompactionResourceEstimate& estimate,
                                   const DurableResourceLimits& limits) -> Status {
    if (estimate.temporary_bytes > limits.max_temporary_compaction_bytes) {
        return fail(ErrorCode::storage_exhausted,
                    "durable compaction exceeds the configured temporary-space budget");
    }
    if (estimate.peak_store_bytes > limits.max_store_bytes) {
        return fail(ErrorCode::storage_exhausted,
                    "durable compaction exceeds the configured peak Store byte budget");
    }
    return {};
}

} // namespace glifistore
