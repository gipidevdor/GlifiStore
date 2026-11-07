#include "benchmark_metadata.hpp"
#include "glifistore/store/store.hpp"
#include "harness.hpp"
#include "parse.hpp"
#include "store/store_internal.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct Options final {
    std::size_t entries{200'000};
    std::size_t key_bytes{16};
    std::size_t value_bytes{64};
    std::size_t workers{1};
    std::size_t hold_ms{};
    bool pressure_relief{};
};

[[nodiscard]] auto parse_size(const char* text) -> std::size_t {
    if (text == nullptr) {
        throw std::invalid_argument{"missing numeric argument"};
    }
    const std::string_view input{text};
    const auto value = glifistore::bench::parse_decimal_size(input);
    if (!value || *value == 0) {
        throw std::invalid_argument{"numeric argument is outside size_t"};
    }
    return *value;
}

[[nodiscard]] auto parse_options(const int argc, char** argv) -> Options {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--help" || argument == "-h") {
            std::cout << "usage: glifistore_memory_census_benchmark "
                         "[--entries N] [--key-bytes N] [--value-bytes N] [--workers N] "
                         "[--hold-ms N] [--pressure-relief]\n";
            std::exit(0);
        }
        if (argument == "--pressure-relief") {
            options.pressure_relief = true;
            continue;
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument{"missing argument value"};
        }
        if (argument == "--entries") {
            options.entries = parse_size(argv[++index]);
        } else if (argument == "--key-bytes") {
            options.key_bytes = parse_size(argv[++index]);
        } else if (argument == "--value-bytes") {
            options.value_bytes = parse_size(argv[++index]);
        } else if (argument == "--workers") {
            options.workers = parse_size(argv[++index]);
        } else if (argument == "--hold-ms") {
            options.hold_ms = parse_size(argv[++index]);
        } else {
            throw std::invalid_argument{"unknown argument: " + std::string{argument}};
        }
    }
    if (options.key_bytes < 16 || options.workers > glifistore::kMaximumWorkerCount ||
        options.hold_ms > 600'000U) {
        throw std::invalid_argument{
            "memory census requires key-bytes >= 16, a supported Worker count, and hold-ms <= 600000"};
    }
    return options;
}

[[nodiscard]] auto key_for(const std::size_t index, const std::size_t size) -> std::string {
    auto suffix = std::to_string(index);
    if (suffix.size() > size) {
        throw std::invalid_argument{"key size cannot represent every requested entry"};
    }
    std::string key(size, 'k');
    std::ranges::copy(suffix, key.end() - static_cast<std::ptrdiff_t>(suffix.size()));
    return key;
}

[[nodiscard]] auto saturating_add(const std::size_t left, const std::size_t right) noexcept -> std::size_t {
    return right > std::numeric_limits<std::size_t>::max() - left ? std::numeric_limits<std::size_t>::max()
                                                                  : left + right;
}

[[nodiscard]] auto saturating_multiply(const std::size_t left, const std::size_t right) noexcept
    -> std::size_t {
    return left != 0 && right > std::numeric_limits<std::size_t>::max() / left
               ? std::numeric_limits<std::size_t>::max()
               : left * right;
}

} // namespace

