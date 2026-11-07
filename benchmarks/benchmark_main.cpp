#include "harness.hpp"
#include "parse.hpp"
#include "suite.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Options {
    glifistore::bench::BenchmarkKind kind{glifistore::bench::BenchmarkKind::all};
    glifistore::bench::RunSettings settings{};
    bool full_suite{};
    std::optional<std::size_t> operations;
    std::optional<std::size_t> key_size;
    std::optional<std::size_t> value_size;
    std::optional<std::size_t> workers;
    std::optional<std::size_t> threads;
    std::optional<glifistore::bench::ParallelDistribution> distribution;
    std::optional<bool> random_access;
};

[[nodiscard]] auto parse_size(const char* value, const char* flag) -> std::size_t {
    if (value == nullptr) {
        std::cerr << "missing value for " << flag << '\n';
        std::exit(2);
    }
    const std::string_view text{value};
    const auto parsed = glifistore::bench::parse_decimal_size(text);
    if (!parsed) {
        throw std::invalid_argument{"invalid value for " + std::string{flag} + ": " + std::string{text}};
    }
    return *parsed;
}

[[nodiscard]] auto parse_options(int argc, char** argv) -> Options {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg{argv[index]};
        if (arg == "--suite") {
            options.full_suite = true;
            continue;
        }
        if (arg == "--random") {
            options.random_access = true;
            continue;
        }
        if (arg == "--pin-cpu") {
            options.settings.pin_cpu = true;
            continue;
        }
        if (arg == "--latency") {
            options.settings.latency = true;
            continue;
        }
        if (arg == "--filter" && index + 1 < argc) {
            options.kind = glifistore::bench::parse_kind(argv[++index]);
            continue;
        }
        if (arg == "--ops" && index + 1 < argc) {
            options.operations = parse_size(argv[++index], "--ops");
            continue;
        }
        if (arg == "--key-size" && index + 1 < argc) {
            options.key_size = parse_size(argv[++index], "--key-size");
            continue;
        }
        if (arg == "--value-size" && index + 1 < argc) {
            options.value_size = parse_size(argv[++index], "--value-size");
            continue;
        }
        if (arg == "--workers" && index + 1 < argc) {
            options.workers = parse_size(argv[++index], "--workers");
            continue;
        }
        if (arg == "--threads" && index + 1 < argc) {
            options.threads = parse_size(argv[++index], "--threads");
            continue;
        }
        if (arg == "--distribution" && index + 1 < argc) {
            const auto distribution = glifistore::bench::parse_distribution(argv[++index]);
            if (!distribution) {
                std::cerr << "unknown distribution: " << argv[index] << '\n';
                std::exit(2);
            }
            options.distribution = distribution;
            continue;
        }
        if (arg == "--warmup" && index + 1 < argc) {
            options.settings.warmup_iterations = parse_size(argv[++index], "--warmup");
            continue;
        }
        if (arg == "--repeats" && index + 1 < argc) {
            options.settings.measured_iterations = parse_size(argv[++index], "--repeats");
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            std::cout
                << "usage: glifistore_benchmarks [--suite] [--random] [--pin-cpu] [--latency]\n"
                << "       [--filter all|index-all|index|index-insert|index-replace|index-find-hit|\n"
                << "                "
                   "index-find-miss|index-churn-miss|index-erase|store-put|store-put-batch|store-get|\n"
                << "                store-put-get|\n"
                << "                store-read-after-write|store-parallel-put|store-parallel-get|\n"
                << "                store-parallel-read-after-write|store-parallel-all|\n"
                << "                store-durable-put|store-durable-get|store-durable-put-get|\n"
                << "                store-durable-read-after-write|store-durable-recovery-open|\n"
                << "                store-durable-periodic-put|store-durable-periodic-get|\n"
                << "                store-durable-periodic-put-get|\n"
                << "                store-durable-periodic-read-after-write|store-durable-periodic-all|\n"
                << "                store-durable-group-put|store-durable-group-get|\n"
                << "                store-durable-group-put-get|\n"
                << "                store-durable-group-read-after-write|store-durable-group-all|\n"
                << "                store-durable-group-parallel-put|\n"
                << "                store-durable-parallel-put|store-durable-parallel-get|\n"
                << "                store-durable-parallel-read-after-write|store-durable-all|\n"
                << "                store-durable-parallel-all]\n"
                << "       [--ops N] [--key-size N] [--value-size N] [--workers N] [--threads N]\n"
                << "       [--distribution uniform|worker-affine|owner-bound|single-worker|zipf]\n"
                << "       [--warmup N] [--repeats N]\n";
            std::exit(0);
        }
        std::cerr << "unknown argument: " << arg << '\n';
        std::exit(2);
    }
    return options;
}

