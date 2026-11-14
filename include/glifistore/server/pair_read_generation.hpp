/**
 * @file
 * @brief Server aliases for immutable paired read generations.
 */
#pragma once

// Compatibility header. The immutable paired read generation moved into
// glifistore_core (ADR 0032) so the embedded Store and glifistored share one
// publication authority. The daemon keeps the historical names.
#include "glifistore/store/paired/read_generation.hpp"

namespace glifistore::server {

using store::paired::PairReadGeneration;
using store::paired::PairReadMerge;
using store::paired::ReadMutation;

} // namespace glifistore::server