int main(const int argc, char** argv) try {
    const auto options = parse_options(argc, argv);
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = options.workers},
                                           .maintenance = {.mode = glifistore::MaintenanceMode::disabled}});
    if (!opened) {
        throw std::runtime_error{"cannot open volatile paired Store"};
    }
    auto& store = **opened;
    std::vector<std::byte> value(options.value_bytes, std::byte{0x5A});
    constexpr std::size_t kBatchSize = 32;
    for (std::size_t cursor = 0; cursor < options.entries;) {
        std::vector<std::string> keys;
        std::vector<glifistore::Store::PutItem> items;
        keys.reserve(kBatchSize);
        items.reserve(kBatchSize);
        for (; cursor < options.entries && keys.size() < kBatchSize; ++cursor) {
            keys.push_back(key_for(cursor, options.key_bytes));
        }
        for (const auto& key : keys) {
            items.push_back({.key = key, .value = value});
        }
        const auto statuses = store.put_batch(items);
        if (statuses.size() != items.size() ||
            !std::ranges::all_of(statuses, [](const auto& status) { return status.has_value(); })) {
            throw std::runtime_error{"memory census seed failed"};
        }
    }

    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    if (runtime == nullptr) {
        throw std::runtime_error{"paired runtime is unavailable"};
    }
    const auto pairs = runtime->stats();
    if (pairs.size() != options.workers) {
        throw std::runtime_error{"paired census Worker count mismatch"};
    }

    std::size_t mutable_index_bytes{};
    std::size_t mutable_index_entries{};
    for (std::size_t worker = 0; worker < options.workers; ++worker) {
        const auto index = glifistore::detail::StoreAccess::worker(store, worker).index().stats();
        mutable_index_entries = saturating_add(mutable_index_entries, index.size);
        mutable_index_bytes = saturating_add(
            mutable_index_bytes, saturating_add(index.table_allocated_bytes, index.arena_allocated_bytes));
    }

    std::size_t segment_count{};
    std::size_t segment_used_bytes{};
    std::size_t segment_live_bytes{};
    std::size_t segment_capacity_bytes{};
    for (const auto& segment : glifistore::detail::StoreAccess::segments(store)) {
        const auto stats = segment->stats();
        ++segment_count;
        segment_used_bytes = saturating_add(segment_used_bytes, stats.used_bytes);
        segment_live_bytes = saturating_add(segment_live_bytes, stats.live_bytes);
        segment_capacity_bytes = saturating_add(segment_capacity_bytes, segment->capacity());
    }

    std::size_t read_generation_bytes{};
    std::size_t read_generation_base_entries{};
    std::size_t read_generation_base_capacity{};
    std::size_t read_generation_base_record_bytes{};
    std::size_t read_generation_base_record_mapped_bytes{};
    std::size_t read_generation_base_lookup_bytes{};
    std::size_t read_generation_delta_entries{};
    std::size_t read_generation_delta_record_versions{};
    std::size_t read_generation_delta_bytes{};
    std::size_t retired_generation_count{};
    std::size_t active_read_merges{};
    std::size_t mutation_lane_storage_bytes{};
    for (const auto& pair : pairs) {
        const auto& memory = pair.read_generation_memory;
        read_generation_bytes =
            saturating_add(read_generation_bytes, memory.current_allocated_lower_bound_bytes);
        read_generation_base_entries = saturating_add(read_generation_base_entries, memory.base_entries);
        read_generation_base_capacity = saturating_add(read_generation_base_capacity, memory.base_capacity);
        read_generation_base_record_bytes =
            saturating_add(read_generation_base_record_bytes, memory.base_record_storage_bytes);
        read_generation_base_record_mapped_bytes =
            saturating_add(read_generation_base_record_mapped_bytes, memory.base_record_mapped_storage_bytes);
        read_generation_base_lookup_bytes =
            saturating_add(read_generation_base_lookup_bytes, memory.base_lookup_storage_bytes);
        read_generation_delta_entries = saturating_add(read_generation_delta_entries, memory.delta_entries);
        read_generation_delta_record_versions =
            saturating_add(read_generation_delta_record_versions, memory.delta_record_versions);
        read_generation_delta_bytes =
            saturating_add(read_generation_delta_bytes, memory.delta_allocated_lower_bound_bytes);
        retired_generation_count = saturating_add(retired_generation_count, pair.retired_generation_count);
        active_read_merges = saturating_add(active_read_merges, pair.read_merge_active ? 1U : 0U);
        mutation_lane_storage_bytes =
            saturating_add(mutation_lane_storage_bytes, pair.payload_arena_storage_bytes);
    }
    const auto read_generation_spare_mapping_bytes =
        glifistore::store::paired::immutable_base_spare_mapping_bytes();

    const auto logical_payload_bytes =
        saturating_multiply(options.entries, saturating_add(options.key_bytes, options.value_bytes));
    const auto old_lookup_counterfactual_bytes = saturating_multiply(read_generation_base_capacity, 17U);
    const auto compact_lookup_saved_bytes =
        old_lookup_counterfactual_bytes > read_generation_base_lookup_bytes
            ? old_lookup_counterfactual_bytes - read_generation_base_lookup_bytes
            : 0U;
    auto attributed_live_lower_bound_bytes = mutable_index_bytes;
    attributed_live_lower_bound_bytes = saturating_add(attributed_live_lower_bound_bytes, segment_used_bytes);
    attributed_live_lower_bound_bytes =
        saturating_add(attributed_live_lower_bound_bytes, read_generation_bytes);
    attributed_live_lower_bound_bytes =
        saturating_add(attributed_live_lower_bound_bytes, mutation_lane_storage_bytes);
    auto attributed_reserved_lower_bound_bytes = mutable_index_bytes;
    attributed_reserved_lower_bound_bytes =
        saturating_add(attributed_reserved_lower_bound_bytes, segment_capacity_bytes);
    attributed_reserved_lower_bound_bytes =
        saturating_add(attributed_reserved_lower_bound_bytes, read_generation_bytes);
    attributed_reserved_lower_bound_bytes =
        saturating_add(attributed_reserved_lower_bound_bytes, mutation_lane_storage_bytes);
    attributed_reserved_lower_bound_bytes =
        saturating_add(attributed_reserved_lower_bound_bytes, read_generation_spare_mapping_bytes);
    const auto non_allocator_mapping_bytes =
        saturating_add(read_generation_base_record_mapped_bytes, read_generation_spare_mapping_bytes);
    const auto attributed_allocator_payload_lower_bound_bytes =
        attributed_reserved_lower_bound_bytes >= non_allocator_mapping_bytes
            ? attributed_reserved_lower_bound_bytes - non_allocator_mapping_bytes
            : 0U;
    const auto process_before_relief = glifistore::bench::process_memory_snapshot();
    const auto allocator_before_relief = glifistore::bench::allocator_memory_snapshot();
    const auto pressure_relief = options.pressure_relief ? glifistore::bench::allocator_pressure_relief()
                                                         : glifistore::bench::AllocatorPressureReliefSample{};
    const auto process = glifistore::bench::process_memory_snapshot();
    const auto allocator = glifistore::bench::allocator_memory_snapshot();
    const auto unattributed_rss_bytes = process.rss_after_bytes > attributed_live_lower_bound_bytes
                                            ? process.rss_after_bytes - attributed_live_lower_bound_bytes
                                            : 0U;
    const auto unattributed_allocator_in_use_bytes =
        allocator.bytes_in_use > attributed_allocator_payload_lower_bound_bytes
            ? allocator.bytes_in_use - attributed_allocator_payload_lower_bound_bytes
            : 0U;
    const auto allocator_reserve_slack_bytes = allocator.bytes_reserved > allocator.bytes_in_use
                                                   ? allocator.bytes_reserved - allocator.bytes_in_use
                                                   : 0U;

    std::cout << "# glifistore paired memory census\n";
    glifistore::bench::print_common_metadata(std::cout, 0, 1);
    std::cout << "# accounting=allocation-payload-lower-bound-plus-process-rss\n";
    std::cout << "entries=" << options.entries << " key_bytes=" << options.key_bytes
              << " value_bytes=" << options.value_bytes << " workers=" << options.workers
              << " logical_payload_bytes=" << logical_payload_bytes
              << " mutable_index_entries=" << mutable_index_entries
              << " mutable_index_allocated_bytes=" << mutable_index_bytes
              << " segment_count=" << segment_count << " segment_used_bytes=" << segment_used_bytes
              << " segment_live_bytes=" << segment_live_bytes
              << " segment_capacity_bytes=" << segment_capacity_bytes
              << " read_generation_base_entries=" << read_generation_base_entries
              << " read_generation_base_capacity=" << read_generation_base_capacity
              << " read_generation_base_record_storage_bytes=" << read_generation_base_record_bytes
              << " read_generation_base_record_mapped_storage_bytes="
              << read_generation_base_record_mapped_bytes
              << " read_generation_base_lookup_storage_bytes=" << read_generation_base_lookup_bytes
              << " read_generation_delta_entries=" << read_generation_delta_entries
              << " read_generation_delta_record_versions=" << read_generation_delta_record_versions
              << " read_generation_delta_allocated_lower_bound_bytes=" << read_generation_delta_bytes
              << " read_generation_allocated_lower_bound_bytes=" << read_generation_bytes
              << " retired_generation_count=" << retired_generation_count
              << " active_read_merges=" << active_read_merges
              << " read_generation_spare_mapping_bytes=" << read_generation_spare_mapping_bytes
              << " old_base_lookup_counterfactual_bytes=" << old_lookup_counterfactual_bytes
              << " compact_lookup_saved_bytes=" << compact_lookup_saved_bytes
              << " mutation_lane_storage_bytes=" << mutation_lane_storage_bytes
              << " attributed_live_payload_lower_bound_bytes=" << attributed_live_lower_bound_bytes
              << " attributed_reserved_payload_lower_bound_bytes=" << attributed_reserved_lower_bound_bytes
              << " attributed_allocator_payload_lower_bound_bytes="
              << attributed_allocator_payload_lower_bound_bytes
              << " allocator_stats_available=" << (allocator.available ? 1 : 0)
              << " allocator_bytes_reserved_before_relief=" << allocator_before_relief.bytes_reserved
              << " allocator_blocks_in_use=" << allocator.blocks_in_use
              << " allocator_bytes_in_use=" << allocator.bytes_in_use
              << " allocator_peak_bytes_in_use=" << allocator.peak_bytes_in_use
              << " allocator_bytes_reserved=" << allocator.bytes_reserved
              << " unattributed_allocator_in_use_bytes=" << unattributed_allocator_in_use_bytes
              << " allocator_reserve_slack_bytes=" << allocator_reserve_slack_bytes
              << " pressure_relief_requested=" << (options.pressure_relief ? 1 : 0)
              << " pressure_relief_available=" << (pressure_relief.available ? 1 : 0)
              << " pressure_relief_released=" << (pressure_relief.released ? 1 : 0)
              << " pressure_relief_exact_released_bytes=" << (pressure_relief.exact_released_bytes ? 1 : 0)
              << " pressure_relief_released_bytes=" << pressure_relief.released_bytes
              << " process_rss_before_relief_bytes=" << process_before_relief.rss_after_bytes
              << " process_physical_footprint_before_relief_bytes="
              << process_before_relief.physical_footprint_bytes
              << " process_rss_bytes=" << process.rss_after_bytes
              << " process_peak_rss_bytes=" << process.peak_rss_bytes
              << " process_physical_footprint_bytes=" << process.physical_footprint_bytes
              << " process_peak_physical_footprint_bytes=" << process.peak_physical_footprint_bytes
              << " process_reusable_bytes=" << process.reusable_bytes
              << " process_internal_bytes=" << process.internal_bytes
              << " process_compressed_bytes=" << process.compressed_bytes
              << " unattributed_rss_bytes=" << unattributed_rss_bytes << '\n';
    if (options.hold_ms != 0) {
        std::cout << "# holding_process_for_vm_inspection_ms=" << options.hold_ms << '\n' << std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds{options.hold_ms});
    }
    if (!store.close()) {
        throw std::runtime_error{"memory census Store close failed"};
    }
    if (glifistore::store::paired::immutable_base_spare_mapping_bytes() != 0) {
        throw std::runtime_error{"immutable-base spare mapping survived Store close"};
    }
    return 0;
} catch (const std::exception& error) {

    std::cerr << "memory census benchmark error: " << error.what() << '\n';
    return 1;
}
