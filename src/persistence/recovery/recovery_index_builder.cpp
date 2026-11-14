#include "persistence/recovery/recovery_index_builder.hpp"

#include "glifistore/core/key_hash.hpp"
#include "glifistore/persistence/resource_limits.hpp"

#include <limits>
#include <utility>

namespace glifistore::recovery {

auto RecoveryIndexBuilder::build(const WorkerId worker, const std::size_t worker_index,
                                 const std::size_t worker_count, const WorkerRoutingState routing,
                                 const DurableResourceLimits& limits, WorkerScanResult&& scan,
                                 DurableRecoveryStats& recovery_stats) -> Result<RecoveredWorkerState> {
    if (scan.maximum_sequence.value == std::numeric_limits<std::uint64_t>::max()) {
        return fail(ErrorCode::arithmetic_overflow,
                    "Worker sequence space is exhausted during durable recovery");
    }

    const auto live_key_limit =
        durable_worker_live_key_limit(worker_index, worker_count, limits.max_live_keys);
    std::size_t visible_keys{};
    for (const auto& [key, record] : scan.latest) {
        static_cast<void>(key);
        if (!record.deleted && !record.expired) {
            ++visible_keys;
        }
    }
    if (visible_keys > live_key_limit) {
        return fail(ErrorCode::resource_exhausted,
                    "durable recovery exceeds a Worker live-key budget partition");
    }

    Index index{routing};
    if (auto reserved = index.reserve(scan.latest.size()); !reserved) {
        return unexpected(reserved.error());
    }
    std::uint64_t active_live_record_bytes{};
    std::uint64_t sealed_live_record_bytes{};
    for (const auto& [key, record] : scan.latest) {
        if (record.deleted) {
            ++recovery_stats.rebuild.tombstones;
            continue;
        }
        if (record.expired) {
            ++recovery_stats.rebuild.expired;
            continue;
        }
        const HashedKey hashed{.key = key, .hash = record.key_hash};
        if (auto inserted = index.insert_or_assign(hashed, record.reference); !inserted) {
            return unexpected(inserted.error());
        }
        auto& live_bytes = record.reference.segment_id == scan.active_segment ? active_live_record_bytes
                                                                              : sealed_live_record_bytes;
        if (record.reference.size.value > std::numeric_limits<std::uint64_t>::max() - live_bytes) {
            return fail(ErrorCode::arithmetic_overflow,
                        "durable recovery live Record byte count overflows uint64_t");
        }
        live_bytes += record.reference.size.value;
        ++recovery_stats.rebuild.records_visible;
    }

    if (scan.active_requires_rotation) {
        ++recovery_stats.workers_requiring_rotation;
    }
    return RecoveredWorkerState{
        .worker_id = worker,
        .index = std::move(index),
        .next_sequence = SequenceNumber{scan.maximum_sequence.value + 1},
        .active_segment = scan.active_segment,
        .active_requires_rotation = scan.active_requires_rotation,
        .active_live_record_bytes = active_live_record_bytes,
        .sealed_live_record_bytes = sealed_live_record_bytes,
    };
}

} // namespace glifistore::recovery
