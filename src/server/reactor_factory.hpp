#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/server/abuse_limits.hpp"
#include "glifistore/server/connection_handoff.hpp"
#include "glifistore/server/disk_read_executor.hpp"
#include "glifistore/server/pair_writer.hpp"
#include "glifistore/server/reactor.hpp"
#include "glifistore/server/security_audit.hpp"
#include "glifistore/server/tls.hpp"
#include "glifistore/store/store.hpp"

#include <memory>
#include <vector>

namespace glifistore::server {

class ReactorFactory final {
  public:
    ReactorFactory() = delete;

    [[nodiscard]] static auto create_all(const ReactorConfig& config, Store& store,
                                         ConnectionHandoffMesh& mesh, DiskReadExecutor& disk_reads,
                                         PairWriterPool& pair_writers, ServerLifecycleProbes lifecycle_probes,
                                         const std::shared_ptr<TlsContext>& tls_context,
                                         std::shared_ptr<AbuseController> abuse = {},
                                         std::shared_ptr<SecurityAudit> security_audit = {})
        -> Result<std::vector<std::unique_ptr<Reactor>>>;
};

} // namespace glifistore::server
