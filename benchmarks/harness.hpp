#pragma once

#include "benchmark_metadata.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace glifistore::bench {

inline constexpr std::size_t kDefaultOperations = 200'000;
inline constexpr std::size_t kDefaultWarmupIterations = 1;
inline constexpr std::size_t kDefaultMeasuredIterations = 7;

enum class BenchmarkKind {
    index_insert,
    index_replace,
    index_find_hit,
    index_find_miss,
    index_churn_miss,
    index_erase,
    index_insert_find,
    index_all,
    store_put,
    store_put_batch,
    store_get,
    store_put_get,
    store_read_after_write,
    store_parallel_put,
    store_parallel_get,
    store_parallel_read_after_write,
    store_parallel_all,
    store_durable_put,
    store_durable_get,
    store_durable_put_get,
    store_durable_read_after_write,
    store_durable_recovery_open,
    store_durable_periodic_put,
    store_durable_periodic_get,
    store_durable_periodic_put_get,
    store_durable_periodic_read_after_write,
    store_durable_periodic_all,
    store_durable_group_put,
    store_durable_group_get,
    store_durable_group_put_get,
    store_durable_group_read_after_write,
    store_durable_group_all,
    store_durable_group_parallel_put,
    store_durable_parallel_put,
    store_durable_parallel_get,
    store_durable_parallel_read_after_write,
    store_durable_all,
    store_durable_parallel_all,
    all
};

enum class ParallelDistribution { uniform, worker_affine, owner_bound, single_worker, zipf };

[[nodiscard]] inline auto is_parallel_benchmark(const BenchmarkKind kind) noexcept -> bool {
    return kind == BenchmarkKind::store_parallel_put || kind == BenchmarkKind::store_parallel_get ||
           kind == BenchmarkKind::store_parallel_read_after_write ||
           kind == BenchmarkKind::store_parallel_all || kind == BenchmarkKind::store_durable_parallel_put ||
           kind == BenchmarkKind::store_durable_parallel_get ||
           kind == BenchmarkKind::store_durable_parallel_read_after_write ||
           kind == BenchmarkKind::store_durable_parallel_all ||
           kind == BenchmarkKind::store_durable_group_parallel_put;
}

[[nodiscard]] inline auto is_durable_benchmark(const BenchmarkKind kind) noexcept -> bool {
    return kind == BenchmarkKind::store_durable_put || kind == BenchmarkKind::store_durable_get ||
           kind == BenchmarkKind::store_durable_put_get ||
           kind == BenchmarkKind::store_durable_read_after_write ||
           kind == BenchmarkKind::store_durable_recovery_open ||
           kind == BenchmarkKind::store_durable_periodic_put ||
           kind == BenchmarkKind::store_durable_periodic_get ||
           kind == BenchmarkKind::store_durable_periodic_put_get ||
           kind == BenchmarkKind::store_durable_periodic_read_after_write ||
           kind == BenchmarkKind::store_durable_periodic_all ||
           kind == BenchmarkKind::store_durable_group_put || kind == BenchmarkKind::store_durable_group_get ||
           kind == BenchmarkKind::store_durable_group_put_get ||
           kind == BenchmarkKind::store_durable_group_read_after_write ||
           kind == BenchmarkKind::store_durable_group_all ||
           kind == BenchmarkKind::store_durable_group_parallel_put ||
           kind == BenchmarkKind::store_durable_parallel_put ||
           kind == BenchmarkKind::store_durable_parallel_get ||
           kind == BenchmarkKind::store_durable_parallel_read_after_write ||
           kind == BenchmarkKind::store_durable_all || kind == BenchmarkKind::store_durable_parallel_all;
}

[[nodiscard]] inline auto distribution_name(const ParallelDistribution distribution) noexcept
    -> std::string_view {
    switch (distribution) {
    case ParallelDistribution::uniform:
        return "uniform";
    case ParallelDistribution::worker_affine:
        return "worker-affine";
    case ParallelDistribution::owner_bound:
        return "owner-bound";
    case ParallelDistribution::single_worker:
        return "single-worker";
    case ParallelDistribution::zipf:
        return "zipf";
    }
    return "unknown";
}

