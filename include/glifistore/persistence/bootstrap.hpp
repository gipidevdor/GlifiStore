/**
 * @file
 * @brief Durable catalog creation/open validation before runtime admission.
 */
#pragma once

#include "glifistore/core/worker_routing.hpp"
#include "glifistore/persistence/filesystem.hpp"
#include "glifistore/store/config.hpp"

#include <cstddef>
#include <optional>

namespace glifistore {

/**
 * @brief Create or validate the durable catalog before runtime recovery/admission.
 * @param directory Already exclusively locked namespace, retained by the caller.
 * @param mode Existing/open-or-create/create-new policy.
 * @param creation_worker_count Fixed owner count for creation.
 * @param required_worker_count Optional explicit count that reopen must match.
 * @param limits Capacity/recovery policy limits.
 * @param creation_routing Routing identity for creation and explicit reopen checks.
 * @param enable_blobs Explicit ADR 0042 format opt-in; never silently upgrades a v1 Store.
 * @return Success or a compatibility, namespace, capacity, or I/O error.
 */
[[nodiscard]] auto prepare_durable_store(DataDirectory& directory, DurableOpenMode mode,
                                         std::size_t creation_worker_count,
                                         std::optional<std::size_t> required_worker_count,
                                         const DurableResourceLimits& limits = {},
                                         const WorkerRoutingConfig& creation_routing = {},
                                         bool enable_blobs = false) -> Status;

} // namespace glifistore
