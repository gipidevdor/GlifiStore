#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/persistence/manifest.hpp"
#include "glifistore/store/config.hpp"

#include <cstddef>
#include <cstdint>

namespace glifistore {

struct CompactionResourceEstimate final {
    std::uint64_t temporary_bytes{};
    std::uint64_t reclaimed_bytes{};
    std::uint64_t peak_store_bytes{};
    std::uint64_t intent_bytes{};
};

// Pure resource arithmetic for durable compaction. Planning stays free of I/O.
[[nodiscard]] auto estimate_compaction_resources(const Manifest& current, const Manifest& next,
                                                 std::size_t source_count, std::size_t output_count)
    -> Result<CompactionResourceEstimate>;

[[nodiscard]] auto validate_compaction_resources(const CompactionResourceEstimate& estimate,
                                                 const DurableResourceLimits& limits) -> Status;

[[nodiscard]] auto validate_compaction_write_amplification(std::size_t source_count, std::size_t output_count,
                                                           const DurableResourceLimits& limits,
                                                           std::uint64_t blob_reclaim_bytes = 0) -> Status;

} // namespace glifistore