[[nodiscard]] inline auto parse_distribution(const std::string_view value)
    -> std::optional<ParallelDistribution> {
    if (value == "uniform") {
        return ParallelDistribution::uniform;
    }
    if (value == "worker-affine") {
        return ParallelDistribution::worker_affine;
    }
    if (value == "owner-bound") {
        return ParallelDistribution::owner_bound;
    }
    if (value == "single-worker") {
        return ParallelDistribution::single_worker;
    }
    if (value == "zipf") {
        return ParallelDistribution::zipf;
    }
    return std::nullopt;
}

struct Config {
    std::size_t operations{kDefaultOperations};
    std::size_t key_size{16};
    std::size_t value_size{64};
    std::size_t workers{1};
    std::size_t threads{1};
    ParallelDistribution distribution{ParallelDistribution::uniform};
    bool random_access{false};
    bool durable_periodic{false};
    bool durable_group{false};
    /// Durable Stores keep every Segment cold (ADR 0046 images disabled); names gain `_cold`.
    bool cold_reads{false};
};

struct RunSettings {
    std::size_t warmup_iterations{kDefaultWarmupIterations};
    std::size_t measured_iterations{kDefaultMeasuredIterations};
    bool pin_cpu{false};
    bool latency{false};
};

struct ResourceSample {
    std::size_t rss_before_bytes{};
    std::size_t rss_after_bytes{};
    std::size_t peak_rss_bytes{};
    std::size_t physical_footprint_bytes{};
    std::size_t peak_physical_footprint_bytes{};
    std::size_t reusable_bytes{};
    std::size_t internal_bytes{};
    std::size_t compressed_bytes{};
    std::size_t ingress_bytes{};
    std::size_t egress_bytes{};
};

// Native allocator snapshot used only for diagnostic memory censuses. Unsupported
// allocator/platform combinations report available=false rather than fabricating
// a portable value from process RSS.
struct AllocatorMemorySample {
    bool available{};
    std::size_t blocks_in_use{};
    std::size_t bytes_in_use{};
    std::size_t peak_bytes_in_use{};
    std::size_t bytes_reserved{};
};

struct AllocatorPressureReliefSample {
    bool available{};
    bool released{};
    bool exact_released_bytes{};
    std::size_t released_bytes{};
};

struct Result {
    std::string name;
    Config config;
    RunSettings settings{};
    std::size_t operations{};
    std::size_t hits{};
    std::size_t samples{};
    double median_seconds{};
    double min_seconds{};
    double max_seconds{};
    double median_ns_per_op{};
    double min_ns_per_op{};
    double max_ns_per_op{};
    double median_ops_per_second{};
    double min_ops_per_second{};
    double max_ops_per_second{};
    double median_rss_bytes{};
    double min_rss_bytes{};
    double max_rss_bytes{};
    double median_rss_delta_bytes{};
    double peak_rss_bytes{};
    std::size_t ingress_bytes{};
    std::size_t egress_bytes{};
    double median_ingress_bytes_per_second{};
    double min_ingress_bytes_per_second{};
    double max_ingress_bytes_per_second{};
    double median_egress_bytes_per_second{};
    double min_egress_bytes_per_second{};
    double max_egress_bytes_per_second{};
    double median_duplex_bytes_per_second{};
    double min_duplex_bytes_per_second{};
    double max_duplex_bytes_per_second{};
    std::size_t latency_samples{};
    double p50_latency_ns{};
    double p95_latency_ns{};
    double p99_latency_ns{};
    double p999_latency_ns{};
    double median_durable_queue_wait_ns{};
    double maximum_durable_queue_wait_ns{};
    double median_durable_service_ns{};
    double maximum_durable_service_ns{};
    double median_durable_commit_ns{};
    double maximum_durable_commit_ns{};
    double median_durable_batch_records{};
    double maximum_durable_batch_records{};
    std::uint64_t durable_completed{};
    std::uint64_t durable_rejected{};
    std::uint64_t durable_expired{};
    std::uint64_t durable_committed_batches{};
    std::uint64_t durable_committed_records{};
    std::uint64_t durable_committed_bytes{};
    std::uint64_t durable_failed_batches{};
    std::uint64_t durable_maximum_queue_depth{};
    std::uint64_t durable_maximum_queued_bytes{};
    std::uint64_t durable_pending_records{};
    std::uint64_t durable_pending_bytes{};
    std::uint64_t durable_record_limit_closes{};
    std::uint64_t durable_byte_limit_closes{};
    std::uint64_t durable_adaptive_target_closes{};
    std::uint64_t durable_deadline_closes{};
    double median_paired_writer_batch_records{};
    double median_paired_writer_batch_wait_ns{};
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
    double median_reactor_input_buffer_compactions{};
    double maximum_reactor_input_buffer_compactions{};
    double median_reactor_input_buffer_bytes_moved{};
    double maximum_reactor_input_buffer_bytes_moved{};
    double median_reactor_output_buffer_compactions{};
    double maximum_reactor_output_buffer_compactions{};
    double median_reactor_output_buffer_bytes_moved{};
    double maximum_reactor_output_buffer_bytes_moved{};
};

