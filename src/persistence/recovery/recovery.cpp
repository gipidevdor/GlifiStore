#include "glifistore/persistence/recovery.hpp"

#include "persistence/recovery/recovery_budget.hpp"
#include "persistence/recovery/recovery_catalog.hpp"
#include "persistence/recovery/recovery_index_builder.hpp"
#include "persistence/recovery/recovery_scanner.hpp"

#include <utility>
#include <vector>

namespace glifistore {
namespace {

struct RecoveredWorkers final {
    std::vector<RecoveredSegmentState> segments{};
    std::vector<RecoveredWorkerState> workers{};
    DurableRecoveryStats stats{};
};

[[nodiscard]] auto recover_workers(recovery::RecoveryCatalog& catalog, recovery::RecoveryScanner& scanner,
                                   const DurableResourceLimits& limits) -> Result<RecoveredWorkers> {
    RecoveredWorkers recovered{
        .segments = std::vector<RecoveredSegmentState>(catalog.manifest.segments.size()),
    };
    recovered.workers.reserve(catalog.manifest.worker_count);

    for (std::size_t worker_index = 0; worker_index < catalog.manifest.worker_count; ++worker_index) {
        const auto worker_id = WorkerId{static_cast<std::uint32_t>(worker_index)};
        auto scan = scanner.scan_worker(worker_id, catalog.segments_by_worker[worker_index]);
        if (!scan) {
            return unexpected(scan.error());
        }
        recovered.stats.rebuild.records_scanned += scan->stats.records_scanned;
        recovered.stats.segments_scanned += scan->segments.size();
        for (auto& segment : scan->segments) {
            recovered.segments[segment.catalog_index] = segment.state;
        }
        auto worker = recovery::RecoveryIndexBuilder::build(
            worker_id, worker_index, catalog.manifest.worker_count, catalog.manifest.worker_routing(), limits,
            std::move(*scan), recovered.stats);
        if (!worker) {
            return unexpected(worker.error());
        }
        recovered.workers.push_back(std::move(*worker));
    }
    return recovered;
}

[[nodiscard]] auto assemble_recovery_state(recovery::RecoveryCatalog&& catalog, RecoveredWorkers&& recovered)
    -> DurableRecoveryState {
    return DurableRecoveryState{
        .manifest = std::move(catalog.manifest),
        .namespace_audit = std::move(catalog.namespace_audit),
        .segments = std::move(recovered.segments),
        .workers = std::move(recovered.workers),
        .stats = recovered.stats,
    };
}

} // namespace

auto recover_durable_state(DataDirectory& directory, const std::uint64_t now_ns,
                           const DurableResourceLimits& limits,
                           const DurableCompactionIntent* compaction_intent) -> Result<DurableRecoveryState> {
    recovery::RecoveryMemoryBudget budget{limits.max_recovery_memory_bytes};
    auto catalog = recovery::load_recovery_catalog(directory, limits, compaction_intent, budget);
    if (!catalog) {
        return unexpected(catalog.error());
    }
    recovery::RecoveryScanner scanner{directory, catalog->manifest, now_ns, budget};
    auto workers = recover_workers(*catalog, scanner, limits);
    if (!workers) {
        return unexpected(workers.error());
    }
    if (compaction_intent == nullptr) {
        std::vector<std::string> stale_temporaries;
        for (const auto& issue : catalog->namespace_audit.issues) {
            if (issue.kind == NamespaceIssueKind::stale_manifest_temporary ||
                issue.kind == NamespaceIssueKind::stale_segment_temporary ||
                issue.kind == NamespaceIssueKind::stale_compaction_temporary) {
                stale_temporaries.push_back(issue.name);
            }
        }
        if (auto cleaned = directory.cleanup_recovery_temporaries(stale_temporaries); !cleaned) {
            return unexpected(cleaned.error());
        }
        if (!stale_temporaries.empty()) {
            auto clean_audit = audit_data_directory(directory, catalog->manifest);
            if (!clean_audit) {
                return unexpected(clean_audit.error());
            }
            if (auto safe = validate_namespace_for_recovery(*clean_audit); !safe) {
                return unexpected(safe.error());
            }
            catalog->namespace_audit = std::move(*clean_audit);
        }
    }
    return assemble_recovery_state(std::move(*catalog), std::move(*workers));
}

} // namespace glifistore
