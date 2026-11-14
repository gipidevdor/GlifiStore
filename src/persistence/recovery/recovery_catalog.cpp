#include "persistence/recovery/recovery_catalog.hpp"

#include "glifistore/persistence/resource_limits.hpp"

#include <utility>

namespace glifistore::recovery {

auto load_recovery_catalog(DataDirectory& directory, const DurableResourceLimits& limits,
                           const DurableCompactionIntent* compaction_intent, RecoveryMemoryBudget& budget)
    -> Result<RecoveryCatalog> {
    if (auto valid = validate_durable_resource_limits(limits); !valid) {
        return unexpected(valid.error());
    }
    auto manifest = directory.read_manifest(limits.max_manifest_bytes);
    if (!manifest) {
        return unexpected(manifest.error());
    }
    if (auto resources = validate_durable_manifest_resources(*manifest, limits); !resources) {
        return unexpected(resources.error());
    }
    const auto manifest_memory = durable_manifest_bytes(manifest->segments.size());
    if (!manifest_memory) {
        return unexpected(manifest_memory.error());
    }
    if (auto memory = budget.retain(*manifest_memory); !memory) {
        return unexpected(memory.error());
    }
    if (auto memory = budget.retain_repeated(kRecoveryBytesPerSegment, manifest->segments.size()); !memory) {
        return unexpected(memory.error());
    }
    if (auto memory = budget.retain_repeated(kRecoveryBytesPerWorker, manifest->worker_count); !memory) {
        return unexpected(memory.error());
    }
    auto namespace_audit = audit_data_directory(directory, *manifest);
    if (!namespace_audit) {
        return unexpected(namespace_audit.error());
    }
    const auto safe =
        compaction_intent != nullptr
            ? validate_namespace_for_compaction_recovery(*namespace_audit, *manifest, *compaction_intent)
            : validate_namespace_for_recovery(*namespace_audit);
    if (!safe) {
        return unexpected(safe.error());
    }

    std::vector<std::vector<std::size_t>> segments_by_worker(manifest->worker_count);
    for (std::size_t index = 0; index < manifest->segments.size(); ++index) {
        const auto owner = manifest->segments[index].owner_worker.value;
        segments_by_worker[owner].push_back(index);
    }

    return RecoveryCatalog{
        .manifest = std::move(*manifest),
        .namespace_audit = std::move(*namespace_audit),
        .segments_by_worker = std::move(segments_by_worker),
    };
}

} // namespace glifistore::recovery
