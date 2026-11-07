#include "benchmark_metadata.hpp"
#include "glifistore/core/error.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/core/types.hpp"
#include "glifistore/persistence/filesystem_hooks.hpp"
#include "glifistore/segment/record.hpp"
#include "glifistore/store/config.hpp"
#include "glifistore/store/store.hpp"
#include "parse.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <latch>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kBenchmarkWorkerCount{2};

enum class Mode : std::uint8_t {
    disabled,
    cooperative,
    background,
};

enum class Scenario : std::uint8_t {
    mixed,
    forced_rotation,
    idle,
    churn,
};

struct Options {
    std::size_t warmups{1};
    std::size_t repeats{3};
    std::size_t operations{15'360};
    std::size_t threads{4};
    std::size_t keys{128};
    std::size_t value_bytes{std::size_t{64} * 1024U};
    std::size_t reclaim_value_bytes{std::size_t{256} * 1024U};
    std::size_t put_percent{5};
    std::size_t maintenance_interval_ms{10};
    std::size_t maintenance_copy_bytes_per_sec{};
    std::size_t compaction_delta_entries{4096};
    bool same_worker{};
    std::size_t cooldown_ms{250};
    std::size_t idle_duration_ms{3'000};
    std::optional<Mode> mode;
    Scenario scenario{Scenario::mixed};
    bool warmups_set{};
    bool repeats_set{};
    bool operations_set{};
    bool value_bytes_set{};
    bool put_percent_set{};
    bool maintenance_interval_set{};
};

struct LatencySummary {
    std::size_t samples{};
    std::uint64_t p50_ns{};
    std::uint64_t p95_ns{};
    std::uint64_t p99_ns{};
    std::uint64_t p999_ns{};
    std::uint64_t maximum_ns{};
};

struct CooperativeStats {
    std::uint64_t attempts{};
    std::uint64_t useful{};
    std::uint64_t conflicts{};
    std::uint64_t bytes_copied{};
    std::uint64_t unexpected_errors{};
};

struct CompactionStartGate {
    std::latch* start{};
    std::latch* snapshot_ready{};
    std::latch* compaction_intent_started{};
    std::latch* rotation_entered{};
    std::filesystem::path store;
    std::thread::id opener_thread;
    std::atomic_bool claimed{false};
    std::atomic_bool intent_claimed{false};
    std::atomic_bool snapshot_released{false};

    void release_snapshot() noexcept {
        if (snapshot_ready != nullptr && !snapshot_released.exchange(true, std::memory_order_acq_rel)) {
            snapshot_ready->count_down();
        }
    }

    static auto available_space_bytes(void* context) -> glifistore::Result<std::uint64_t> {
        auto& gate = *static_cast<CompactionStartGate*>(context);
        if (std::this_thread::get_id() != gate.opener_thread &&
            !gate.claimed.exchange(true, std::memory_order_acq_rel)) {
            gate.start->wait();
            gate.release_snapshot();
        }
        std::error_code error;
        const auto space = std::filesystem::space(gate.store, error);
        if (error) {
            return glifistore::fail(glifistore::ErrorCode::io_error,
                                    "maintenance benchmark space probe failed");
        }
        return static_cast<std::uint64_t>(space.available);
    }

    static auto before(void* context, const glifistore::FilesystemOperation operation) -> glifistore::Status {
        auto& gate = *static_cast<CompactionStartGate*>(context);
        if (operation == glifistore::FilesystemOperation::write_compaction_intent &&
            gate.compaction_intent_started != nullptr && gate.rotation_entered != nullptr &&
            !gate.intent_claimed.exchange(true, std::memory_order_acq_rel)) {
            gate.compaction_intent_started->count_down();
            gate.rotation_entered->wait();
        }
        return {};
    }
};

struct ThreadStats {
    std::uint64_t gets{};
    std::uint64_t get_hits{};
    std::uint64_t puts{};
    std::uint64_t put_successes{};
    std::uint64_t generation_backpressure_retries{};
    std::uint64_t compaction_conflict_retries{};
    std::uint64_t failures{};
    std::vector<std::uint64_t> all_latency_ns;
    std::vector<std::uint64_t> get_latency_ns;
    std::vector<std::uint64_t> put_latency_ns;
};

struct Sample {
    Mode mode{};
    std::size_t repeat{};
    double foreground_seconds{};
    std::uint64_t operations{};
    std::uint64_t gets{};
    std::uint64_t puts{};
    std::uint64_t generation_backpressure_retries{};
    std::uint64_t compaction_conflict_retries{};
    LatencySummary all_latency;
    LatencySummary get_latency;
    LatencySummary put_latency;
    std::uint64_t maintenance_attempts{};
    std::uint64_t maintenance_completed{};
    std::uint64_t maintenance_useful{};
    std::uint64_t maintenance_conflicts{};
    std::uint64_t maintenance_bytes_copied{};
    std::uint64_t maintenance_skips{};
    std::uint64_t maintenance_evaluations{};
    std::uint64_t maintenance_no_gain_scans_suppressed{};
    std::uint64_t maintenance_no_gain_retry_after_ns{};
    std::uint64_t maintenance_pacing_delay_ns{};
    std::uint64_t maintenance_pacing_sleep_count{};
    std::uint64_t maintenance_pacing_burst_bytes{};
    glifistore::MaintenanceSkipReason maintenance_last_skip{};
    glifistore::MaintenanceObservation maintenance_observation{};
    glifistore::DurableRotationStats rotation{};
    glifistore::DurableCompactionMetrics compaction{};
    std::size_t segments_after{};
};

[[nodiscard]] auto is_generation_backpressure(const glifistore::Error& error) noexcept -> bool {
    if (error.code != glifistore::ErrorCode::resource_exhausted) {
        return false;
    }
    return error.message == "mutation rejected until paired Reader reaches quiescence" ||
           error.message == "mutation rejected until incremental read merge advances";
}

struct RotationSample {
    Mode mode{};
    std::size_t repeat{};
    double rotation_seconds{};
    std::uint64_t publication_wait_ns{};
    std::uint64_t seal_ns{};
    std::uint64_t create_ns{};
    std::uint64_t manifest_publication_ns{};
    std::uint64_t other_execution_ns{};
    std::uint64_t execution_ns{};
    std::uint64_t accounted_total_ns{};
    std::uint64_t final_record_commit_ns{};
    std::uint64_t measurement_residual_ns{};
    std::uint64_t rotation_attempts{};
    std::uint64_t rotations_committed{};
    std::uint64_t compaction_waits{};
    std::uint64_t final_record_commit_attempts{};
    std::uint64_t final_record_commits{};
    std::uint64_t maintenance_attempts{};
    std::uint64_t maintenance_completed{};
    std::uint64_t maintenance_useful{};
    std::uint64_t maintenance_conflicts{};
    std::uint64_t maintenance_bytes_copied{};
    std::size_t segments_after{};
};

struct IdleSample {
    Mode mode{};
    std::size_t repeat{};
    double wall_seconds{};
    std::uint64_t cpu_nanoseconds{};
    std::uint64_t evaluation_cycles{};
    std::uint64_t skips{};
    std::uint64_t last_eval_duration_ns{};
};

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        auto pattern =
            (std::filesystem::temp_directory_path() / "glifistore-maintenance-bench-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* created = ::mkdtemp(writable.data());
        if (created == nullptr) {
            throw std::runtime_error("failed to create maintenance benchmark directory");
        }
        root_ = created;
        store_ = root_ / "store";
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    auto operator=(const TemporaryDirectory&) -> TemporaryDirectory& = delete;

    [[nodiscard]] auto store() const -> const std::filesystem::path& {
        return store_;
    }

  private:
    std::filesystem::path root_;
    std::filesystem::path store_;
};

[[nodiscard]] auto mode_name(const Mode mode) noexcept -> std::string_view {
    switch (mode) {
    case Mode::disabled:
        return "disabled";
    case Mode::cooperative:
        return "cooperative";
    case Mode::background:
        return "background";
    }
    return "unknown";
}

[[nodiscard]] auto parse_mode(const std::string_view value) -> Mode {
    if (value == "disabled") {
        return Mode::disabled;
    }
    if (value == "cooperative") {
        return Mode::cooperative;
    }
    if (value == "background") {
        return Mode::background;
    }
    throw std::runtime_error("unknown maintenance mode: " + std::string{value});
}

[[nodiscard]] auto scenario_name(const Scenario scenario) noexcept -> std::string_view {
    switch (scenario) {
    case Scenario::mixed:
        return "mixed";
    case Scenario::forced_rotation:
        return "forced-rotation";
    case Scenario::idle:
        return "idle";
    case Scenario::churn:
        return "churn";
    }
    return "unknown";
}

[[nodiscard]] auto skip_reason_name(const glifistore::MaintenanceSkipReason reason) noexcept
    -> std::string_view {
    switch (reason) {
    case glifistore::MaintenanceSkipReason::none:
        return "none";
    case glifistore::MaintenanceSkipReason::mode_disabled:
        return "mode-disabled";
    case glifistore::MaintenanceSkipReason::mode_cooperative:
        return "mode-cooperative";
    case glifistore::MaintenanceSkipReason::no_gain:
        return "no-gain";
    case glifistore::MaintenanceSkipReason::no_candidate:
        return "no-candidate";
    case glifistore::MaintenanceSkipReason::budget:
        return "budget";
    case glifistore::MaintenanceSkipReason::store_closed:
        return "store-closed";
    case glifistore::MaintenanceSkipReason::sequence_conflict:
        return "sequence-conflict";
    case glifistore::MaintenanceSkipReason::policy_deferred:
        return "policy-deferred";
    case glifistore::MaintenanceSkipReason::reclaim_threshold:
        return "reclaim-threshold";
    case glifistore::MaintenanceSkipReason::copy_budget:
        return "copy-budget";
    case glifistore::MaintenanceSkipReason::rate_budget:
        return "rate-budget";
    case glifistore::MaintenanceSkipReason::latency_budget:
        return "latency-budget";
    }
    return "unknown";
}

[[nodiscard]] auto parse_scenario(const std::string_view value) -> Scenario {
    if (value == "mixed") {
        return Scenario::mixed;
    }
    if (value == "forced-rotation") {
        return Scenario::forced_rotation;
    }
    if (value == "idle") {
        return Scenario::idle;
    }
    if (value == "churn") {
        return Scenario::churn;
    }
    throw std::runtime_error("unknown maintenance scenario: " + std::string{value});
}

[[nodiscard]] auto parse_size(const char* value, const std::string_view flag) -> std::size_t {
    if (value == nullptr) {
        throw std::runtime_error("missing value for " + std::string{flag});
    }
    const std::string_view text{value};
    const auto parsed = glifistore::bench::parse_decimal_size(text);
    if (!parsed) {
        throw std::runtime_error("invalid value for " + std::string{flag} + ": " + std::string{text});
    }
    return *parsed;
}

[[nodiscard]] auto parse_options(int argc, char** argv) -> Options {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--warmup" && index + 1 < argc) {
            options.warmups = parse_size(argv[++index], argument);
            options.warmups_set = true;
        } else if (argument == "--repeats" && index + 1 < argc) {
            options.repeats = parse_size(argv[++index], argument);
            options.repeats_set = true;
        } else if (argument == "--compaction-delta-entries" && index + 1 < argc) {
            options.compaction_delta_entries = parse_size(argv[++index], argument);
        } else if (argument == "--same-worker") {
            options.same_worker = true;
        } else if (argument == "--operations" && index + 1 < argc) {
            options.operations = parse_size(argv[++index], argument);
            options.operations_set = true;
        } else if (argument == "--threads" && index + 1 < argc) {
            options.threads = parse_size(argv[++index], argument);
        } else if (argument == "--keys" && index + 1 < argc) {
            options.keys = parse_size(argv[++index], argument);
        } else if (argument == "--value-bytes" && index + 1 < argc) {
            options.value_bytes = parse_size(argv[++index], argument);
            options.value_bytes_set = true;
        } else if (argument == "--reclaim-value-bytes" && index + 1 < argc) {
            options.reclaim_value_bytes = parse_size(argv[++index], argument);
        } else if (argument == "--put-percent" && index + 1 < argc) {
            options.put_percent = parse_size(argv[++index], argument);
            options.put_percent_set = true;
        } else if (argument == "--maintenance-interval-ms" && index + 1 < argc) {
            options.maintenance_interval_ms = parse_size(argv[++index], argument);
            options.maintenance_interval_set = true;
        } else if (argument == "--maintenance-copy-bytes-per-sec" && index + 1 < argc) {
            options.maintenance_copy_bytes_per_sec = parse_size(argv[++index], argument);
        } else if (argument == "--cooldown-ms" && index + 1 < argc) {
            options.cooldown_ms = parse_size(argv[++index], argument);
        } else if (argument == "--idle-duration-ms" && index + 1 < argc) {
            options.idle_duration_ms = parse_size(argv[++index], argument);
        } else if (argument == "--mode" && index + 1 < argc) {
            options.mode = parse_mode(argv[++index]);
        } else if (argument == "--scenario" && index + 1 < argc) {
            options.scenario = parse_scenario(argv[++index]);
        } else if (argument == "--help" || argument == "-h") {
            std::cout << "usage: glifistore_maintenance_benchmark [--warmup N] [--repeats N]"
                         " [--operations N] [--threads N] [--keys N] [--value-bytes N]"
                         " [--reclaim-value-bytes N]"
                         " [--put-percent N] [--maintenance-interval-ms N]"
                         " [--maintenance-copy-bytes-per-sec N]"
                         " [--compaction-delta-entries N] [--same-worker]"
                         " [--cooldown-ms N] [--idle-duration-ms N]"
                         " [--scenario mixed|forced-rotation|idle|churn]"
                         " [--mode disabled|cooperative|background]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown or incomplete argument: " + std::string{argument});
        }
    }
    if (options.scenario == Scenario::idle && !options.warmups_set) {
        options.warmups = 0;
    }
    if (options.scenario == Scenario::churn) {
        if (!options.warmups_set) {
            options.warmups = 0;
        }
        if (!options.operations_set) {
            options.operations = 8'192;
        }
        if (!options.value_bytes_set) {
            options.value_bytes = std::size_t{32} * 1024U;
        }
        if (!options.put_percent_set) {
            options.put_percent = 100;
        }
    }
    if (options.repeats == 0 || options.operations == 0 || options.threads == 0 || options.keys == 0 ||
        options.value_bytes < sizeof(std::uint64_t) || options.reclaim_value_bytes < sizeof(std::uint64_t) ||
        options.put_percent > 100 || options.maintenance_interval_ms == 0 ||
        options.maintenance_interval_ms > std::numeric_limits<std::uint32_t>::max() ||
        options.idle_duration_ms == 0) {
        throw std::runtime_error(
            "repeats/operations/threads/keys/interval must be nonzero; value must hold a marker; "
            "put-percent must be in 0..100");
    }
    return options;
}

