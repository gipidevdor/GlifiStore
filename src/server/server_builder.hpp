#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/server/reactor.hpp"
#include "glifistore/server/security_audit.hpp"
#include "glifistore/store/store.hpp"
#include "server/server_runtime.hpp"

#include <memory>
#include <utility>
#include <vector>

namespace glifistore::server {

class ServerBuilder final {
  public:
    ServerBuilder(const ReactorConfig& config, StoreConfig store_config);

    // Builds store, executors, and mesh. Reactors are left empty until Server
    // exists so lifecycle probes can capture a stable Server*.
    [[nodiscard]] auto build() -> Result<ServerRuntime>;

    [[nodiscard]] auto create_reactors(Store& store, ConnectionHandoffMesh& mesh,
                                       DiskReadExecutor& disk_reads, PairWriterPool& pair_writers,
                                       ServerLifecycleProbes probes)
        -> Result<std::vector<std::unique_ptr<Reactor>>>;

  private:
    ReactorConfig config_;
    StoreConfig store_config_;
    std::shared_ptr<TlsContext> tls_context_;
    std::shared_ptr<AbuseController> abuse_;
    std::shared_ptr<SecurityAudit> security_audit_;
};

} // namespace glifistore::server
