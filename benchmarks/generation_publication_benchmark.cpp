#include "benchmark_metadata.hpp"
#include "experimental/generation_slot_pool.hpp"
#include "experimental/pair_read_generation_shell.hpp"
#include "glifistore/server/thread_affinity.hpp"
#include "glifistore/store/paired/read_generation.hpp"
#include "parse.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Generation = glifistore::store::paired::PairReadGeneration;
using Mutation = glifistore::store::paired::ReadMutation;
using SharedPool = glifistore::experimental::GenerationSlotPool<Generation, 65>;
using DirectPool = glifistore::experimental::PairReadGenerationDirectSlotPool<65>;
using PublishStatus = glifistore::experimental::GenerationSlotPublishStatus;

struct Options final {
    std::size_t operations{20'000};
    std::size_t warmup{2};
    std::size_t repeats{9};
    bool affinity{true};
    bool reader_get{};
};

[[nodiscard]] auto parse_positive(const char* text) -> std::size_t {
    if (text == nullptr) {
        throw std::invalid_argument{"missing numeric argument"};
    }
    const std::string_view input{text};
    const auto value = glifistore::bench::parse_decimal_size(input);
    if (!value || *value == 0U) {
        throw std::invalid_argument{"numeric argument must be positive"};
    }
    return *value;
}

[[nodiscard]] auto parse_options(const int argc, char** argv) -> Options {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--help" || argument == "-h") {
            std::cout << "usage: glifistore_generation_publication_benchmark "
                         "[--ops N] [--warmup N] [--repeats N] [--no-affinity] "
                         "[--reader-work adopt|get]\n";
            std::exit(0);
        }
        if (argument == "--no-affinity") {
            options.affinity = false;
            continue;
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument{"missing argument value"};
        }
        if (argument == "--ops") {
            options.operations = parse_positive(argv[++index]);
        } else if (argument == "--warmup") {
            options.warmup = parse_positive(argv[++index]);
        } else if (argument == "--repeats") {
            options.repeats = parse_positive(argv[++index]);
        } else if (argument == "--reader-work") {
            const std::string_view value{argv[++index]};
            if (value != "adopt" && value != "get") {
                throw std::invalid_argument{"reader work must be adopt or get"};
            }
            options.reader_get = value == "get";
        } else {
            throw std::invalid_argument{"unknown argument: " + std::string{argument}};
        }
    }
    if (options.operations > Generation::kMaximumIncrementalDeltaEntries) {
        throw std::invalid_argument{"operation count exceeds the incremental delta bound"};
    }
    return options;
}

[[nodiscard]] auto bytes(const std::string_view text) noexcept -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

struct Material final {
    glifistore::WorkerRoutingState routing{};
    std::shared_ptr<glifistore::Segment> segment;
    std::string key{"generation-publication-affine"};
    std::uint64_t hash{};
    std::vector<glifistore::RecordRef> records;
};

[[nodiscard]] auto make_material(const std::size_t operations) -> Material {
    Material material;
    material.segment = std::make_shared<glifistore::Segment>(glifistore::SegmentId{702});
    material.hash = glifistore::hash_key_routing(material.key, material.routing);
    material.records.reserve(operations);
    const std::array value{std::byte{0x51}, std::byte{0x53}};
    for (std::size_t index = 0; index < operations; ++index) {
        auto record = material.segment->append({.sequence = glifistore::SequenceNumber{index + 1U},
                                                .opcode = glifistore::Opcode::put,
                                                .key_hash = material.hash,
                                                .key = bytes(material.key),
                                                .value = value});
        if (!record) {
            throw std::runtime_error{"cannot prepare publication benchmark Segment"};
        }
        material.records.push_back(*record);
    }
    return material;
}

struct alignas(128) ThreadResult final {
    glifistore::server::ExecutorAffinityResult affinity{};
    std::uint64_t operations{};
    std::uint64_t epoch_skips{};
    std::uint64_t checksum{};
    bool failed{};
};

struct Measurement final {
    double seconds{};
    std::uint64_t writer_operations{};
    std::uint64_t reader_adoptions{};
    std::uint64_t reader_epoch_skips{};
    std::uint64_t checksum{};
    std::uint64_t pool_exhaustions{};
    std::uint64_t live_high_watermark{};
    double publication_p50_ns{};
    double publication_p99_ns{};
    double reader_get_p50_ns{};
    double reader_get_p99_ns{};
    glifistore::server::ExecutorAffinityResult reader_affinity{};
    glifistore::server::ExecutorAffinityResult writer_affinity{};
};

