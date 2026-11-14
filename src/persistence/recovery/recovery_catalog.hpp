#pragma once

#include "glifistore/persistence/filesystem.hpp"
#include "glifistore/persistence/manifest.hpp"
#include "glifistore/persistence/namespace_audit.hpp"
#include "glifistore/store/config.hpp"
#include "persistence/recovery/recovery_budget.hpp"

#include <cstddef>
#include <vector>

namespace glifistore::recovery {

struct RecoveryCatalog final {
    Manifest manifest;
    NamespaceAuditReport namespace_audit;
    // Manifest segment indices grouped by owner Worker, preserving manifest order.
    std::vector<std::vector<std::size_t>> segments_by_worker;
};

[[nodiscard]] auto load_recovery_catalog(DataDirectory& directory, const DurableResourceLimits& limits,
                                         const DurableCompactionIntent* compaction_intent,
                                         RecoveryMemoryBudget& budget) -> Result<RecoveryCatalog>;

} // namespace glifistore::recovery