[[nodiscard]] auto maintenance_mode(const Mode mode) noexcept -> glifistore::MaintenanceMode {
    switch (mode) {
    case Mode::disabled:
        return glifistore::MaintenanceMode::disabled;
    case Mode::cooperative:
        return glifistore::MaintenanceMode::cooperative;
    case Mode::background:
        return glifistore::MaintenanceMode::background;
    }
    return glifistore::MaintenanceMode::disabled;
}

[[nodiscard]] auto store_config(const Options& options, const Mode mode,
                                const std::filesystem::path& directory,
                                const glifistore::DurableOpenMode open_mode, CompactionStartGate* start_gate)
    -> glifistore::StoreConfig {
    const auto interval = static_cast<std::uint32_t>(options.maintenance_interval_ms);
    const bool default_idle_intervals =
        options.scenario == Scenario::idle && !options.maintenance_interval_set;
    glifistore::StoreConfig config{
        .worker_config = {.explicit_count = kBenchmarkWorkerCount},
        .storage_mode = glifistore::StorageMode::durable_periodic,
        .data_directory = directory,
        .durable_open_mode = open_mode,
        .durable_periodic =
            {
                .sync_interval_ms = 60'000,
                .batch =
                    glifistore::DurableGroupConfig{
                        .max_records = 4'096,
                        .max_bytes = 32U * 1024U * 1024U,
                        .max_wait_ms = 60'000,
                    },
            },
        .maintenance =
            {
                .mode = maintenance_mode(mode),
                .max_copy_bytes_per_sec = options.maintenance_copy_bytes_per_sec,
                .min_eval_interval_ms = default_idle_intervals ? 1'000U : interval,
                .max_eval_interval_ms = default_idle_intervals ? 60'000U : interval,
                .dead_byte_ratio_bp_normal = 5'000,
            },
    };
    config.durable_limits.max_compaction_delta_entries = options.compaction_delta_entries;
    if (start_gate != nullptr) {
        config.filesystem_hooks = {
            .context = start_gate,
            .before = &CompactionStartGate::before,
            .available_space_bytes = &CompactionStartGate::available_space_bytes,
        };
    }
    return config;
}

