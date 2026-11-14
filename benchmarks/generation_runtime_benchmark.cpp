#include "benchmark_metadata.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/store/store.hpp"
#include "parse.hpp"

#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

struct Options final {
    std::size_t operations{20'000};
    std::size_t warmup{1};
    std::size_t repeats{7};
    std::size_t threads{1};
    bool slot_pool{};
    bool compare{};
    std::string workload{"put"};
};

[[nodiscard]] auto parse_options(const int argc, char** argv) -> Options {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--help") {
            std::cout << "usage: glifistore_generation_runtime_benchmark "
                         "[--ops N] [--warmup N] [--repeats N] [--threads N] "
                         "[--generation-slot-pool false|true|both] [--workload put|get|put-get]\n";
            std::exit(0);
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument{"missing argument value"};
        }
        const std::string_view value{argv[++index]};
        if (argument == "--generation-slot-pool") {
            if (value != "false" && value != "true" && value != "both") {
                throw std::invalid_argument{"generation-slot-pool must be false, true or both"};
            }
            options.slot_pool = value == "true";
            options.compare = value == "both";
        } else if (argument == "--workload") {
            if (value != "put" && value != "get" && value != "put-get") {
                throw std::invalid_argument{"workload must be put, get or put-get"};
            }
            options.workload = value;
        } else {
            const auto parsed = glifistore::bench::parse_decimal_size(value);
            if (!parsed || *parsed == 0 || *parsed > 10'000'000) {
                throw std::invalid_argument{"invalid bounded positive integer"};
            }
            if (argument == "--ops") {
                options.operations = *parsed;
            } else if (argument == "--warmup") {
                options.warmup = *parsed;
            } else if (argument == "--repeats") {
                options.repeats = *parsed;
            } else if (argument == "--threads" && *parsed <= 64) {
                options.threads = *parsed;
            } else {
                throw std::invalid_argument{"unknown option or thread count outside 1..64"};
            }
        }
    }
    return options;
}

struct alignas(128) ThreadResult final {
    std::uint64_t checksum{};
    bool failed{};
};

struct Measurement final {
    double seconds{};
    std::uint64_t checksum{};
};

[[nodiscard]] auto run_once(const Options& options) -> Measurement {
    constexpr std::size_t kKeysPerWorker = 64;
    const std::string payload(64, 'g');
    const auto value = std::as_bytes(std::span{payload.data(), payload.size()});
    glifistore::StoreConfig config;
    config.worker_config.explicit_count = options.threads;
    config.concurrency = glifistore::StoreConcurrencyMode::paired;
    config.paired.generation_slot_pool = options.slot_pool;
    auto opened = glifistore::Store::open(config);
    if (!opened) {
        throw std::runtime_error{"cannot open paired production Store"};
    }
    auto store = std::move(*opened);
    std::vector<std::vector<std::string>> keys(options.threads);
    for (std::size_t worker = 0; worker < options.threads; ++worker) {
        for (std::size_t candidate = 0; keys[worker].size() < kKeysPerWorker; ++candidate) {
            auto key = "generation-runtime-" + std::to_string(candidate);
            if (glifistore::route_worker(key, options.threads) == worker) {
                if (!store->put(key, value)) {
                    throw std::runtime_error{"cannot seed production Store"};
                }
                keys[worker].push_back(std::move(key));
            }
        }
    }
    std::vector<ThreadResult> results(options.threads);
    std::vector<std::thread> callers;
    callers.reserve(options.threads);
    Clock::time_point started{};
    std::barrier start{static_cast<std::ptrdiff_t>(options.threads + 1U),
                       [&]() noexcept { started = Clock::now(); }};
    for (std::size_t thread = 0; thread < options.threads; ++thread) {
        callers.emplace_back([&, thread] {
            start.arrive_and_wait();
            for (std::size_t operation = 0; operation < options.operations; ++operation) {
                const auto& key = keys[thread][operation % kKeysPerWorker];
                if (options.workload != "get" && !store->put(key, value)) {
                    results[thread].failed = true;
                    return;
                }
                if (options.workload != "put") {
                    auto read = store->get(key);
                    if (!read || read->bytes.size() != value.size() || read->bytes.front() != value.front()) {
                        results[thread].failed = true;
                        return;
                    }
                }
                ++results[thread].checksum;
            }
        });
    }
    start.arrive_and_wait();
    for (auto& caller : callers) {
        caller.join();
    }
    Measurement measurement{.seconds = std::chrono::duration<double>(Clock::now() - started).count()};
    for (const auto& result : results) {
        if (result.failed) {
            throw std::runtime_error{"production operation or read-after-write validation failed"};
        }
        measurement.checksum += result.checksum;
    }
    for (const auto& worker_keys : keys) {
        for (const auto& key : worker_keys) {
            auto read = store->get(key);
            if (!read || read->bytes.size() != value.size() || read->bytes.front() != value.front()) {
                throw std::runtime_error{"final production Store validation failed"};
            }
        }
    }
    if (!store->close()) {
        throw std::runtime_error{"production Store close failed"};
    }
    return measurement;
}
} // namespace

int main(const int argc, char** argv) try {
    const auto options = parse_options(argc, argv);
    for (std::size_t warmup = 0; warmup < options.warmup; ++warmup) {
        for (std::size_t mode = 0; mode < (options.compare ? 2U : 1U); ++mode) {
            auto trial = options;
            trial.slot_pool = options.compare ? mode != 0 : options.slot_pool;
            static_cast<void>(run_once(trial));
        }
    }
    glifistore::bench::print_common_metadata(std::cout, options.warmup, options.repeats);
    std::cout
        << "# runtime=official paired embedded Store; worker-affine callers; no CPU pinning\n"
        << "# storage=volatile; keys_per_worker=64; value_bytes=64; one operation is one workload cycle\n"
        << "# comparison_order=" << (options.compare ? "alternating A/B and B/A" : "single mode") << '\n'
        << "workload\tgeneration_slot_pool\tthreads\trepeat\toperations\tseconds\toperations_per_second\tns_"
           "per_operation\tchecksum\n";
    for (std::size_t repeat = 0; repeat < options.repeats; ++repeat) {
        for (std::size_t mode = 0; mode < (options.compare ? 2U : 1U); ++mode) {
            auto trial = options;
            trial.slot_pool = options.compare ? ((repeat + mode) & 1U) != 0U : options.slot_pool;
            const auto measurement = run_once(trial);
            const auto operations = options.operations * options.threads;
            std::cout << options.workload << '\t' << std::boolalpha << trial.slot_pool << '\t'
                      << options.threads << '\t' << repeat << '\t' << operations << '\t' << std::fixed
                      << std::setprecision(9) << measurement.seconds << '\t' << std::setprecision(2)
                      << static_cast<double>(operations) / measurement.seconds << '\t'
                      << measurement.seconds * 1e9 / static_cast<double>(operations) << '\t'
                      << measurement.checksum << '\n';
        }
    }
    return 0;
} catch (const std::exception& error) {

    std::cerr << "generation runtime benchmark error: " << error.what() << '\n';
    return 1;
}
