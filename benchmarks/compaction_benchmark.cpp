#include "benchmark_metadata.hpp"
#include "glifistore/core/error.hpp"
#include "glifistore/store/config.hpp"
#include "glifistore/store/store.hpp"
#include "parse.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint64_t kInitialNowNs{100};
constexpr std::uint64_t kExpiredNowNs{300};
constexpr std::uint64_t kExpireAtNs{200};

struct Options {
    std::size_t warmups{1};
    std::size_t repeats{3};
    std::size_t value_bytes{std::size_t{256} * 1024U};
    std::optional<std::string> scenario;
};

struct Scenario {
    std::string_view name;
    std::size_t operations;
    std::size_t live_keys;
    bool expire_even_keys{};
    bool expect_compacted{};
};

struct SegmentFootprint {
    std::size_t count{};
    std::uint64_t logical_bytes{};
    std::uint64_t allocated_bytes{};
};

struct Sample {
    Scenario scenario;
    std::size_t repeat{};
    double seed_seconds{};
    double compact_seconds{};
    double reopen_seconds{};
    double verify_seconds{};
    bool compacted{};
    SegmentFootprint before;
    SegmentFootprint after;
    glifistore::CompactionResult compaction;
};

class BenchmarkClock final : public glifistore::StoreClock {
  public:
    [[nodiscard]] auto now_ns() const noexcept -> std::uint64_t override {
        return now_ns_.load(std::memory_order_relaxed);
    }

    void set(const std::uint64_t value) noexcept {
        now_ns_.store(value, std::memory_order_relaxed);
    }

  private:
    std::atomic<std::uint64_t> now_ns_{kInitialNowNs};
};

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        auto pattern =
            (std::filesystem::temp_directory_path() / "glifistore-compaction-bench-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* created = ::mkdtemp(writable.data());
        if (created == nullptr) {
            throw std::runtime_error("failed to create compaction benchmark directory");
        }
        root_ = created;
        path_ = root_ / "store";
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    auto operator=(const TemporaryDirectory&) -> TemporaryDirectory& = delete;

    [[nodiscard]] auto path() const -> const std::filesystem::path& {
        return path_;
    }

  private:
    std::filesystem::path root_;
    std::filesystem::path path_;
};

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
        } else if (argument == "--repeats" && index + 1 < argc) {
            options.repeats = parse_size(argv[++index], argument);
        } else if (argument == "--value-bytes" && index + 1 < argc) {
            options.value_bytes = parse_size(argv[++index], argument);
        } else if (argument == "--scenario" && index + 1 < argc) {
            options.scenario = argv[++index];
        } else if (argument == "--help" || argument == "-h") {
            std::cout << "usage: glifistore_compaction_benchmark [--warmup N] [--repeats N]"
                         " [--value-bytes N]"
                         " [--scenario high-reclaim|medium-reclaim|low-reclaim|"
                         "copy-heavy|ttl-50|no-gain]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown or incomplete argument: " + std::string{argument});
        }
    }
    if (options.repeats == 0 || options.value_bytes < sizeof(std::uint64_t)) {
        throw std::runtime_error("repeats must be nonzero and value-bytes must hold a sequence marker");
    }
    return options;
}

[[nodiscard]] auto seconds_since(const Clock::time_point start) -> double {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

[[nodiscard]] auto benchmark_key(const std::size_t index) -> std::string {
    return "reclaim-key-" + std::to_string(1'000'000U + index).substr(1);
}

[[nodiscard]] auto segment_footprint(const std::filesystem::path& directory) -> SegmentFootprint {
    SegmentFootprint footprint;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const auto name = entry.path().filename().string();
        if (!name.starts_with("segment-") || !name.ends_with(".glifi")) {
            continue;
        }
        struct stat status{};
        if (::stat(entry.path().c_str(), &status) != 0) {
            throw std::runtime_error("failed to stat benchmark Segment");
        }
        ++footprint.count;
        footprint.logical_bytes += static_cast<std::uint64_t>(status.st_size);
        footprint.allocated_bytes += static_cast<std::uint64_t>(status.st_blocks) * 512U;
    }
    return footprint;
}

[[nodiscard]] auto store_config(const std::filesystem::path& directory,
                                const std::shared_ptr<BenchmarkClock>& clock,
                                const glifistore::DurableOpenMode open_mode) -> glifistore::StoreConfig {
    glifistore::StoreConfig config{
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_periodic,
        .data_directory = directory,
        .durable_open_mode = open_mode,
        .durable_periodic =
            {
                .sync_interval_ms = 60'000,
                .batch =
                    glifistore::DurableGroupConfig{
                        .max_records = 4096,
                        .max_bytes = 32U * 1024U * 1024U,
                        .max_wait_ms = 60'000,
                    },
            },
        .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
        .clock = clock,
    };
    return config;
}