[[nodiscard]] auto open_store(const Options& options, const Mode mode, const std::filesystem::path& directory,
                              const glifistore::DurableOpenMode open_mode,
                              CompactionStartGate* start_gate = nullptr)
    -> std::unique_ptr<glifistore::Store> {
    auto opened = glifistore::Store::open(store_config(options, mode, directory, open_mode, start_gate));
    if (!opened) {
        throw std::runtime_error("failed to open maintenance benchmark Store: " + opened.error().message);
    }
    return std::move(*opened);
}

void require_status(const glifistore::Status& status, const std::string_view operation) {
    if (!status) {
        throw std::runtime_error(std::string{operation} + " failed: " + status.error().message);
    }
}

[[nodiscard]] auto key_for_worker(const std::size_t worker, const std::size_t ordinal,
                                  const std::string_view prefix) -> std::string {
    std::size_t matched{};
    for (std::size_t suffix = 0; suffix < 1'000'000; ++suffix) {
        auto candidate = std::string{prefix} + std::to_string(suffix);
        if (glifistore::route_worker(candidate, 2) != worker) {
            continue;
        }
        if (matched++ == ordinal) {
            return candidate;
        }
    }
    throw std::runtime_error("failed to construct a routed maintenance benchmark key");
}

[[nodiscard]] auto mix(std::uint64_t value) noexcept -> std::uint64_t {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] auto elapsed_ns(const Clock::time_point started) -> std::uint64_t {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
}

[[nodiscard]] auto process_cpu_ns() -> std::uint64_t {
    std::timespec sampled{};
    if (::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &sampled) != 0) {
        throw std::runtime_error("failed to sample process CPU time");
    }
    return static_cast<std::uint64_t>(sampled.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(sampled.tv_nsec);
}

[[nodiscard]] auto summarize(std::vector<std::uint64_t> values) -> LatencySummary {
    if (values.empty()) {
        return {};
    }
    std::ranges::sort(values);
    const auto percentile = [&values](const double quantile) {
        const auto rank = static_cast<std::size_t>(std::ceil(quantile * static_cast<double>(values.size())));
        return values[std::min(std::max(std::size_t{1}, rank), values.size()) - 1U];
    };
    return {
        .samples = values.size(),
        .p50_ns = percentile(0.50),
        .p95_ns = percentile(0.95),
        .p99_ns = percentile(0.99),
        .p999_ns = percentile(0.999),
        .maximum_ns = values.back(),
    };
}

[[nodiscard]] auto merge_latency(const std::vector<ThreadStats>& threads,
                                 const std::vector<std::uint64_t> ThreadStats::* member)
    -> std::vector<std::uint64_t> {
    std::size_t count{};
    for (const auto& thread : threads) {
        count += (thread.*member).size();
    }
    std::vector<std::uint64_t> merged;
    merged.reserve(count);
    for (const auto& thread : threads) {
        const auto& values = thread.*member;
        merged.insert(merged.end(), values.begin(), values.end());
    }
    return merged;
}

[[nodiscard]] auto segment_count(const std::filesystem::path& directory) -> std::size_t {
    std::size_t count{};
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const auto name = entry.path().filename().string();
        if (name.starts_with("segment-") && name.ends_with(".glifi")) {
            ++count;
        }
    }
    return count;
}

void verify_reopened(glifistore::Store& store, const std::vector<std::string>& keys,
                     const std::size_t value_bytes) {
    require_status(store.verify_index(), "verify_index");
    for (const auto& key : keys) {
        const auto found = store.get(key);
        if (!found || found->bytes.size() != value_bytes) {
            throw std::runtime_error("reopened maintenance benchmark key is missing or truncated");
        }
    }
}