struct KeyMaterial {
    std::vector<std::string> keys;
    std::vector<std::vector<std::byte>> values;
    std::vector<std::size_t> order;
};

[[nodiscard]] auto cpu_pin_applied() noexcept -> bool;
[[nodiscard]] auto try_cpu_pin(bool requested) -> bool;
[[nodiscard]] auto process_memory_snapshot() noexcept -> ResourceSample;
[[nodiscard]] auto allocator_memory_snapshot() noexcept -> AllocatorMemorySample;
[[nodiscard]] auto allocator_pressure_relief() noexcept -> AllocatorPressureReliefSample;

[[nodiscard]] inline auto validate_run_settings(const RunSettings& settings, const Config& config) -> bool {
    if (config.operations == 0) {
        std::cerr << "benchmark error: --ops must be greater than zero\n";
        return false;
    }
    if (config.workers == 0) {
        std::cerr << "benchmark error: --workers must be greater than zero\n";
        return false;
    }
    if (config.threads == 0) {
        std::cerr << "benchmark error: --threads must be greater than zero\n";
        return false;
    }
    if (config.distribution == ParallelDistribution::worker_affine && config.threads > config.workers) {
        std::cerr << "benchmark error: worker-affine requires --threads <= --workers\n";
        return false;
    }
    if (settings.pin_cpu && config.threads > 1) {
        std::cerr << "benchmark error: --pin-cpu cannot be used with multi-thread benchmarks\n";
        return false;
    }
    if (settings.measured_iterations == 0) {
        std::cerr << "benchmark error: --repeats must be greater than zero\n";
        return false;
    }
    return true;
}

[[nodiscard]] inline auto make_key(const std::size_t index, const std::size_t key_size) -> std::string {
    const auto decimal = std::to_string(index);
    if (decimal.size() >= key_size) {
        return decimal.substr(0, key_size);
    }
    std::string key = decimal;
    key.resize(key_size, 'k');
    return key;
}

[[nodiscard]] inline auto make_value(const std::size_t index, const std::size_t value_size)
    -> std::vector<std::byte> {
    std::vector<std::byte> value(value_size);
    for (std::size_t offset = 0; offset < value_size; ++offset) {
        value[offset] = static_cast<std::byte>((index + offset) & 0xFFU);
    }
    return value;
}

[[nodiscard]] inline auto make_key_material(const Config& config) -> KeyMaterial {
    KeyMaterial material;
    material.keys.reserve(config.operations);
    material.values.reserve(config.operations);
    material.order.reserve(config.operations);
    for (std::size_t index = 0; index < config.operations; ++index) {
        material.keys.push_back(make_key(index, config.key_size));
        material.values.push_back(make_value(index, config.value_size));
        material.order.push_back(index);
    }
    if (config.random_access) {
        std::mt19937_64 random{0xC0FFEEULL};
        std::ranges::shuffle(material.order, random);
    }
    return material;
}

