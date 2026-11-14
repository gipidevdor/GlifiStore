#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/persistence/runtime_catalog.hpp"
#include "glifistore/server/abuse_limits.hpp"
#include "glifistore/server/pair_writer.hpp"
#include "glifistore/server/security_audit.hpp"
#include "glifistore/store/maintenance_types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace glifistore::server {

struct ExecutorStats final {
    std::size_t active_connections{};
    std::size_t adopted_connections{};
    std::uint64_t output_scatter_responses{};
    std::uint64_t output_scatter_bytes{};
    std::uint64_t output_scatter_partial_writes{};
    std::uint64_t output_scatter_completions{};
    std::uint64_t input_buffer_compactions{};
    std::uint64_t input_buffer_bytes_moved{};
    std::uint64_t output_buffer_compactions{};
    std::uint64_t output_buffer_bytes_moved{};
};

// Consistent point-in-time view used by ServerStatsReporter. Collection of
// these fields is separate from ASCII serialization.
struct ServerStatsSnapshot final {
    bool live{};
    bool ready{};
    bool store_operational{};
    std::vector<ExecutorStats> executors{};
    MaintenanceSnapshot maintenance{};
    std::vector<PairWriterStats> mutations{};
    std::vector<DurableBatchWorkerStats> batches{};
    AbuseStats abuse{};
    SecurityAuditStats security_audit{};
    bool tls_enabled{};
    bool tls_mtls{};
    bool tls_crl{};
    bool tls_ocsp_fail_closed{};
    bool authz_enabled{};
    std::size_t authz_principals{};
};

class ServerStatsReporter final {
  public:
    ServerStatsReporter() = delete;

    [[nodiscard]] static auto render(const ServerStatsSnapshot& snapshot, std::size_t maximum_bytes)
        -> Result<std::string>;

    [[nodiscard]] static auto maintenance_state_name(MaintenanceState state) noexcept -> std::string_view;
    [[nodiscard]] static auto maintenance_pressure_name(MaintenancePressureLevel level) noexcept
        -> std::string_view;
    [[nodiscard]] static auto maintenance_skip_reason_name(MaintenanceSkipReason reason) noexcept
        -> std::string_view;
    [[nodiscard]] static auto maintenance_activation_reason_name(MaintenanceActivationReason reason) noexcept
        -> std::string_view;
};

} // namespace glifistore::server