[[nodiscard]] auto settle_background_maintenance(glifistore::Store& store, const std::uint64_t minimum_useful,
                                                 const std::uint64_t worker_count)
    -> glifistore::MaintenanceSnapshot {
    const auto deadline = Clock::now() + std::chrono::seconds{5};
    auto stable_since = Clock::now();
    auto snapshot = store.maintenance_snapshot();
    auto observed_useful = snapshot.useful_compactions;
    auto observed_conflicts = snapshot.sequence_conflicts;
    auto stable_evaluation_start = snapshot.evaluation_cycles;
    while (Clock::now() < deadline) {
        snapshot = store.maintenance_snapshot();
        if (snapshot.useful_compactions != observed_useful ||
            snapshot.sequence_conflicts != observed_conflicts) {
            observed_useful = snapshot.useful_compactions;
            observed_conflicts = snapshot.sequence_conflicts;
            stable_evaluation_start = snapshot.evaluation_cycles;
            stable_since = Clock::now();
        }
        const bool quiescent = snapshot.state != glifistore::MaintenanceState::evaluating &&
                               snapshot.state != glifistore::MaintenanceState::compacting &&
                               snapshot.state != glifistore::MaintenanceState::draining;
        const auto& observation = snapshot.last_observation;
        const bool no_reclaimable_candidate =
            !observation.compaction_candidate_worker ||
            (observation.candidate_dead_byte_ratio_bp && *observation.candidate_dead_byte_ratio_bp < 5'000U);
        if (!quiescent || !no_reclaimable_candidate) {
            stable_evaluation_start = snapshot.evaluation_cycles;
            stable_since = Clock::now();
        }
        const auto stable_evaluations = snapshot.evaluation_cycles - stable_evaluation_start;
        if (snapshot.useful_compactions >= minimum_useful && quiescent && no_reclaimable_candidate &&
            stable_evaluations >= worker_count &&
            Clock::now() - stable_since >= std::chrono::milliseconds{100}) {
            return snapshot;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    throw std::runtime_error("background maintenance did not reach a stable useful-compaction state");
}

[[nodiscard]] auto run_sample(const Options& options, const Mode mode, const std::size_t repeat) -> Sample {
    TemporaryDirectory directory;
    std::vector<std::string> reclaim_keys;
    std::vector<std::string> foreground_keys;
    reclaim_keys.reserve(options.keys);
    foreground_keys.reserve(options.keys);
    for (std::size_t index = 0; index < options.keys; ++index) {
        reclaim_keys.push_back(key_for_worker(0, index, "maintenance-reclaim-"));
        foreground_keys.push_back(
            key_for_worker(options.same_worker ? 0 : 1, index, "maintenance-foreground-"));
    }

    auto seed_store =
        open_store(options, Mode::disabled, directory.store(), glifistore::DurableOpenMode::create_new);
    std::vector<std::byte> seed_value(options.reclaim_value_bytes, std::byte{0x5A});
    constexpr std::size_t seed_operations{1'024};
    for (std::size_t operation = 0; operation < seed_operations; ++operation) {
        const auto key_index = operation % reclaim_keys.size();
        const auto marker = static_cast<std::uint64_t>(operation);
        std::memcpy(seed_value.data(), &marker, sizeof(marker));
        require_status(seed_store->put(reclaim_keys[key_index], seed_value), "reclaim seed put");
    }
    std::vector<std::byte> foreground_value(options.value_bytes, std::byte{0x3C});
    for (std::size_t index = 0; index < foreground_keys.size(); ++index) {
        const auto marker = static_cast<std::uint64_t>(seed_operations + index);
        std::memcpy(foreground_value.data(), &marker, sizeof(marker));
        require_status(seed_store->put(foreground_keys[index], foreground_value), "foreground preload put");
    }
    require_status(seed_store->flush(), "seed flush");
    require_status(seed_store->close(), "seed Store close");
    seed_store.reset();

    std::latch start{1};
    std::latch snapshot_ready{options.same_worker && mode == Mode::cooperative ? 1 : 0};
    CompactionStartGate start_gate{
        .start = &start,
        .snapshot_ready = options.same_worker && mode == Mode::cooperative ? &snapshot_ready : nullptr,
        .store = directory.store(),
        .opener_thread = std::this_thread::get_id(),
    };
    auto store =
        open_store(options, mode, directory.store(), glifistore::DurableOpenMode::open_existing, &start_gate);

    std::vector<ThreadStats> thread_stats(options.threads);
    for (auto& thread : thread_stats) {
        const auto capacity =
            options.operations / options.threads + (options.operations % options.threads == 0 ? 0U : 1U);
        thread.all_latency_ns.reserve(capacity);
        thread.get_latency_ns.reserve(capacity);
        thread.put_latency_ns.reserve(capacity * options.put_percent / 100U + 1U);
    }

    const bool cooperative = mode == Mode::cooperative;
    std::latch ready{static_cast<std::ptrdiff_t>(options.threads + (cooperative ? 1U : 0U))};
    CooperativeStats cooperative_stats;
    std::vector<std::thread> workers;
    workers.reserve(options.threads);

    for (std::size_t thread_index = 0; thread_index < options.threads; ++thread_index) {
        workers.emplace_back([&, thread_index] {
            auto& stats = thread_stats[thread_index];
            std::vector<std::byte> value(options.value_bytes, std::byte{0xA5});
            ready.count_down();
            start.wait();
            snapshot_ready.wait();
            for (std::size_t operation = thread_index; operation < options.operations;
                 operation += options.threads) {
                const auto selected = mix(operation);
                const auto key_index = static_cast<std::size_t>((selected >> 8U) % options.keys);
                const bool put = selected % 100U < options.put_percent;
                const auto started = Clock::now();
                bool succeeded{};
                if (put) {
                    const auto marker = static_cast<std::uint64_t>(operation + options.keys);
                    std::memcpy(value.data(), &marker, sizeof(marker));
                    const auto retry_deadline = Clock::now() + std::chrono::seconds{5};
                    for (;;) {
                        auto status = store->put(foreground_keys[key_index], value);
                        if (status) {
                            succeeded = true;
                            break;
                        }
                        const bool compaction_conflict =
                            options.same_worker &&
                            status.error().code == glifistore::ErrorCode::sequence_conflict;
                        if ((!is_generation_backpressure(status.error()) && !compaction_conflict) ||
                            Clock::now() >= retry_deadline) {
                            break;
                        }
                        if (compaction_conflict) {
                            ++stats.compaction_conflict_retries;
                        } else {
                            ++stats.generation_backpressure_retries;
                        }
                        if (((stats.generation_backpressure_retries + stats.compaction_conflict_retries) &
                             63U) == 0U) {
                            std::this_thread::sleep_for(std::chrono::microseconds{50});
                        } else {
                            std::this_thread::yield();
                        }
                    }
                    ++stats.puts;
                    stats.put_successes += succeeded ? 1U : 0U;
                } else {
                    const auto found = store->get(foreground_keys[key_index]);
                    succeeded = found.has_value() && found->bytes.size() == options.value_bytes;
                    ++stats.gets;
                    stats.get_hits += succeeded ? 1U : 0U;
                }
                const auto latency = elapsed_ns(started);
                stats.all_latency_ns.push_back(latency);
                (put ? stats.put_latency_ns : stats.get_latency_ns).push_back(latency);
                stats.failures += succeeded ? 0U : 1U;
            }
        });
    }

    std::thread cooperative_worker;
    if (cooperative) {
        cooperative_worker = std::thread([&] {
            ready.count_down();
            start.wait();
            ++cooperative_stats.attempts;
            const auto compacted = store->compact();
            start_gate.release_snapshot();
            if (compacted) {
                if (compacted->compacted) {
                    ++cooperative_stats.useful;
                    cooperative_stats.bytes_copied += compacted->bytes_copied;
                }
            } else if (compacted.error().code == glifistore::ErrorCode::sequence_conflict) {
                ++cooperative_stats.conflicts;
            } else {
                ++cooperative_stats.unexpected_errors;
            }
        });
    }

    ready.wait();
    const auto foreground_started = Clock::now();
    start.count_down();
    for (auto& worker : workers) {
        worker.join();
    }
    const auto foreground_seconds = std::chrono::duration<double>(Clock::now() - foreground_started).count();
    if (cooperative_worker.joinable()) {
        cooperative_worker.join();
    }

    std::uint64_t operations{};
    std::uint64_t gets{};
    std::uint64_t get_hits{};
    std::uint64_t puts{};
    std::uint64_t put_successes{};
    std::uint64_t generation_backpressure_retries{};
    std::uint64_t compaction_conflict_retries{};
    std::uint64_t failures{};
    for (const auto& stats : thread_stats) {
        operations += stats.all_latency_ns.size();
        gets += stats.gets;
        get_hits += stats.get_hits;
        puts += stats.puts;
        put_successes += stats.put_successes;
        generation_backpressure_retries += stats.generation_backpressure_retries;
        compaction_conflict_retries += stats.compaction_conflict_retries;
        failures += stats.failures;
    }
    if (operations != options.operations || get_hits != gets || put_successes != puts || failures != 0 ||
        cooperative_stats.unexpected_errors != 0) {
        throw std::runtime_error(
            "foreground or cooperative maintenance validation failed: operations=" +
            std::to_string(operations) + "/" + std::to_string(options.operations) +
            " get_hits=" + std::to_string(get_hits) + "/" + std::to_string(gets) +
            " put_successes=" + std::to_string(put_successes) + "/" + std::to_string(puts) +
            " failures=" + std::to_string(failures) +
            " maintenance_errors=" + std::to_string(cooperative_stats.unexpected_errors));
    }

    auto maintenance = store->maintenance_snapshot();
    if (mode == Mode::background) {
        maintenance = settle_background_maintenance(*store, 1, kBenchmarkWorkerCount);
    }
    const auto named_rotation_execution_ns = maintenance.rotation.total_seal_duration_ns +
                                             maintenance.rotation.total_create_duration_ns +
                                             maintenance.rotation.total_manifest_publication_duration_ns;
    if (named_rotation_execution_ns > maintenance.rotation.total_execution_duration_ns ||
        maintenance.rotation.attempts != maintenance.rotation.committed ||
        maintenance.rotation.committed != maintenance.rotation.final_record_commit_attempts ||
        maintenance.rotation.final_record_commit_attempts != maintenance.rotation.final_record_commits) {
        throw std::runtime_error("foreground rotation phase telemetry is incomplete or inconsistent");
    }
    require_status(store->close(), "Store close");
    store.reset();
    const auto segments_after = segment_count(directory.store());

    auto reopened =
        open_store(options, Mode::disabled, directory.store(), glifistore::DurableOpenMode::open_existing);
    verify_reopened(*reopened, reclaim_keys, options.reclaim_value_bytes);
    verify_reopened(*reopened, foreground_keys, options.value_bytes);
    require_status(reopened->close(), "reopened Store close");

    const auto all_latency = summarize(merge_latency(thread_stats, &ThreadStats::all_latency_ns));
    const auto get_latency = summarize(merge_latency(thread_stats, &ThreadStats::get_latency_ns));
    const auto put_latency = summarize(merge_latency(thread_stats, &ThreadStats::put_latency_ns));
    return {
        .mode = mode,
        .repeat = repeat,
        .foreground_seconds = foreground_seconds,
        .operations = operations,
        .gets = gets,
        .puts = puts,
        .generation_backpressure_retries = generation_backpressure_retries,
        .compaction_conflict_retries = compaction_conflict_retries,
        .all_latency = all_latency,
        .get_latency = get_latency,
        .put_latency = put_latency,
        .maintenance_attempts = cooperative ? cooperative_stats.attempts : maintenance.compact_attempts,
        .maintenance_completed = cooperative ? cooperative_stats.useful : maintenance.compact_completed,
        .maintenance_useful = cooperative ? cooperative_stats.useful : maintenance.useful_compactions,
        .maintenance_conflicts = cooperative ? cooperative_stats.conflicts : maintenance.sequence_conflicts,
        .maintenance_bytes_copied =
            cooperative ? cooperative_stats.bytes_copied : maintenance.total_bytes_copied,
        .maintenance_skips = cooperative ? 0 : maintenance.skips,
        .maintenance_evaluations = cooperative ? 0 : maintenance.evaluation_cycles,
        .maintenance_no_gain_scans_suppressed = cooperative ? 0 : maintenance.no_gain_scans_suppressed,
        .maintenance_no_gain_retry_after_ns = cooperative ? 0 : maintenance.no_gain_retry_after_ns,
        .maintenance_pacing_delay_ns = cooperative ? 0 : maintenance.last_compaction_pacing_delay_ns,
        .maintenance_pacing_sleep_count = cooperative ? 0 : maintenance.last_compaction_pacing_sleep_count,
        .maintenance_pacing_burst_bytes = cooperative ? 0 : maintenance.last_compaction_pacing_burst_bytes,
        .maintenance_last_skip =
            cooperative ? glifistore::MaintenanceSkipReason::none : maintenance.last_skip_reason,
        .maintenance_observation =
            cooperative ? glifistore::MaintenanceObservation{} : maintenance.last_observation,
        .rotation = maintenance.rotation,
        .compaction = maintenance.compaction,
        .segments_after = segments_after,
    };
}

[[nodiscard]] auto run_rotation_sample(const Options& options, const Mode mode, const std::size_t repeat)
    -> RotationSample {
    TemporaryDirectory directory;
    std::vector<std::string> reclaim_keys;
    reclaim_keys.reserve(options.keys);
    for (std::size_t index = 0; index < options.keys; ++index) {
        reclaim_keys.push_back(key_for_worker(0, index, "rotation-reclaim-"));
    }
    const auto rotating_key = key_for_worker(1, 0, "rotation-foreground-");

    auto seed_store =
        open_store(options, Mode::disabled, directory.store(), glifistore::DurableOpenMode::create_new);
    std::vector<std::byte> seed_value(options.reclaim_value_bytes, std::byte{0x5A});
    constexpr std::size_t seed_operations{1'024};
    for (std::size_t operation = 0; operation < seed_operations; ++operation) {
        const auto key_index = operation % reclaim_keys.size();
        const auto marker = static_cast<std::uint64_t>(operation);
        std::memcpy(seed_value.data(), &marker, sizeof(marker));
        require_status(seed_store->put(reclaim_keys[key_index], seed_value), "rotation reclaim seed put");
    }

    std::vector<std::byte> rotation_value(options.reclaim_value_bytes, std::byte{0x3C});
    const auto key_bytes = std::as_bytes(std::span{rotating_key});
    const glifistore::RecordInput sizing{
        .sequence = glifistore::SequenceNumber{1},
        .opcode = glifistore::Opcode::put,
        .type = glifistore::ValueType::bytes,
        .key_hash = glifistore::hash_key(key_bytes),
        .key = key_bytes,
        .value = rotation_value,
    };
    const auto encoded_size = glifistore::encoded_record_size(sizing);
    if (!encoded_size) {
        throw std::runtime_error("failed to size forced-rotation Record: " + encoded_size.error().message);
    }
    const auto payload_bytes = glifistore::kSegmentSizeBytes - glifistore::kSegmentHeaderReservedBytes;
    const auto fill_records = payload_bytes / *encoded_size;
    if (fill_records == 0) {
        throw std::runtime_error("forced-rotation Record does not fit in a Segment");
    }
    for (std::size_t operation = 0; operation < fill_records; ++operation) {
        const auto marker = static_cast<std::uint64_t>(operation);
        std::memcpy(rotation_value.data(), &marker, sizeof(marker));
        require_status(seed_store->put(rotating_key, rotation_value), "forced-rotation fill put");
    }
    require_status(seed_store->flush(), "forced-rotation seed flush");
    require_status(seed_store->close(), "forced-rotation seed Store close");
    seed_store.reset();

    std::latch start{1};
    std::latch compaction_intent_started{1};
    std::latch rotation_entered{1};
    CompactionStartGate start_gate{
        .start = &start,
        .compaction_intent_started = mode == Mode::disabled ? nullptr : &compaction_intent_started,
        .rotation_entered = mode == Mode::disabled ? nullptr : &rotation_entered,
        .store = directory.store(),
        .opener_thread = std::this_thread::get_id(),
    };
    auto store =
        open_store(options, mode, directory.store(), glifistore::DurableOpenMode::open_existing, &start_gate);

    CooperativeStats cooperative_stats;
    std::thread cooperative_worker;
    if (mode == Mode::cooperative) {
        cooperative_worker = std::thread([&] {
            start.wait();
            ++cooperative_stats.attempts;
            const auto compacted = store->compact();
            if (compacted) {
                if (compacted->compacted) {
                    ++cooperative_stats.useful;
                    cooperative_stats.bytes_copied += compacted->bytes_copied;
                }
            } else if (compacted.error().code == glifistore::ErrorCode::sequence_conflict) {
                ++cooperative_stats.conflicts;
            } else {
                ++cooperative_stats.unexpected_errors;
            }
        });
    }

    glifistore::Status rotation;
    std::uint64_t rotation_latency_ns{};
    std::thread writer{[&] {
        start.wait();
        if (mode != Mode::disabled) {
            compaction_intent_started.wait();
            rotation_entered.count_down();
        }
        const auto marker = std::numeric_limits<std::uint64_t>::max();
        std::memcpy(rotation_value.data(), &marker, sizeof(marker));
        const auto started = Clock::now();
        rotation = store->put(rotating_key, rotation_value);
        rotation_latency_ns = elapsed_ns(started);
    }};

    start.count_down();
    writer.join();
    if (cooperative_worker.joinable()) {
        cooperative_worker.join();
    }
    if (!rotation || cooperative_stats.unexpected_errors != 0) {
        throw std::runtime_error(
            "forced rotation or cooperative compaction failed: rotation=" +
            std::string{rotation ? "committed" : rotation.error().message} +
            " maintenance_errors=" + std::to_string(cooperative_stats.unexpected_errors));
    }

    auto maintenance = store->maintenance_snapshot();
    if (mode == Mode::background) {
        maintenance = settle_background_maintenance(*store, 1, kBenchmarkWorkerCount);
    }
    const auto rotation_stats = maintenance.rotation;
    const auto named_execution_ns = rotation_stats.last_seal_duration_ns +
                                    rotation_stats.last_create_duration_ns +
                                    rotation_stats.last_manifest_publication_duration_ns;
    const auto end_to_end_accounted_ns =
        rotation_stats.last_total_duration_ns + rotation_stats.last_final_record_commit_duration_ns;
    if (rotation_stats.attempts != 1 || rotation_stats.committed != 1 ||
        rotation_stats.compaction_waits != (mode == Mode::disabled ? 0U : 1U) ||
        rotation_stats.final_record_commit_attempts != 1 || rotation_stats.final_record_commits != 1 ||
        named_execution_ns > rotation_stats.last_execution_duration_ns ||
        end_to_end_accounted_ns > rotation_latency_ns) {
        throw std::runtime_error("forced-rotation telemetry validation failed: attempts=" +
                                 std::to_string(rotation_stats.attempts) +
                                 " committed=" + std::to_string(rotation_stats.committed) +
                                 " compaction_waits=" + std::to_string(rotation_stats.compaction_waits) +
                                 " final_commits=" + std::to_string(rotation_stats.final_record_commits) +
                                 " accounted_ns=" + std::to_string(end_to_end_accounted_ns) +
                                 " wall_ns=" + std::to_string(rotation_latency_ns));
    }
    require_status(store->close(), "forced-rotation Store close");
    store.reset();
    const auto segments_after = segment_count(directory.store());

    auto reopened =
        open_store(options, Mode::disabled, directory.store(), glifistore::DurableOpenMode::open_existing);
    verify_reopened(*reopened, reclaim_keys, options.reclaim_value_bytes);
    verify_reopened(*reopened, std::vector<std::string>{rotating_key}, options.reclaim_value_bytes);
    require_status(reopened->close(), "forced-rotation reopened Store close");

    return {
        .mode = mode,
        .repeat = repeat,
        .rotation_seconds = static_cast<double>(rotation_latency_ns) / 1'000'000'000.0,
        .publication_wait_ns = rotation_stats.last_publication_wait_duration_ns,
        .seal_ns = rotation_stats.last_seal_duration_ns,
        .create_ns = rotation_stats.last_create_duration_ns,
        .manifest_publication_ns = rotation_stats.last_manifest_publication_duration_ns,
        .other_execution_ns = rotation_stats.last_execution_duration_ns - named_execution_ns,
        .execution_ns = rotation_stats.last_execution_duration_ns,
        .accounted_total_ns = rotation_stats.last_total_duration_ns,
        .final_record_commit_ns = rotation_stats.last_final_record_commit_duration_ns,
        .measurement_residual_ns = rotation_latency_ns - end_to_end_accounted_ns,
        .rotation_attempts = rotation_stats.attempts,
        .rotations_committed = rotation_stats.committed,
        .compaction_waits = rotation_stats.compaction_waits,
        .final_record_commit_attempts = rotation_stats.final_record_commit_attempts,
        .final_record_commits = rotation_stats.final_record_commits,
        .maintenance_attempts =
            mode == Mode::cooperative ? cooperative_stats.attempts : maintenance.compact_attempts,
        .maintenance_completed =
            mode == Mode::cooperative ? cooperative_stats.useful : maintenance.compact_completed,
        .maintenance_useful =
            mode == Mode::cooperative ? cooperative_stats.useful : maintenance.useful_compactions,
        .maintenance_conflicts =
            mode == Mode::cooperative ? cooperative_stats.conflicts : maintenance.sequence_conflicts,
        .maintenance_bytes_copied =
            mode == Mode::cooperative ? cooperative_stats.bytes_copied : maintenance.total_bytes_copied,
        .segments_after = segments_after,
    };
}

[[nodiscard]] auto run_idle_sample(const Options& options, const Mode mode, const std::size_t repeat)
    -> IdleSample {
    TemporaryDirectory directory;
    std::latch start{1};
    CompactionStartGate start_gate{
        .start = &start,
        .store = directory.store(),
        .opener_thread = std::this_thread::get_id(),
    };
    auto store =
        open_store(options, mode, directory.store(), glifistore::DurableOpenMode::create_new, &start_gate);
    const auto cpu_started = process_cpu_ns();
    const auto wall_started = Clock::now();
    start.count_down();
    std::this_thread::sleep_for(std::chrono::milliseconds{options.idle_duration_ms});
    const auto wall_seconds = std::chrono::duration<double>(Clock::now() - wall_started).count();
    const auto cpu_nanoseconds = process_cpu_ns() - cpu_started;
    const auto maintenance = store->maintenance_snapshot();
    require_status(store->close(), "idle Store close");
    return {
        .mode = mode,
        .repeat = repeat,
        .wall_seconds = wall_seconds,
        .cpu_nanoseconds = cpu_nanoseconds,
        .evaluation_cycles = maintenance.evaluation_cycles,
        .skips = maintenance.skips,
        .last_eval_duration_ns = maintenance.last_eval_duration_ns,
    };
}

[[nodiscard]] auto microseconds(const std::uint64_t nanoseconds) -> double {
    return static_cast<double>(nanoseconds) / 1'000.0;
}

void print_sample(const Sample& sample) {
    const auto operations_per_second =
        sample.foreground_seconds > 0.0 ? static_cast<double>(sample.operations) / sample.foreground_seconds
                                        : 0.0;
    std::cout << mode_name(sample.mode) << ',' << sample.repeat << ',' << sample.operations << ','
              << sample.gets << ',' << sample.puts << ',' << sample.generation_backpressure_retries << ','
              << sample.foreground_seconds << ',' << operations_per_second << ','
              << microseconds(sample.all_latency.p50_ns) << ',' << microseconds(sample.all_latency.p95_ns)
              << ',' << microseconds(sample.all_latency.p99_ns) << ','
              << microseconds(sample.all_latency.maximum_ns) << ',' << microseconds(sample.get_latency.p50_ns)
              << ',' << microseconds(sample.get_latency.p95_ns) << ','
              << microseconds(sample.get_latency.p99_ns) << ',' << microseconds(sample.get_latency.maximum_ns)
              << ',' << microseconds(sample.put_latency.p50_ns) << ','
              << microseconds(sample.put_latency.p95_ns) << ',' << microseconds(sample.put_latency.p99_ns)
              << ',' << microseconds(sample.put_latency.maximum_ns) << ',' << sample.maintenance_attempts
              << ',' << sample.maintenance_completed << ',' << sample.maintenance_useful << ','
              << sample.maintenance_conflicts << ',' << sample.maintenance_bytes_copied << ','
              << sample.maintenance_skips << ',' << sample.maintenance_evaluations << ','
              << sample.maintenance_no_gain_scans_suppressed << ','
              << sample.maintenance_no_gain_retry_after_ns << ','
              << microseconds(sample.maintenance_pacing_delay_ns) / 1'000.0 << ','
              << sample.maintenance_pacing_sleep_count << ',' << sample.maintenance_pacing_burst_bytes << ','
              << skip_reason_name(sample.maintenance_last_skip) << ',';
    if (sample.maintenance_observation.compaction_candidate_worker) {
        std::cout << *sample.maintenance_observation.compaction_candidate_worker;
    } else {
        std::cout << "none";
    }
    std::cout << ',' << sample.maintenance_observation.candidate_sealed_record_bytes << ','
              << sample.maintenance_observation.candidate_live_record_bytes << ','
              << sample.maintenance_observation.candidate_dead_record_bytes << ',';
    if (sample.maintenance_observation.candidate_dead_byte_ratio_bp) {
        std::cout << *sample.maintenance_observation.candidate_dead_byte_ratio_bp;
    } else {
        std::cout << "none";
    }
    const auto named_execution_ns = sample.rotation.total_seal_duration_ns +
                                    sample.rotation.total_create_duration_ns +
                                    sample.rotation.total_manifest_publication_duration_ns;
    std::cout << ',' << sample.rotation.attempts << ',' << sample.rotation.committed << ','
              << sample.rotation.compaction_waits << ',' << sample.rotation.final_record_commit_attempts
              << ',' << sample.rotation.final_record_commits << ','
              << microseconds(sample.rotation.total_publication_wait_duration_ns) / 1'000.0 << ','
              << microseconds(sample.rotation.total_seal_duration_ns) / 1'000.0 << ','
              << microseconds(sample.rotation.total_create_duration_ns) / 1'000.0 << ','
              << microseconds(sample.rotation.total_manifest_publication_duration_ns) / 1'000.0 << ','
              << microseconds(sample.rotation.total_execution_duration_ns - named_execution_ns) / 1'000.0
              << ',' << microseconds(sample.rotation.total_execution_duration_ns) / 1'000.0 << ','
              << microseconds(sample.rotation.total_final_record_commit_duration_ns) / 1'000.0 << ','
              << microseconds(sample.rotation.maximum_total_duration_ns) / 1'000.0 << ','
              << sample.segments_after << ',' << microseconds(sample.all_latency.p999_ns) << ','
              << microseconds(sample.get_latency.p999_ns) << ',' << microseconds(sample.put_latency.p999_ns)
              << ',' << sample.compaction.replayed_records << ',' << sample.compaction.delta_overflow_aborts
              << ',' << sample.compaction.manifest_conflict_aborts << ','
              << sample.compaction.abandoned_copy_bytes << ','
              << sample.compaction.maximum_snapshot_duration_ns << ','
              << sample.compaction.maximum_commit_gate_duration_ns << ','
              << sample.compaction_conflict_retries << '\n';
}

void print_rotation_sample(const RotationSample& sample) {
    std::cout << mode_name(sample.mode) << ',' << sample.repeat << ',' << sample.rotation_seconds << ','
              << sample.rotation_seconds * 1'000.0 << ','
              << microseconds(sample.publication_wait_ns) / 1'000.0 << ','
              << microseconds(sample.seal_ns) / 1'000.0 << ',' << microseconds(sample.create_ns) / 1'000.0
              << ',' << microseconds(sample.manifest_publication_ns) / 1'000.0 << ','
              << microseconds(sample.other_execution_ns) / 1'000.0 << ','
              << microseconds(sample.execution_ns) / 1'000.0 << ','
              << microseconds(sample.accounted_total_ns) / 1'000.0 << ','
              << microseconds(sample.final_record_commit_ns) / 1'000.0 << ','
              << microseconds(sample.measurement_residual_ns) / 1'000.0 << ',' << sample.rotation_attempts
              << ',' << sample.rotations_committed << ',' << sample.compaction_waits << ','
              << sample.final_record_commit_attempts << ',' << sample.final_record_commits << ','
              << sample.maintenance_attempts << ',' << sample.maintenance_completed << ','
              << sample.maintenance_useful << ',' << sample.maintenance_conflicts << ','
              << sample.maintenance_bytes_copied << ',' << sample.segments_after << '\n';
}

void print_idle_sample(const IdleSample& sample) {
    const auto cpu_ms = static_cast<double>(sample.cpu_nanoseconds) / 1'000'000.0;
    const auto cpu_duty = sample.wall_seconds > 0.0
                              ? static_cast<double>(sample.cpu_nanoseconds) /
                                    (sample.wall_seconds * static_cast<double>(1'000'000'000ULL)) * 100.0
                              : 0.0;
    std::cout << mode_name(sample.mode) << ',' << sample.repeat << ',' << sample.wall_seconds << ',' << cpu_ms
              << ',' << cpu_duty << ',' << sample.evaluation_cycles << ',' << sample.skips << ','
              << microseconds(sample.last_eval_duration_ns) << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        constexpr std::array modes{Mode::disabled, Mode::cooperative, Mode::background};
        std::vector<Mode> selected;
        for (const auto mode : modes) {
            if (options.mode && *options.mode != mode) {
                continue;
            }
            const bool omit_cooperative_by_default =
                !options.mode && mode == Mode::cooperative &&
                (options.scenario == Scenario::idle || options.scenario == Scenario::churn);
            if (!omit_cooperative_by_default) {
                selected.push_back(mode);
            }
        }
        if (selected.empty()) {
            throw std::runtime_error("maintenance benchmark selected no modes");
        }

        std::cout << "# benchmark=glifistore_concurrent_maintenance\n";
        glifistore::bench::print_common_metadata(std::cout, options.warmups, options.repeats);
        std::cout << "# scenario=" << scenario_name(options.scenario) << '\n';
        std::cout << "# threads=" << options.threads << ";operations=" << options.operations
                  << ";keys=" << options.keys << ";value_bytes=" << options.value_bytes
                  << ";reclaim_value_bytes=" << options.reclaim_value_bytes
                  << ";put_percent=" << options.put_percent << ";cooldown_ms=" << options.cooldown_ms << '\n';
        std::cout << std::fixed << std::setprecision(6);

        if (options.scenario == Scenario::idle) {
            std::cout << "# storage_mode=durable-periodic;workers=2;foreground=none;duration_ms="
                      << options.idle_duration_ms << '\n';
            if (options.maintenance_interval_set) {
                std::cout << "# background_policy=min_eval_interval_ms=max_eval_interval_ms="
                          << options.maintenance_interval_ms << '\n';
            } else {
                std::cout << "# background_policy=product_defaults;min_eval_interval_ms=1000;"
                             "max_eval_interval_ms=60000\n";
            }
            std::cout << "mode,repeat,wall_s,process_cpu_ms,process_cpu_duty_pct,evaluation_cycles,"
                         "maintenance_skips,last_eval_us\n";
            for (std::size_t warmup = 0; warmup < options.warmups; ++warmup) {
                const auto offset = warmup % selected.size();
                for (std::size_t index = 0; index < selected.size(); ++index) {
                    static_cast<void>(
                        run_idle_sample(options, selected[(index + offset) % selected.size()], 0));
                    std::this_thread::sleep_for(std::chrono::milliseconds{options.cooldown_ms});
                }
            }
            for (std::size_t repeat = 1; repeat <= options.repeats; ++repeat) {
                const auto offset = (repeat - 1U) % selected.size();
                for (std::size_t index = 0; index < selected.size(); ++index) {
                    print_idle_sample(
                        run_idle_sample(options, selected[(index + offset) % selected.size()], repeat));
                    std::this_thread::sleep_for(std::chrono::milliseconds{options.cooldown_ms});
                }
            }
            return 0;
        }

        std::cout << "# storage_mode=durable-periodic;workers=2;reclaim_worker=0;foreground_worker="
                  << (options.same_worker ? 0 : 1)
                  << ";seed_operations=1024;seed_flush=true;compaction_delta_entries="
                  << options.compaction_delta_entries << '\n';
        std::cout << "# cooperative_policy=one public Store::compact at foreground start\n";
        std::cout << "# background_policy=min_eval_interval_ms=max_eval_interval_ms="
                  << options.maintenance_interval_ms
                  << ";dead_byte_ratio_bp_normal=5000;"
                     "max_copy_bytes_per_cycle=default;max_no_gain_attempts=default;"
                     "max_copy_bytes_per_sec="
                  << options.maintenance_copy_bytes_per_sec << '\n';

        if (options.scenario == Scenario::forced_rotation) {
            std::cout << "# overlap=compaction intent publication releases a forced unrelated-Worker "
                         "rotation\n";
            std::cout << "mode,repeat,rotation_s,rotation_ms,publication_wait_ms,seal_ms,create_ms,"
                         "manifest_publication_ms,other_execution_ms,execution_ms,accounted_total_ms,"
                         "final_record_commit_ms,measurement_residual_ms,rotation_attempts,"
                         "rotations_committed,compaction_waits,final_record_commit_attempts,"
                         "final_record_commits,maintenance_attempts,"
                         "maintenance_completed,maintenance_useful,maintenance_conflicts,"
                         "maintenance_bytes_copied,segments_after\n";
            for (std::size_t warmup = 0; warmup < options.warmups; ++warmup) {
                const auto offset = warmup % selected.size();
                for (std::size_t index = 0; index < selected.size(); ++index) {
                    static_cast<void>(
                        run_rotation_sample(options, selected[(index + offset) % selected.size()], 0));
                    std::this_thread::sleep_for(std::chrono::milliseconds{options.cooldown_ms});
                }
            }
            for (std::size_t repeat = 1; repeat <= options.repeats; ++repeat) {
                const auto offset = (repeat - 1U) % selected.size();
                for (std::size_t index = 0; index < selected.size(); ++index) {
                    print_rotation_sample(
                        run_rotation_sample(options, selected[(index + offset) % selected.size()], repeat));
                    std::this_thread::sleep_for(std::chrono::milliseconds{options.cooldown_ms});
                }
            }
            return 0;
        }

        std::cout << "# latency_measurement=per-operation steady_clock;instrumented throughput\n";
        std::cout << "mode,repeat,operations,get_operations,put_operations,"
                     "generation_backpressure_retries,foreground_s,"
                     "foreground_ops_s,p50_us,p95_us,p99_us,max_us,get_p50_us,get_p95_us,"
                     "get_p99_us,get_max_us,put_p50_us,put_p95_us,put_p99_us,put_max_us,"
                     "maintenance_attempts,maintenance_completed,maintenance_useful,"
                     "maintenance_conflicts,maintenance_bytes_copied,maintenance_skips,"
                     "maintenance_evaluations,maintenance_no_gain_scans_suppressed,"
                     "maintenance_no_gain_retry_after_ns,maintenance_pacing_delay_ms,"
                     "maintenance_pacing_sleep_count,maintenance_pacing_burst_bytes,"
                     "maintenance_last_skip,candidate_worker,"
                     "candidate_sealed_bytes,candidate_live_bytes,candidate_dead_bytes,"
                     "candidate_dead_ratio_bp,rotation_attempts,rotations_committed,"
                     "rotation_compaction_waits,rotation_final_record_commit_attempts,"
                     "rotation_final_record_commits,rotation_total_publication_wait_ms,"
                     "rotation_total_seal_ms,rotation_total_create_ms,"
                     "rotation_total_manifest_publication_ms,rotation_total_other_execution_ms,"
                     "rotation_total_execution_ms,rotation_total_final_record_commit_ms,"
                     "rotation_maximum_total_ms,segments_after,p999_us,get_p999_us,put_p999_us,"
                     "compaction_replayed_records,compaction_delta_overflow_aborts,"
                     "compaction_manifest_conflict_aborts,compaction_abandoned_copy_bytes,"
                     "compaction_maximum_snapshot_ns,compaction_maximum_commit_gate_ns,"
                     "compaction_conflict_retries\n";

        for (std::size_t warmup = 0; warmup < options.warmups; ++warmup) {
            const auto offset = warmup % selected.size();
            for (std::size_t index = 0; index < selected.size(); ++index) {
                static_cast<void>(run_sample(options, selected[(index + offset) % selected.size()], 0));
                std::this_thread::sleep_for(std::chrono::milliseconds{options.cooldown_ms});
            }
        }
        for (std::size_t repeat = 1; repeat <= options.repeats; ++repeat) {
            const auto offset = (repeat - 1U) % selected.size();
            for (std::size_t index = 0; index < selected.size(); ++index) {
                print_sample(run_sample(options, selected[(index + offset) % selected.size()], repeat));
                std::this_thread::sleep_for(std::chrono::milliseconds{options.cooldown_ms});
            }
        }
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "maintenance benchmark error: " << exception.what() << '\n';
        return 1;
    }
}