[[nodiscard]] inline auto make_miss_key_material(const Config& config) -> KeyMaterial {
    KeyMaterial material;
    material.keys.reserve(config.operations);
    material.order.reserve(config.operations);
    for (std::size_t index = 0; index < config.operations; ++index) {
        material.keys.push_back(make_key(config.operations + index, config.key_size));
        material.order.push_back(index);
    }
    if (config.random_access) {
        std::mt19937_64 random{0xDEADBEEFULL};
        std::ranges::shuffle(material.order, random);
    }
    return material;
}

[[nodiscard]] inline auto median(std::vector<double> values) -> double {
    if (values.empty()) {
        return 0.0;
    }
    std::ranges::sort(values);
    const auto middle = values.size() / 2U;
    if (values.size() % 2U == 1U) {
        return values[middle];
    }
    return (values[middle - 1U] + values[middle]) / 2.0;
}

[[nodiscard]] inline auto finalize_result(std::string name, const Config& config, const RunSettings& settings,
                                          const std::size_t operations, const std::size_t hits,
                                          std::vector<double> seconds,
                                          std::vector<ResourceSample> resources = {}) -> Result {
    if (seconds.empty()) {
        return Result{.name = std::move(name),
                      .config = config,
                      .settings = settings,
                      .operations = operations,
                      .hits = hits};
    }

    std::vector<double> ns_per_op;
    std::vector<double> ops_per_second;
    std::vector<double> rss_bytes;
    std::vector<double> rss_delta_bytes;
    std::vector<double> ingress_bytes_per_second;
    std::vector<double> egress_bytes_per_second;
    std::vector<double> duplex_bytes_per_second;
    ns_per_op.reserve(seconds.size());
    ops_per_second.reserve(seconds.size());
    rss_bytes.reserve(resources.size());
    rss_delta_bytes.reserve(resources.size());
    ingress_bytes_per_second.reserve(resources.size());
    egress_bytes_per_second.reserve(resources.size());
    duplex_bytes_per_second.reserve(resources.size());
    for (std::size_t index = 0; index < seconds.size(); ++index) {
        const auto elapsed = seconds[index];
        ns_per_op.push_back(operations > 0 ? elapsed * 1.0e9 / static_cast<double>(operations) : 0.0);
        ops_per_second.push_back(elapsed > 0.0 ? static_cast<double>(operations) / elapsed : 0.0);
        if (index < resources.size()) {
            const auto& resource = resources[index];
            rss_bytes.push_back(static_cast<double>(resource.rss_after_bytes));
            const auto delta = resource.rss_after_bytes > resource.rss_before_bytes
                                   ? resource.rss_after_bytes - resource.rss_before_bytes
                                   : 0;
            rss_delta_bytes.push_back(static_cast<double>(delta));
            ingress_bytes_per_second.push_back(
                elapsed > 0.0 ? static_cast<double>(resource.ingress_bytes) / elapsed : 0.0);
            egress_bytes_per_second.push_back(
                elapsed > 0.0 ? static_cast<double>(resource.egress_bytes) / elapsed : 0.0);
            duplex_bytes_per_second.push_back(
                elapsed > 0.0 ? static_cast<double>(resource.ingress_bytes + resource.egress_bytes) / elapsed
                              : 0.0);
        }
    }

    const auto first_resource = resources.empty() ? ResourceSample{} : resources.front();
    const auto peak_rss =
        resources.empty()
            ? 0.0
            : static_cast<double>(
                  std::ranges::max_element(resources, {}, &ResourceSample::peak_rss_bytes)->peak_rss_bytes);

    return Result{
        .name = std::move(name),
        .config = config,
        .settings = settings,
        .operations = operations,
        .hits = hits,
        .samples = seconds.size(),
        .median_seconds = median(seconds),
        .min_seconds = *std::ranges::min_element(seconds),
        .max_seconds = *std::ranges::max_element(seconds),
        .median_ns_per_op = median(ns_per_op),
        .min_ns_per_op = *std::ranges::min_element(ns_per_op),
        .max_ns_per_op = *std::ranges::max_element(ns_per_op),
        .median_ops_per_second = median(ops_per_second),
        .min_ops_per_second = *std::ranges::min_element(ops_per_second),
        .max_ops_per_second = *std::ranges::max_element(ops_per_second),
        .median_rss_bytes = median(rss_bytes),
        .min_rss_bytes = rss_bytes.empty() ? 0.0 : *std::ranges::min_element(rss_bytes),
        .max_rss_bytes = rss_bytes.empty() ? 0.0 : *std::ranges::max_element(rss_bytes),
        .median_rss_delta_bytes = median(rss_delta_bytes),
        .peak_rss_bytes = peak_rss,
        .ingress_bytes = first_resource.ingress_bytes,
        .egress_bytes = first_resource.egress_bytes,
        .median_ingress_bytes_per_second = median(ingress_bytes_per_second),
        .min_ingress_bytes_per_second =
            ingress_bytes_per_second.empty() ? 0.0 : *std::ranges::min_element(ingress_bytes_per_second),
        .max_ingress_bytes_per_second =
            ingress_bytes_per_second.empty() ? 0.0 : *std::ranges::max_element(ingress_bytes_per_second),
        .median_egress_bytes_per_second = median(egress_bytes_per_second),
        .min_egress_bytes_per_second =
            egress_bytes_per_second.empty() ? 0.0 : *std::ranges::min_element(egress_bytes_per_second),
        .max_egress_bytes_per_second =
            egress_bytes_per_second.empty() ? 0.0 : *std::ranges::max_element(egress_bytes_per_second),
        .median_duplex_bytes_per_second = median(duplex_bytes_per_second),
        .min_duplex_bytes_per_second =
            duplex_bytes_per_second.empty() ? 0.0 : *std::ranges::min_element(duplex_bytes_per_second),
        .max_duplex_bytes_per_second =
            duplex_bytes_per_second.empty() ? 0.0 : *std::ranges::max_element(duplex_bytes_per_second),
    };
}

