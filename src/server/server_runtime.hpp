#pragma once

#include "glifistore/server/connection_handoff.hpp"
#include "glifistore/server/disk_read_executor.hpp"
#include "glifistore/server/pair_writer.hpp"
#include "glifistore/server/reactor.hpp"
#include "glifistore/store/store.hpp"

#include <memory>
#include <vector>

namespace glifistore::server {

// Owned runtime graph assembled by ServerBuilder. Server is the lifecycle
// façade over this aggregate, not the composition root.
struct ServerRuntime final {
    std::unique_ptr<Store> store;
    std::unique_ptr<DiskReadExecutor> disk_reads;
    std::unique_ptr<PairWriterPool> pair_writers;
    ConnectionHandoffMesh mesh;
    std::vector<std::unique_ptr<Reactor>> reactors;
};

} // namespace glifistore::server
