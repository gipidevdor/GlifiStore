/**
 * @file
 * @brief Server-facing latency histogram compatibility aliases.
 */
#pragma once

// Compatibility shim: LatencyHistogram lives in glifistore_core so paired Store
// stats and daemon STATS share one definition (ADR 0032).
#include "glifistore/core/latency_histogram.hpp"

namespace glifistore::server {

using ::glifistore::append_latency_histogram;
using ::glifistore::latency_histogram_bound_label;
using ::glifistore::LatencyHistogram;

} // namespace glifistore::server