[[nodiscard]] inline auto validate_sample_hits(const std::string_view name, const std::size_t expected_hits,
                                               const std::size_t actual_hits, const std::size_t sample_index)
    -> bool {
    if (actual_hits == expected_hits) {
        return true;
    }
    std::cerr << "benchmark error: " << name << " sample " << sample_index
              << " expected hits=" << expected_hits << " got hits=" << actual_hits << '\n';
    return false;
}

template <typename SetupFn, typename BodyFn>
[[nodiscard]] inline auto benchmark_collect_timed(const RunSettings& settings, const std::size_t operations,
                                                  const Config& config, std::string name,
                                                  const std::size_t expected_hits, SetupFn&& setup,
                                                  BodyFn&& body) -> Result {
    const auto run_iteration = [&](const bool timed, const std::size_t sample_index)
        -> std::optional<std::tuple<std::size_t, double, ResourceSample>> {
        auto context = setup();
        if (!timed) {
            (void)body(context);
            return std::nullopt;
        }
        auto resources = process_memory_snapshot();
        const auto started = std::chrono::steady_clock::now();
        const auto hits = body(context);
        const auto elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const auto after = process_memory_snapshot();
        resources.rss_after_bytes = after.rss_after_bytes;
        resources.peak_rss_bytes = after.peak_rss_bytes;
        if (!validate_sample_hits(name, expected_hits, hits, sample_index)) {
            return std::nullopt;
        }
        return std::tuple{hits, elapsed, resources};
    };

    for (std::size_t iteration = 0; iteration < settings.warmup_iterations; ++iteration) {
        (void)run_iteration(false, iteration);
    }

    std::vector<double> samples;
    std::vector<ResourceSample> resources;
    samples.reserve(settings.measured_iterations);
    resources.reserve(settings.measured_iterations);
    std::size_t hits = 0;
    for (std::size_t iteration = 0; iteration < settings.measured_iterations; ++iteration) {
        const auto measured = run_iteration(true, iteration + 1U);
        if (!measured) {
            return Result{
                .name = std::move(name), .config = config, .settings = settings, .operations = operations};
        }
        hits = std::get<0>(*measured);
        samples.push_back(std::get<1>(*measured));
        resources.push_back(std::get<2>(*measured));
    }

    return finalize_result(std::move(name), config, settings, operations, hits, std::move(samples),
                           std::move(resources));
}

