#include "glifistore/client/client.hpp"
#include "parse.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using PipelineRequest = glifistore::client::PipelineRequest;

struct Options {
    std::string host{"127.0.0.1"};
    std::uint16_t port{};
    std::size_t workers{4};
    std::size_t operations{200'000};
    std::size_t pipeline{64};
    std::size_t warmup{1};
    std::size_t repeats{7};
    std::string execution{"concurrent"};
};

struct Workload {
    std::vector<std::vector<std::string>> keys;
    std::vector<std::vector<std::string>> values;
    std::vector<std::vector<std::vector<PipelineRequest>>> batches;
};

[[nodiscard]] auto as_bytes(const std::string_view value) noexcept -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

[[noreturn]] void fail(const std::string_view message) {
    std::cerr << "benchmark error: " << message << '\n';
    std::exit(1);
}

[[nodiscard]] auto parse_size(const std::string_view text, const std::string_view option) -> std::size_t {
    const auto value = glifistore::bench::parse_decimal_size(text);
    if (!value || *value == 0) {
        fail(std::string{option} + " must be a positive integer");
    }
    return *value;
}

[[nodiscard]] auto parse_options(const int argc, char** argv) -> Options {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option{argv[index]};
        if (option == "--help") {
            std::cout << "usage: glifistore_client_benchmark --port N [--host HOST] [--workers N] "
                         "[--ops N] [--pipeline N] [--warmup N] [--repeats N] "
                         "[--execution concurrent|sequential|batch]\n";
            std::exit(0);
        }
        if (index + 1 >= argc) {
            fail(std::string{option} + " requires a value");
        }
        const std::string_view value{argv[++index]};
        if (option == "--host") {
            options.host = value;
        } else if (option == "--port") {
            const auto parsed = parse_size(value, option);
            if (parsed > std::numeric_limits<std::uint16_t>::max()) {
                fail("--port is outside the TCP port range");
            }
            options.port = static_cast<std::uint16_t>(parsed);
        } else if (option == "--workers") {
            options.workers = parse_size(value, option);
        } else if (option == "--ops") {
            options.operations = parse_size(value, option);
        } else if (option == "--pipeline") {
            options.pipeline = parse_size(value, option);
        } else if (option == "--warmup") {
            options.warmup = value == "0" ? 0 : parse_size(value, option);
        } else if (option == "--repeats") {
            options.repeats = parse_size(value, option);
        } else if (option == "--execution") {
            options.execution = value;
        } else {
            fail(std::string{"unknown option: "} + std::string{option});
        }
    }
    if (options.port == 0) {
        fail("--port is required");
    }
    if (options.execution != "concurrent" && options.execution != "sequential" &&
        options.execution != "batch") {
        fail("--execution must be concurrent, sequential, or batch");
    }
    if (options.pipeline > std::numeric_limits<std::size_t>::max() / 2U) {
        fail("--pipeline is too large");
    }
    if (options.operations > std::numeric_limits<std::size_t>::max() / 2U) {
        fail("--ops is too large");
    }
    if (options.workers >= static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max())) {
        fail("--workers is too large");
    }
    return options;
}

[[nodiscard]] auto make_workload(glifistore::client::Client& client, const Options& options) -> Workload {
    Workload workload;
    workload.keys.resize(options.workers);
    workload.values.resize(options.workers);
    std::vector<std::size_t> quotas(options.workers, options.operations / options.workers);
    for (std::size_t worker = 0; worker < options.operations % options.workers; ++worker) {
        ++quotas[worker];
    }
    for (std::size_t worker = 0; worker < options.workers; ++worker) {
        workload.keys[worker].reserve(quotas[worker]);
        workload.values[worker].reserve(quotas[worker]);
    }

    std::size_t remaining = options.operations;
    for (std::size_t candidate = 0; remaining != 0; ++candidate) {
        auto key = std::string{"cpp-bench-"} + std::to_string(candidate);
        const auto worker = client.worker_for(key);
        if (worker >= quotas.size() || quotas[worker] == 0) {
            continue;
        }
        workload.keys[worker].push_back(std::move(key));
        workload.values[worker].emplace_back(64, static_cast<char>(candidate & 0xFFU));
        --quotas[worker];
        --remaining;
    }

    workload.batches.resize(options.workers);
    const auto pairs_per_batch = options.pipeline;
    for (std::size_t worker = 0; worker < options.workers; ++worker) {
        auto& worker_batches = workload.batches[worker];
        const auto pair_count = workload.keys[worker].size();
        worker_batches.reserve(pair_count / pairs_per_batch + (pair_count % pairs_per_batch == 0 ? 0U : 1U));
        for (std::size_t begin = 0; begin < pair_count; begin += pairs_per_batch) {
            const auto end = std::min(pair_count, begin + pairs_per_batch);
            auto& batch = worker_batches.emplace_back();
            batch.reserve((end - begin) * 2U);
            for (std::size_t index = begin; index < end; ++index) {
                batch.push_back({.opcode = glifistore::client::PipelineOpcode::put,
                                 .key = as_bytes(workload.keys[worker][index]),
                                 .value = as_bytes(workload.values[worker][index])});
                batch.push_back({.opcode = glifistore::client::PipelineOpcode::get,
                                 .key = as_bytes(workload.keys[worker][index])});
            }
        }
    }
    return workload;
}