[[nodiscard]] auto percentile(std::vector<std::uint64_t> samples, const double quantile) -> double {
    if (samples.empty()) {
        return 0.0;
    }
    std::sort(samples.begin(), samples.end());
    const auto index = static_cast<std::size_t>(std::min<double>(
        static_cast<double>(samples.size() - 1U), quantile * static_cast<double>(samples.size() - 1U)));
    return static_cast<double>(samples[index]);
}

template <typename Pool, typename Publish>
[[nodiscard]] auto run_threads(Pool& pool, const Material& material, const bool affinity,
                               const bool reader_get, Publish&& publish) -> Measurement {
    std::atomic_bool writer_done{};
    std::atomic_bool reader_done{};
    ThreadResult reader_result;
    ThreadResult writer_result;
    std::vector<std::uint64_t> publication_samples;
    publication_samples.reserve(material.records.size() / 64U + 1U);
    std::vector<std::uint64_t> reader_get_samples;
    reader_get_samples.reserve(material.records.size());
    Clock::time_point started{};
    std::barrier start{3, [&] noexcept { started = Clock::now(); }};

    std::thread reader([&] {
        reader_result.affinity = glifistore::server::configure_executor_thread(0, affinity);
        start.arrive_and_wait();
        std::uint64_t previous_epoch{};
        while (!writer_done.load(std::memory_order_acquire) || previous_epoch < material.records.size()) {
            const auto* adopted = pool.adopt();
            if (adopted == nullptr || adopted->visible_through() != adopted->epoch() ||
                adopted->epoch() < previous_epoch) {
                reader_result.failed = true;
                break;
            }
            if (adopted->epoch() != previous_epoch) {
                reader_result.epoch_skips += adopted->epoch() - previous_epoch - 1U;
                previous_epoch = adopted->epoch();
            }
            if (reader_get) {
                const auto sampled = (reader_result.operations & 255U) == 0U;
                const auto sample_start = sampled ? Clock::now() : Clock::time_point{};
                const auto found = adopted->get({material.key, material.hash}, 0);
                if (!found) {
                    // Epoch zero legitimately has no mutation yet.
                    if (adopted->epoch() != 0U) {
                        reader_result.failed = true;
                        break;
                    }
                } else if (sampled) {
                    reader_get_samples.push_back(static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - sample_start)
                            .count()));
                }
            }
            ++reader_result.operations;
        }
        const auto* final = pool.adopt();
        if (final == nullptr || final->epoch() != material.records.size() ||
            !final->get({material.key, material.hash}, 0)) {
            reader_result.failed = true;
        } else {
            reader_result.checksum = final->epoch() + final->visible_through();
        }
        if (!pool.mark_reader_quiescent()) {
            reader_result.failed = true;
        }
        reader_done.store(true, std::memory_order_release);
    });

    std::thread writer([&] {
        writer_result.affinity = glifistore::server::configure_executor_thread(1, affinity);
        start.arrive_and_wait();
        for (std::size_t index = 0; index < material.records.size(); ++index) {
            const Mutation mutation{.key = {material.key, material.hash},
                                    .record = material.records[index],
                                    .segment = material.segment,
                                    .opcode = glifistore::Opcode::put};
            const auto sampled = (index & 63U) == 0U;
            const auto sample_start = sampled ? Clock::now() : Clock::time_point{};
            for (;;) {
                auto reservation = pool.try_reserve();
                if (!reservation) {
                    std::this_thread::yield();
                    continue;
                }
                reservation->mark_store_linearized();
                if (publish(*reservation, std::span{&mutation, 1}) != PublishStatus::published) {
                    writer_result.failed = true;
                }
                break;
            }
            if (writer_result.failed) {
                break;
            }
            if (sampled) {
                publication_samples.push_back(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - sample_start)
                        .count()));
            }
            ++writer_result.operations;
        }
        pool.stop_admission();
        writer_done.store(true, std::memory_order_release);
        while (!reader_done.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        if (!pool.try_finish_shutdown()) {
            writer_result.failed = true;
        }
    });

    start.arrive_and_wait();
    writer.join();
    reader.join();
    const auto elapsed = Clock::now() - started;
    if (reader_result.failed || writer_result.failed) {
        throw std::runtime_error{"two-thread publication protocol failed"};
    }
    const auto stats = pool.stats();
    return {.seconds = std::chrono::duration<double>(elapsed).count(),
            .writer_operations = writer_result.operations,
            .reader_adoptions = reader_result.operations,
            .reader_epoch_skips = reader_result.epoch_skips,
            .checksum = reader_result.checksum,
            .pool_exhaustions = stats.pool_exhaustions,
            .live_high_watermark = stats.live_high_watermark,
            .publication_p50_ns = percentile(publication_samples, 0.50),
            .publication_p99_ns = percentile(publication_samples, 0.99),
            .reader_get_p50_ns = percentile(reader_get_samples, 0.50),
            .reader_get_p99_ns = percentile(reader_get_samples, 0.99),
            .reader_affinity = reader_result.affinity,
            .writer_affinity = writer_result.affinity};
}

