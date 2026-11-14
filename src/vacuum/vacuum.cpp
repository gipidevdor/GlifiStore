#include "glifistore/vacuum/vacuum.hpp"

#include <algorithm>
#include <limits>
#include <unordered_map>

namespace glifistore {
namespace {

auto make_catalog(std::span<const SegmentPtr> segments) -> std::unordered_map<SegmentId, Segment*> {
    std::unordered_map<SegmentId, Segment*> result;
    for (const auto& segment : segments) {
        result.emplace(segment->id(), segment.get());
    }
    return result;
}

} // namespace

auto VacuumPlanner::candidates(std::span<const SegmentPtr> segments) const -> std::vector<SegmentId> {
    struct Candidate {
        SegmentId id{};
        double ratio{};
    };

    std::vector<Candidate> ranked;
    for (const auto& segment : segments) {
        const auto stats = segment->stats();
        if (segment->state() != SegmentState::sealed || stats.record_count == 0) {
            continue;
        }
        const auto ratio = static_cast<double>(stats.live_bytes) / static_cast<double>(stats.used_bytes);
        if (ratio <= policy_.maximum_live_ratio) {
            ranked.push_back({segment->id(), ratio});
        }
    }
    std::ranges::sort(ranked, {}, &Candidate::ratio);
    if (ranked.size() > policy_.maximum_segments_per_run) {
        ranked.resize(policy_.maximum_segments_per_run);
    }
    std::vector<SegmentId> result;
    result.reserve(ranked.size());
    for (const auto& candidate : ranked) {
        result.push_back(candidate.id);
    }
    return result;
}

auto VacuumBuilder::rebuild(const Index& current_index, std::span<const SegmentPtr> current_segments,
                            const SegmentId first_new_segment, const WorkerId owner) const
    -> Result<VacuumResult> {
    std::vector<SegmentId> candidates;
    candidates.reserve(current_segments.size());
    for (const auto& segment : current_segments) {
        candidates.push_back(segment->id());
    }
    auto next_id = first_new_segment.value;
    bool exhausted{};
    return rebuild(
        current_index, current_segments, candidates, [owner, &next_id, &exhausted]() -> Result<SegmentPtr> {
            if (exhausted) {
                return fail(ErrorCode::arithmetic_overflow, "vacuum Segment identity space is exhausted");
            }
            const auto id = SegmentId{next_id};
            if (next_id == std::numeric_limits<std::uint64_t>::max()) {
                exhausted = true;
            } else {
                ++next_id;
            }
            return std::make_shared<Segment>(id, owner);
        });
}

auto VacuumBuilder::rebuild(const Index& current_index, const std::span<const SegmentPtr> current_segments,
                            const std::span<const SegmentId> candidates, SegmentAllocator allocate_segment,
                            const std::uint64_t now_ns) const -> Result<VacuumResult> {
    if (!allocate_segment) {
        return fail(ErrorCode::invalid_argument, "vacuum requires a replacement Segment allocator");
    }
    const auto catalog = make_catalog(current_segments);

    struct SourceAccounting {
        Segment* segment{};
        std::uint64_t records{};
        std::uint64_t bytes{};
    };

    std::unordered_map<SegmentId, SourceAccounting> sources;
    sources.reserve(candidates.size());
    WorkerId source_owner{};
    bool has_source_owner{};
    for (const auto id : candidates) {
        const auto found = catalog.find(id);
        if (found == catalog.end() || found->second->state() != SegmentState::sealed) {
            return fail(ErrorCode::invalid_reference,
                        "vacuum candidate is absent from the catalog or is not sealed");
        }
        if (!sources.emplace(id, SourceAccounting{.segment = found->second}).second) {
            return fail(ErrorCode::invalid_argument, "vacuum candidate set contains a duplicate identity");
        }
        if (!has_source_owner) {
            source_owner = found->second->owner();
            has_source_owner = true;
        } else if (found->second->owner() != source_owner) {
            return fail(ErrorCode::invalid_argument, "vacuum candidates must belong to one Worker");
        }
    }

    auto new_index = current_index.make_empty();
    if (auto reserved = new_index.reserve(current_index.stats().size); !reserved) {
        return unexpected(reserved.error());
    }
    std::vector<SegmentPtr> new_segments;
    VacuumStats stats{.old_segments = candidates.size()};
    const auto allocate_replacement = [&]() -> Status {
        auto allocated = allocate_segment();
        if (!allocated) {
            return unexpected(allocated.error());
        }
        if (!*allocated || (*allocated)->state() != SegmentState::active ||
            (has_source_owner && (*allocated)->owner() != source_owner) ||
            catalog.contains((*allocated)->id())) {
            return fail(ErrorCode::invalid_argument,
                        "vacuum allocator returned an invalid replacement Segment");
        }
        for (const auto& existing : new_segments) {
            if (existing->id() == (*allocated)->id()) {
                return fail(ErrorCode::invalid_argument,
                            "vacuum allocator returned a duplicate replacement identity");
            }
        }
        new_segments.push_back(std::move(*allocated));
        return {};
    };

    for (const auto& entry : current_index.entries()) {
        const auto found = catalog.find(entry.record.segment_id);
        if (found == catalog.end()) {
            return fail(ErrorCode::invalid_reference, "index points to a segment absent from the catalog");
        }
        const auto source = sources.find(entry.record.segment_id);
        if (source == sources.end()) {
            auto inserted = new_index.insert_or_assign(entry.key, entry.record);
            if (!inserted) {
                return unexpected(inserted.error());
            }
            continue;
        }
        auto record = found->second->read(entry.record);
        if (!record) {
            return unexpected(record.error());
        }
        ++source->second.records;
        source->second.bytes += entry.record.size.value;
        ++stats.source_records_verified;
        stats.source_bytes_verified += entry.record.size.value;
        if (record->expired(now_ns)) {
            ++stats.expired_records_dropped;
            continue;
        }
        const RecordInput input{
            .sequence = record->sequence,
            .opcode = record->opcode,
            .type = record->type,
            .flags = record->flags,
            .key_hash = record->key_hash,
            .expire_at_ns = record->expire_at_ns,
            .key = record->key,
            .value = record->value,
        };
        if (new_segments.empty()) {
            if (auto allocated = allocate_replacement(); !allocated) {
                return unexpected(allocated.error());
            }
        }
        auto ref = new_segments.back()->append(input);
        if (!ref && ref.error().code == ErrorCode::segment_full) {
            if (auto sealed = new_segments.back()->seal(); !sealed) {
                return unexpected(sealed.error());
            }
            if (auto allocated = allocate_replacement(); !allocated) {
                return unexpected(allocated.error());
            }
            ref = new_segments.back()->append(input);
        }
        if (!ref) {
            return unexpected(ref.error());
        }
        if (auto live = new_segments.back()->mark_live(*ref); !live) {
            return unexpected(live.error());
        }
        auto inserted = new_index.insert_or_assign(entry.key, *ref);
        if (!inserted) {
            return unexpected(inserted.error());
        }
        ++stats.records_copied;
        stats.bytes_copied += ref->size.value;
    }
    for (const auto& [id, source] : sources) {
        static_cast<void>(id);
        const auto source_stats = source.segment->stats();
        if (source.records != source_stats.live_records || source.bytes != source_stats.live_bytes) {
            return fail(ErrorCode::corrupted_data,
                        "vacuum candidate liveness accounting disagrees with the current Index");
        }
    }
    for (const auto& segment : new_segments) {
        if (segment->state() == SegmentState::active) {
            if (auto sealed = segment->seal(); !sealed) {
                return unexpected(sealed.error());
            }
        }
    }
    stats.new_segments = new_segments.size();
    return VacuumResult{.index = std::move(new_index), .segments = std::move(new_segments), .stats = stats};
}

} // namespace glifistore
