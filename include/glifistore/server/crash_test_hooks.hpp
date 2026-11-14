/**
 * @file
 * @brief Explicit daemon crash checkpoints used only by crash campaigns.
 */
#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/store/config.hpp"

namespace glifistore::server {

/// Lab-only crash seams for process-kill matrices against a real glifistored exec.
/// Activated only when ALL of:
/// - `GLIFISTORE_CRASH_TEST=1`
/// - `GLIFISTORE_CRASH_KILL_AT=<filesystem_operation_name>[#N]`
/// - `GLIFISTORE_CRASH_CHECKPOINT_DIR=<writable dir>`
/// are set. Production deployments leave these unset (no-op).
[[nodiscard]] auto maybe_install_crash_test_hooks(StoreConfig& store) -> Status;

} // namespace glifistore::server
