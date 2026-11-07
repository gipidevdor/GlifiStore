#include "glifistore/client/client.hpp"
#include "glifistore/core/hot_path_phases.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/core/little_endian.hpp"
#include "glifistore/server/protocol.hpp"
#include "glifistore/server/server.hpp"
#include "harness.hpp"
#include "parse.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <latch>
#include <limits>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using glifistore::bench::Config;
using glifistore::bench::ParallelDistribution;
using glifistore::bench::Result;
using glifistore::bench::RunSettings;

enum class StorageProfile : std::uint8_t { volatile_memory, durable_sync, durable_group, durable_periodic };
enum class Workload : std::uint8_t {
    read_after_write,
    get_only,
    read_99_write_1,
    read_95_write_5,
    read_90_write_10,
};

struct Options {
    Config config{.workers = 1, .threads = 1, .distribution = ParallelDistribution::owner_bound};
    RunSettings settings{};
    std::size_t pipeline{32};
    std::size_t client_pipeline{};
    bool executor_affinity{};
    bool latency{};
    bool latency_split{};
    std::size_t latency_sample_stride{1};
    bool client_api{};
    Workload workload{Workload::read_after_write};
    StorageProfile storage{StorageProfile::volatile_memory};
    std::uint32_t group_max_records{32};
    std::uint32_t group_max_bytes{65'536};
    std::uint32_t group_max_wait_ms{10};
    std::uint32_t periodic_sync_ms{1'000};
    std::uint32_t maintenance_suspend_on_p99_latency_ms{};
    std::uint32_t maintenance_suspend_on_p99_min_samples{32};
    std::uint32_t maintenance_max_latency_deferral_ms{30'000};
    std::size_t maintenance_overlap_seed_operations{};
    std::size_t maintenance_overlap_seed_keys{128};
    std::size_t maintenance_overlap_seed_value_bytes{std::size_t{256} * 1024U};
    std::uint32_t maintenance_overlap_eval_ms{250};
    std::uint32_t maintenance_overlap_release_ms{500};
};

struct DurableProfileSample {
    double average_queue_wait_ns{};
    double maximum_queue_wait_ns{};
    double average_service_ns{};
    double maximum_service_ns{};
    double average_commit_ns{};
    double maximum_commit_ns{};
    double average_batch_records{};
    double maximum_batch_records{};
    std::uint64_t completed{};
    std::uint64_t rejected{};
    std::uint64_t expired{};
    std::uint64_t committed_batches{};
    std::uint64_t committed_records{};
    std::uint64_t committed_bytes{};
    std::uint64_t failed_batches{};
    std::uint64_t maximum_queue_depth{};
    std::uint64_t maximum_queued_bytes{};
    std::uint64_t pending_records{};
    std::uint64_t pending_bytes{};
    std::uint64_t record_limit_closes{};
    std::uint64_t byte_limit_closes{};
    std::uint64_t adaptive_target_closes{};
    std::uint64_t deadline_closes{};
    double average_paired_writer_batch_records{};
    double average_paired_writer_batch_wait_ns{};
    double maximum_paired_writer_batch_wait_ns{};
    std::uint64_t paired_writer_batches{};
    std::uint64_t paired_writer_batch_records{};
    std::uint64_t paired_writer_durability_deadline_closes{};
    std::uint64_t paired_writer_queue_deadline_closes{};
    std::uint64_t paired_sync_turn_splits{};
    std::uint64_t paired_sync_async_fairness_turns{};
    std::uint64_t paired_publications{};
    std::uint64_t paired_publication_records{};
    std::uint64_t paired_completion_notifications{};
    std::uint64_t maintenance_evaluations{};
    std::uint64_t maintenance_compact_attempts{};
    std::uint64_t maintenance_useful_compactions{};
    std::uint64_t maintenance_latency_suspends{};
    std::uint64_t maintenance_latency_debt_overrides{};
    std::uint64_t maintenance_foreground_latency_samples{};
    std::uint64_t maintenance_last_foreground_p99_ns{};
};

struct ReactorProfileSample {
    std::uint64_t input_buffer_compactions{};
    std::uint64_t input_buffer_bytes_moved{};
    std::uint64_t output_buffer_compactions{};
    std::uint64_t output_buffer_bytes_moved{};
};

struct ClientWork {
    std::vector<std::vector<std::byte>> batches;
    std::size_t response_count{};
};

struct Sample {
    std::size_t hits{};
    double seconds{};
    glifistore::bench::ResourceSample resources{};
    std::vector<double> latency_ns;
    std::vector<double> get_latency_ns;
    std::vector<double> put_latency_ns;
    bool valid{};
    DurableProfileSample durable;
    ReactorProfileSample reactor;
};

struct ClientResult {
    std::size_t hits{};
    std::size_t ingress_bytes{};
    std::size_t egress_bytes{};
    std::vector<double> latency_ns;
    std::vector<double> get_latency_ns;
    std::vector<double> put_latency_ns;
};

[[nodiscard]] auto parse_size(const std::string_view value, const char* flag) -> std::size_t {
    const auto parsed = glifistore::bench::parse_decimal_size(value);
    if (!parsed) {
        std::cerr << "invalid value for " << flag << ": " << value << '\n';
        std::exit(2);
    }
    return *parsed;
}

[[nodiscard]] auto parse_u32(const std::string_view value, const char* flag) -> std::uint32_t {
    const auto parsed = parse_size(value, flag);
    if (parsed > std::numeric_limits<std::uint32_t>::max()) {
        std::cerr << "value for " << flag << " exceeds uint32: " << value << '\n';
        std::exit(2);
    }
    return static_cast<std::uint32_t>(parsed);
}

[[nodiscard]] auto parse_storage(const std::string_view value) -> StorageProfile {
    if (value == "volatile") {
        return StorageProfile::volatile_memory;
    }
    if (value == "durable-sync") {
        return StorageProfile::durable_sync;
    }
    if (value == "durable-group") {
        return StorageProfile::durable_group;
    }
    if (value == "durable-periodic") {
        return StorageProfile::durable_periodic;
    }
    std::cerr << "invalid value for --storage-mode: " << value << '\n';
    std::exit(2);
}

[[nodiscard]] auto storage_name(const StorageProfile profile) noexcept -> std::string_view {
    switch (profile) {
    case StorageProfile::volatile_memory:
        return "volatile";
    case StorageProfile::durable_sync:
        return "durable-sync";
    case StorageProfile::durable_group:
        return "durable-group";
    case StorageProfile::durable_periodic:
        return "durable-periodic";
    }
    return "unknown";
}

[[nodiscard]] auto workload_name(const Workload workload) noexcept -> std::string_view {
    switch (workload) {
    case Workload::read_after_write:
        return "read_after_write";
    case Workload::get_only:
        return "get_only";
    case Workload::read_99_write_1:
        return "read_99_write_1";
    case Workload::read_95_write_5:
        return "read_95_write_5";
    case Workload::read_90_write_10:
        return "read_90_write_10";
    }
    return "unknown";
}

[[nodiscard]] constexpr auto mixed_write_period(const Workload workload) noexcept
    -> std::optional<std::size_t> {
    switch (workload) {
    case Workload::read_99_write_1:
        return 100U;
    case Workload::read_95_write_5:
        return 20U;
    case Workload::read_90_write_10:
        return 10U;
    case Workload::read_after_write:
    case Workload::get_only:
        return std::nullopt;
    }
    return std::nullopt;
}

class BenchmarkDataDirectory final {
  public:
    explicit BenchmarkDataDirectory(const StorageProfile storage) {
        if (storage == StorageProfile::volatile_memory) {
            return;
        }
        static std::atomic_uint64_t counter{};
        path_ = std::filesystem::temp_directory_path() /
                ("glifistore-server-bench-" + std::to_string(static_cast<unsigned long>(::getpid())) + '-' +
                 std::to_string(counter.fetch_add(1U, std::memory_order_relaxed)));
    }

    ~BenchmarkDataDirectory() {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }

    BenchmarkDataDirectory(const BenchmarkDataDirectory&) = delete;
    auto operator=(const BenchmarkDataDirectory&) -> BenchmarkDataDirectory& = delete;

    [[nodiscard]] auto path() const noexcept -> const std::filesystem::path& {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

class MaintenanceOverlapGate final {
  public:
    explicit MaintenanceOverlapGate(std::filesystem::path directory) : directory_(std::move(directory)) {}

    void release() noexcept {
        if (!released_.exchange(true, std::memory_order_acq_rel)) {
            start_.count_down();
        }
    }

    [[nodiscard]] static auto available_space_bytes(void* context) -> glifistore::Result<std::uint64_t> {
        auto& gate = *static_cast<MaintenanceOverlapGate*>(context);
        if (std::this_thread::get_id() != gate.opener_thread_) {
            gate.start_.wait();
        }
        std::error_code error;
        const auto space = std::filesystem::space(gate.directory_, error);
        if (error) {
            return glifistore::fail(glifistore::ErrorCode::io_error, "server benchmark space probe failed");
        }
        return static_cast<std::uint64_t>(space.available);
    }

  private:
    std::filesystem::path directory_;
    std::thread::id opener_thread_{std::this_thread::get_id()};
    std::latch start_{1};
    std::atomic_bool released_{};
};

[[nodiscard]] auto
store_config(const Options& options, const BenchmarkDataDirectory& directory,
             const glifistore::DurableOpenMode open_mode = glifistore::DurableOpenMode::create_new)
    -> glifistore::StoreConfig {
    glifistore::StoreConfig config{.worker_config = {.explicit_count = options.config.workers}};
    switch (options.storage) {
    case StorageProfile::volatile_memory:
        return config;
    case StorageProfile::durable_sync:
        config.storage_mode = glifistore::StorageMode::durable_sync;
        break;
    case StorageProfile::durable_group:
        config.storage_mode = glifistore::StorageMode::durable_group;
        break;
    case StorageProfile::durable_periodic:
        config.storage_mode = glifistore::StorageMode::durable_periodic;
        break;
    }
    config.data_directory = directory.path();
    config.durable_open_mode = open_mode;
    config.durable_group = {.max_records = options.group_max_records,
                            .max_bytes = options.group_max_bytes,
                            .max_wait_ms = options.group_max_wait_ms,
                            .min_records = 1};
    config.durable_periodic = {.sync_interval_ms = options.periodic_sync_ms,
                               .batch =
                                   glifistore::DurableGroupConfig{.max_records = options.group_max_records,
                                                                  .max_bytes = options.group_max_bytes,
                                                                  .max_wait_ms = options.group_max_wait_ms,
                                                                  .min_records = 1}};
    config.maintenance.suspend_on_p99_latency_ms = options.maintenance_suspend_on_p99_latency_ms;
    config.maintenance.suspend_on_p99_min_samples = options.maintenance_suspend_on_p99_min_samples;
    config.maintenance.max_latency_deferral_ms = options.maintenance_max_latency_deferral_ms;
    if (options.maintenance_overlap_seed_operations != 0) {
        config.maintenance.mode = glifistore::MaintenanceMode::background;
        config.maintenance.min_eval_interval_ms = options.maintenance_overlap_eval_ms;
        config.maintenance.max_eval_interval_ms = options.maintenance_overlap_eval_ms;
        config.maintenance.dead_byte_ratio_bp_normal = 5'000;
    }
    return config;
}

[[nodiscard]] auto key_for_worker(const std::size_t worker, const std::size_t workers,
                                  const std::size_t ordinal) -> std::string {
    std::size_t matched{};
    for (std::size_t suffix = 0; suffix < 10'000'000; ++suffix) {
        auto key = std::string{"maintenance-overlap-"} + std::to_string(suffix);
        if (glifistore::route_worker(key, workers) == worker && matched++ == ordinal) {
            return key;
        }
    }
    return {};
}

[[nodiscard]] auto seed_maintenance_overlap(const Options& options, const BenchmarkDataDirectory& directory)
    -> bool {
    if (options.maintenance_overlap_seed_operations == 0) {
        return true;
    }
    auto config = store_config(options, directory);
    config.maintenance.mode = glifistore::MaintenanceMode::disabled;
    auto opened = glifistore::Store::open(config);
    if (!opened) {
        return false;
    }
    auto store = std::move(*opened);
    std::vector<std::string> keys;
    keys.reserve(options.maintenance_overlap_seed_keys);
    for (std::size_t ordinal = 0; ordinal < options.maintenance_overlap_seed_keys; ++ordinal) {
        // Keep reclaim work off the first owner-bound foreground lane. With at
        // least two Workers this isolates global/catalog interference from
        // same-Worker generation conflicts.
        auto key = key_for_worker(options.config.workers - 1U, options.config.workers, ordinal);
        if (key.empty()) {
            return false;
        }
        keys.push_back(std::move(key));
    }
    std::vector<std::byte> value(options.maintenance_overlap_seed_value_bytes, std::byte{0xA5});
    for (std::size_t operation = 0; operation < options.maintenance_overlap_seed_operations; ++operation) {
        if (!store->put(keys[operation % keys.size()], value)) {
            return false;
        }
    }
    return store->flush().has_value() && store->close().has_value();
}

[[nodiscard]] auto reactor_config(const Options& options) -> glifistore::server::ReactorConfig {
    return {.port = 0,
            .maximum_connections = std::max(std::size_t{16}, options.config.threads * 2U),
            .worker_count = options.config.workers,
            .executor_affinity = options.executor_affinity,
            .durable_mutation_queue_wait_ms = 0};
}

[[nodiscard]] auto durable_profile(const glifistore::server::Server& server,
                                   const std::vector<glifistore::server::PairWriterStats>& paired_before,
                                   const std::vector<glifistore::DurableBatchWorkerStats>& durable_before)
    -> DurableProfileSample {
    DurableProfileSample result;
    std::uint64_t queue_wait_ns{};
    std::uint64_t service_ns{};
    std::uint64_t writer_batches{};
    std::uint64_t writer_batch_wait_ns{};
    const auto counter_delta = [](const auto after, const auto before) {
        return after - std::min(after, before);
    };
    for (const auto& worker : server.pair_writer_stats()) {
        const auto baseline = std::ranges::find(paired_before, worker.worker_index,
                                                &glifistore::server::PairWriterStats::worker_index);
        const auto* before = baseline == paired_before.end() ? nullptr : &*baseline;
        result.completed += counter_delta(worker.completed, before == nullptr ? 0U : before->completed);
        result.rejected += counter_delta(worker.rejected, before == nullptr ? 0U : before->rejected);
        result.expired +=
            counter_delta(worker.expired_before_store, before == nullptr ? 0U : before->expired_before_store);
        result.maximum_queue_depth =
            std::max(result.maximum_queue_depth, static_cast<std::uint64_t>(worker.maximum_queue_depth));
        result.maximum_queued_bytes =
            std::max(result.maximum_queued_bytes, static_cast<std::uint64_t>(worker.maximum_queued_bytes));
        queue_wait_ns +=
            counter_delta(worker.total_queue_wait_ns, before == nullptr ? 0U : before->total_queue_wait_ns);
        service_ns +=
            counter_delta(worker.total_service_ns, before == nullptr ? 0U : before->total_service_ns);
        const auto batch_delta =
            counter_delta(worker.writer_batches, before == nullptr ? 0U : before->writer_batches);
        writer_batches += batch_delta;
        writer_batch_wait_ns += counter_delta(worker.total_writer_batch_wait_ns,
                                              before == nullptr ? 0U : before->total_writer_batch_wait_ns);
        result.paired_writer_batches += batch_delta;
        result.paired_writer_batch_records +=
            counter_delta(worker.writer_batch_records, before == nullptr ? 0U : before->writer_batch_records);
        result.paired_writer_durability_deadline_closes +=
            counter_delta(worker.writer_batch_durability_deadline_closes,
                          before == nullptr ? 0U : before->writer_batch_durability_deadline_closes);
        result.paired_writer_queue_deadline_closes +=
            counter_delta(worker.writer_batch_queue_deadline_closes,
                          before == nullptr ? 0U : before->writer_batch_queue_deadline_closes);
        result.paired_sync_turn_splits +=
            counter_delta(worker.sync_turn_splits, before == nullptr ? 0U : before->sync_turn_splits);
        result.paired_sync_async_fairness_turns += counter_delta(
            worker.sync_async_fairness_turns, before == nullptr ? 0U : before->sync_async_fairness_turns);
        result.paired_publications +=
            counter_delta(worker.publications, before == nullptr ? 0U : before->publications);
        result.paired_publication_records +=
            counter_delta(worker.publication_records, before == nullptr ? 0U : before->publication_records);
        result.paired_completion_notifications += counter_delta(
            worker.completion_notifications, before == nullptr ? 0U : before->completion_notifications);
        result.maximum_queue_wait_ns =
            std::max(result.maximum_queue_wait_ns, static_cast<double>(worker.maximum_queue_wait_ns));
        result.maximum_service_ns =
            std::max(result.maximum_service_ns, static_cast<double>(worker.maximum_service_ns));
        result.maximum_paired_writer_batch_wait_ns =
            std::max(result.maximum_paired_writer_batch_wait_ns,
                     static_cast<double>(worker.maximum_writer_batch_wait_ns));
    }
    result.average_queue_wait_ns =
        result.completed == 0 ? 0.0
                              : static_cast<double>(queue_wait_ns) / static_cast<double>(result.completed);
    const auto serviced = result.completed - std::min(result.completed, result.expired);
    result.average_service_ns =
        serviced == 0 ? 0.0 : static_cast<double>(service_ns) / static_cast<double>(serviced);
    result.average_paired_writer_batch_records =
        writer_batches == 0
            ? 0.0
            : static_cast<double>(result.paired_writer_batch_records) / static_cast<double>(writer_batches);
    result.average_paired_writer_batch_wait_ns =
        writer_batches == 0 ? 0.0
                            : static_cast<double>(writer_batch_wait_ns) / static_cast<double>(writer_batches);

    std::uint64_t commit_ns{};
    for (const auto& worker : server.durable_batch_stats()) {
        const auto baseline = std::ranges::find(durable_before, worker.worker_id,
                                                &glifistore::DurableBatchWorkerStats::worker_id);
        const auto* before = baseline == durable_before.end() ? nullptr : &*baseline;
        result.committed_batches +=
            counter_delta(worker.committed_batches, before == nullptr ? 0U : before->committed_batches);
        result.committed_records +=
            counter_delta(worker.committed_records, before == nullptr ? 0U : before->committed_records);
        result.committed_bytes +=
            counter_delta(worker.committed_bytes, before == nullptr ? 0U : before->committed_bytes);
        result.failed_batches +=
            counter_delta(worker.failed_batches, before == nullptr ? 0U : before->failed_batches);
        result.pending_records += worker.pending_records;
        result.pending_bytes += worker.pending_bytes;
        result.record_limit_closes +=
            counter_delta(worker.record_limit_closes, before == nullptr ? 0U : before->record_limit_closes);
        result.byte_limit_closes +=
            counter_delta(worker.byte_limit_closes, before == nullptr ? 0U : before->byte_limit_closes);
        result.adaptive_target_closes += counter_delta(
            worker.adaptive_target_closes, before == nullptr ? 0U : before->adaptive_target_closes);
        result.deadline_closes +=
            counter_delta(worker.deadline_closes, before == nullptr ? 0U : before->deadline_closes);
        commit_ns += counter_delta(worker.total_commit_duration_ns,
                                   before == nullptr ? 0U : before->total_commit_duration_ns);
        result.maximum_commit_ns =
            std::max(result.maximum_commit_ns, static_cast<double>(worker.maximum_commit_duration_ns));
        result.maximum_batch_records =
            std::max(result.maximum_batch_records, static_cast<double>(worker.maximum_batch_records));
    }
    result.average_commit_ns =
        result.committed_batches == 0
            ? 0.0
            : static_cast<double>(commit_ns) / static_cast<double>(result.committed_batches);
    result.average_batch_records =
        result.committed_batches == 0
            ? 0.0
            : static_cast<double>(result.committed_records) / static_cast<double>(result.committed_batches);
    const auto maintenance = server.maintenance_snapshot();
    result.maintenance_evaluations = maintenance.evaluation_cycles;
    result.maintenance_compact_attempts = maintenance.compact_attempts;
    result.maintenance_useful_compactions = maintenance.useful_compactions;
    result.maintenance_latency_suspends = maintenance.latency_suspends;
    result.maintenance_latency_debt_overrides = maintenance.latency_debt_overrides;
    result.maintenance_foreground_latency_samples = maintenance.foreground_latency_samples;
    result.maintenance_last_foreground_p99_ns = maintenance.last_foreground_p99_ns;
    return result;
}

[[nodiscard]] auto reactor_profile(const glifistore::server::Server& server) -> ReactorProfileSample {
    const auto stats = server.reactor_buffer_stats();
    return {.input_buffer_compactions = stats.input_compactions,
            .input_buffer_bytes_moved = stats.input_bytes_moved,
            .output_buffer_compactions = stats.output_compactions,
            .output_buffer_bytes_moved = stats.output_bytes_moved};
}

[[nodiscard]] auto options(const int argc, char** argv) -> Options {
    Options result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        const auto next_size = [&](const char* flag) {
            if (index + 1 >= argc) {
                std::cerr << "missing value for " << flag << '\n';
                std::exit(2);
            }
            return parse_size(argv[++index], flag);
        };
        if (argument == "--ops") {
            result.config.operations = next_size("--ops");
        } else if (argument == "--key-size") {
            result.config.key_size = next_size("--key-size");
        } else if (argument == "--value-size") {
            result.config.value_size = next_size("--value-size");
        } else if (argument == "--workers") {
            result.config.workers = next_size("--workers");
        } else if (argument == "--clients") {
            result.config.threads = next_size("--clients");
        } else if (argument == "--pipeline") {
            result.pipeline = next_size("--pipeline");
        } else if (argument == "--client-pipeline") {
            result.client_api = true;
            result.client_pipeline = next_size("--client-pipeline");
        } else if (argument == "--warmup") {
            result.settings.warmup_iterations = next_size("--warmup");
        } else if (argument == "--repeats") {
            result.settings.measured_iterations = next_size("--repeats");
        } else if (argument == "--executor-affinity") {
            result.executor_affinity = true;
        } else if (argument == "--latency") {
            result.latency = true;
        } else if (argument == "--latency-split") {
            result.latency = true;
            result.latency_split = true;
        } else if (argument == "--latency-sample-stride") {
            result.latency = true;
            result.latency_sample_stride = next_size("--latency-sample-stride");
            if (result.latency_sample_stride == 0) {
                std::cerr << "--latency-sample-stride must be >= 1\n";
                std::exit(2);
            }
        } else if (argument == "--client-api") {
            result.client_api = true;
        } else if (argument == "--storage-mode") {
            if (index + 1 >= argc) {
                std::cerr << "missing value for --storage-mode\n";
                std::exit(2);
            }
            result.storage = parse_storage(argv[++index]);
        } else if (argument == "--workload") {
            if (index + 1 >= argc) {
                std::cerr << "missing value for --workload\n";
                std::exit(2);
            }
            const std::string_view workload{argv[++index]};
            if (workload == "read-after-write") {
                result.workload = Workload::read_after_write;
            } else if (workload == "get-only") {
                result.workload = Workload::get_only;
            } else if (workload == "read-99-write-1") {
                result.workload = Workload::read_99_write_1;
            } else if (workload == "read-95-write-5") {
                result.workload = Workload::read_95_write_5;
            } else if (workload == "read-90-write-10") {
                result.workload = Workload::read_90_write_10;
            } else {
                std::cerr << "invalid value for --workload: " << workload << '\n';
                std::exit(2);
            }
        } else if (argument == "--group-max-records") {
            if (index + 1 >= argc) {
                std::cerr << "missing value for --group-max-records\n";
                std::exit(2);
            }
            result.group_max_records = parse_u32(argv[++index], "--group-max-records");
        } else if (argument == "--group-max-bytes") {
            if (index + 1 >= argc) {
                std::cerr << "missing value for --group-max-bytes\n";
                std::exit(2);
            }
            result.group_max_bytes = parse_u32(argv[++index], "--group-max-bytes");
        } else if (argument == "--group-max-wait-ms") {
            if (index + 1 >= argc) {
                std::cerr << "missing value for --group-max-wait-ms\n";
                std::exit(2);
            }
            result.group_max_wait_ms = parse_u32(argv[++index], "--group-max-wait-ms");
        } else if (argument == "--periodic-sync-ms") {
            if (index + 1 >= argc) {
                std::cerr << "missing value for --periodic-sync-ms\n";
                std::exit(2);
            }
            result.periodic_sync_ms = parse_u32(argv[++index], "--periodic-sync-ms");
        } else if (argument == "--maintenance-suspend-on-p99-latency-ms") {
            if (index + 1 >= argc) {
                std::cerr << "missing value for --maintenance-suspend-on-p99-latency-ms\n";
                std::exit(2);
            }
            result.maintenance_suspend_on_p99_latency_ms =
                parse_u32(argv[++index], "--maintenance-suspend-on-p99-latency-ms");
        } else if (argument == "--maintenance-suspend-on-p99-min-samples") {
            if (index + 1 >= argc) {
                std::cerr << "missing value for --maintenance-suspend-on-p99-min-samples\n";
                std::exit(2);
            }
            result.maintenance_suspend_on_p99_min_samples =
                parse_u32(argv[++index], "--maintenance-suspend-on-p99-min-samples");
        } else if (argument == "--maintenance-max-latency-deferral-ms") {
            if (index + 1 >= argc) {
                std::cerr << "missing value for --maintenance-max-latency-deferral-ms\n";
                std::exit(2);
            }
            result.maintenance_max_latency_deferral_ms =
                parse_u32(argv[++index], "--maintenance-max-latency-deferral-ms");
        } else if (argument == "--maintenance-overlap-seed-operations") {
            result.maintenance_overlap_seed_operations = next_size("--maintenance-overlap-seed-operations");
        } else if (argument == "--maintenance-overlap-seed-keys") {
            result.maintenance_overlap_seed_keys = next_size("--maintenance-overlap-seed-keys");
        } else if (argument == "--maintenance-overlap-seed-value-bytes") {
            result.maintenance_overlap_seed_value_bytes = next_size("--maintenance-overlap-seed-value-bytes");
        } else if (argument == "--maintenance-overlap-eval-ms") {
            if (index + 1 >= argc) {
                std::cerr << "missing value for --maintenance-overlap-eval-ms\n";
                std::exit(2);
            }
            result.maintenance_overlap_eval_ms = parse_u32(argv[++index], "--maintenance-overlap-eval-ms");
        } else if (argument == "--maintenance-overlap-release-ms") {
            if (index + 1 >= argc) {
                std::cerr << "missing value for --maintenance-overlap-release-ms\n";
                std::exit(2);
            }
            result.maintenance_overlap_release_ms =
                parse_u32(argv[++index], "--maintenance-overlap-release-ms");
        } else if (argument == "--help" || argument == "-h") {
            std::cout << "usage: glifistore_server_benchmarks [--ops N] [--key-size N]"
                         " [--value-size N] [--workers N] [--clients N] [--pipeline N]"
                         " [--executor-affinity] [--latency] [--latency-split]"
                         " [--latency-sample-stride N] [--client-api] [--client-pipeline N]"
                         " [--workload read-after-write|get-only|read-99-write-1|read-95-write-5|"
                         "read-90-write-10]"
                         " [--storage-mode volatile|durable-sync|durable-group|durable-periodic]"
                         " [--group-max-records N] [--group-max-bytes N] [--group-max-wait-ms N]"
                         " [--periodic-sync-ms N]"
                         " [--maintenance-suspend-on-p99-latency-ms N]"
                         " [--maintenance-suspend-on-p99-min-samples N]"
                         " [--maintenance-max-latency-deferral-ms N]"
                         " [--maintenance-overlap-seed-operations N]"
                         " [--maintenance-overlap-seed-keys N]"
                         " [--maintenance-overlap-seed-value-bytes N]"
                         " [--maintenance-overlap-eval-ms N]"
                         " [--maintenance-overlap-release-ms N]"
                         " [--warmup N] [--repeats N]\n";
            std::exit(0);
        } else {
            std::cerr << "unknown or incomplete argument: " << argument << '\n';
            std::exit(2);
        }
    }
    return result;
}

[[nodiscard]] auto bytes(const std::string_view value) noexcept -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

[[nodiscard]] auto connect_to(const std::uint16_t port) -> int {
    const auto descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) {
        return -1;
    }
    const int enabled = 1;
    static_cast<void>(::setsockopt(descriptor, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)));
    timeval timeout{.tv_sec = 10, .tv_usec = 0};
    static_cast<void>(::setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    static_cast<void>(::setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)));
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    static_cast<void>(::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)));
#endif
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    static_cast<void>(::inet_pton(AF_INET, "127.0.0.1", &endpoint.sin_addr));
    if (::connect(descriptor, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
        static_cast<void>(::close(descriptor));
        return -1;
    }
    return descriptor;
}

