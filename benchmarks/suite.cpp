#include "suite.hpp"

#include "glifistore/core/key_hash.hpp"
#include "glifistore/index/index.hpp"
#include "glifistore/store/config.hpp"
#include "glifistore/store/store.hpp"
#include "harness.hpp"

#include <atomic>
#include <cmath>
#include <filesystem>
#include <latch>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

namespace glifistore::bench {
namespace {

[[nodiscard]] auto bytes(std::span<const std::byte> value) -> std::span<const std::byte> {
    return value;
}

struct ParallelMaterial {
    KeyMaterial material;
    std::vector<std::vector<std::size_t>> thread_order;
};

struct ParallelSample {
    std::size_t hits{};
    double seconds{};
    std::vector<double> latency_ns;
};

struct DurableContext {
    std::filesystem::path data_dir;

    explicit DurableContext(std::filesystem::path path) : data_dir(std::move(path)) {}

    DurableContext(const DurableContext&) = delete;
    auto operator=(const DurableContext&) -> DurableContext& = delete;
    DurableContext(DurableContext&&) = default;
    auto operator=(DurableContext&&) -> DurableContext& = default;

    ~DurableContext() {
        std::error_code ignored;
        std::filesystem::remove_all(data_dir, ignored);
    }
};

[[nodiscard]] auto temporary_data_directory() -> std::filesystem::path {
    static std::atomic<std::uint64_t> counter{0};
    return std::filesystem::temp_directory_path() /
           ("glifistore-bench-" + std::to_string(static_cast<unsigned long>(::getpid())) + '-' +
            std::to_string(counter.fetch_add(1U, std::memory_order_relaxed)));
}

[[nodiscard]] auto durable_store_config(const Config& config, const std::filesystem::path& data_dir,
                                        const glifistore::DurableOpenMode open_mode)
    -> glifistore::StoreConfig {
    glifistore::StoreConfig store_config{.worker_config = {.explicit_count = config.workers},
                                         .data_directory = data_dir,
                                         .durable_open_mode = open_mode};
    if (config.durable_group) {
        store_config.storage_mode = glifistore::StorageMode::durable_group;
    } else if (config.durable_periodic) {
        store_config.storage_mode = glifistore::StorageMode::durable_periodic;
    } else {
        store_config.storage_mode = glifistore::StorageMode::durable_sync;
    }
    return store_config;
}

[[nodiscard]] auto open_durable_store(const Config& config, const std::filesystem::path& data_dir,
                                      const glifistore::DurableOpenMode open_mode) -> std::unique_ptr<Store> {
    auto opened = Store::open(durable_store_config(config, data_dir, open_mode));
    return opened ? std::move(*opened) : nullptr;
}

[[nodiscard]] auto durable_put_succeeded(const Status& status) -> bool {
    return status.has_value();
}

[[nodiscard]] auto durable_put_benchmark_name(const Config& config) -> std::string {
    if (config.durable_group) {
        return "store_durable_group_put";
    }
    return config.durable_periodic ? "store_durable_periodic_put" : "store_durable_put";
}

[[nodiscard]] auto durable_get_benchmark_name(const Config& config) -> std::string {
    if (config.durable_group) {
        return "store_durable_group_get_copy";
    }
    return config.durable_periodic ? "store_durable_periodic_get_copy" : "store_durable_get_copy";
}

[[nodiscard]] auto durable_put_get_benchmark_name(const Config& config) -> std::string {
    if (config.durable_group) {
        return "store_durable_group_put_get_copy";
    }
    return config.durable_periodic ? "store_durable_periodic_put_get_copy" : "store_durable_put_get_copy";
}

[[nodiscard]] auto durable_read_after_write_benchmark_name(const Config& config) -> std::string {
    if (config.durable_group) {
        return "store_durable_group_read_after_write_copy";
    }
    return config.durable_periodic ? "store_durable_periodic_read_after_write_copy"
                                   : "store_durable_read_after_write_copy";
}

[[nodiscard]] auto make_parallel_material(const Config& config) -> ParallelMaterial {
    ParallelMaterial result;
    result.material.keys.reserve(config.operations);
    result.material.values.reserve(config.operations);
    result.material.order.reserve(config.operations);
    result.thread_order.resize(config.threads);

    std::vector<double> zipf_weights(config.workers);
    for (std::size_t worker = 0; worker < config.workers; ++worker) {
        zipf_weights[worker] = 1.0 / std::pow(static_cast<double>(worker + 1U), 1.2);
    }
    std::mt19937_64 random{0x51A7D15AULL};
    std::discrete_distribution<std::size_t> zipf_owner(zipf_weights.begin(), zipf_weights.end());

    std::size_t candidate = 0;
    for (std::size_t operation = 0; operation < config.operations; ++operation) {
        std::size_t desired_owner = 0;
        switch (config.distribution) {
        case ParallelDistribution::uniform:
        case ParallelDistribution::worker_affine:
        case ParallelDistribution::owner_bound:
            desired_owner = operation % config.workers;
            break;
        case ParallelDistribution::single_worker:
            desired_owner = 0;
            break;
        case ParallelDistribution::zipf:
            desired_owner = zipf_owner(random);
            break;
        }

        std::string key;
        do {
            key = make_key(candidate++, config.key_size);
        } while (route_worker(key, config.workers) != desired_owner);

        result.material.keys.push_back(std::move(key));
        result.material.values.push_back(make_value(operation, config.value_size));
        result.material.order.push_back(operation);
        const auto client_thread = config.distribution == ParallelDistribution::worker_affine ||
                                           config.distribution == ParallelDistribution::owner_bound
                                       ? desired_owner % config.threads
                                       : (operation / config.workers) % config.threads;
        result.thread_order[client_thread].push_back(operation);
    }

    if (config.random_access) {
        for (std::size_t thread = 0; thread < result.thread_order.size(); ++thread) {
            std::mt19937_64 thread_random{0xC0FFEEULL + thread};
            std::ranges::shuffle(result.thread_order[thread], thread_random);
        }
    }
    return result;
}

template <typename Fn>
[[nodiscard]] auto run_parallel_threads(const std::size_t thread_count,
                                        const std::size_t latency_capacity_per_thread, Fn&& fn)
    -> ParallelSample {
    std::latch ready{static_cast<std::ptrdiff_t>(thread_count)};
    std::latch start{1};
    std::latch done{static_cast<std::ptrdiff_t>(thread_count)};
    std::vector<std::size_t> thread_hits(thread_count);
    std::vector<std::vector<double>> thread_latencies(thread_count);
    if (latency_capacity_per_thread != 0) {
        for (auto& latency : thread_latencies) {
            latency.reserve(latency_capacity_per_thread);
        }
    }
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (std::size_t thread = 0; thread < thread_count; ++thread) {
        threads.emplace_back([&, thread]() {
            ready.count_down();
            start.wait();
            if constexpr (requires { fn(thread, thread_latencies[thread]); }) {
                thread_hits[thread] = fn(thread, thread_latencies[thread]);
            } else {
                thread_hits[thread] = fn(thread);
            }
            done.count_down();
        });
    }
    ready.wait();
    const auto started = std::chrono::steady_clock::now();
    start.count_down();
    done.wait();
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    for (auto& thread : threads) {
        thread.join();
    }
    std::size_t hits = 0;
    for (const auto thread_result : thread_hits) {
        hits += thread_result;
    }
    std::vector<double> latency_ns;
    if (latency_capacity_per_thread != 0) {
        std::size_t latency_count = 0;
        for (const auto& thread_latency : thread_latencies) {
            latency_count += thread_latency.size();
        }
        latency_ns.reserve(latency_count);
        for (auto& thread_latency : thread_latencies) {
            latency_ns.insert(latency_ns.end(), std::make_move_iterator(thread_latency.begin()),
                              std::make_move_iterator(thread_latency.end()));
        }
    }
    return {.hits = hits, .seconds = elapsed, .latency_ns = std::move(latency_ns)};
}

template <typename BodyFn, typename Context>
[[nodiscard]] auto invoke_parallel_body(BodyFn& body, Context& context, const std::size_t thread,
                                        std::vector<double>& latency_ns) -> std::size_t {
    if constexpr (requires { body(context, thread, latency_ns); }) {
        return body(context, thread, latency_ns);
    } else {
        return body(context, thread);
    }
}

[[nodiscard]] auto percentile(const std::vector<double>& sorted, const double quantile) -> double {
    if (sorted.empty()) {
        return 0.0;
    }
    const auto rank = static_cast<std::size_t>(std::ceil(quantile * static_cast<double>(sorted.size())));
    return sorted[std::min(std::max(std::size_t{1}, rank), sorted.size()) - 1U];
}

template <typename SetupFn, typename BodyFn>
[[nodiscard]] auto benchmark_collect_parallel(const RunSettings& settings, const std::size_t operations,
                                              const Config& config, std::string name,
                                              const std::size_t expected_hits, SetupFn&& setup, BodyFn&& body)
    -> Result {
    for (std::size_t iteration = 0; iteration < settings.warmup_iterations; ++iteration) {
        auto context = setup();
        (void)run_parallel_threads(config.threads, 0,
                                   [&](const std::size_t thread, std::vector<double>& latency_ns) {
                                       return invoke_parallel_body(body, context, thread, latency_ns);
                                   });
    }

    std::vector<double> samples;
    std::vector<ResourceSample> resources;
    std::vector<double> latency_ns;
    samples.reserve(settings.measured_iterations);
    resources.reserve(settings.measured_iterations);
    std::size_t hits = 0;
    for (std::size_t iteration = 0; iteration < settings.measured_iterations; ++iteration) {
        auto context = setup();
        auto resource = process_memory_snapshot();
        const auto latency_capacity =
            settings.latency ? operations / config.threads + (operations % config.threads == 0 ? 0U : 1U)
                             : 0U;
        auto sample =
            run_parallel_threads(config.threads, latency_capacity,
                                 [&](const std::size_t thread, std::vector<double>& thread_latency_ns) {
                                     return invoke_parallel_body(body, context, thread, thread_latency_ns);
                                 });
        const auto after = process_memory_snapshot();
        resource.rss_after_bytes = after.rss_after_bytes;
        resource.peak_rss_bytes = after.peak_rss_bytes;
        if (!validate_sample_hits(name, expected_hits, sample.hits, iteration + 1U)) {
            return Result{
                .name = std::move(name), .config = config, .settings = settings, .operations = operations};
        }
        hits = sample.hits;
        samples.push_back(sample.seconds);
        resources.push_back(resource);
        latency_ns.insert(latency_ns.end(), std::make_move_iterator(sample.latency_ns.begin()),
                          std::make_move_iterator(sample.latency_ns.end()));
    }
    auto result = finalize_result(std::move(name), config, settings, operations, hits, std::move(samples),
                                  std::move(resources));
    if (!latency_ns.empty()) {
        std::ranges::sort(latency_ns);
        result.latency_samples = latency_ns.size();
        result.p50_latency_ns = percentile(latency_ns, 0.50);
        result.p95_latency_ns = percentile(latency_ns, 0.95);
        result.p99_latency_ns = percentile(latency_ns, 0.99);
        result.p999_latency_ns = percentile(latency_ns, 0.999);
    }
    return result;
}

[[nodiscard]] auto make_index_ref(const std::size_t index_in_order) -> RecordRef {
    return RecordRef{
        .segment_id = SegmentId{1},
        .offset = RecordOffset{static_cast<std::uint32_t>((index_in_order * 64U) % 1'000'000U)},
        .size = RecordSize{64},
        .sequence = SequenceNumber{index_in_order + 1},
        .generation = GenerationId{1},
    };
}

[[nodiscard]] auto make_reserved_index(const std::size_t operations) -> std::unique_ptr<Index> {
    auto index = std::make_unique<Index>();
    if (!index->reserve(operations)) {
        return nullptr;
    }
    return index;
}

[[nodiscard]] auto populate_index(Index& index, const KeyMaterial& material) -> bool {
    for (const auto index_in_order : material.order) {
        if (!index.insert_or_assign(material.keys[index_in_order], make_index_ref(index_in_order))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto run_index_insert(const Config& config, const RunSettings& settings) -> Result {
    const auto material = make_key_material(config);
    return benchmark_collect_timed(
        settings, config.operations, config, "index_insert", config.operations,
        [&]() -> std::unique_ptr<Index> { return make_reserved_index(config.operations); },
        [&](std::unique_ptr<Index>& index) -> std::size_t {
            if (index == nullptr) {
                return 0;
            }
            std::size_t inserted = 0;
            for (const auto index_in_order : material.order) {
                if (index->insert_or_assign(material.keys[index_in_order], make_index_ref(index_in_order))) {
                    ++inserted;
                }
            }
            return inserted;
        });
}

[[nodiscard]] auto run_index_replace(const Config& config, const RunSettings& settings) -> Result {
    const auto material = make_key_material(config);
    return benchmark_collect_timed(
        settings, config.operations, config, "index_replace", config.operations,
        [&]() -> std::unique_ptr<Index> {
            auto index = make_reserved_index(config.operations);
            if (index == nullptr || !populate_index(*index, material)) {
                return nullptr;
            }
            return index;
        },
        [&](std::unique_ptr<Index>& index) -> std::size_t {
            if (index == nullptr) {
                return 0;
            }
            std::size_t replaced = 0;
            for (const auto index_in_order : material.order) {
                const auto replacement = make_index_ref(index_in_order + config.operations);
                if (index->insert_or_assign(material.keys[index_in_order], replacement)) {
                    ++replaced;
                }
            }
            return replaced;
        });
}

[[nodiscard]] auto run_index_find_hit(const Config& config, const RunSettings& settings) -> Result {
    const auto material = make_key_material(config);
    return benchmark_collect_timed(
        settings, config.operations, config, "index_find_hit", config.operations,
        [&]() -> std::unique_ptr<Index> {
            auto index = make_reserved_index(config.operations);
            if (index == nullptr || !populate_index(*index, material)) {
                return nullptr;
            }
            return index;
        },
        [&](std::unique_ptr<Index>& index) -> std::size_t {
            if (index == nullptr) {
                return 0;
            }
            std::size_t hits = 0;
            for (const auto index_in_order : material.order) {
                hits += index->find(material.keys[index_in_order]).has_value() ? 1U : 0U;
            }
            return hits;
        });
}

[[nodiscard]] auto run_index_find_miss(const Config& config, const RunSettings& settings) -> Result {
    const auto material = make_key_material(config);
    const auto miss_material = make_miss_key_material(config);
    return benchmark_collect_timed(
        settings, config.operations, config, "index_find_miss", config.operations,
        [&]() -> std::unique_ptr<Index> {
            auto index = make_reserved_index(config.operations);
            if (index == nullptr || !populate_index(*index, material)) {
                return nullptr;
            }
            return index;
        },
        [&](std::unique_ptr<Index>& index) -> std::size_t {
            if (index == nullptr) {
                return 0;
            }
            std::size_t misses = 0;
            for (const auto index_in_order : miss_material.order) {
                misses += index->find(miss_material.keys[index_in_order]).has_value() ? 0U : 1U;
            }
            return misses;
        });
}

[[nodiscard]] auto run_index_churn_miss(const Config& config, const RunSettings& settings) -> Result {
    const auto material = make_key_material(config);
    const auto miss_material = make_miss_key_material(config);
    return benchmark_collect_timed(
        settings, config.operations, config, "index_churn_miss", config.operations,
        [&]() -> std::unique_ptr<Index> {
            auto index = make_reserved_index(config.operations);
            if (index == nullptr || !populate_index(*index, material)) {
                return nullptr;
            }
            const auto erase_count = config.operations - config.operations / 4U;
            for (std::size_t position = 0; position < erase_count; ++position) {
                const auto key_index = material.order[position];
                const auto& key = material.keys[key_index];
                if (!index->erase_no_compact(HashedKey{key, hash_key(key)}).previous) {
                    return nullptr;
                }
            }
            const std::string_view preparation_key{"churn-maintenance-probe"};
            if (!index->prepare_insert(HashedKey{preparation_key, hash_key(preparation_key)})) {
                return nullptr;
            }
            return index;
        },
        [&](std::unique_ptr<Index>& index) -> std::size_t {
            if (index == nullptr) {
                return 0;
            }
            std::size_t misses = 0;
            for (const auto index_in_order : miss_material.order) {
                misses += index->find(miss_material.keys[index_in_order]).has_value() ? 0U : 1U;
            }
            return misses;
        });
}

[[nodiscard]] auto run_index_erase(const Config& config, const RunSettings& settings) -> Result {
    const auto material = make_key_material(config);
    return benchmark_collect_timed(
        settings, config.operations, config, "index_erase", config.operations,
        [&]() -> std::unique_ptr<Index> {
            auto index = make_reserved_index(config.operations);
            if (index == nullptr || !populate_index(*index, material)) {
                return nullptr;
            }
            return index;
        },
        [&](std::unique_ptr<Index>& index) -> std::size_t {
            if (index == nullptr) {
                return 0;
            }
            std::size_t erased = 0;
            for (const auto index_in_order : material.order) {
                if (index->erase(material.keys[index_in_order]).previous.has_value()) {
                    ++erased;
                }
            }
            return erased;
        });
}

[[nodiscard]] auto run_index_insert_find(const Config& config, const RunSettings& settings) -> Result {
    const auto material = make_key_material(config);
    return benchmark_collect_timed(
        settings, config.operations * 2U, config, "index_insert_find", config.operations,
        [&]() -> std::unique_ptr<Index> { return make_reserved_index(config.operations); },
        [&](std::unique_ptr<Index>& index) -> std::size_t {
            if (index == nullptr) {
                return 0;
            }
            for (const auto index_in_order : material.order) {
                const auto& key = material.keys[index_in_order];
                if (!index->insert_or_assign(key, make_index_ref(index_in_order))) {
                    return 0;
                }
            }
            std::size_t hits = 0;
            for (const auto index_in_order : material.order) {
                hits += index->find(material.keys[index_in_order]).has_value() ? 1U : 0U;
            }
            return hits;
        });
}

[[nodiscard]] auto run_store_put(const Config& config, const RunSettings& settings) -> Result {
    const auto material = make_key_material(config);
    return benchmark_collect_timed(
        settings, config.operations, config, "store_put", config.operations,
        [&]() -> std::unique_ptr<Store> {
            auto opened = Store::open({.worker_config = {.explicit_count = config.workers}});
            if (!opened) {
                return nullptr;
            }
            return std::move(*opened);
        },
        [&](std::unique_ptr<Store>& store) -> std::size_t {
            if (store == nullptr) {
                return 0;
            }
            std::size_t writes = 0;
            for (const auto index_in_order : material.order) {
                if (store->put(material.keys[index_in_order], bytes(material.values[index_in_order]))
                        .has_value()) {
                    ++writes;
                }
            }
            return writes;
        });
}

[[nodiscard]] auto run_store_put_batch(const Config& config, const RunSettings& settings) -> Result {
    const auto material = make_key_material(config);
    constexpr std::size_t kBatchSize = 32;
    return benchmark_collect_timed(
        settings, config.operations, config, "store_put_batch", config.operations,
        [&]() -> std::unique_ptr<Store> {
            auto opened = Store::open({.worker_config = {.explicit_count = config.workers}});
            if (!opened) {
                return nullptr;
            }
            return std::move(*opened);
        },
        [&](std::unique_ptr<Store>& store) -> std::size_t {
            if (store == nullptr) {
                return 0;
            }
            std::size_t writes = 0;
            std::vector<Store::PutItem> batch;
            batch.reserve(kBatchSize);
            for (std::size_t cursor = 0; cursor < material.order.size();) {
                batch.clear();
                while (cursor < material.order.size() && batch.size() < kBatchSize) {
                    const auto index_in_order = material.order[cursor++];
                    batch.push_back(Store::PutItem{.key = material.keys[index_in_order],
                                                   .value = bytes(material.values[index_in_order])});
                }
                const auto statuses = store->put_batch(batch);
                for (const auto& status : statuses) {
                    if (status.has_value()) {
                        ++writes;
                    }
                }
            }
            return writes;
        });
}

[[nodiscard]] auto run_store_get(const Config& config, const RunSettings& settings) -> Result {
    const auto material = make_key_material(config);
    return benchmark_collect_timed(
        settings, config.operations, config, "store_get_copy", config.operations,
        [&]() -> std::unique_ptr<Store> {
            auto opened = Store::open({.worker_config = {.explicit_count = config.workers}});
            if (!opened) {
                return nullptr;
            }
            auto store = std::move(*opened);
            for (const auto index_in_order : material.order) {
                if (!store->put(material.keys[index_in_order], bytes(material.values[index_in_order]))
                         .has_value()) {
                    return nullptr;
                }
            }
            return store;
        },
        [&](std::unique_ptr<Store>& store) -> std::size_t {
            if (store == nullptr) {
                return 0;
            }
            std::size_t hits = 0;
            for (const auto index_in_order : material.order) {
                if (store->get(material.keys[index_in_order]).has_value()) {
                    ++hits;
                }
            }
            return hits;
        });
}

[[nodiscard]] auto run_store_put_get(const Config& config, const RunSettings& settings) -> Result {
    const auto material = make_key_material(config);
    return benchmark_collect_timed(
        settings, config.operations * 2U, config, "store_put_get_copy", config.operations,
        [&]() -> std::unique_ptr<Store> {
            auto opened = Store::open({.worker_config = {.explicit_count = config.workers}});
            if (!opened) {
                return nullptr;
            }
            return std::move(*opened);
        },
        [&](std::unique_ptr<Store>& store) -> std::size_t {
            if (store == nullptr) {
                return 0;
            }
            for (const auto index_in_order : material.order) {
                if (!store->put(material.keys[index_in_order], bytes(material.values[index_in_order]))
                         .has_value()) {
                    return 0;
                }
            }
            std::size_t hits = 0;
            for (const auto index_in_order : material.order) {
                if (store->get(material.keys[index_in_order]).has_value()) {
                    ++hits;
                }
            }
            return hits;
        });
}

[[nodiscard]] auto run_store_read_after_write(const Config& config, const RunSettings& settings) -> Result {
    const auto material = make_key_material(config);
    const auto expected_hits = config.operations * 2U;
    return benchmark_collect_timed(
        settings, expected_hits, config, "store_read_after_write_copy", expected_hits,
        [&]() -> std::unique_ptr<Store> {
            auto opened = Store::open({.worker_config = {.explicit_count = config.workers}});
            if (!opened) {
                return nullptr;
            }
            return std::move(*opened);
        },
        [&](std::unique_ptr<Store>& store) -> std::size_t {
            if (store == nullptr) {
                return 0;
            }
            std::size_t hits = 0;
            for (const auto index_in_order : material.order) {
                if (store->put(material.keys[index_in_order], bytes(material.values[index_in_order]))
                        .has_value()) {
                    ++hits;
                }
                if (store->get(material.keys[index_in_order]).has_value()) {
                    ++hits;
                }
            }
            return hits;
        });
}

[[nodiscard]] auto open_parallel_store(const Config& config) -> std::unique_ptr<Store> {
    auto opened = Store::open({.worker_config = {.explicit_count = config.workers}});
    return opened ? std::move(*opened) : nullptr;
}

[[nodiscard]] auto run_store_parallel_put(const Config& config, const RunSettings& settings) -> Result {
    const auto material = make_parallel_material(config);
    return benchmark_collect_parallel(
        settings, config.operations, config, "store_parallel_put", config.operations,
        [&]() { return open_parallel_store(config); },
        [&](std::unique_ptr<Store>& store, const std::size_t thread,
            std::vector<double>& latency_ns) -> std::size_t {
            if (store == nullptr) {
                return 0;
            }
            std::size_t writes = 0;
            const bool measure_latency = settings.latency && latency_ns.capacity() != 0U;
            for (const auto index_in_order : material.thread_order[thread]) {
                const auto started = measure_latency ? std::chrono::steady_clock::now()
                                                     : std::chrono::steady_clock::time_point{};
                if (store->put(material.material.keys[index_in_order],
                               bytes(material.material.values[index_in_order]))) {
                    ++writes;
                }
                if (measure_latency) {
                    latency_ns.push_back(
                        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - started)
                            .count());
                }
            }
            return writes;
        });
}

[[nodiscard]] auto run_store_parallel_get(const Config& config, const RunSettings& settings) -> Result {
    const auto material = make_parallel_material(config);
    return benchmark_collect_parallel(
        settings, config.operations, config, "store_parallel_get_copy", config.operations,
        [&]() -> std::unique_ptr<Store> {
            auto store = open_parallel_store(config);
            if (store == nullptr) {
                return nullptr;
            }
            for (const auto index_in_order : material.material.order) {
                if (!store->put(material.material.keys[index_in_order],
                                bytes(material.material.values[index_in_order]))) {
                    return nullptr;
                }
            }
            return store;
        },
        [&](std::unique_ptr<Store>& store, const std::size_t thread) -> std::size_t {
            if (store == nullptr) {
                return 0;
            }
            std::size_t hits = 0;
            for (const auto index_in_order : material.thread_order[thread]) {
                hits += store->get(material.material.keys[index_in_order]).has_value() ? 1U : 0U;
            }
            return hits;
        });
}

[[nodiscard]] auto run_store_parallel_read_after_write(const Config& config, const RunSettings& settings)
    -> Result {
    const auto material = make_parallel_material(config);
    const auto expected_hits = config.operations * 2U;
    return benchmark_collect_parallel(
        settings, expected_hits, config, "store_parallel_read_after_write_copy", expected_hits,
        [&]() { return open_parallel_store(config); },
        [&](std::unique_ptr<Store>& store, const std::size_t thread) -> std::size_t {
            if (store == nullptr) {
                return 0;
            }
            std::size_t hits = 0;
            for (const auto index_in_order : material.thread_order[thread]) {
                const auto& key = material.material.keys[index_in_order];
                if (store->put(key, bytes(material.material.values[index_in_order]))) {
                    ++hits;
                }
                if (store->get(key)) {
                    ++hits;
                }
            }
            return hits;
        });
}

[[nodiscard]] auto run_store_durable_put(const Config& config, const RunSettings& settings) -> Result {
    const auto material = make_key_material(config);
    return benchmark_collect_timed(
        settings, config.operations, config, durable_put_benchmark_name(config), config.operations,
        [&]() -> std::unique_ptr<DurableContext> {
            const auto data_dir = temporary_data_directory();
            if (data_dir.empty()) {
                return nullptr;
            }
            return std::make_unique<DurableContext>(data_dir);
        },
        [&](std::unique_ptr<DurableContext>& context) -> std::size_t {
            if (context == nullptr) {
                return 0;
            }
            auto store = open_durable_store(config, context->data_dir, DurableOpenMode::create_new);
            if (store == nullptr) {
                return 0;
            }
            std::size_t writes = 0;
            for (const auto index_in_order : material.order) {
                if (durable_put_succeeded(
                        store->put(material.keys[index_in_order], bytes(material.values[index_in_order])))) {
                    ++writes;
                }
            }
            return writes;
        });
}

[[nodiscard]] auto run_store_durable_get(const Config& config, const RunSettings& settings) -> Result {
    const auto material = make_key_material(config);
    return benchmark_collect_timed(
        settings, config.operations, config, durable_get_benchmark_name(config), config.operations,
        [&]() -> std::pair<std::unique_ptr<DurableContext>, std::unique_ptr<Store>> {
            const auto data_dir = temporary_data_directory();
            if (data_dir.empty()) {
                return {nullptr, nullptr};
            }
            auto context = std::make_unique<DurableContext>(data_dir);
            auto store = open_durable_store(config, context->data_dir, DurableOpenMode::create_new);
            if (store == nullptr) {
                return {std::move(context), nullptr};
            }
            for (const auto index_in_order : material.order) {
                if (!durable_put_succeeded(
                        store->put(material.keys[index_in_order], bytes(material.values[index_in_order])))) {
                    return {std::move(context), nullptr};
                }
            }
            return {std::move(context), std::move(store)};
        },
        [&](std::pair<std::unique_ptr<DurableContext>, std::unique_ptr<Store>>& context) -> std::size_t {
            if (context.second == nullptr) {
                return 0;
            }
            std::size_t hits = 0;
            for (const auto index_in_order : material.order) {
                if (context.second->get(material.keys[index_in_order]).has_value()) {
                    ++hits;
                }
            }
            return hits;
        });
}

[[nodiscard]] auto run_store_durable_put_get(const Config& config, const RunSettings& settings) -> Result {
    const auto material = make_key_material(config);
    return benchmark_collect_timed(
        settings, config.operations * 2U, config, durable_put_get_benchmark_name(config), config.operations,
        [&]() -> std::unique_ptr<DurableContext> {
            const auto data_dir = temporary_data_directory();
            if (data_dir.empty()) {
                return nullptr;
            }
            return std::make_unique<DurableContext>(data_dir);
        },
        [&](std::unique_ptr<DurableContext>& context) -> std::size_t {
            if (context == nullptr) {
                return 0;
            }
            auto store = open_durable_store(config, context->data_dir, DurableOpenMode::create_new);
            if (store == nullptr) {
                return 0;
            }
            for (const auto index_in_order : material.order) {
                if (!durable_put_succeeded(
                        store->put(material.keys[index_in_order], bytes(material.values[index_in_order])))) {
                    return 0;
                }
            }
            std::size_t hits = 0;
            for (const auto index_in_order : material.order) {
                if (store->get(material.keys[index_in_order]).has_value()) {
                    ++hits;
                }
            }
            return hits;
        });
}

[[nodiscard]] auto run_store_durable_read_after_write(const Config& config, const RunSettings& settings)
    -> Result {
    const auto material = make_key_material(config);
    const auto expected_hits = config.operations * 2U;
    return benchmark_collect_timed(
        settings, expected_hits, config, durable_read_after_write_benchmark_name(config), expected_hits,
        [&]() -> std::unique_ptr<DurableContext> {
            const auto data_dir = temporary_data_directory();
            if (data_dir.empty()) {
                return nullptr;
            }
            return std::make_unique<DurableContext>(data_dir);
        },
        [&](std::unique_ptr<DurableContext>& context) -> std::size_t {
            if (context == nullptr) {
                return 0;
            }
            auto store = open_durable_store(config, context->data_dir, DurableOpenMode::create_new);
            if (store == nullptr) {
                return 0;
            }
            std::size_t hits = 0;
            for (const auto index_in_order : material.order) {
                if (durable_put_succeeded(
                        store->put(material.keys[index_in_order], bytes(material.values[index_in_order])))) {
                    ++hits;
                }
                if (store->get(material.keys[index_in_order]).has_value()) {
                    ++hits;
                }
            }
            return hits;
        });
}

[[nodiscard]] auto run_store_durable_recovery_open(const Config& config, const RunSettings& settings)
    -> Result {
    const auto material = make_key_material(config);
    return benchmark_collect_timed(
        settings, config.operations, config, "store_durable_recovery_open", config.operations,
        [&]() -> std::unique_ptr<DurableContext> {
            const auto data_dir = temporary_data_directory();
            if (data_dir.empty()) {
                return nullptr;
            }
            auto context = std::make_unique<DurableContext>(data_dir);
            auto store = open_durable_store(config, context->data_dir, DurableOpenMode::create_new);
            if (store == nullptr) {
                return nullptr;
            }
            for (const auto index_in_order : material.order) {
                if (!durable_put_succeeded(
                        store->put(material.keys[index_in_order], bytes(material.values[index_in_order])))) {
                    return nullptr;
                }
            }
            store.reset();
            return context;
        },
        [&](std::unique_ptr<DurableContext>& context) -> std::size_t {
            if (context == nullptr) {
                return 0;
            }
            auto store = open_durable_store(config, context->data_dir, DurableOpenMode::open_existing);
            if (store == nullptr) {
                return 0;
            }
            std::size_t hits = 0;
            for (const auto index_in_order : material.order) {
                if (store->get(material.keys[index_in_order]).has_value()) {
                    ++hits;
                }
            }
            return hits;
        });
}

[[nodiscard]] auto open_durable_parallel_store(const Config& config, const std::filesystem::path& data_dir)
    -> std::unique_ptr<Store> {
    return open_durable_store(config, data_dir, DurableOpenMode::create_new);
}

[[nodiscard]] auto run_store_durable_parallel_put(const Config& config, const RunSettings& settings)
    -> Result {
    const auto material = make_parallel_material(config);
    return benchmark_collect_parallel(
        settings, config.operations, config, durable_put_benchmark_name(config), config.operations,
        [&]() -> std::pair<std::unique_ptr<DurableContext>, std::unique_ptr<Store>> {
            const auto data_dir = temporary_data_directory();
            if (data_dir.empty()) {
                return {nullptr, nullptr};
            }
            auto context = std::make_unique<DurableContext>(data_dir);
            auto store = open_durable_parallel_store(config, context->data_dir);
            if (store == nullptr) {
                return {std::move(context), nullptr};
            }
            return {std::move(context), std::move(store)};
        },
        [&](std::pair<std::unique_ptr<DurableContext>, std::unique_ptr<Store>>& context,
            const std::size_t thread, std::vector<double>& latency_ns) -> std::size_t {
            if (context.second == nullptr) {
                return 0;
            }
            std::size_t writes = 0;
            for (const auto index_in_order : material.thread_order[thread]) {
                const auto started = settings.latency ? std::chrono::steady_clock::now()
                                                      : std::chrono::steady_clock::time_point{};
                const auto succeeded = durable_put_succeeded(context.second->put(
                    material.material.keys[index_in_order], bytes(material.material.values[index_in_order])));
                if (settings.latency) {
                    latency_ns.push_back(
                        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - started)
                            .count());
                }
                if (succeeded) {
                    ++writes;
                }
            }
            return writes;
        });
}

[[nodiscard]] auto run_store_durable_parallel_get(const Config& config, const RunSettings& settings)
    -> Result {
    const auto material = make_parallel_material(config);
    return benchmark_collect_parallel(
        settings, config.operations, config, "store_durable_parallel_get_copy", config.operations,
        [&]() -> std::pair<std::unique_ptr<DurableContext>, std::unique_ptr<Store>> {
            const auto data_dir = temporary_data_directory();
            if (data_dir.empty()) {
                return {nullptr, nullptr};
            }
            auto context = std::make_unique<DurableContext>(data_dir);
            auto store = open_durable_parallel_store(config, context->data_dir);
            if (store == nullptr) {
                return {std::move(context), nullptr};
            }
            for (const auto index_in_order : material.material.order) {
                if (!durable_put_succeeded(store->put(material.material.keys[index_in_order],
                                                      bytes(material.material.values[index_in_order])))) {
                    return {std::move(context), nullptr};
                }
            }
            return {std::move(context), std::move(store)};
        },
        [&](std::pair<std::unique_ptr<DurableContext>, std::unique_ptr<Store>>& context,
            const std::size_t thread, std::vector<double>& latency_ns) -> std::size_t {
            if (context.second == nullptr) {
                return 0;
            }
            std::size_t hits = 0;
            for (const auto index_in_order : material.thread_order[thread]) {
                const auto started = settings.latency ? std::chrono::steady_clock::now()
                                                      : std::chrono::steady_clock::time_point{};
                const auto found = context.second->get(material.material.keys[index_in_order]).has_value();
                if (settings.latency) {
                    latency_ns.push_back(
                        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - started)
                            .count());
                }
                hits += found ? 1U : 0U;
            }
            return hits;
        });
}

[[nodiscard]] auto run_store_durable_parallel_read_after_write(const Config& config,
                                                               const RunSettings& settings) -> Result {
    const auto material = make_parallel_material(config);
    const auto expected_hits = config.operations * 2U;
    return benchmark_collect_parallel(
        settings, expected_hits, config, "store_durable_parallel_read_after_write_copy", expected_hits,
        [&]() -> std::pair<std::unique_ptr<DurableContext>, std::unique_ptr<Store>> {
            const auto data_dir = temporary_data_directory();
            if (data_dir.empty()) {
                return {nullptr, nullptr};
            }
            auto context = std::make_unique<DurableContext>(data_dir);
            auto store = open_durable_parallel_store(config, context->data_dir);
            if (store == nullptr) {
                return {std::move(context), nullptr};
            }
            return {std::move(context), std::move(store)};
        },
        [&](std::pair<std::unique_ptr<DurableContext>, std::unique_ptr<Store>>& context,
            const std::size_t thread) -> std::size_t {
            if (context.second == nullptr) {
                return 0;
            }
            std::size_t hits = 0;
            for (const auto index_in_order : material.thread_order[thread]) {
                const auto& key = material.material.keys[index_in_order];
                if (durable_put_succeeded(
                        context.second->put(key, bytes(material.material.values[index_in_order])))) {
                    ++hits;
                }
                if (context.second->get(key)) {
                    ++hits;
                }
            }
            return hits;
        });
}

[[nodiscard]] auto run_single(const BenchmarkKind kind, const Config& config, const RunSettings& settings)
    -> Result {
    switch (kind) {
    case BenchmarkKind::index_insert:
        return run_index_insert(config, settings);
    case BenchmarkKind::index_replace:
        return run_index_replace(config, settings);
    case BenchmarkKind::index_find_hit:
        return run_index_find_hit(config, settings);
    case BenchmarkKind::index_find_miss:
        return run_index_find_miss(config, settings);
    case BenchmarkKind::index_churn_miss:
        return run_index_churn_miss(config, settings);
    case BenchmarkKind::index_erase:
        return run_index_erase(config, settings);
    case BenchmarkKind::index_insert_find:
        return run_index_insert_find(config, settings);
    case BenchmarkKind::store_put:
        return run_store_put(config, settings);
    case BenchmarkKind::store_put_batch:
        return run_store_put_batch(config, settings);
    case BenchmarkKind::store_get:
        return run_store_get(config, settings);
    case BenchmarkKind::store_put_get:
        return run_store_put_get(config, settings);
    case BenchmarkKind::store_read_after_write:
        return run_store_read_after_write(config, settings);
    case BenchmarkKind::store_parallel_put:
        return run_store_parallel_put(config, settings);
    case BenchmarkKind::store_parallel_get:
        return run_store_parallel_get(config, settings);
    case BenchmarkKind::store_parallel_read_after_write:
        return run_store_parallel_read_after_write(config, settings);
    case BenchmarkKind::store_durable_put:
        return run_store_durable_put(config, settings);
    case BenchmarkKind::store_durable_get:
        return run_store_durable_get(config, settings);
    case BenchmarkKind::store_durable_put_get:
        return run_store_durable_put_get(config, settings);
    case BenchmarkKind::store_durable_read_after_write:
        return run_store_durable_read_after_write(config, settings);
    case BenchmarkKind::store_durable_recovery_open:
        return run_store_durable_recovery_open(config, settings);
    case BenchmarkKind::store_durable_periodic_put:
        return run_store_durable_put(Config{.operations = config.operations,
                                            .key_size = config.key_size,
                                            .value_size = config.value_size,
                                            .workers = config.workers,
                                            .threads = config.threads,
                                            .distribution = config.distribution,
                                            .random_access = config.random_access,
                                            .durable_periodic = true},
                                     settings);
    case BenchmarkKind::store_durable_periodic_get:
        return run_store_durable_get(Config{.operations = config.operations,
                                            .key_size = config.key_size,
                                            .value_size = config.value_size,
                                            .workers = config.workers,
                                            .threads = config.threads,
                                            .distribution = config.distribution,
                                            .random_access = config.random_access,
                                            .durable_periodic = true},
                                     settings);
    case BenchmarkKind::store_durable_periodic_put_get:
        return run_store_durable_put_get(Config{.operations = config.operations,
                                                .key_size = config.key_size,
                                                .value_size = config.value_size,
                                                .workers = config.workers,
                                                .threads = config.threads,
                                                .distribution = config.distribution,
                                                .random_access = config.random_access,
                                                .durable_periodic = true},
                                         settings);
    case BenchmarkKind::store_durable_periodic_read_after_write:
        return run_store_durable_read_after_write(Config{.operations = config.operations,
                                                         .key_size = config.key_size,
                                                         .value_size = config.value_size,
                                                         .workers = config.workers,
                                                         .threads = config.threads,
                                                         .distribution = config.distribution,
                                                         .random_access = config.random_access,
                                                         .durable_periodic = true},
                                                  settings);
    case BenchmarkKind::store_durable_group_put:
        return run_store_durable_put(Config{.operations = config.operations,
                                            .key_size = config.key_size,
                                            .value_size = config.value_size,
                                            .workers = config.workers,
                                            .threads = config.threads,
                                            .distribution = config.distribution,
                                            .random_access = config.random_access,
                                            .durable_group = true},
                                     settings);
    case BenchmarkKind::store_durable_group_get:
        return run_store_durable_get(Config{.operations = config.operations,
                                            .key_size = config.key_size,
                                            .value_size = config.value_size,
                                            .workers = config.workers,
                                            .threads = config.threads,
                                            .distribution = config.distribution,
                                            .random_access = config.random_access,
                                            .durable_group = true},
                                     settings);
    case BenchmarkKind::store_durable_group_put_get:
        return run_store_durable_put_get(Config{.operations = config.operations,
                                                .key_size = config.key_size,
                                                .value_size = config.value_size,
                                                .workers = config.workers,
                                                .threads = config.threads,
                                                .distribution = config.distribution,
                                                .random_access = config.random_access,
                                                .durable_group = true},
                                         settings);
    case BenchmarkKind::store_durable_group_read_after_write:
        return run_store_durable_read_after_write(Config{.operations = config.operations,
                                                         .key_size = config.key_size,
                                                         .value_size = config.value_size,
                                                         .workers = config.workers,
                                                         .threads = config.threads,
                                                         .distribution = config.distribution,
                                                         .random_access = config.random_access,
                                                         .durable_group = true},
                                                  settings);
    case BenchmarkKind::store_durable_group_parallel_put:
        return run_store_durable_parallel_put(Config{.operations = config.operations,
                                                     .key_size = config.key_size,
                                                     .value_size = config.value_size,
                                                     .workers = config.workers,
                                                     .threads = config.threads,
                                                     .distribution = config.distribution,
                                                     .random_access = config.random_access,
                                                     .durable_group = true},
                                              settings);
    case BenchmarkKind::store_durable_parallel_put:
        return run_store_durable_parallel_put(config, settings);
    case BenchmarkKind::store_durable_parallel_get:
        return run_store_durable_parallel_get(config, settings);
    case BenchmarkKind::store_durable_parallel_read_after_write:
        return run_store_durable_parallel_read_after_write(config, settings);
    case BenchmarkKind::all:
    case BenchmarkKind::index_all:
    case BenchmarkKind::store_parallel_all:
    case BenchmarkKind::store_durable_all:
    case BenchmarkKind::store_durable_parallel_all:
    case BenchmarkKind::store_durable_periodic_all:
    case BenchmarkKind::store_durable_group_all:
        break;
    }
    return {};
}

[[nodiscard]] auto index_benchmark_kinds() -> std::vector<BenchmarkKind> {
    return {BenchmarkKind::index_insert,    BenchmarkKind::index_replace,    BenchmarkKind::index_find_hit,
            BenchmarkKind::index_find_miss, BenchmarkKind::index_churn_miss, BenchmarkKind::index_erase};
}

[[nodiscard]] auto all_benchmark_kinds() -> std::vector<BenchmarkKind> {
    auto kinds = index_benchmark_kinds();
    kinds.push_back(BenchmarkKind::store_put);
    kinds.push_back(BenchmarkKind::store_get);
    kinds.push_back(BenchmarkKind::store_put_get);
    kinds.push_back(BenchmarkKind::store_read_after_write);
    return kinds;
}

[[nodiscard]] auto parallel_benchmark_kinds() -> std::vector<BenchmarkKind> {
    return {BenchmarkKind::store_parallel_put, BenchmarkKind::store_parallel_get,
            BenchmarkKind::store_parallel_read_after_write};
}

[[nodiscard]] auto durable_benchmark_kinds() -> std::vector<BenchmarkKind> {
    return {BenchmarkKind::store_durable_put, BenchmarkKind::store_durable_get,
            BenchmarkKind::store_durable_put_get, BenchmarkKind::store_durable_read_after_write,
            BenchmarkKind::store_durable_recovery_open};
}

[[nodiscard]] auto durable_parallel_benchmark_kinds() -> std::vector<BenchmarkKind> {
    return {BenchmarkKind::store_durable_parallel_put, BenchmarkKind::store_durable_parallel_get,
            BenchmarkKind::store_durable_parallel_read_after_write};
}

[[nodiscard]] auto durable_periodic_benchmark_kinds() -> std::vector<BenchmarkKind> {
    return {BenchmarkKind::store_durable_periodic_put, BenchmarkKind::store_durable_periodic_get,
            BenchmarkKind::store_durable_periodic_put_get,
            BenchmarkKind::store_durable_periodic_read_after_write};
}

[[nodiscard]] auto durable_group_benchmark_kinds() -> std::vector<BenchmarkKind> {
    return {BenchmarkKind::store_durable_group_put, BenchmarkKind::store_durable_group_get,
            BenchmarkKind::store_durable_group_put_get, BenchmarkKind::store_durable_group_read_after_write};
}

} // namespace

auto run_benchmark(const BenchmarkKind kind, const Config& config, const RunSettings& settings)
    -> std::vector<Result> {
    std::vector<Result> results;
    if (kind == BenchmarkKind::index_all) {
        for (const auto isolated : index_benchmark_kinds()) {
            results.push_back(run_single(isolated, config, settings));
        }
        return results;
    }
    if (kind == BenchmarkKind::all) {
        for (const auto isolated : all_benchmark_kinds()) {
            results.push_back(run_single(isolated, config, settings));
        }
        return results;
    }
    if (kind == BenchmarkKind::store_parallel_all) {
        for (const auto isolated : parallel_benchmark_kinds()) {
            results.push_back(run_single(isolated, config, settings));
        }
        return results;
    }
    if (kind == BenchmarkKind::store_durable_all) {
        for (const auto isolated : durable_benchmark_kinds()) {
            results.push_back(run_single(isolated, config, settings));
        }
        return results;
    }
    if (kind == BenchmarkKind::store_durable_parallel_all) {
        for (const auto isolated : durable_parallel_benchmark_kinds()) {
            results.push_back(run_single(isolated, config, settings));
        }
        return results;
    }
    if (kind == BenchmarkKind::store_durable_periodic_all) {
        for (const auto isolated : durable_periodic_benchmark_kinds()) {
            results.push_back(run_single(isolated, config, settings));
        }
        return results;
    }
    if (kind == BenchmarkKind::store_durable_group_all) {
        for (const auto isolated : durable_group_benchmark_kinds()) {
            results.push_back(run_single(isolated, config, settings));
        }
        return results;
    }
    results.push_back(run_single(kind, config, settings));
    return results;
}

auto quick_configs() -> std::vector<Config> {
    return {Config{.operations = kDefaultOperations,
                   .key_size = 16,
                   .value_size = 64,
                   .workers = 1,
                   .random_access = false},
            Config{.operations = kDefaultOperations,
                   .key_size = 16,
                   .value_size = 64,
                   .workers = 1,
                   .random_access = true}};
}

auto suite_configs() -> std::vector<Config> {
    std::vector<Config> configs;
    for (const std::size_t key_size : {8U, 16U, 32U, 64U, 256U}) {
        for (const std::size_t value_size : {0U, 64U, 256U}) {
            for (const bool random_access : {false, true}) {
                configs.push_back(Config{.operations = kDefaultOperations,
                                         .key_size = key_size,
                                         .value_size = value_size,
                                         .workers = 1,
                                         .random_access = random_access});
            }
        }
    }
    return configs;
}

} // namespace glifistore::bench
