/**
 * @file
 * @brief Durable capacity, free-space, descriptor, and recovery-budget validation.
 */
#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/persistence/manifest.hpp"
#include "glifistore/store/config.hpp"

#include <cstddef>
#include <cstdint>

namespace glifistore {

class DataDirectory;

[[nodiscard]] auto validate_durable_resource_limits(const DurableResourceLimits& limits) -> Status;
[[nodiscard]] auto durable_manifest_bytes(std::size_t segment_count) -> Result<std::uint64_t>;
[[nodiscard]] auto validate_durable_bootstrap_resources(std::size_t worker_count,
                                                        const DurableResourceLimits& limits) -> Status;
[[nodiscard]] auto validate_durable_manifest_resources(const Manifest& manifest,
                                                       const DurableResourceLimits& limits) -> Status;
[[nodiscard]] auto validate_durable_rotation_resources(std::size_t current_segment_count,
                                                       const DurableResourceLimits& limits) -> Status;
[[nodiscard]] auto require_durable_available_space(const DataDirectory& directory,
                                                   std::uint64_t additional_bytes,
                                                   const DurableResourceLimits& limits) -> Status;
[[nodiscard]] auto durable_worker_live_key_limit(std::size_t worker_index, std::size_t worker_count,
                                                 std::size_t total_limit) noexcept -> std::size_t;

} // namespace glifistore
