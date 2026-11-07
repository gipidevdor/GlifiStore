#include "persistence/recovery/recovery_scanner.hpp"

#include "glifistore/core/key_hash.hpp"
#include "glifistore/persistence/segment_file.hpp"

#include <limits>
#include <string>
#include <utility>

namespace glifistore::recovery {
namespace {

struct WorkerScanContext {
    WorkerId worker_id{};
    std::size_t worker_count{};
    WorkerRoutingState routing{};
    SegmentId segment_id{};
    std::uint64_t now_ns{};
    SequenceNumber maximum_sequence{};
    LatestMap* latest{};
    RebuildStats* stats{};
    RecoveryMemoryBudget* memory_budget{};
};

auto segment_error(const ManifestSegmentEntry& entry, Error error) -> Unexpected {
    if (error.code == ErrorCode::not_found) {
        error.code = ErrorCode::corrupted_data;
    }
    error.message = "manifest Segment " + std::to_string(entry.segment_id.value) + ": " + error.message;
    return unexpected(std::move(error));
}

auto visit_recovery_record(void* opaque, const RecordRef& reference, const RecordView& record) -> Status {
    auto& context = *static_cast<WorkerScanContext*>(opaque);
    const auto computed_hash = hash_key_routing(record.key, context.routing);
    if (computed_hash != record.key_hash) {
        return fail(ErrorCode::corrupted_data,
                    "Segment " + std::to_string(context.segment_id.value) +
                        " contains a Record whose key hash does not match its key bytes");
    }
    if (route_worker(record.key_hash, context.worker_count) != context.worker_id.value) {
        return fail(ErrorCode::corrupted_data, "Segment " + std::to_string(context.segment_id.value) +
                                                   " contains a Record routed to another Worker");
    }

    ++context.stats->records_scanned;
    if (record.sequence.value > context.maximum_sequence.value) {
        context.maximum_sequence = record.sequence;
    }

    const auto key_bytes = static_cast<std::uint64_t>(record.key.size());
    if (key_bytes > (std::numeric_limits<std::uint64_t>::max() - kRecoveryBytesPerKey) / 2U) {
        return fail(ErrorCode::arithmetic_overflow, "durable recovery key memory estimate overflow");
    }
    const auto entry_bytes = kRecoveryBytesPerKey + key_bytes * 2U;
    if (auto memory = context.memory_budget->ensure_peak(entry_bytes); !memory) {
        return memory;
    }
    std::string key{record.key_string()};
    const auto current = context.latest->find(key);
    if (current != context.latest->end() &&
        current->second.reference.sequence.value == record.sequence.value) {
        return fail(ErrorCode::sequence_conflict,
                    "duplicate highest sequence for one key during durable recovery");
    }
    LatestRecord candidate{.reference = reference,
                           .key_hash = record.key_hash,
                           .deleted = record.opcode == Opcode::erase,
                           .expired = record.expired(context.now_ns)};
    if (current == context.latest->end()) {
        if (auto memory = context.memory_budget->retain(entry_bytes); !memory) {
            return memory;
        }
        context.latest->emplace(std::move(key), candidate);
    } else if (current->second.reference.sequence.value < record.sequence.value) {
        current->second = candidate;
    }
    return {};
}

auto validate_lifecycle(const ManifestSegmentEntry& entry, const SegmentCommit& commit,
                        bool& active_requires_rotation) -> Status {
    if (entry.role == ManifestSegmentRole::sealed) {
        if (commit.state != PersistedSegmentState::sealed) {
            return fail(ErrorCode::corrupted_data,
                        "manifest sealed Segment has an active persisted commit state");
        }
        return {};
    }
    if (commit.state == PersistedSegmentState::sealed) {
        active_requires_rotation = true;
    }
    return {};
}

} // namespace

RecoveryScanner::RecoveryScanner(DataDirectory& directory, const Manifest& manifest,
                                 const std::uint64_t now_ns, RecoveryMemoryBudget& budget)
    : directory_(directory), manifest_(manifest), now_ns_(now_ns), budget_(budget) {}

auto RecoveryScanner::scan_worker(const WorkerId worker, const std::span<const std::size_t> catalog_indices)
    -> Result<WorkerScanResult> {
    WorkerScanResult result;
    WorkerScanContext context{
        .worker_id = worker,
        .worker_count = manifest_.worker_count,
        .routing = manifest_.worker_routing(),
        .now_ns = now_ns_,
        .latest = &result.latest,
        .stats = &result.stats,
        .memory_budget = &budget_,
    };

    for (const auto catalog_index : catalog_indices) {
        const auto& entry = manifest_.segments[catalog_index];
        const SegmentHeaderIdentity expected{.store_id = manifest_.store_id,
                                             .segment_id = entry.segment_id,
                                             .generation = entry.generation,
                                             .owner_worker = entry.owner_worker,
                                             .format_version = manifest_.format_version};
        auto file = DurableSegmentFile::open(directory_, expected, SegmentFileOpenMode::read_only);
        if (!file) {
            return segment_error(entry, file.error());
        }

        bool segment_requires_rotation{};
        if (auto lifecycle =
                validate_lifecycle(entry, file->selected_commit().commit, segment_requires_rotation);
            !lifecycle) {
            return segment_error(entry, lifecycle.error());
        }
        if (entry.role == ManifestSegmentRole::active) {
            result.active_segment = entry.segment_id;
            result.active_requires_rotation = segment_requires_rotation;
        }

        const auto& commit = file->selected_commit().commit;
        if (commit.record_count != 0 && context.maximum_sequence.value != 0 &&
            commit.first_sequence.value <= context.maximum_sequence.value) {
            return segment_error(
                entry, Error{ErrorCode::corrupted_data,
                             "committed sequence range overlaps or reverses a preceding Worker Segment"});
        }
        context.segment_id = entry.segment_id;
        if (auto scanned = file->visit_committed_records(&context, &visit_recovery_record); !scanned) {
            return segment_error(entry, scanned.error());
        }
        result.segments.push_back({
            .catalog_index = catalog_index,
            .state =
                RecoveredSegmentState{
                    .selected = file->selected_commit(),
                },
        });
    }

    result.maximum_sequence = context.maximum_sequence;
    return result;
}

} // namespace glifistore::recovery
