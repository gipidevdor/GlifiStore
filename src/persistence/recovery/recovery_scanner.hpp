#pragma once

#include "glifistore/core/types.hpp"
#include "glifistore/index/index_types.hpp"
#include "glifistore/persistence/filesystem.hpp"
#include "glifistore/persistence/manifest.hpp"
#include "glifistore/persistence/recovery.hpp"
#include "glifistore/segment/record.hpp"
#include "persistence/recovery/recovery_budget.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace glifistore::recovery {

struct LatestRecord final {
    RecordRef reference;
    std::uint64_t key_hash{};
    bool deleted{};
    bool expired{};
};

using LatestMap = std::unordered_map<std::string, LatestRecord>;

struct WorkerScanSegment final {
    std::size_t catalog_index{};
    RecoveredSegmentState state;
};

struct WorkerScanResult final {
    LatestMap latest;
    SequenceNumber maximum_sequence{};
    SegmentId active_segment{};
    bool active_requires_rotation{};
    std::vector<WorkerScanSegment> segments;
    RebuildStats stats{};
};

class RecoveryScanner final {
  public:
    RecoveryScanner(DataDirectory& directory, const Manifest& manifest, std::uint64_t now_ns,
                    RecoveryMemoryBudget& budget);

    [[nodiscard]] auto scan_worker(WorkerId worker, std::span<const std::size_t> catalog_indices)
        -> Result<WorkerScanResult>;

  private:
    DataDirectory& directory_;
    const Manifest& manifest_;
    std::uint64_t now_ns_{};
    RecoveryMemoryBudget& budget_;
};

} // namespace glifistore::recovery
