#include "glifistore/worker/pool.hpp"

namespace glifistore {

WorkerPool::WorkerPool(GlobalSegmentManager& manager, const std::size_t worker_count,
                       const WorkerRoutingState routing)
    : manager_(manager) {
    workers_.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        workers_.push_back(
            std::make_unique<Worker>(WorkerId{static_cast<std::uint32_t>(index)}, manager_, routing));
    }
}

} // namespace glifistore