[[nodiscard]] auto send_all(const int descriptor, const std::span<const std::byte> data) -> bool {
    std::size_t sent{};
    while (sent < data.size()) {
#if defined(__linux__)
        const auto written = ::send(descriptor, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
#else
        const auto written = ::send(descriptor, data.data() + sent, data.size() - sent, 0);
#endif
        if (written > 0) {
            sent += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

[[nodiscard]] auto load_u32(const std::span<const std::byte> input) noexcept -> std::uint32_t {
    return glifistore::le::get_u32(input, 0);
}

class BufferedResponseReader final {
  public:
    explicit BufferedResponseReader(const std::size_t reserve_bytes) {
        buffer_.reserve(reserve_bytes);
    }

    [[nodiscard]] auto receive(const int descriptor)
        -> glifistore::Result<glifistore::server::DecodedFrame<glifistore::server::ResponseView>> {
        while (true) {
            const auto available = buffer_.size() - offset_;
            if (available >= sizeof(std::uint32_t)) {
                const std::span<const std::byte> pending{buffer_.data() + offset_, available};
                const auto size = static_cast<std::size_t>(load_u32(pending));
                if (size < glifistore::server::kResponseHeaderBytes ||
                    size > glifistore::server::kMaxFrameBytes) {
                    return glifistore::fail(glifistore::ErrorCode::invalid_record,
                                            "benchmark response size is invalid");
                }
                if (available >= size) {
                    auto decoded = glifistore::server::decode_response(pending.first(size));
                    if (decoded) {
                        offset_ += size;
                    }
                    return decoded;
                }
            }

            if (offset_ > 0) {
                buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(offset_));
                offset_ = 0;
            }
            std::array<std::byte, std::size_t{64} * 1024U> chunk;
            const auto count = ::recv(descriptor, chunk.data(), chunk.size(), 0);
            if (count > 0) {
                buffer_.insert(buffer_.end(), chunk.begin(),
                               chunk.begin() + static_cast<std::ptrdiff_t>(count));
                continue;
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            return glifistore::fail(glifistore::ErrorCode::io_error, "benchmark response receive failed");
        }
    }

  private:
    std::vector<std::byte> buffer_;
    std::size_t offset_{};
};

[[nodiscard]] auto make_material(const Config& config) -> glifistore::bench::KeyMaterial {
    glifistore::bench::KeyMaterial material;
    material.keys.reserve(config.operations);
    material.values.reserve(config.operations);
    std::size_t candidate{};
    for (std::size_t operation = 0; operation < config.operations; ++operation) {
        std::string key;
        const auto client = operation % config.threads;
        do {
            key = glifistore::bench::make_key(candidate++, config.key_size);
        } while (glifistore::route_worker(key, config.workers) != client % config.workers);
        material.keys.push_back(std::move(key));
        material.values.push_back(glifistore::bench::make_value(operation, config.value_size));
    }
    return material;
}

[[nodiscard]] auto prepare_work(const Config& config, const std::size_t pipeline,
                                const glifistore::bench::KeyMaterial& material, const Workload workload)
    -> std::vector<ClientWork> {
    std::vector<ClientWork> work(config.threads);
    for (std::size_t client = 0; client < config.threads; ++client) {
        std::vector<std::byte> batch;
        std::size_t keys_in_batch{};
        for (std::size_t operation = client; operation < config.operations; operation += config.threads) {
            if (workload == Workload::read_after_write) {
                const auto put = glifistore::server::encode_request({
                    .opcode = glifistore::server::RequestOpcode::put,
                    .request_id = operation * 2U,
                    .key = bytes(material.keys[operation]),
                    .value = material.values[operation],
                });
                if (!put) {
                    return {};
                }
                batch.insert(batch.end(), put->begin(), put->end());
                ++work[client].response_count;
            }
            const auto write_period = mixed_write_period(workload);
            if (write_period && operation % *write_period == 0) {
                const auto put = glifistore::server::encode_request({
                    .opcode = glifistore::server::RequestOpcode::put,
                    .request_id = operation * 2U,
                    .key = bytes(material.keys[operation]),
                    .value = material.values[operation],
                });
                if (!put) {
                    return {};
                }
                batch.insert(batch.end(), put->begin(), put->end());
            } else {
                const auto get = glifistore::server::encode_request({
                    .opcode = glifistore::server::RequestOpcode::get,
                    .request_id = operation * 2U + 1U,
                    .key = bytes(material.keys[operation]),
                });
                if (!get) {
                    return {};
                }
                batch.insert(batch.end(), get->begin(), get->end());
            }
            ++keys_in_batch;
            ++work[client].response_count;
            if (keys_in_batch == pipeline) {
                work[client].batches.push_back(std::move(batch));
                batch.clear();
                keys_in_batch = 0;
            }
        }
        if (!batch.empty()) {
            work[client].batches.push_back(std::move(batch));
        }
    }
    return work;
}

[[nodiscard]] auto run_client(const int descriptor, const ClientWork& work,
                              const glifistore::bench::KeyMaterial& material, const std::size_t pipeline,
                              const bool measure_latency, const bool latency_split,
                              const std::size_t latency_sample_stride, const Workload workload)
    -> ClientResult {
    const auto bytes_per_pair =
        2U * (glifistore::server::kResponseHeaderBytes + material.values.front().size());
    const auto response_capacity = pipeline > glifistore::server::kMaxFrameBytes / bytes_per_pair
                                       ? glifistore::server::kMaxFrameBytes
                                       : pipeline * bytes_per_pair;
    BufferedResponseReader responses{response_capacity};
    ClientResult result;
    if (measure_latency) {
        result.latency_ns.reserve(work.response_count / std::max<std::size_t>(latency_sample_stride, 1U) +
                                  1U);
        if (latency_split) {
            result.get_latency_ns.reserve(result.latency_ns.capacity());
            result.put_latency_ns.reserve(result.latency_ns.capacity());
        }
    }
    std::size_t responses_remaining = work.response_count;
    std::size_t latency_tick{};
    for (const auto& batch : work.batches) {
        const auto batch_started =
            measure_latency ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        if (!send_all(descriptor, batch)) {
            return {};
        }
        result.ingress_bytes += batch.size();
        const auto frames_per_operation = workload == Workload::read_after_write ? 2U : 1U;
        const auto batch_responses = std::min(pipeline * frames_per_operation, responses_remaining);
        for (std::size_t index = 0; index < batch_responses; ++index) {
            auto decoded = responses.receive(descriptor);
            if (!decoded || !decoded->complete ||
                decoded->frame.status != glifistore::server::ResponseStatus::ok) {
                return {};
            }
            result.egress_bytes += decoded->consumed;
            const auto request_id = decoded->frame.request_id;
            const bool is_get = (request_id & 1U) != 0U;
            const bool sample_this = measure_latency && (workload == Workload::read_after_write || is_get) &&
                                     (latency_tick % latency_sample_stride == 0U);
            if (measure_latency && (workload == Workload::read_after_write || is_get)) {
                ++latency_tick;
            }
            if (sample_this) {
                const auto elapsed = std::chrono::steady_clock::now() - batch_started;
                const auto ns = std::chrono::duration<double, std::nano>(elapsed).count();
                result.latency_ns.push_back(ns);
                if (latency_split) {
                    if (is_get) {
                        result.get_latency_ns.push_back(ns);
                    } else {
                        result.put_latency_ns.push_back(ns);
                    }
                }
            }
            const auto operation = request_id / 2U;
            if (operation >= material.values.size()) {
                return {};
            }
            if (is_get && !std::ranges::equal(decoded->frame.value, material.values[operation])) {
                return {};
            }
            ++result.hits;
        }
        responses_remaining -= batch_responses;
    }
    return result;
}

[[nodiscard]] auto seed_get_workload(const Options& options, const std::vector<int>& descriptors,
                                     const glifistore::bench::KeyMaterial& material) -> bool {
    if (options.workload == Workload::read_after_write) {
        return true;
    }
    constexpr std::size_t seed_pipeline = 64;
    // Seed traffic is outside the timed region, but it still crosses the real bounded Reactor.
    // Bound both record count and bytes so large-value GET workloads do not manufacture a
    // multi-megabyte input burst unrelated to the measured pipeline.
    constexpr std::size_t maximum_seed_batch_bytes = std::size_t{2} * 1024U * 1024U;
    for (std::size_t client = 0; client < descriptors.size(); ++client) {
        BufferedResponseReader responses{seed_pipeline * glifistore::server::kResponseHeaderBytes};
        std::vector<std::byte> batch;
        std::size_t pending{};
        const auto flush = [&]() {
            if (!send_all(descriptors[client], batch)) {
                return false;
            }
            for (std::size_t response = 0; response < pending; ++response) {
                auto decoded = responses.receive(descriptors[client]);
                if (!decoded || decoded->frame.status != glifistore::server::ResponseStatus::ok) {
                    return false;
                }
            }
            batch.clear();
            pending = 0;
            return true;
        };
        for (std::size_t operation = client; operation < options.config.operations;
             operation += options.config.threads) {
            auto put = glifistore::server::encode_request({.opcode = glifistore::server::RequestOpcode::put,
                                                           .request_id = operation * 2U,
                                                           .key = bytes(material.keys[operation]),
                                                           .value = material.values[operation]});
            if (!put) {
                return false;
            }
            const bool seed_batch_full = batch.size() >= maximum_seed_batch_bytes;
            const bool seed_batch_would_overflow =
                !seed_batch_full && put->size() > maximum_seed_batch_bytes - batch.size();
            if (!batch.empty() && (seed_batch_full || seed_batch_would_overflow)) {
                if (!flush()) {
                    return false;
                }
            }
            batch.insert(batch.end(), put->begin(), put->end());
            if (++pending == seed_pipeline && !flush()) {
                return false;
            }
        }
        if (pending != 0 && !flush()) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto run_sample(const Options& options, const glifistore::bench::KeyMaterial& material,
                              const std::vector<ClientWork>& work) -> Sample {
    BenchmarkDataDirectory directory{options.storage};
    if (!seed_maintenance_overlap(options, directory)) {
        return {};
    }
    const auto open_mode = options.maintenance_overlap_seed_operations == 0
                               ? glifistore::DurableOpenMode::create_new
                               : glifistore::DurableOpenMode::open_existing;
    MaintenanceOverlapGate maintenance_gate{directory.path()};
    auto config = store_config(options, directory, open_mode);
    if (options.maintenance_overlap_seed_operations != 0) {
        config.filesystem_hooks = {.context = &maintenance_gate,
                                   .available_space_bytes = &MaintenanceOverlapGate::available_space_bytes};
    }
    auto server = glifistore::server::Server::create(reactor_config(options), std::move(config));
    if (!server || !(*server)->start()) {
        maintenance_gate.release();
        return {};
    }
    std::vector<int> descriptors;
    descriptors.reserve(options.config.threads);
    const auto cleanup = [&] {
        maintenance_gate.release();
        for (const auto descriptor : descriptors) {
            static_cast<void>(::close(descriptor));
        }
        (*server)->request_stop();
        static_cast<void>((*server)->join());
    };
    for (std::size_t client = 0; client < options.config.threads; ++client) {
        const auto descriptor = connect_to((*server)->port());
        if (descriptor < 0) {
            cleanup();
            return {};
        }
        descriptors.push_back(descriptor);
    }
    for (std::size_t client = 0; client < descriptors.size(); ++client) {
        BufferedResponseReader responses{glifistore::server::kResponseHeaderBytes + 64U};
        const auto init = glifistore::server::encode_request({
            .opcode = glifistore::server::RequestOpcode::init,
            .request_id = 0xFFFF'FFFF'0000'0000ULL + client * 2U,
        });
        const auto bind = glifistore::server::encode_request({
            .opcode = glifistore::server::RequestOpcode::bind_worker,
            .request_id = 0xFFFF'FFFF'0000'0001ULL + client * 2U,
            .target_worker = static_cast<std::uint32_t>(client % options.config.workers),
        });
        if (!init || !bind || !send_all(descriptors[client], *init)) {
            cleanup();
            return {};
        }
        auto initialized = responses.receive(descriptors[client]);
        if (!initialized || initialized->frame.status != glifistore::server::ResponseStatus::ok ||
            initialized->frame.worker_count != options.config.workers ||
            !send_all(descriptors[client], *bind)) {
            cleanup();
            return {};
        }
        auto bound = responses.receive(descriptors[client]);
        if (!bound || bound->frame.status != glifistore::server::ResponseStatus::ok ||
            bound->frame.owner_worker != client % options.config.workers) {
            cleanup();
            return {};
        }
    }
    if (!seed_get_workload(options, descriptors, material)) {
        cleanup();
        return {};
    }
    const auto paired_before = (*server)->pair_writer_stats();
    const auto durable_before = (*server)->durable_batch_stats();
    glifistore::hot_path::reset();

    auto resources = glifistore::bench::process_memory_snapshot();
    std::latch ready{static_cast<std::ptrdiff_t>(options.config.threads)};
    std::latch start{1};
    std::vector<ClientResult> client_results(options.config.threads);
    std::vector<std::thread> clients;
    clients.reserve(options.config.threads);
    for (std::size_t client = 0; client < options.config.threads; ++client) {
        clients.emplace_back([&, client] {
            ready.count_down();
            start.wait();
            client_results[client] =
                run_client(descriptors[client], work[client], material, options.pipeline, options.latency,
                           options.latency_split, options.latency_sample_stride, options.workload);
        });
    }
    ready.wait();
    std::thread maintenance_releaser;
    if (options.maintenance_overlap_seed_operations != 0) {
        maintenance_releaser = std::thread([&] {
            start.wait();
            std::this_thread::sleep_for(std::chrono::milliseconds{options.maintenance_overlap_release_ms});
            maintenance_gate.release();
        });
    }
    const auto started = std::chrono::steady_clock::now();
    start.count_down();
    for (auto& client : clients) {
        client.join();
    }
    if (maintenance_releaser.joinable()) {
        maintenance_releaser.join();
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const auto after = glifistore::bench::process_memory_snapshot();
    resources.rss_after_bytes = after.rss_after_bytes;
    resources.peak_rss_bytes = after.peak_rss_bytes;
    std::size_t hits{};
    for (const auto& client_result : client_results) {
        hits += client_result.hits;
        resources.ingress_bytes += client_result.ingress_bytes;
        resources.egress_bytes += client_result.egress_bytes;
    }
    const auto expected =
        options.config.operations * (options.workload == Workload::read_after_write ? 2U : 1U);
    std::vector<double> latency_ns;
    std::vector<double> get_latency_ns;
    std::vector<double> put_latency_ns;
    if (options.latency) {
        latency_ns.reserve(expected);
        for (auto& client_result : client_results) {
            latency_ns.insert(latency_ns.end(), std::make_move_iterator(client_result.latency_ns.begin()),
                              std::make_move_iterator(client_result.latency_ns.end()));
            get_latency_ns.insert(get_latency_ns.end(),
                                  std::make_move_iterator(client_result.get_latency_ns.begin()),
                                  std::make_move_iterator(client_result.get_latency_ns.end()));
            put_latency_ns.insert(put_latency_ns.end(),
                                  std::make_move_iterator(client_result.put_latency_ns.begin()),
                                  std::make_move_iterator(client_result.put_latency_ns.end()));
        }
    }
    for (const auto descriptor : descriptors) {
        static_cast<void>(::close(descriptor));
    }
    const auto profile = durable_profile(**server, paired_before, durable_before);
    const auto reactor = reactor_profile(**server);
    (*server)->request_stop();
    const auto stopped = (*server)->join();
    return {.hits = hits,
            .seconds = elapsed,
            .resources = resources,
            .latency_ns = std::move(latency_ns),
            .get_latency_ns = std::move(get_latency_ns),
            .put_latency_ns = std::move(put_latency_ns),
            .valid = stopped.has_value() && hits == expected,
            .durable = profile,
            .reactor = reactor};
}

[[nodiscard]] auto run_client_api_sample(const Options& options,
                                         const glifistore::bench::KeyMaterial& material) -> Sample {
    BenchmarkDataDirectory directory{options.storage};
    if (!seed_maintenance_overlap(options, directory)) {
        return {};
    }
    const auto open_mode = options.maintenance_overlap_seed_operations == 0
                               ? glifistore::DurableOpenMode::create_new
                               : glifistore::DurableOpenMode::open_existing;
    MaintenanceOverlapGate maintenance_gate{directory.path()};
    auto config = store_config(options, directory, open_mode);
    if (options.maintenance_overlap_seed_operations != 0) {
        config.filesystem_hooks = {.context = &maintenance_gate,
                                   .available_space_bytes = &MaintenanceOverlapGate::available_space_bytes};
    }
    auto server = glifistore::server::Server::create(reactor_config(options), std::move(config));
    if (!server || !(*server)->start()) {
        maintenance_gate.release();
        return {};
    }
    auto connected = glifistore::client::Client::connect({.port = (*server)->port()});
    if (!connected) {
        maintenance_gate.release();
        (*server)->request_stop();
        static_cast<void>((*server)->join());
        return {};
    }
    auto client = std::move(*connected);
    const auto paired_before = (*server)->pair_writer_stats();
    const auto durable_before = (*server)->durable_batch_stats();
    glifistore::hot_path::reset();
    auto resources = glifistore::bench::process_memory_snapshot();
    std::latch ready{static_cast<std::ptrdiff_t>(options.config.threads)};
    std::latch start{1};
    std::vector<ClientResult> client_results(options.config.threads);
    std::vector<std::thread> clients;
    clients.reserve(options.config.threads);
    for (std::size_t client_index = 0; client_index < options.config.threads; ++client_index) {
        clients.emplace_back([&, client_index] {
            auto& result = client_results[client_index];
            if (options.latency) {
                result.latency_ns.reserve((options.config.operations / options.config.threads + 1U) * 2U /
                                          std::max<std::size_t>(options.latency_sample_stride, 1U));
                if (options.latency_split) {
                    result.get_latency_ns.reserve(result.latency_ns.capacity());
                    result.put_latency_ns.reserve(result.latency_ns.capacity());
                }
            }
            std::size_t latency_tick{};
            std::vector<std::vector<glifistore::client::PipelineRequest>> pipeline_batches;
            if (options.client_pipeline != 0) {
                std::vector<std::vector<glifistore::client::PipelineRequest>> pending(client.worker_count());
                for (auto& batch : pending) {
                    batch.reserve(options.client_pipeline * 2U);
                }
                for (std::size_t operation = client_index; operation < options.config.operations;
                     operation += options.config.threads) {
                    auto& batch = pending[client.worker_for(material.keys[operation])];
                    batch.push_back(
                        {.opcode = glifistore::client::PipelineOpcode::put,
                         .key = bytes(material.keys[operation]),
                         .value = {material.values[operation].data(), material.values[operation].size()}});
                    batch.push_back({.opcode = glifistore::client::PipelineOpcode::get,
                                     .key = bytes(material.keys[operation])});
                    if (batch.size() == options.client_pipeline * 2U) {
                        pipeline_batches.push_back(std::move(batch));
                        batch.clear();
                        batch.reserve(options.client_pipeline * 2U);
                    }
                }
                for (auto& batch : pending) {
                    if (!batch.empty()) {
                        pipeline_batches.push_back(std::move(batch));
                    }
                }
            }
            ready.count_down();
            start.wait();
            if (options.client_pipeline != 0) {
                for (const auto& batch : pipeline_batches) {
                    const auto batch_started = std::chrono::steady_clock::now();
                    auto executed = client.execute_pipeline(batch);
                    if (!executed || executed->size() != batch.size()) {
                        return;
                    }
                    for (std::size_t index = 0; index < batch.size(); ++index) {
                        if (!(*executed)[index].succeeded()) {
                            return;
                        }
                        if (batch[index].opcode == glifistore::client::PipelineOpcode::get &&
                            !std::ranges::equal((*executed)[index].value, batch[index - 1U].value)) {
                            return;
                        }
                        if (options.latency) {
                            const auto sample_this = latency_tick % options.latency_sample_stride == 0U;
                            ++latency_tick;
                            if (sample_this) {
                                const auto ns = std::chrono::duration<double, std::nano>(
                                                    std::chrono::steady_clock::now() - batch_started)
                                                    .count();
                                result.latency_ns.push_back(ns);
                                if (options.latency_split) {
                                    if (batch[index].opcode == glifistore::client::PipelineOpcode::get) {
                                        result.get_latency_ns.push_back(ns);
                                    } else {
                                        result.put_latency_ns.push_back(ns);
                                    }
                                }
                            }
                        }
                    }
                    result.hits += batch.size();
                    for (const auto& request : batch) {
                        result.ingress_bytes += glifistore::server::kRequestHeaderBytes + request.key.size() +
                                                request.value.size();
                        result.egress_bytes +=
                            glifistore::server::kResponseHeaderBytes +
                            (request.opcode == glifistore::client::PipelineOpcode::get ? request.value.size()
                                                                                       : 0U);
                    }
                    for (std::size_t index = 1; index < batch.size(); index += 2U) {
                        result.egress_bytes += batch[index - 1U].value.size();
                    }
                }
                return;
            }
            for (std::size_t operation = client_index; operation < options.config.operations;
                 operation += options.config.threads) {
                const auto put_started = std::chrono::steady_clock::now();
                if (!client
                         .put(bytes(material.keys[operation]),
                              {material.values[operation].data(), material.values[operation].size()})
                         .committed()) {
                    return;
                }
                if (options.latency) {
                    const auto sample_this = latency_tick % options.latency_sample_stride == 0U;
                    ++latency_tick;
                    if (sample_this) {
                        const auto ns = std::chrono::duration<double, std::nano>(
                                            std::chrono::steady_clock::now() - put_started)
                                            .count();
                        result.latency_ns.push_back(ns);
                        if (options.latency_split) {
                            result.put_latency_ns.push_back(ns);
                        }
                    }
                }
                ++result.hits;
                const auto get_started = std::chrono::steady_clock::now();
                auto loaded = client.get(material.keys[operation]);
                if (!loaded || !std::ranges::equal(*loaded, material.values[operation])) {
                    return;
                }
                if (options.latency) {
                    const auto sample_this = latency_tick % options.latency_sample_stride == 0U;
                    ++latency_tick;
                    if (sample_this) {
                        const auto ns = std::chrono::duration<double, std::nano>(
                                            std::chrono::steady_clock::now() - get_started)
                                            .count();
                        result.latency_ns.push_back(ns);
                        if (options.latency_split) {
                            result.get_latency_ns.push_back(ns);
                        }
                    }
                }
                ++result.hits;
                result.ingress_bytes += 2U * glifistore::server::kRequestHeaderBytes +
                                        material.keys[operation].size() * 2U +
                                        material.values[operation].size();
                result.egress_bytes +=
                    2U * glifistore::server::kResponseHeaderBytes + material.values[operation].size();
            }
        });
    }
    ready.wait();
    std::thread maintenance_releaser;
    if (options.maintenance_overlap_seed_operations != 0) {
        maintenance_releaser = std::thread([&] {
            start.wait();
            std::this_thread::sleep_for(std::chrono::milliseconds{options.maintenance_overlap_release_ms});
            maintenance_gate.release();
        });
    }
    const auto started = std::chrono::steady_clock::now();
    start.count_down();
    for (auto& thread : clients) {
        thread.join();
    }
    if (maintenance_releaser.joinable()) {
        maintenance_releaser.join();
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const auto after = glifistore::bench::process_memory_snapshot();
    resources.rss_after_bytes = after.rss_after_bytes;
    resources.peak_rss_bytes = after.peak_rss_bytes;
    std::size_t hits{};
    std::vector<double> latency_ns;
    std::vector<double> get_latency_ns;
    std::vector<double> put_latency_ns;
    for (auto& result : client_results) {
        hits += result.hits;
        resources.ingress_bytes += result.ingress_bytes;
        resources.egress_bytes += result.egress_bytes;
        latency_ns.insert(latency_ns.end(), std::make_move_iterator(result.latency_ns.begin()),
                          std::make_move_iterator(result.latency_ns.end()));
        get_latency_ns.insert(get_latency_ns.end(), std::make_move_iterator(result.get_latency_ns.begin()),
                              std::make_move_iterator(result.get_latency_ns.end()));
        put_latency_ns.insert(put_latency_ns.end(), std::make_move_iterator(result.put_latency_ns.begin()),
                              std::make_move_iterator(result.put_latency_ns.end()));
    }
    client.close();
    const auto profile = durable_profile(**server, paired_before, durable_before);
    const auto reactor = reactor_profile(**server);
    (*server)->request_stop();
    const auto stopped = (*server)->join();
    const auto expected = options.config.operations * 2U;
    return {.hits = hits,
            .seconds = elapsed,
            .resources = resources,
            .latency_ns = std::move(latency_ns),
            .get_latency_ns = std::move(get_latency_ns),
            .put_latency_ns = std::move(put_latency_ns),
            .valid = stopped.has_value() && hits == expected,
            .durable = profile,
            .reactor = reactor};
}

[[nodiscard]] auto percentile(const std::vector<double>& sorted, const double quantile) -> double {
    if (sorted.empty()) {
        return 0.0;
    }
    const auto rank = static_cast<std::size_t>(std::ceil(quantile * static_cast<double>(sorted.size())));
    return sorted[std::min(std::max(std::size_t{1}, rank), sorted.size()) - 1U];
}

struct LatencyExtras {
    double max_latency_ns{};
    std::size_t get_latency_samples{};
    double p50_get_latency_ns{};
    double p95_get_latency_ns{};
    double p99_get_latency_ns{};
    double p999_get_latency_ns{};
    double max_get_latency_ns{};
    std::size_t put_latency_samples{};
    double p50_put_latency_ns{};
    double p95_put_latency_ns{};
    double p99_put_latency_ns{};
    double p999_put_latency_ns{};
    double max_put_latency_ns{};
};

struct LatencyPercentiles {
    std::size_t samples{};
    double p50{};
    double p95{};
    double p99{};
    double p999{};
    double maximum{};
};

[[nodiscard]] auto latency_percentiles(std::vector<double> samples) -> LatencyPercentiles {
    if (samples.empty()) {
        return {};
    }
    std::ranges::sort(samples);
    return {.samples = samples.size(),
            .p50 = percentile(samples, 0.50),
            .p95 = percentile(samples, 0.95),
            .p99 = percentile(samples, 0.99),
            .p999 = percentile(samples, 0.999),
            .maximum = samples.back()};
}

[[nodiscard]] auto run_benchmark(const Options& options) -> std::pair<Result, LatencyExtras> {
    const auto material = make_material(options.config);
    const auto work = options.client_api
                          ? std::vector<ClientWork>{}
                          : prepare_work(options.config, options.pipeline, material, options.workload);
    if (!options.client_api && work.size() != options.config.threads) {
        return {};
    }
    const auto sample = [&] {
        return options.client_api ? run_client_api_sample(options, material)
                                  : run_sample(options, material, work);
    };
    for (std::size_t iteration = 0; iteration < options.settings.warmup_iterations; ++iteration) {
        if (!sample().valid) {
            return {};
        }
    }
    std::vector<double> seconds;
    std::vector<glifistore::bench::ResourceSample> resources;
    std::vector<double> latency_ns;
    std::vector<double> get_latency_ns;
    std::vector<double> put_latency_ns;
    std::vector<DurableProfileSample> durable_profiles;
    std::vector<ReactorProfileSample> reactor_profiles;
    seconds.reserve(options.settings.measured_iterations);
    resources.reserve(options.settings.measured_iterations);
    durable_profiles.reserve(options.settings.measured_iterations);
    reactor_profiles.reserve(options.settings.measured_iterations);
    std::size_t hits{};
    for (std::size_t iteration = 0; iteration < options.settings.measured_iterations; ++iteration) {
        auto measured = sample();
        if (!measured.valid) {
            return {};
        }
        hits = measured.hits;
        seconds.push_back(measured.seconds);
        resources.push_back(measured.resources);
        durable_profiles.push_back(measured.durable);
        reactor_profiles.push_back(measured.reactor);
        latency_ns.insert(latency_ns.end(), std::make_move_iterator(measured.latency_ns.begin()),
                          std::make_move_iterator(measured.latency_ns.end()));
        get_latency_ns.insert(get_latency_ns.end(), std::make_move_iterator(measured.get_latency_ns.begin()),
                              std::make_move_iterator(measured.get_latency_ns.end()));
        put_latency_ns.insert(put_latency_ns.end(), std::make_move_iterator(measured.put_latency_ns.begin()),
                              std::make_move_iterator(measured.put_latency_ns.end()));
    }
    const auto client_name = options.client_pipeline != 0             ? "cpp_client_pipeline_read_after_write"
                             : options.client_api                     ? "cpp_client_read_after_write"
                             : options.workload == Workload::get_only ? "server_tcp_get_only"
                             : options.workload == Workload::read_99_write_1  ? "server_tcp_read_99_write_1"
                             : options.workload == Workload::read_95_write_5  ? "server_tcp_read_95_write_5"
                             : options.workload == Workload::read_90_write_10 ? "server_tcp_read_90_write_10"
                                                                              : "server_tcp_read_after_write";
    const auto benchmark_name = std::string{client_name} + '_' + std::string{storage_name(options.storage)};
    auto result = glifistore::bench::finalize_result(
        benchmark_name, options.config, options.settings,
        options.config.operations * (options.workload == Workload::read_after_write ? 2U : 1U), hits,
        std::move(seconds), std::move(resources));
    LatencyExtras extras{};
    if (!latency_ns.empty()) {
        std::ranges::sort(latency_ns);
        result.latency_samples = latency_ns.size();
        result.p50_latency_ns = percentile(latency_ns, 0.50);
        result.p95_latency_ns = percentile(latency_ns, 0.95);
        result.p99_latency_ns = percentile(latency_ns, 0.99);
        result.p999_latency_ns = percentile(latency_ns, 0.999);
        extras.max_latency_ns = latency_ns.back();
    }
    if (options.latency_split) {
        const auto get = latency_percentiles(std::move(get_latency_ns));
        extras.get_latency_samples = get.samples;
        extras.p50_get_latency_ns = get.p50;
        extras.p95_get_latency_ns = get.p95;
        extras.p99_get_latency_ns = get.p99;
        extras.p999_get_latency_ns = get.p999;
        extras.max_get_latency_ns = get.maximum;
        const auto put = latency_percentiles(std::move(put_latency_ns));
        extras.put_latency_samples = put.samples;
        extras.p50_put_latency_ns = put.p50;
        extras.p95_put_latency_ns = put.p95;
        extras.p99_put_latency_ns = put.p99;
        extras.p999_put_latency_ns = put.p999;
        extras.max_put_latency_ns = put.maximum;
    }
    const auto median_profile = [&](auto member) {
        std::vector<double> values;
        values.reserve(durable_profiles.size());
        for (const auto& profile : durable_profiles) {
            values.push_back(static_cast<double>(profile.*member));
        }
        return glifistore::bench::median(std::move(values));
    };
    const auto maximum_profile = [&](auto member) {
        double maximum{};
        for (const auto& profile : durable_profiles) {
            maximum = std::max(maximum, static_cast<double>(profile.*member));
        }
        return maximum;
    };
    result.median_durable_queue_wait_ns = median_profile(&DurableProfileSample::average_queue_wait_ns);
    result.maximum_durable_queue_wait_ns = maximum_profile(&DurableProfileSample::maximum_queue_wait_ns);
    result.median_durable_service_ns = median_profile(&DurableProfileSample::average_service_ns);
    result.maximum_durable_service_ns = maximum_profile(&DurableProfileSample::maximum_service_ns);
    result.median_durable_commit_ns = median_profile(&DurableProfileSample::average_commit_ns);
    result.maximum_durable_commit_ns = maximum_profile(&DurableProfileSample::maximum_commit_ns);
    result.median_durable_batch_records = median_profile(&DurableProfileSample::average_batch_records);
    result.maximum_durable_batch_records = maximum_profile(&DurableProfileSample::maximum_batch_records);
    result.durable_completed = static_cast<std::uint64_t>(median_profile(&DurableProfileSample::completed));
    result.durable_rejected = static_cast<std::uint64_t>(maximum_profile(&DurableProfileSample::rejected));
    result.durable_expired = static_cast<std::uint64_t>(maximum_profile(&DurableProfileSample::expired));
    result.durable_committed_batches =
        static_cast<std::uint64_t>(median_profile(&DurableProfileSample::committed_batches));
    result.durable_committed_records =
        static_cast<std::uint64_t>(median_profile(&DurableProfileSample::committed_records));
    result.durable_committed_bytes =
        static_cast<std::uint64_t>(median_profile(&DurableProfileSample::committed_bytes));
    result.durable_failed_batches =
        static_cast<std::uint64_t>(maximum_profile(&DurableProfileSample::failed_batches));
    result.durable_maximum_queue_depth =
        static_cast<std::uint64_t>(maximum_profile(&DurableProfileSample::maximum_queue_depth));
    result.durable_maximum_queued_bytes =
        static_cast<std::uint64_t>(maximum_profile(&DurableProfileSample::maximum_queued_bytes));
    result.durable_pending_records =
        static_cast<std::uint64_t>(maximum_profile(&DurableProfileSample::pending_records));
    result.durable_pending_bytes =
        static_cast<std::uint64_t>(maximum_profile(&DurableProfileSample::pending_bytes));
    result.durable_record_limit_closes =
        static_cast<std::uint64_t>(median_profile(&DurableProfileSample::record_limit_closes));
    result.durable_byte_limit_closes =
        static_cast<std::uint64_t>(median_profile(&DurableProfileSample::byte_limit_closes));
    result.durable_adaptive_target_closes =
        static_cast<std::uint64_t>(median_profile(&DurableProfileSample::adaptive_target_closes));
    result.durable_deadline_closes =
        static_cast<std::uint64_t>(median_profile(&DurableProfileSample::deadline_closes));
    result.median_paired_writer_batch_records =
        median_profile(&DurableProfileSample::average_paired_writer_batch_records);
    result.median_paired_writer_batch_wait_ns =
        median_profile(&DurableProfileSample::average_paired_writer_batch_wait_ns);
    result.maximum_paired_writer_batch_wait_ns =
        maximum_profile(&DurableProfileSample::maximum_paired_writer_batch_wait_ns);
    result.paired_writer_batches =
        static_cast<std::uint64_t>(median_profile(&DurableProfileSample::paired_writer_batches));
    result.paired_writer_batch_records =
        static_cast<std::uint64_t>(median_profile(&DurableProfileSample::paired_writer_batch_records));
    result.paired_writer_durability_deadline_closes = static_cast<std::uint64_t>(
        median_profile(&DurableProfileSample::paired_writer_durability_deadline_closes));
    result.paired_writer_queue_deadline_closes = static_cast<std::uint64_t>(
        maximum_profile(&DurableProfileSample::paired_writer_queue_deadline_closes));
    result.paired_sync_turn_splits =
        static_cast<std::uint64_t>(maximum_profile(&DurableProfileSample::paired_sync_turn_splits));
    result.paired_sync_async_fairness_turns =
        static_cast<std::uint64_t>(maximum_profile(&DurableProfileSample::paired_sync_async_fairness_turns));
    result.paired_publications =
        static_cast<std::uint64_t>(median_profile(&DurableProfileSample::paired_publications));
    result.paired_publication_records =
        static_cast<std::uint64_t>(median_profile(&DurableProfileSample::paired_publication_records));
    result.paired_completion_notifications =
        static_cast<std::uint64_t>(median_profile(&DurableProfileSample::paired_completion_notifications));
    const auto median_reactor_profile = [&](auto member) {
        std::vector<double> values;
        values.reserve(reactor_profiles.size());
        for (const auto& profile : reactor_profiles) {
            values.push_back(static_cast<double>(profile.*member));
        }
        return glifistore::bench::median(std::move(values));
    };
    const auto maximum_reactor_profile = [&](auto member) {
        double maximum{};
        for (const auto& profile : reactor_profiles) {
            maximum = std::max(maximum, static_cast<double>(profile.*member));
        }
        return maximum;
    };
    result.median_reactor_input_buffer_compactions =
        median_reactor_profile(&ReactorProfileSample::input_buffer_compactions);
    result.maximum_reactor_input_buffer_compactions =
        maximum_reactor_profile(&ReactorProfileSample::input_buffer_compactions);
    result.median_reactor_input_buffer_bytes_moved =
        median_reactor_profile(&ReactorProfileSample::input_buffer_bytes_moved);
    result.maximum_reactor_input_buffer_bytes_moved =
        maximum_reactor_profile(&ReactorProfileSample::input_buffer_bytes_moved);
    result.median_reactor_output_buffer_compactions =
        median_reactor_profile(&ReactorProfileSample::output_buffer_compactions);
    result.maximum_reactor_output_buffer_compactions =
        maximum_reactor_profile(&ReactorProfileSample::output_buffer_compactions);
    result.median_reactor_output_buffer_bytes_moved =
        median_reactor_profile(&ReactorProfileSample::output_buffer_bytes_moved);
    result.maximum_reactor_output_buffer_bytes_moved =
        maximum_reactor_profile(&ReactorProfileSample::output_buffer_bytes_moved);
    if (options.maintenance_overlap_seed_operations != 0) {
        std::cout << "# maintenance_profile_scope=median-per-sample-counters-and-cross-sample-maxima\n";
        std::cout << "# maintenance_evaluations_median="
                  << median_profile(&DurableProfileSample::maintenance_evaluations)
                  << ";compact_attempts_median="
                  << median_profile(&DurableProfileSample::maintenance_compact_attempts)
                  << ";useful_compactions_median="
                  << median_profile(&DurableProfileSample::maintenance_useful_compactions)
                  << ";latency_suspends_median="
                  << median_profile(&DurableProfileSample::maintenance_latency_suspends)
                  << ";latency_suspends_max="
                  << maximum_profile(&DurableProfileSample::maintenance_latency_suspends)
                  << ";latency_debt_overrides_max="
                  << maximum_profile(&DurableProfileSample::maintenance_latency_debt_overrides)
                  << ";foreground_samples_median="
                  << median_profile(&DurableProfileSample::maintenance_foreground_latency_samples)
                  << ";foreground_p99_ns_max="
                  << maximum_profile(&DurableProfileSample::maintenance_last_foreground_p99_ns) << '\n';
    }
    return {std::move(result), extras};
}

} // namespace

int main(int argc, char** argv) try {
    const auto parsed = options(argc, argv);
    if (!glifistore::bench::validate_run_settings(parsed.settings, parsed.config) || parsed.pipeline == 0 ||
        (parsed.client_api && parsed.workload != Workload::read_after_write) ||
        (parsed.client_api && parsed.client_pipeline > parsed.config.operations) ||
        parsed.client_pipeline > glifistore::client::ClientConfig{}.maximum_pipeline_requests / 2U ||
        parsed.group_max_records == 0 || parsed.group_max_bytes == 0 || parsed.group_max_wait_ms == 0 ||
        parsed.periodic_sync_ms == 0 || parsed.maintenance_suspend_on_p99_min_samples == 0 ||
        (parsed.maintenance_overlap_seed_operations != 0 &&
         (parsed.storage == StorageProfile::volatile_memory || parsed.config.workers < 2 ||
          parsed.config.threads != 1 || parsed.maintenance_overlap_seed_keys == 0 ||
          parsed.maintenance_overlap_seed_value_bytes == 0 || parsed.maintenance_overlap_eval_ms == 0 ||
          parsed.maintenance_overlap_release_ms == 0))) {
        return 2;
    }
    std::cout << "# glifistore TCP server benchmark\n";
    glifistore::bench::print_metadata(std::cout, parsed.settings);
    std::cout << "# client_mode="
              << (parsed.client_pipeline != 0 ? "public-cpp-pipeline"
                  : parsed.client_api         ? "public-cpp-api"
                                              : "raw-wire")
              << '\n';
    std::cout << "# execution_scope=same-process-loopback\n";
    std::cout << "# timed_thread_model=client-threads+one-reactor+one-writer-per-worker\n";
    std::cout << "# timed_foreground_thread_floor=" << parsed.config.threads + parsed.config.workers * 2U
              << '\n';
    std::cout << "# pipeline="
              << (parsed.client_pipeline != 0 ? parsed.client_pipeline
                  : parsed.client_api         ? 1
                                              : parsed.pipeline)
              << '\n';
    std::cout << "# routing=owner-bound-connections\n";
    std::cout << "# traffic_scope=timed-protocol-frames-excluding-init-bind\n";
    std::cout << "# memory_scope=whole-benchmark-process-rss\n";
    std::cout << "# storage_mode=" << storage_name(parsed.storage) << '\n';
    std::cout << "# workload=" << workload_name(parsed.workload) << '\n';
    std::cout << "# durable_profile_scope=median-per-sample-averages-and-cross-sample-maxima\n";
    std::cout << "# durable_commit_metric=batch-commit-boundary;unbatched-sync-reports-zero\n";
    std::cout << "# group_max_records=" << parsed.group_max_records << '\n';
    std::cout << "# group_max_bytes=" << parsed.group_max_bytes << '\n';
    std::cout << "# group_max_wait_ms=" << parsed.group_max_wait_ms << '\n';
    std::cout << "# periodic_sync_ms=" << parsed.periodic_sync_ms << '\n';
    std::cout << "# maintenance_suspend_on_p99_latency_ms=" << parsed.maintenance_suspend_on_p99_latency_ms
              << '\n';
    std::cout << "# maintenance_suspend_on_p99_min_samples=" << parsed.maintenance_suspend_on_p99_min_samples
              << '\n';
    std::cout << "# maintenance_max_latency_deferral_ms=" << parsed.maintenance_max_latency_deferral_ms
              << '\n';
    std::cout << "# maintenance_overlap_seed_operations=" << parsed.maintenance_overlap_seed_operations
              << '\n';
    std::cout << "# maintenance_overlap_seed_keys=" << parsed.maintenance_overlap_seed_keys << '\n';
    std::cout << "# maintenance_overlap_seed_value_bytes=" << parsed.maintenance_overlap_seed_value_bytes
              << '\n';
    std::cout << "# maintenance_overlap_seed_worker="
              << (parsed.maintenance_overlap_seed_operations == 0 ? 0U : parsed.config.workers - 1U) << '\n';
    std::cout << "# maintenance_overlap_eval_ms=" << parsed.maintenance_overlap_eval_ms << '\n';
    std::cout << "# maintenance_overlap_release_ms=" << parsed.maintenance_overlap_release_ms << '\n';
    std::cout << "# durable_mutation_queue_wait_ms=0\n";
    std::cout << "# executor_affinity_requested=" << (parsed.executor_affinity ? 1 : 0) << '\n';
    std::cout << "# latency_measurement="
              << (parsed.latency ? (parsed.client_pipeline != 0 ? "pipeline-batch-completion"
                                    : parsed.client_api         ? "synchronous-api-call"
                                                                : "pipelined-response")
                                 : "disabled")
              << '\n';
    std::cout << "# latency_sample_stride=" << parsed.latency_sample_stride << '\n';
    std::cout << "# latency_split=" << (parsed.latency_split ? 1 : 0) << '\n';
#if defined(__APPLE__)
    std::cout << "# executor_affinity_semantics=mach-advisory\n";
#elif defined(__linux__)
    std::cout << "# executor_affinity_semantics=cpu-pinned\n";
#else
    std::cout << "# executor_affinity_semantics=unavailable\n";
#endif
    const auto [result, extras] = run_benchmark(parsed);
    if (result.samples != parsed.settings.measured_iterations) {
        std::cerr << "benchmark error: TCP sample validation failed\n";
        return 1;
    }
    glifistore::bench::print_result(std::cout, result);
    if (parsed.latency || parsed.latency_split) {
        std::cout << "latency_extras";
        if (parsed.latency) {
            std::cout << " max_latency_ns=" << extras.max_latency_ns;
        }
        if (parsed.latency_split) {
            std::cout << " get_latency_samples=" << extras.get_latency_samples
                      << " p50_get_latency_ns=" << extras.p50_get_latency_ns
                      << " p95_get_latency_ns=" << extras.p95_get_latency_ns
                      << " p99_get_latency_ns=" << extras.p99_get_latency_ns
                      << " p999_get_latency_ns=" << extras.p999_get_latency_ns
                      << " max_get_latency_ns=" << extras.max_get_latency_ns
                      << " put_latency_samples=" << extras.put_latency_samples
                      << " p50_put_latency_ns=" << extras.p50_put_latency_ns
                      << " p95_put_latency_ns=" << extras.p95_put_latency_ns
                      << " p99_put_latency_ns=" << extras.p99_put_latency_ns
                      << " p999_put_latency_ns=" << extras.p999_put_latency_ns
                      << " max_put_latency_ns=" << extras.max_put_latency_ns;
        }
        std::cout << '\n';
    }
    return 0;
} catch (const std::exception& exception) {

    std::cerr << "benchmark error: " << exception.what() << '\n';
    return 1;
} catch (...) {
    std::cerr << "benchmark error: unknown exception\n";
    return 1;
}
