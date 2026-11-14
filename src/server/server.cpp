#include "glifistore/server/server.hpp"

#include "glifistore/core/fault_injection.hpp"
#include "server/server_builder.hpp"
#include "server/server_runtime.hpp"
#include "server/server_stats.hpp"
#include "store/store_internal.hpp"

#include <filesystem>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace glifistore::server {
namespace {

[[nodiscard]] auto probe_server_live(const void* context) noexcept -> bool {
    return static_cast<const Server*>(context)->live();
}

[[nodiscard]] auto probe_server_ready(const void* context) noexcept -> bool {
    return static_cast<const Server*>(context)->ready();
}

[[nodiscard]] auto probe_server_stats(const void* context, std::string& out) noexcept -> bool {
    try {
        auto report = static_cast<const Server*>(context)->stats_report();
        if (!report) {
            return false;
        }
        out = std::move(*report);
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] auto format_backup_ok_report(const DurableStoreBackupReport& report) -> std::string {
    if (glifistore::fault::consume_fail(glifistore::fault::Site::backup_report)) {
        throw std::bad_alloc{};
    }
    const auto destination = report.destination.string();
    std::string out;
    out.reserve(256U + destination.size());
    out.append("status=ok\n");
    out.append("destination=");
    out.append(destination);
    out.push_back('\n');
    out.append("files_copied=");
    out.append(std::to_string(report.files_copied));
    out.push_back('\n');
    out.append("bytes_copied=");
    out.append(std::to_string(report.bytes_copied));
    out.push_back('\n');
    out.append("admission_fence_ns=");
    out.append(std::to_string(report.admission_fence_ns));
    out.push_back('\n');
    out.append("catalog_copy_ns=");
    out.append(std::to_string(report.catalog_copy_ns));
    out.push_back('\n');
    out.append("destination_verify_ns=");
    out.append(std::to_string(report.destination_verify_ns));
    out.push_back('\n');
    out.append("segment_copy_workers=");
    out.append(std::to_string(report.segment_copy_workers));
    out.push_back('\n');
    out.append("source_crc_scanned=");
    out.push_back(report.source_crc_scanned ? '1' : '0');
    out.push_back('\n');
    out.append("destination_crc_scanned=");
    out.push_back(report.destination_crc_scanned ? '1' : '0');
    out.push_back('\n');
    out.append("source_segments=");
    out.append(std::to_string(report.source_verification.segments.size()));
    out.push_back('\n');
    out.append("destination_segments=");
    out.append(std::to_string(report.destination_verification.segments.size()));
    out.push_back('\n');
    return out;
}

void assign_probe_failure(std::string& out, const std::string_view message) noexcept {
    try {
        out.assign(message);
    } catch (...) {
        out.clear();
    }
}

[[nodiscard]] auto probe_server_backup(void* context, const std::string_view destination,
                                       std::string& out) noexcept -> bool {
    try {
        auto* server = static_cast<Server*>(context);
        auto report = server->backup_to(std::filesystem::path{std::string{destination}});
        if (!report) {
            out = report.error().message.empty() ? std::string{"backup failed"}
                                                 : std::string{report.error().message};
            return false;
        }
        // Backup already committed. Report formatting must not flip success → false
        // (wire INTERNAL_ERROR / client new_attempt while the destination holds the copy).
        try {
            out = format_backup_ok_report(*report);
        } catch (...) {
            out = "status=ok\n";
        }
        return true;
    } catch (const std::exception& exception) {
        assign_probe_failure(out, exception.what());
        return false;
    } catch (...) {
        assign_probe_failure(out, "backup failed");
        return false;
    }
}

} // namespace

Server::Server(ReactorConfig config, ServerRuntime&& runtime)
    : config_(std::move(config)), store_(std::move(runtime.store)),
      disk_reads_(std::move(runtime.disk_reads)), pair_writers_(std::move(runtime.pair_writers)),
      mesh_(std::move(runtime.mesh)), reactors_(std::move(runtime.reactors)) {
    threads_.reserve(store_->worker_count());
    affinity_results_.resize(store_->worker_count());
}

Server::~Server() {
    request_stop();
    static_cast<void>(join());
}

auto Server::create(const ReactorConfig& config, StoreConfig store_config)
    -> Result<std::unique_ptr<Server>> {
    ServerBuilder builder{config, std::move(store_config)};
    auto runtime = builder.build();
    if (!runtime) {
        return unexpected(runtime.error());
    }
    auto server = std::unique_ptr<Server>{new Server{config, std::move(*runtime)}};
    const ServerLifecycleProbes lifecycle_probes{
        .live = probe_server_live,
        .ready = probe_server_ready,
        .stats = probe_server_stats,
        .backup = probe_server_backup,
        .context = server.get(),
    };
    auto reactors = builder.create_reactors(*server->store_, server->mesh_, *server->disk_reads_,
                                            *server->pair_writers_, lifecycle_probes);
    if (!reactors) {
        return unexpected(reactors.error());
    }
    server->reactors_ = std::move(*reactors);
    return server;
}

auto Server::port() const noexcept -> std::uint16_t {
    if (reactors_.empty()) {
        return 0;
    }
    const auto cleartext = reactors_.front()->cleartext_port();
    return cleartext != 0 ? cleartext : reactors_.front()->tls_port();
}

auto Server::cleartext_port() const noexcept -> std::uint16_t {
    return reactors_.empty() ? 0 : reactors_.front()->cleartext_port();
}

auto Server::tls_port() const noexcept -> std::uint16_t {
    return reactors_.empty() ? 0 : reactors_.front()->tls_port();
}

auto Server::unix_socket_path() const -> std::string {
    if (reactors_.empty()) {
        return {};
    }
    return std::string{reactors_.front()->unix_socket_path()};
}

auto Server::first_failure() const -> std::optional<Error> {
    const std::lock_guard lock{failure_mutex_};
    return failure_;
}

auto Server::reactor_buffer_stats() const noexcept -> ReactorBufferStats {
    ReactorBufferStats stats;
    for (const auto& reactor : reactors_) {
        stats.input_compactions += reactor->input_buffer_compactions();
        stats.input_bytes_moved += reactor->input_buffer_bytes_moved();
        stats.output_compactions += reactor->output_buffer_compactions();
        stats.output_bytes_moved += reactor->output_buffer_bytes_moved();
    }
    return stats;
}

auto Server::stats_report() const -> Result<std::string> {
    if (!live()) {
        return fail(ErrorCode::unavailable, "server is not live");
    }
    ServerStatsSnapshot snapshot{
        .live = true,
        .ready = ready(),
        .store_operational = store_operational(),
        .maintenance = store_->maintenance_snapshot(),
        .mutations = pair_writer_stats(),
        .batches = durable_batch_stats(),
    };
    snapshot.executors.reserve(reactors_.size());
    for (const auto& reactor : reactors_) {
        snapshot.executors.push_back(ExecutorStats{
            .active_connections = reactor->active_connections(),
            .adopted_connections = reactor->adopted_connections(),
            .output_scatter_responses = reactor->output_scatter_responses(),
            .output_scatter_bytes = reactor->output_scatter_bytes(),
            .output_scatter_partial_writes = reactor->output_scatter_partial_writes(),
            .output_scatter_completions = reactor->output_scatter_completions(),
            .input_buffer_compactions = reactor->input_buffer_compactions(),
            .input_buffer_bytes_moved = reactor->input_buffer_bytes_moved(),
            .output_buffer_compactions = reactor->output_buffer_compactions(),
            .output_buffer_bytes_moved = reactor->output_buffer_bytes_moved(),
        });
    }
    if (!reactors_.empty()) {
        snapshot.abuse = reactors_.front()->abuse_stats();
        snapshot.security_audit = reactors_.front()->security_audit_stats();
    }
    snapshot.tls_enabled = config_.tls.requested();
    snapshot.tls_mtls = config_.tls.mtls_enabled();
    snapshot.tls_crl = config_.tls.crl_enabled();
    snapshot.tls_ocsp_fail_closed = config_.tls.ocsp_fail_closed;
    snapshot.authz_enabled = config_.authz.enabled();
    snapshot.authz_principals = config_.authz.size();
    constexpr std::size_t kStatsBudgetBytes = std::size_t{256} * 1024U;
    return ServerStatsReporter::render(snapshot, kStatsBudgetBytes);
}

auto Server::backup_to(const std::filesystem::path& destination, const bool scan_records)
    -> Result<DurableStoreBackupReport> {
    if (!live()) {
        return fail(ErrorCode::unavailable, "server is not live");
    }
    if (stop_requested()) {
        return fail(ErrorCode::unavailable, "server is shutting down");
    }
    if (!store_) {
        return fail(ErrorCode::unavailable, "server has no Store");
    }
    return store_->backup_to(destination, scan_records);
}

auto Server::adopted_connections_per_executor() const -> std::vector<std::size_t> {
    std::vector<std::size_t> adopted;
    adopted.reserve(reactors_.size());
    for (const auto& reactor : reactors_) {
        adopted.push_back(reactor->adopted_connections());
    }
    return adopted;
}

auto Server::active_connections_per_executor() const -> std::vector<std::size_t> {
    std::vector<std::size_t> active;
    active.reserve(reactors_.size());
    for (const auto& reactor : reactors_) {
        active.push_back(reactor->active_connections());
    }
    return active;
}

auto Server::executor_affinity_results() const -> std::vector<ExecutorAffinityResult> {
    return affinity_results_;
}

auto Server::pair_writer_stats() const -> std::vector<PairWriterStats> {
    return pair_writers_->stats();
}

auto Server::durable_batch_stats() const -> std::vector<DurableBatchWorkerStats> {
    return detail::StoreAccess::batch_stats(*store_);
}

} // namespace glifistore::server