void apply_overrides(glifistore::bench::Config& config, const Options& options) {
    if (options.operations) {
        config.operations = *options.operations;
    }
    if (options.key_size) {
        config.key_size = *options.key_size;
    }
    if (options.value_size) {
        config.value_size = *options.value_size;
    }
    if (options.workers) {
        config.workers = *options.workers;
    }
    if (options.threads) {
        config.threads = *options.threads;
    }
    if (options.distribution) {
        config.distribution = *options.distribution;
    }
    if (options.random_access) {
        config.random_access = *options.random_access;
    }
}

[[nodiscard]] auto has_custom_config(const Options& options) -> bool {
    return options.operations || options.key_size || options.value_size || options.workers ||
           options.threads || options.distribution || options.random_access;
}

[[nodiscard]] auto result_is_valid(const glifistore::bench::Result& result) -> bool {
    return result.samples == result.settings.measured_iterations;
}

[[nodiscard]] auto supports_latency(const glifistore::bench::BenchmarkKind kind) noexcept -> bool {
    return kind == glifistore::bench::BenchmarkKind::store_parallel_put ||
           kind == glifistore::bench::BenchmarkKind::store_durable_group_parallel_put ||
           kind == glifistore::bench::BenchmarkKind::store_durable_parallel_put ||
           kind == glifistore::bench::BenchmarkKind::store_durable_parallel_get;
}

} // namespace

int main(int argc, char** argv) try {
    const auto options = parse_options(argc, argv);
    if ((options.threads || options.distribution) &&
        !glifistore::bench::is_parallel_benchmark(options.kind)) {
        std::cerr << "benchmark error: --threads and --distribution require a store-parallel filter\n";
        return 2;
    }
    if (options.settings.latency && !supports_latency(options.kind)) {
        std::cerr << "benchmark error: --latency requires store-parallel-put, "
                     "store-durable-group-parallel-put, "
                     "store-durable-parallel-put, or store-durable-parallel-get\n";
        return 2;
    }
    std::vector<glifistore::bench::Config> configs;
    if (has_custom_config(options)) {
        glifistore::bench::Config config;
        apply_overrides(config, options);
        configs.push_back(config);
    } else if (options.full_suite) {
        configs = glifistore::bench::suite_configs();
    } else {
        configs = glifistore::bench::quick_configs();
    }

    for (const auto& config : configs) {
        if (!glifistore::bench::validate_run_settings(options.settings, config)) {
            return 1;
        }
    }

    std::cout << "# glifistore benchmark\n";
    (void)glifistore::bench::try_cpu_pin(options.settings.pin_cpu);
    glifistore::bench::print_metadata(std::cout, options.settings);
    std::cout << "# latency_measurement="
              << (options.settings.latency ? "per-operation-steady-clock" : "disabled") << '\n';

    for (auto config : configs) {
        apply_overrides(config, options);
        for (const auto& result : glifistore::bench::run_benchmark(options.kind, config, options.settings)) {
            if (!result_is_valid(result)) {
                std::cerr << "benchmark error: " << result.name << " produced invalid samples\n";
                return 1;
            }
            glifistore::bench::print_result(std::cout, result);
        }
    }
    return 0;
} catch (const std::exception& exception) {

    std::cerr << "benchmark error: " << exception.what() << '\n';
    return 1;
} catch (...) {
    std::cerr << "benchmark error: unknown exception\n";
    return 1;
}