[[nodiscard]] auto open_store(const std::filesystem::path& directory,
                              const std::shared_ptr<BenchmarkClock>& clock,
                              const glifistore::DurableOpenMode open_mode)
    -> std::unique_ptr<glifistore::Store> {
    auto opened = glifistore::Store::open(store_config(directory, clock, open_mode));
    if (!opened) {
        throw std::runtime_error("failed to open benchmark Store: " + opened.error().message);
    }
    return std::move(*opened);
}

void require_status(const glifistore::Status& status, const std::string_view operation) {
    if (!status) {
        throw std::runtime_error(std::string{operation} + " failed: " + status.error().message);
    }
}

void verify_model(glifistore::Store& store, const Scenario& scenario,
                  const std::vector<std::uint64_t>& expected_markers) {
    require_status(store.verify_index(), "verify_index");
    for (std::size_t key_index = 0; key_index < scenario.live_keys; ++key_index) {
        const auto found = store.get(benchmark_key(key_index));
        if (scenario.expire_even_keys && key_index % 2U == 0) {
            if (found || found.error().code != glifistore::ErrorCode::not_found) {
                throw std::runtime_error("expired benchmark key remained visible");
            }
            continue;
        }
        if (!found || found->bytes.size() < sizeof(std::uint64_t)) {
            throw std::runtime_error("visible benchmark key is missing or truncated");
        }
        std::uint64_t marker{};
        std::memcpy(&marker, found->bytes.data(), sizeof(marker));
        if (marker != expected_markers[key_index]) {
            throw std::runtime_error("benchmark value marker does not match the latest mutation");
        }
    }
}

[[nodiscard]] auto run_sample(const Scenario scenario, const std::size_t repeat,
                              const std::size_t value_bytes) -> Sample {
    TemporaryDirectory directory;
    auto clock = std::make_shared<BenchmarkClock>();
    auto store = open_store(directory.path(), clock, glifistore::DurableOpenMode::create_new);

    std::vector<std::byte> value(value_bytes);
    for (std::size_t index = 0; index < value.size(); ++index) {
        value[index] = std::byte{static_cast<unsigned char>((index * 131U + 17U) & 0xFFU)};
    }
    std::vector<std::uint64_t> expected_markers(scenario.live_keys);

    const auto seed_start = Clock::now();
    for (std::size_t operation = 0; operation < scenario.operations; ++operation) {
        const auto key_index = operation % scenario.live_keys;
        const auto marker = static_cast<std::uint64_t>(operation);
        std::memcpy(value.data(), &marker, sizeof(marker));
        const auto expire_at =
            scenario.expire_even_keys && key_index % 2U == 0 ? kExpireAtNs : std::uint64_t{0};
        require_status(store->put(benchmark_key(key_index), value, expire_at), "seed put");
        expected_markers[key_index] = marker;
    }
    require_status(store->flush(), "seed flush");
    const auto seed_seconds = seconds_since(seed_start);
    clock->set(kExpiredNowNs);

    const auto before = segment_footprint(directory.path());
    const auto compact_start = Clock::now();
    auto compacted = store->compact();
    const auto compact_seconds = seconds_since(compact_start);
    if (!compacted) {
        throw std::runtime_error("compaction failed: " + compacted.error().message);
    }
    if (compacted->compacted != scenario.expect_compacted) {
        throw std::runtime_error("compaction usefulness differed from benchmark scenario");
    }
    const auto result = *compacted;
    require_status(store->close(), "Store close");
    store.reset();

    const auto after = segment_footprint(directory.path());
    const auto reopen_start = Clock::now();
    auto reopened = open_store(directory.path(), clock, glifistore::DurableOpenMode::open_existing);
    const auto reopen_seconds = seconds_since(reopen_start);
    const auto verify_start = Clock::now();
    verify_model(*reopened, scenario, expected_markers);
    const auto verify_seconds = seconds_since(verify_start);
    require_status(reopened->close(), "reopened Store close");

    return {
        .scenario = scenario,
        .repeat = repeat,
        .seed_seconds = seed_seconds,
        .compact_seconds = compact_seconds,
        .reopen_seconds = reopen_seconds,
        .verify_seconds = verify_seconds,
        .compacted = result.compacted,
        .before = before,
        .after = after,
        .compaction = result,
    };
}

[[nodiscard]] auto mib(const std::uint64_t bytes) -> double {
    return static_cast<double>(bytes) / static_cast<double>(1U << 20U);
}

[[nodiscard]] auto safe_rate(const std::uint64_t bytes, const double seconds) -> double {
    return seconds > 0.0 ? mib(bytes) / seconds : 0.0;
}

