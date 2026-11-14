#pragma once

#include "glifistore/persistence/recovery.hpp"
#include "glifistore/store/config.hpp"
#include "persistence/recovery/recovery_scanner.hpp"

namespace glifistore::recovery {

class RecoveryIndexBuilder final {
  public:
    RecoveryIndexBuilder() = delete;

    [[nodiscard]] static auto build(WorkerId worker, std::size_t worker_index, std::size_t worker_count,
                                    WorkerRoutingState routing, const DurableResourceLimits& limits,
                                    WorkerScanResult&& scan, DurableRecoveryStats& recovery_stats)
        -> Result<RecoveredWorkerState>;
};

} // namespace glifistore::recovery
