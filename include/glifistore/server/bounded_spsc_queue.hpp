/**
 * @file
 * @brief Server alias for the paired runtime single-producer, single-consumer queue.
 */
#pragma once

// Compatibility header. The bounded SPSC ring moved into glifistore_core with
// the paired shard runtime (ADR 0032); the daemon keeps using the same type for
// its Reader-owned completion lanes.
#include "glifistore/store/paired/bounded_spsc_queue.hpp"

namespace glifistore::server {

template <typename T> using BoundedSpscQueue = store::paired::BoundedSpscQueue<T>;

} // namespace glifistore::server
