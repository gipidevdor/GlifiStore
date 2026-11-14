#include "glifistore/core/types.hpp"
#include "glifistore/worker/topology.hpp"
#include "test.hpp"

GLIFI_TEST("worker sizing respects physical cores reservation and memory") {
    const glifistore::WorkerTopology topology{
        .logical_cpus = 16,
        .physical_cores = 8,
        .available_cpus = 16,
        .available_memory_bytes = 4 * glifistore::kSegmentSizeBytes,
    };
    const auto chosen = glifistore::WorkerCountPolicy::choose(topology, {.reserved_cores = 1});
    GLIFI_REQUIRE(chosen == 4);
}

GLIFI_TEST("worker sizing always returns at least one and honors explicit override") {
    const glifistore::WorkerTopology topology{};
    GLIFI_REQUIRE(glifistore::WorkerCountPolicy::choose(topology, {.reserved_cores = 99}) == 1);
    GLIFI_REQUIRE(
        glifistore::WorkerCountPolicy::choose(topology, {.explicit_count = 12, .maximum_workers = 8}) == 8);
}