[[nodiscard]] auto run_shared(const Material& material, const bool affinity, const bool reader_get)
    -> Measurement {
    auto initial = Generation::empty(material.routing);
    if (!initial) {
        throw std::runtime_error{"cannot create shared initial generation"};
    }
    auto pool_result = SharedPool::create(std::move(*initial));
    if (!pool_result) {
        throw std::runtime_error{"cannot create shared generation pool"};
    }
    auto& pool = **pool_result;
    return run_threads(pool, material, affinity, reader_get,
                       [&](SharedPool::Reservation& reservation,
                           const std::span<const Mutation> mutations) -> PublishStatus {
                           auto next =
                               Generation::publish_incremental(pool.writer_generation_owner(), mutations);
                           if (!next) {
                               reservation.reset();
                               return PublishStatus::invalid_generation;
                           }
                           return pool.commit(reservation, std::move(*next));
                       });
}

[[nodiscard]] auto run_direct(const Material& material, const bool affinity, const bool reader_get)
    -> Measurement {
    auto pool_result = DirectPool::create(material.routing);
    if (!pool_result) {
        throw std::runtime_error{"cannot create direct generation pool"};
    }
    auto& pool = **pool_result;
    return run_threads(pool, material, affinity, reader_get,
                       [&](DirectPool::Reservation& reservation, const std::span<const Mutation> mutations)
                           -> PublishStatus { return pool.publish_incremental(reservation, mutations); });
}

void print(const std::string_view implementation, const std::size_t repeat, const Measurement& measurement) {
    const auto operations_per_second =
        static_cast<double>(measurement.writer_operations) / measurement.seconds;
    std::cout << implementation << '\t' << repeat << '\t' << std::fixed << std::setprecision(6)
              << measurement.seconds << '\t' << std::setprecision(0) << operations_per_second << '\t'
              << std::setprecision(2)
              << (measurement.seconds * 1'000'000'000.0 / static_cast<double>(measurement.writer_operations))
              << '\t' << measurement.publication_p50_ns << '\t' << measurement.publication_p99_ns << '\t'
              << measurement.reader_get_p50_ns << '\t' << measurement.reader_get_p99_ns << '\t'
              << measurement.reader_adoptions << '\t' << measurement.reader_epoch_skips << '\t'
              << measurement.pool_exhaustions << '\t' << measurement.live_high_watermark << '\t'
              << glifistore::server::affinity_mode_name(measurement.reader_affinity.mode) << '\t'
              << measurement.reader_affinity.cpu << '\t'
              << glifistore::server::affinity_mode_name(measurement.writer_affinity.mode) << '\t'
              << measurement.writer_affinity.cpu << '\t' << measurement.checksum << '\n';
}

} // namespace

int main(const int argc, char** argv) try {
    const auto options = parse_options(argc, argv);
    const auto material = make_material(options.operations);
    for (std::size_t index = 0; index < options.warmup; ++index) {
        static_cast<void>(run_shared(material, options.affinity, options.reader_get));
        static_cast<void>(run_direct(material, options.affinity, options.reader_get));
    }
    glifistore::bench::print_common_metadata(std::cout, options.warmup, options.repeats);
    std::cout << "implementation\trepeat\tseconds\tpublications_per_second\tns_per_publication\t"
                 "sample_p50_ns\tsample_p99_ns\treader_get_p50_ns\treader_get_p99_ns\t"
                 "reader_adoptions\treader_epoch_skips\t"
                 "pool_exhaustions\tlive_high_watermark\treader_affinity\treader_cpu\t"
                 "writer_affinity\twriter_cpu\tchecksum\n";
    for (std::size_t repeat = 0; repeat < options.repeats; ++repeat) {
        if ((repeat & 1U) == 0U) {
            print("shared_slot_protocol", repeat, run_shared(material, options.affinity, options.reader_get));
            print("direct_slot_protocol", repeat, run_direct(material, options.affinity, options.reader_get));
        } else {
            print("direct_slot_protocol", repeat, run_direct(material, options.affinity, options.reader_get));
            print("shared_slot_protocol", repeat, run_shared(material, options.affinity, options.reader_get));
        }
    }
    return 0;
} catch (const std::exception& error) {

    std::cerr << "generation publication benchmark error: " << error.what() << '\n';
    return 1;
}