inline void print_metadata(std::ostream& out, const RunSettings& settings) {
    print_common_metadata(out, settings.warmup_iterations, settings.measured_iterations);
    out << "# cpu_pin_requested=" << (settings.pin_cpu ? 1 : 0) << '\n';
    out << "# cpu_pin_applied=" << (cpu_pin_applied() ? 1 : 0) << '\n';
}

inline void print_result(std::ostream& out, const Result& result) {
    out << "name=" << result.name << ' ' << "key_size=" << result.config.key_size << ' '
        << "value_size=" << result.config.value_size << ' ' << "workers=" << result.config.workers << ' '
        << "threads=" << result.config.threads << ' '
        << "distribution=" << distribution_name(result.config.distribution) << ' '
        << "random=" << (result.config.random_access ? 1 : 0) << ' ' << "operations=" << result.operations
        << ' ' << "hits=" << result.hits << ' ' << "samples=" << result.samples << ' '
        << "warmup=" << result.settings.warmup_iterations << ' ' << "median_seconds=" << result.median_seconds
        << ' ' << "min_seconds=" << result.min_seconds << ' ' << "max_seconds=" << result.max_seconds << ' '
        << "median_ops_per_second=" << result.median_ops_per_second << ' '
        << "min_ops_per_second=" << result.min_ops_per_second << ' '
        << "max_ops_per_second=" << result.max_ops_per_second << ' '
        << "median_ns_per_op=" << result.median_ns_per_op << ' ' << "min_ns_per_op=" << result.min_ns_per_op
        << ' ' << "max_ns_per_op=" << result.max_ns_per_op << ' '
        << "median_rss_bytes=" << result.median_rss_bytes << ' ' << "min_rss_bytes=" << result.min_rss_bytes
        << ' ' << "max_rss_bytes=" << result.max_rss_bytes << ' '
        << "median_rss_delta_bytes=" << result.median_rss_delta_bytes << ' '
        << "peak_rss_bytes=" << result.peak_rss_bytes << ' ' << "ingress_bytes=" << result.ingress_bytes
        << ' ' << "egress_bytes=" << result.egress_bytes << ' '
        << "median_ingress_bytes_per_second=" << result.median_ingress_bytes_per_second << ' '
        << "min_ingress_bytes_per_second=" << result.min_ingress_bytes_per_second << ' '
        << "max_ingress_bytes_per_second=" << result.max_ingress_bytes_per_second << ' '
        << "median_egress_bytes_per_second=" << result.median_egress_bytes_per_second << ' '
        << "min_egress_bytes_per_second=" << result.min_egress_bytes_per_second << ' '
        << "max_egress_bytes_per_second=" << result.max_egress_bytes_per_second << ' '
        << "median_duplex_bytes_per_second=" << result.median_duplex_bytes_per_second << ' '
        << "min_duplex_bytes_per_second=" << result.min_duplex_bytes_per_second << ' '
        << "max_duplex_bytes_per_second=" << result.max_duplex_bytes_per_second << ' '
        << "latency_samples=" << result.latency_samples << ' ' << "p50_latency_ns=" << result.p50_latency_ns
        << ' ' << "p95_latency_ns=" << result.p95_latency_ns << ' '
        << "p99_latency_ns=" << result.p99_latency_ns << ' ' << "p999_latency_ns=" << result.p999_latency_ns
        << ' ' << "median_durable_queue_wait_ns=" << result.median_durable_queue_wait_ns << ' '
        << "maximum_durable_queue_wait_ns=" << result.maximum_durable_queue_wait_ns << ' '
        << "median_durable_service_ns=" << result.median_durable_service_ns << ' '
        << "maximum_durable_service_ns=" << result.maximum_durable_service_ns << ' '
        << "median_durable_commit_ns=" << result.median_durable_commit_ns << ' '
        << "maximum_durable_commit_ns=" << result.maximum_durable_commit_ns << ' '
        << "median_durable_batch_records=" << result.median_durable_batch_records << ' '
        << "maximum_durable_batch_records=" << result.maximum_durable_batch_records << ' '
        << "durable_completed=" << result.durable_completed << ' '
        << "durable_rejected=" << result.durable_rejected << ' '
        << "durable_expired=" << result.durable_expired << ' '
        << "durable_committed_batches=" << result.durable_committed_batches << ' '
        << "durable_committed_records=" << result.durable_committed_records << ' '
        << "durable_committed_bytes=" << result.durable_committed_bytes << ' '
        << "durable_failed_batches=" << result.durable_failed_batches << ' '
        << "durable_maximum_queue_depth=" << result.durable_maximum_queue_depth << ' '
        << "durable_maximum_queued_bytes=" << result.durable_maximum_queued_bytes << ' '
        << "durable_pending_records=" << result.durable_pending_records << ' '
        << "durable_pending_bytes=" << result.durable_pending_bytes << ' '
        << "durable_record_limit_closes=" << result.durable_record_limit_closes << ' '
        << "durable_byte_limit_closes=" << result.durable_byte_limit_closes << ' '
        << "durable_adaptive_target_closes=" << result.durable_adaptive_target_closes << ' '
        << "durable_deadline_closes=" << result.durable_deadline_closes << ' '
        << "median_paired_writer_batch_records=" << result.median_paired_writer_batch_records << ' '
        << "median_paired_writer_batch_wait_ns=" << result.median_paired_writer_batch_wait_ns << ' '
        << "maximum_paired_writer_batch_wait_ns=" << result.maximum_paired_writer_batch_wait_ns << ' '
        << "paired_writer_batches=" << result.paired_writer_batches << ' '
        << "paired_writer_batch_records=" << result.paired_writer_batch_records << ' '
        << "paired_writer_durability_deadline_closes=" << result.paired_writer_durability_deadline_closes
        << ' ' << "paired_writer_queue_deadline_closes=" << result.paired_writer_queue_deadline_closes << ' '
        << "paired_sync_turn_splits=" << result.paired_sync_turn_splits << ' '
        << "paired_sync_async_fairness_turns=" << result.paired_sync_async_fairness_turns << ' '
        << "paired_publications=" << result.paired_publications << ' '
        << "paired_publication_records=" << result.paired_publication_records << ' '
        << "paired_completion_notifications=" << result.paired_completion_notifications << ' '
        << "median_reactor_input_buffer_compactions=" << result.median_reactor_input_buffer_compactions << ' '
        << "maximum_reactor_input_buffer_compactions=" << result.maximum_reactor_input_buffer_compactions
        << ' ' << "median_reactor_input_buffer_bytes_moved=" << result.median_reactor_input_buffer_bytes_moved
        << ' '
        << "maximum_reactor_input_buffer_bytes_moved=" << result.maximum_reactor_input_buffer_bytes_moved
        << ' '
        << "median_reactor_output_buffer_compactions=" << result.median_reactor_output_buffer_compactions
        << ' '
        << "maximum_reactor_output_buffer_compactions=" << result.maximum_reactor_output_buffer_compactions
        << ' '
        << "median_reactor_output_buffer_bytes_moved=" << result.median_reactor_output_buffer_bytes_moved
        << ' '
        << "maximum_reactor_output_buffer_bytes_moved=" << result.maximum_reactor_output_buffer_bytes_moved
        << '\n';
}

