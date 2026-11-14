#include "glifistore/core/types.hpp"
#include "glifistore/worker/topology.hpp"

#include <iostream>

int main() {
    const auto topology = glifistore::detect_worker_topology();
    const auto workers = glifistore::WorkerCountPolicy::choose(topology, {});
    std::cout << "GlifiStore architecture bootstrap\n"
              << "segment_size_bytes=" << glifistore::kSegmentSizeBytes << '\n'
              << "logical_cpus=" << topology.logical_cpus << '\n'
              << "physical_cores=" << topology.physical_cores << '\n'
              << "selected_workers=" << workers << '\n';
    return 0;
}
