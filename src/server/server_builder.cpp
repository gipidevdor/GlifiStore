#include "server/server_builder.hpp"

#include "glifistore/server/tls.hpp"
#include "server/reactor_factory.hpp"

#include <chrono>
#include <limits>
#include <utility>

namespace glifistore::server {
namespace {

[[nodiscard]] auto validate_config(const ReactorConfig& config) -> Status {
    constexpr std::size_t maximum_queue_capacity = std::size_t{1} << 30U;
    if (config.maximum_connections == 0 || config.worker_count == 0 || config.event_batch_size == 0 ||
        config.connection_handoff_capacity == 0 || config.disk_read_queue_capacity == 0 ||
        config.durable_mutation_queue_capacity == 0 || config.durable_mutation_queue_bytes == 0 ||
        config.connection_handoff_capacity > maximum_queue_capacity ||
        config.disk_read_queue_capacity > maximum_queue_capacity ||
        config.durable_mutation_queue_capacity > maximum_queue_capacity ||
        config.disk_read_thread_count > kMaximumWorkerCount ||
        config.maximum_connections > std::numeric_limits<std::uint32_t>::max()) {
        return fail(ErrorCode::invalid_argument, "server capacity configuration is outside supported limits");
    }
    if (config.worker_count > kMaximumWorkerCount) {
        return fail(ErrorCode::invalid_argument, "server worker count exceeds the supported maximum");
    }
    if (config.disk_read_thread_count != 0 && config.disk_read_thread_count != config.worker_count) {
        return fail(ErrorCode::invalid_argument, "paired disk-read thread count must equal shard-pair count");
    }
    if (config.maximum_input_bytes < kRequestHeaderBytes ||
        config.maximum_output_bytes < kResponseHeaderBytes) {
        return fail(ErrorCode::invalid_argument, "server buffers are smaller than protocol headers");
    }
    if (config.accepted_socket_send_buffer_bytes >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return fail(ErrorCode::invalid_argument, "accepted socket send buffer exceeds the platform limit");
    }
    if (config.tls_port.has_value() && !config.tls.requested()) {
        return fail(ErrorCode::invalid_argument,
                    "tls_port requires TLS certificate configuration (--tls-cert/--tls-key)");
    }
    if (config.tls_port.has_value() && *config.tls_port != 0 && config.port != 0 &&
        *config.tls_port == config.port) {
        return fail(ErrorCode::invalid_argument,
                    "cleartext port and tls_port must differ (no opportunistic TLS on one endpoint)");
    }
    if (auto tls = validate_tls_config(config.tls); !tls) {
        return tls;
    }
    return {};
}

} // namespace

ServerBuilder::ServerBuilder(const ReactorConfig& config, StoreConfig store_config)
    : config_(config), store_config_(std::move(store_config)) {}

auto ServerBuilder::build() -> Result<ServerRuntime> {
    if (auto valid = validate_config(config_); !valid) {
        return unexpected(valid.error());
    }
    if (store_config_.worker_config.explicit_count &&
        *store_config_.worker_config.explicit_count != config_.worker_count) {
        return fail(ErrorCode::invalid_argument, "Store worker count must match the server executor count");
    }
    store_config_.worker_config.explicit_count = config_.worker_count;
    store_config_.concurrency = StoreConcurrencyMode::paired;
    store_config_.paired.async_lane_capacity = config_.durable_mutation_queue_capacity;
    store_config_.paired.async_lane_payload_bytes = config_.durable_mutation_queue_bytes;
    store_config_.paired.async_queue_wait_ms = config_.durable_mutation_queue_wait_ms;
    store_config_.paired.reader_epoch_lease = true;
    if (store_config_.storage_mode != StorageMode::volatile_memory) {
        // Generation-only ordinary reads for the daemon paired path.
        store_config_.durable_limits.hot_cache_enabled = false;
    }
    auto store = Store::open(store_config_);
    if (!store) {
        return unexpected(store.error());
    }
    auto disk_reads =
        DiskReadExecutor::create(**store, config_.worker_count, config_.disk_read_queue_capacity);
    if (!disk_reads) {
        return unexpected(disk_reads.error());
    }
    auto pair_writers =
        PairWriterPool::create(**store, config_.worker_count, config_.durable_mutation_queue_capacity,
                               config_.durable_mutation_queue_bytes,
                               std::chrono::milliseconds{config_.durable_mutation_queue_wait_ms});
    if (!pair_writers) {
        return unexpected(pair_writers.error());
    }
    if (config_.tls.requested()) {
        auto created = TlsContext::create(config_.tls);
        if (!created) {
            return unexpected(created.error());
        }
        tls_context_ = std::move(*created);
    }
    if (config_.abuse.any_enabled()) {
        abuse_ = std::make_shared<AbuseController>(config_.abuse);
    }
    if (config_.security_audit_events || config_.authz.enabled() ||
        (tls_context_ && tls_context_->mtls_enabled())) {
        security_audit_ = std::make_shared<SecurityAudit>(config_.security_audit_events, config_.quiet);
    }
    const auto worker_count = (*store)->worker_count();
    ServerRuntime runtime{
        .store = std::move(*store),
        .disk_reads = std::move(*disk_reads),
        .pair_writers = std::move(*pair_writers),
        .mesh = ConnectionHandoffMesh{worker_count, config_.connection_handoff_capacity},
        .reactors = {},
    };
    return runtime;
}

auto ServerBuilder::create_reactors(Store& store, ConnectionHandoffMesh& mesh, DiskReadExecutor& disk_reads,
                                    PairWriterPool& pair_writers, const ServerLifecycleProbes probes)
    -> Result<std::vector<std::unique_ptr<Reactor>>> {
    return ReactorFactory::create_all(config_, store, mesh, disk_reads, pair_writers, probes, tls_context_,
                                      abuse_, security_audit_);
}

} // namespace glifistore::server
