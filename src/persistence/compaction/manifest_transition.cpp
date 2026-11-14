#include "glifistore/persistence/compaction.hpp"

#include <limits>
#include <vector>

namespace glifistore {

auto canonical_durable_compaction_manifest(const Manifest& current, const WorkerId worker_id,
                                           const DurableCompactionShape shape) -> Result<Manifest> {
    Manifest expected = current;
    if (current.manifest_generation == std::numeric_limits<std::uint64_t>::max()) {
        return fail(ErrorCode::arithmetic_overflow, "durable compaction manifest generation is exhausted");
    }
    ++expected.manifest_generation;
    expected.segments.clear();
    expected.segments.reserve(current.segments.size() - (shape.source_count - shape.replacement_count));

    std::size_t sealed_seen{};
    for (const auto& entry : current.segments) {
        const bool selected = entry.owner_worker == worker_id && entry.role == ManifestSegmentRole::sealed &&
                              sealed_seen < shape.source_count;
        if (!selected) {
            expected.segments.push_back(entry);
            continue;
        }
        const auto position = sealed_seen++;
        if (position >= shape.replacement_count) {
            continue;
        }
        if (entry.generation.value == std::numeric_limits<std::uint32_t>::max()) {
            return fail(ErrorCode::arithmetic_overflow, "durable compaction Segment generation is exhausted");
        }
        auto replacement = entry;
        ++replacement.generation.value;
        expected.segments.push_back(replacement);
    }
    if (sealed_seen != shape.source_count) {
        return fail(ErrorCode::invalid_argument,
                    "durable compaction prefix exceeds the Worker's sealed Segment set");
    }
    return expected;
}

auto derive_durable_compaction_shape(const Manifest& current, const Manifest& next, const WorkerId worker_id)
    -> Result<DurableCompactionShape> {
    if (!encoded_manifest_size(current) || !encoded_manifest_size(next)) {
        return fail(ErrorCode::invalid_argument,
                    "durable compaction transition contains an invalid v1 manifest");
    }
    if (worker_id.value >= current.worker_count || current.store_id != next.store_id ||
        current.worker_count != next.worker_count || current.routing_algorithm != next.routing_algorithm ||
        current.routing_epoch != next.routing_epoch || current.worker_hash_seed != next.worker_hash_seed ||
        current.next_segment_id != next.next_segment_id ||
        current.next_segment_generation != next.next_segment_generation ||
        current.manifest_generation == std::numeric_limits<std::uint64_t>::max() ||
        next.manifest_generation != current.manifest_generation + 1U) {
        return fail(ErrorCode::invalid_argument,
                    "durable compaction transition changes immutable catalog metadata");
    }

    // The Worker's sealed entries, in Manifest (and therefore sequence) order.
    std::vector<ManifestSegmentEntry> current_sealed;
    std::vector<ManifestSegmentEntry> next_sealed;
    for (const auto& entry : current.segments) {
        if (entry.owner_worker == worker_id && entry.role == ManifestSegmentRole::sealed) {
            current_sealed.push_back(entry);
        }
    }
    for (const auto& entry : next.segments) {
        if (entry.owner_worker == worker_id && entry.role == ManifestSegmentRole::sealed) {
            next_sealed.push_back(entry);
        }
    }
    if (current_sealed.empty() || next_sealed.size() > current_sealed.size()) {
        return fail(ErrorCode::invalid_argument,
                    "durable compaction transition has no sealed source set or grows the catalog");
    }

    // A replacement keeps its source identity with the next generation, so it
    // is distinguishable from an unchanged retained entry.
    std::size_t replacement_count{};
    while (replacement_count < next_sealed.size()) {
        const auto& source = current_sealed[replacement_count];
        const auto& candidate = next_sealed[replacement_count];
        if (candidate.segment_id != source.segment_id ||
            source.generation.value == std::numeric_limits<std::uint32_t>::max() ||
            candidate.generation.value != source.generation.value + 1U) {
            break;
        }
        ++replacement_count;
    }
    const auto retained_count = next_sealed.size() - replacement_count;
    const DurableCompactionShape shape{.source_count = current_sealed.size() - retained_count,
                                       .replacement_count = replacement_count};
    if (shape.source_count == 0 || shape.replacement_count > shape.source_count) {
        return fail(ErrorCode::invalid_argument, "durable compaction transition has no canonical prefix");
    }
    auto expected = canonical_durable_compaction_manifest(current, worker_id, shape);
    if (!expected) {
        return unexpected(expected.error());
    }
    if (*expected != next) {
        return fail(ErrorCode::invalid_argument,
                    "durable compaction next manifest is not the canonical sealed-prefix replacement");
    }
    if (current.format_version == 2 && shape.source_count != current_sealed.size()) {
        return fail(ErrorCode::invalid_argument,
                    "blob-profile durable compaction must replace the complete sealed set");
    }
    return shape;
}

auto validate_durable_compaction_transition(const Manifest& current, const Manifest& next,
                                            const WorkerId worker_id) -> Result<std::size_t> {
    auto shape = derive_durable_compaction_shape(current, next, worker_id);
    if (!shape) {
        return unexpected(shape.error());
    }
    return shape->replacement_count;
}

} // namespace glifistore