[[nodiscard]] auto validate(const std::span<const PipelineRequest> requests,
                            const std::vector<glifistore::client::PipelineResponse>& responses) -> bool {
    if (responses.size() != requests.size()) {
        return false;
    }
    for (std::size_t index = 0; index < requests.size(); ++index) {
        if (!responses[index].succeeded()) {
            return false;
        }
        if (requests[index].opcode == glifistore::client::PipelineOpcode::get &&
            !std::ranges::equal(responses[index].value, requests[index - 1U].value)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto execute_pipeline(glifistore::client::Client& client,
                                    const std::vector<PipelineRequest>& requests) -> bool {
    auto responses = client.execute_pipeline(requests);
    return responses && validate(requests, *responses);
}

[[nodiscard]] auto run_sequential(glifistore::client::Client& client, const Workload& workload) -> double {
    const auto started = Clock::now();
    for (const auto& worker_batches : workload.batches) {
        for (const auto& batch : worker_batches) {
            if (!execute_pipeline(client, batch)) {
                fail("pipeline request failed");
            }
        }
    }
    return std::chrono::duration<double>{Clock::now() - started}.count();
}

[[nodiscard]] auto run_concurrent(glifistore::client::Client& client, const Workload& workload) -> double {
    std::barrier start{static_cast<std::ptrdiff_t>(workload.batches.size() + 1U)};
    std::atomic_bool failed{};
    std::vector<std::thread> threads;
    threads.reserve(workload.batches.size());
    for (const auto& worker_batches : workload.batches) {
        threads.emplace_back([&client, &start, &failed, &worker_batches] {
            start.arrive_and_wait();
            for (const auto& batch : worker_batches) {
                if (!execute_pipeline(client, batch)) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }
    start.arrive_and_wait();
    const auto started = Clock::now();
    for (auto& thread : threads) {
        thread.join();
    }
    if (failed.load(std::memory_order_relaxed)) {
        fail("concurrent pipeline request failed");
    }
    return std::chrono::duration<double>{Clock::now() - started}.count();
}

[[nodiscard]] auto run_batch(glifistore::client::Client& client, const Workload& workload) -> double {
    std::size_t rounds{};
    for (const auto& worker_batches : workload.batches) {
        rounds = std::max(rounds, worker_batches.size());
    }
    const auto started = Clock::now();
    for (std::size_t round = 0; round < rounds; ++round) {
        std::vector<PipelineRequest> requests;
        for (const auto& worker_batches : workload.batches) {
            if (round < worker_batches.size()) {
                requests.insert(requests.end(), worker_batches[round].begin(), worker_batches[round].end());
            }
        }
        auto responses = client.execute_batch(requests);
        if (!responses || !validate(requests, *responses)) {
            fail("batch request failed");
        }
    }
    return std::chrono::duration<double>{Clock::now() - started}.count();
}

[[nodiscard]] auto median(std::vector<double> values) -> double {
    std::ranges::sort(values);
    const auto middle = values.size() / 2U;
    return values.size() % 2U != 0 ? values[middle] : (values[middle - 1U] + values[middle]) / 2.0;
}

} // namespace

int main(const int argc, char** argv) {
    const auto options = parse_options(argc, argv);
    auto connected = glifistore::client::Client::connect({
        .host = options.host,
        .port = options.port,
        .maximum_pipeline_requests = options.pipeline * 2U,
    });
    if (!connected) {
        fail(connected.error().message);
    }
    auto client = std::move(*connected);
    if (client.worker_count() != options.workers) {
        fail("server Worker count does not match --workers");
    }
    const auto workload = make_workload(client, options);
    const auto run = [&] {
        if (options.execution == "sequential") {
            return run_sequential(client, workload);
        }
        if (options.execution == "batch") {
            return run_batch(client, workload);
        }
        return run_concurrent(client, workload);
    };
    for (std::size_t index = 0; index < options.warmup; ++index) {
        (void)run();
    }
    std::vector<double> samples;
    samples.reserve(options.repeats);
    for (std::size_t index = 0; index < options.repeats; ++index) {
        samples.push_back(run());
    }
    std::vector<double> rates;
    rates.reserve(samples.size());
    const auto operation_count = options.operations * 2U;
    for (const auto sample : samples) {
        rates.push_back(static_cast<double>(operation_count) / sample);
    }
    const auto name = options.execution == "batch" ? "cpp_client_batch_read_after_write"
                                                   : "cpp_client_pipeline_read_after_write";
    std::cout << "# glifistore C++ client benchmark\n"
              << "# sdk_version=" << GLIFISTORE_SDK_VERSION
              << " runtime=native execution=" << options.execution << " workers=" << options.workers
              << " pipeline_pairs=" << options.pipeline << " operations=" << operation_count << '\n'
              << "name=" << name << " sdk_version=" << GLIFISTORE_SDK_VERSION
              << " runtime=native execution=" << options.execution << " workers=" << options.workers
              << " pipeline_pairs=" << options.pipeline << " operations=" << operation_count
              << " samples=" << samples.size() << " median_seconds=" << median(samples)
              << " min_seconds=" << *std::ranges::min_element(samples)
              << " max_seconds=" << *std::ranges::max_element(samples)
              << " median_ops_per_second=" << median(rates)
              << " min_ops_per_second=" << *std::ranges::min_element(rates)
              << " max_ops_per_second=" << *std::ranges::max_element(rates) << '\n';
}