[[nodiscard]] inline auto parse_kind(std::string_view value) -> BenchmarkKind {
    if (value == "index-insert") {
        return BenchmarkKind::index_insert;
    }
    if (value == "index-replace") {
        return BenchmarkKind::index_replace;
    }
    if (value == "index-find-hit") {
        return BenchmarkKind::index_find_hit;
    }
    if (value == "index-find-miss") {
        return BenchmarkKind::index_find_miss;
    }
    if (value == "index-churn-miss") {
        return BenchmarkKind::index_churn_miss;
    }
    if (value == "index-erase") {
        return BenchmarkKind::index_erase;
    }
    if (value == "index-all") {
        return BenchmarkKind::index_all;
    }
    if (value == "index") {
        return BenchmarkKind::index_insert_find;
    }
    if (value == "store-put") {
        return BenchmarkKind::store_put;
    }
    if (value == "store-put-batch") {
        return BenchmarkKind::store_put_batch;
    }
    if (value == "store-get") {
        return BenchmarkKind::store_get;
    }
    if (value == "store-put-get") {
        return BenchmarkKind::store_put_get;
    }
    if (value == "store-read-after-write") {
        return BenchmarkKind::store_read_after_write;
    }
    if (value == "store-parallel-put") {
        return BenchmarkKind::store_parallel_put;
    }
    if (value == "store-parallel-get") {
        return BenchmarkKind::store_parallel_get;
    }
    if (value == "store-parallel-read-after-write") {
        return BenchmarkKind::store_parallel_read_after_write;
    }
    if (value == "store-parallel-all") {
        return BenchmarkKind::store_parallel_all;
    }
    if (value == "store-durable-put") {
        return BenchmarkKind::store_durable_put;
    }
    if (value == "store-durable-get") {
        return BenchmarkKind::store_durable_get;
    }
    if (value == "store-durable-put-get") {
        return BenchmarkKind::store_durable_put_get;
    }
    if (value == "store-durable-read-after-write") {
        return BenchmarkKind::store_durable_read_after_write;
    }
    if (value == "store-durable-recovery-open") {
        return BenchmarkKind::store_durable_recovery_open;
    }
    if (value == "store-durable-periodic-put") {
        return BenchmarkKind::store_durable_periodic_put;
    }
    if (value == "store-durable-periodic-get") {
        return BenchmarkKind::store_durable_periodic_get;
    }
    if (value == "store-durable-periodic-put-get") {
        return BenchmarkKind::store_durable_periodic_put_get;
    }
    if (value == "store-durable-periodic-read-after-write") {
        return BenchmarkKind::store_durable_periodic_read_after_write;
    }
    if (value == "store-durable-periodic-all") {
        return BenchmarkKind::store_durable_periodic_all;
    }
    if (value == "store-durable-group-put") {
        return BenchmarkKind::store_durable_group_put;
    }
    if (value == "store-durable-group-get") {
        return BenchmarkKind::store_durable_group_get;
    }
    if (value == "store-durable-group-put-get") {
        return BenchmarkKind::store_durable_group_put_get;
    }
    if (value == "store-durable-group-read-after-write") {
        return BenchmarkKind::store_durable_group_read_after_write;
    }
    if (value == "store-durable-group-all") {
        return BenchmarkKind::store_durable_group_all;
    }
    if (value == "store-durable-group-parallel-put") {
        return BenchmarkKind::store_durable_group_parallel_put;
    }
    if (value == "store-durable-parallel-put") {
        return BenchmarkKind::store_durable_parallel_put;
    }
    if (value == "store-durable-parallel-get") {
        return BenchmarkKind::store_durable_parallel_get;
    }
    if (value == "store-durable-parallel-read-after-write") {
        return BenchmarkKind::store_durable_parallel_read_after_write;
    }
    if (value == "store-durable-all") {
        return BenchmarkKind::store_durable_all;
    }
    if (value == "store-durable-parallel-all") {
        return BenchmarkKind::store_durable_parallel_all;
    }
    return BenchmarkKind::all;
}

} // namespace glifistore::bench
