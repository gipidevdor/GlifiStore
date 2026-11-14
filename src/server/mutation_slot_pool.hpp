#pragma once

// Compatibility header. The paired mutation slot pool moved into
// glifistore_core with ShardPairRuntime (ADR 0032).
#include "glifistore/store/paired/mutation_slot_pool.hpp"

namespace glifistore::server::internal {

using MutationSlotPool = store::paired::MutationSlotPool;

} // namespace glifistore::server::internal