void print_sample(const Sample& sample, const std::size_t value_bytes) {
    const auto logical_reclaimed = sample.before.logical_bytes >= sample.after.logical_bytes
                                       ? sample.before.logical_bytes - sample.after.logical_bytes
                                       : std::uint64_t{0};
    const auto allocated_reclaimed = sample.before.allocated_bytes >= sample.after.allocated_bytes
                                         ? sample.before.allocated_bytes - sample.after.allocated_bytes
                                         : std::uint64_t{0};
    const auto dropped = sample.compaction.source_records_verified >= sample.compaction.records_copied
                             ? sample.compaction.source_records_verified - sample.compaction.records_copied
                             : std::uint64_t{0};
    std::cout << sample.scenario.name << ',' << sample.repeat << ',' << sample.scenario.operations << ','
              << sample.scenario.live_keys << ',' << value_bytes << ',' << (sample.compacted ? 1 : 0) << ','
              << sample.seed_seconds << ',' << sample.compact_seconds * 1'000.0 << ','
              << sample.reopen_seconds * 1'000.0 << ',' << sample.verify_seconds * 1'000.0 << ','
              << sample.before.count << ',' << sample.after.count << ',' << mib(sample.before.logical_bytes)
              << ',' << mib(sample.after.logical_bytes) << ',' << mib(logical_reclaimed) << ','
              << mib(sample.before.allocated_bytes) << ',' << mib(sample.after.allocated_bytes) << ','
              << mib(allocated_reclaimed) << ',' << sample.compaction.source_records_verified << ','
              << mib(sample.compaction.source_bytes_verified) << ',' << sample.compaction.records_copied
              << ',' << mib(sample.compaction.bytes_copied) << ',' << dropped << ','
              << sample.compaction.expired_records_dropped << ','
              << static_cast<double>(sample.compaction.pre_intent_duration_ns) / 1'000'000.0 << ','
              << static_cast<double>(sample.compaction.publication_lease_duration_ns) / 1'000'000.0 << ','
              << static_cast<double>(sample.compaction.pacing_delay_ns) / 1'000'000.0 << ','
              << sample.compaction.pacing_sleep_count << ',' << sample.compaction.pacing_burst_bytes << ','
              << mib(sample.compaction.transient_metadata_lower_bound_bytes) << ','
              << safe_rate(sample.compaction.source_bytes_verified, sample.compact_seconds) << ','
              << safe_rate(sample.compaction.bytes_copied, sample.compact_seconds) << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        constexpr std::array<Scenario, 6> scenarios{
            Scenario{.name = "high-reclaim", .operations = 1024, .live_keys = 128, .expect_compacted = true},
            Scenario{
                .name = "medium-reclaim", .operations = 1024, .live_keys = 512, .expect_compacted = true},
            Scenario{.name = "low-reclaim", .operations = 1024, .live_keys = 768, .expect_compacted = true},
            Scenario{.name = "copy-heavy", .operations = 1536, .live_keys = 1024, .expect_compacted = true},
            Scenario{.name = "ttl-50",
                     .operations = 1024,
                     .live_keys = 1024,
                     .expire_even_keys = true,
                     .expect_compacted = true},
            Scenario{.name = "no-gain", .operations = 1024, .live_keys = 1024, .expect_compacted = false},
        };

        std::cout << "# benchmark=glifistore_durable_compaction\n";
        glifistore::bench::print_common_metadata(std::cout, options.warmups, options.repeats);
        std::cout << "# storage_mode=durable-periodic;seed_flush_before_measurement=true\n";
        std::cout << "# compaction_scope=public Store::compact;one Worker;maintenance disabled\n";
        std::cout << std::fixed << std::setprecision(6);
        std::cout << "scenario,repeat,operations,live_keys,value_bytes,compacted,seed_s,compact_ms,reopen_ms,"
                     "verify_ms,segments_before,segments_after,logical_mib_before,logical_mib_after,"
                     "logical_mib_reclaimed,allocated_mib_before,allocated_mib_after,"
                     "allocated_mib_reclaimed,source_records_verified,source_mib_verified,records_copied,"
                     "copied_mib,records_dropped,expired_records_dropped,pre_intent_ms,"
                     "publication_lease_ms,pacing_delay_ms,pacing_sleep_count,pacing_burst_bytes,"
                     "transient_metadata_lower_bound_mib,effective_scan_mib_s,"
                     "copy_mib_s\n";

        std::vector<Scenario> selected;
        for (const auto scenario : scenarios) {
            if (options.scenario && *options.scenario != scenario.name) {
                continue;
            }
            selected.push_back(scenario);
        }
        if (selected.empty()) {
            throw std::runtime_error("unknown compaction benchmark scenario");
        }
        for (std::size_t warmup = 0; warmup < options.warmups; ++warmup) {
            const auto offset = warmup % selected.size();
            for (std::size_t index = 0; index < selected.size(); ++index) {
                const auto scenario = selected[(index + offset) % selected.size()];
                static_cast<void>(run_sample(scenario, 0, options.value_bytes));
            }
        }
        for (std::size_t repeat = 1; repeat <= options.repeats; ++repeat) {
            const auto offset = (repeat - 1U) % selected.size();
            for (std::size_t index = 0; index < selected.size(); ++index) {
                const auto scenario = selected[(index + offset) % selected.size()];
                print_sample(run_sample(scenario, repeat, options.value_bytes), options.value_bytes);
            }
        }
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "compaction benchmark error: " << exception.what() << '\n';
        return 1;
    }
}
