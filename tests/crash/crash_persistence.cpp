#include "crash_checkpoint.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/persistence/bootstrap.hpp"
#include "glifistore/persistence/recovery.hpp"
#include "glifistore/persistence/runtime_catalog.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "glifistore/segment/record.hpp"
#include "glifistore/store/config.hpp"
#include "glifistore/store/store.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view kSeedKey{"seed"};
constexpr std::string_view kSeedValue{"seed-value"};
constexpr std::string_view kCrashKey{"crash-key"};
constexpr std::string_view kCrashValue{"crash-value"};
constexpr std::string_view kRotateKey{"rotate-key"};
constexpr std::uint64_t kCompactionHistoryNowNs{1'000};
constexpr std::size_t kCompactionHistoryKeyCount{8};
// ADR 0044 selects the shortest prefix among equal estimated reclaim. A Segment holds 63 maximum
// Records, so the multi-output fixtures keep more than 63 live Records in the first two sealed
// Segments: only the complete three-Segment set (two outputs) then reclaims a Segment.
constexpr std::size_t kMultiOutputHistoryKeyCount{88};
constexpr std::array<std::size_t, 3> kMultiOutputRandomSegmentOperations{44, 44, 32};
constexpr std::size_t kMultiOutputRandomOperationCount{kMultiOutputHistoryKeyCount + 32U};
constexpr std::array<std::size_t, 3> kMultiOutputBuildGroupSizes{44, 30, 6};
constexpr std::size_t kMultiOutputBuildKeyCount{80};
constexpr std::size_t kMaxRandomCampaignIterations{10'000};
constexpr std::array<std::string_view, 9> kRandomCampaignBoundaries{
    "sync_directory#1", "write_record#1",     "write_record#32",  "preallocate_segment#2",
    "write_record#64",  "sync_commit_slot#4", "sync_directory#3", "remove_compaction_segment#2",
    "sync_directory#5",
};

[[nodiscard]] auto crash_run_suffix() -> const std::string& {
    static const auto suffix = std::to_string(static_cast<unsigned long long>(::getpid())) + "-" +
                               std::to_string(static_cast<unsigned long long>(
                                   std::chrono::steady_clock::now().time_since_epoch().count()));
    return suffix;
}

struct Options {
    std::string mode{"matrix"};
    std::string scenario{};
    std::string boundary{};
    std::string storage{"sync"};
    std::string checkpoint_action{"kill"};
    std::uint64_t history_seed{};
    std::optional<std::uint64_t> campaign_seed{};
    std::size_t campaign_iterations{};
    std::filesystem::path data_dir{};
    std::filesystem::path checkpoint_dir{};
    std::filesystem::path report_path{};
};

[[nodiscard]] auto parse_unsigned(const std::string_view value) -> std::optional<std::uint64_t> {
    std::uint64_t parsed{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

[[nodiscard]] auto parse_options(int argc, char** argv) -> std::optional<Options> {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg{argv[index]};
        const auto require_value = [&](const char* flag) -> std::optional<std::string_view> {
            if (arg != flag || index + 1 >= argc) {
                return std::nullopt;
            }
            return std::string_view{argv[++index]};
        };
        if (arg == "--mode") {
            const auto value = require_value("--mode");
            if (!value) {
                return std::nullopt;
            }
            options.mode = std::string{*value};
        } else if (arg == "--scenario") {
            const auto value = require_value("--scenario");
            if (!value) {
                return std::nullopt;
            }
            options.scenario = std::string{*value};
        } else if (arg == "--boundary") {
            const auto value = require_value("--boundary");
            if (!value) {
                return std::nullopt;
            }
            options.boundary = std::string{*value};
        } else if (arg == "--storage") {
            const auto value = require_value("--storage");
            if (!value) {
                return std::nullopt;
            }
            options.storage = std::string{*value};
        } else if (arg == "--checkpoint-action") {
            const auto value = require_value("--checkpoint-action");
            if (!value || (*value != "kill" && *value != "pause")) {
                return std::nullopt;
            }
            options.checkpoint_action = std::string{*value};
        } else if (arg == "--data-dir") {
            const auto value = require_value("--data-dir");
            if (!value) {
                return std::nullopt;
            }
            options.data_dir = *value;
        } else if (arg == "--checkpoint-dir") {
            const auto value = require_value("--checkpoint-dir");
            if (!value) {
                return std::nullopt;
            }
            options.checkpoint_dir = *value;
        } else if (arg == "--campaign-seed") {
            const auto value = require_value("--campaign-seed");
            if (!value) {
                return std::nullopt;
            }
            options.campaign_seed = parse_unsigned(*value);
            if (!options.campaign_seed) {
                return std::nullopt;
            }
        } else if (arg == "--iterations") {
            const auto value = require_value("--iterations");
            if (!value) {
                return std::nullopt;
            }
            const auto parsed = parse_unsigned(*value);
            if (!parsed || *parsed == 0 || *parsed > kMaxRandomCampaignIterations) {
                return std::nullopt;
            }
            options.campaign_iterations = static_cast<std::size_t>(*parsed);
        } else if (arg == "--report") {
            const auto value = require_value("--report");
            if (!value) {
                return std::nullopt;
            }
            options.report_path = *value;
        } else if (arg == "--help" || arg == "-h") {
            return std::nullopt;
        } else {
            return std::nullopt;
        }
    }
    return options;
}

void print_usage(const char* program) {
    std::cerr << "usage: " << program
              << " --mode {seed|worker|verify|matrix|copy-matrix|random-matrix|random-campaign|"
                 "pause-smoke|periodic-matrix|group-matrix} "
                 "[--scenario bootstrap|put|rotate|compact|compact-multi-build|compact-multi-random|"
                 "compact-multi-rollback|compact-multi-retire] "
                 "[--boundary OP]\n"
              << "       [--storage {sync|periodic|group}] [--data-dir PATH] [--checkpoint-dir PATH]\n"
              << "       worker only: [--checkpoint-action {kill|pause}]\n"
              << "       random-campaign: --campaign-seed UINT64 --iterations 1.."
              << kMaxRandomCampaignIterations << " [--report NEW_TSV]\n";
}

[[nodiscard]] auto periodic_storage(const Options& options) -> bool {
    return options.storage == "periodic";
}

[[nodiscard]] auto group_storage(const Options& options) -> bool {
    return options.storage == "group";
}

[[nodiscard]] auto durable_runtime_options(const Options& options) -> glifistore::DurableRuntimeOptions {
    if (group_storage(options)) {
        return {.commit_sync = glifistore::SegmentCommitSync::immediate,
                .sync_interval_ms = 50,
                .batch =
                    glifistore::DurableGroupConfig{.max_records = 32, .max_bytes = 65536, .max_wait_ms = 50},
                .strict_ack = true};
    }
    if (periodic_storage(options)) {
        return {.commit_sync = glifistore::SegmentCommitSync::deferred,
                .sync_interval_ms = 50,
                .batch =
                    glifistore::DurableGroupConfig{.max_records = 32, .max_bytes = 65536, .max_wait_ms = 50}};
    }
    return {};
}

[[nodiscard]] auto recovery_storage_mode(const Options& options) -> glifistore::StorageMode {
    if (group_storage(options)) {
        return glifistore::StorageMode::durable_group;
    }
    return periodic_storage(options) ? glifistore::StorageMode::durable_periodic
                                     : glifistore::StorageMode::durable_sync;
}

[[nodiscard]] auto open_runtime_for_worker(const Options& options, glifistore::FilesystemHooks hooks)
    -> std::unique_ptr<glifistore::DurableRuntimeCatalog> {
    auto directory = glifistore::DataDirectory::open_and_lock(
        options.data_dir, glifistore::DataDirectoryOpenMode::existing, hooks);
    if (!directory) {
        return nullptr;
    }
    const auto now_ns = options.scenario == "compact" || options.scenario == "compact-multi-random"
                            ? kCompactionHistoryNowNs
                            : std::uint64_t{0};
    auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory), now_ns,
                                                                  durable_runtime_options(options));
    if (!runtime) {
        return nullptr;
    }
    return std::move(*runtime);
}

[[nodiscard]] auto bytes(std::string_view value) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

[[nodiscard]] auto owned_text(const glifistore::OwnedValue& value) -> std::string {
    return {reinterpret_cast<const char*>(value.bytes.data()), value.bytes.size()};
}

[[nodiscard]] auto recovery_store_id() -> glifistore::StoreId {
    return {std::byte{0x20}, std::byte{0x21}, std::byte{0x22}, std::byte{0x23},
            std::byte{0x24}, std::byte{0x25}, std::byte{0x26}, std::byte{0x27},
            std::byte{0x28}, std::byte{0x29}, std::byte{0x2A}, std::byte{0x2B},
            std::byte{0x2C}, std::byte{0x2D}, std::byte{0x2E}, std::byte{0x2F}};
}

void append_record(glifistore::DurableSegmentFile& file, const std::uint64_t sequence,
                   const std::string_view key, const std::string_view value,
                   const glifistore::Opcode opcode = glifistore::Opcode::put,
                   const std::uint64_t expire_at_ns = 0, const bool commit_immediately = true) {
    const auto encoded = glifistore::encode_record({
        .sequence = glifistore::SequenceNumber{sequence},
        .opcode = opcode,
        .type = glifistore::ValueType::bytes,
        .flags = 0,
        .key_hash = glifistore::hash_key(key),
        .expire_at_ns = expire_at_ns,
        .key = bytes(key),
        .value = bytes(value),
    });
    if (!encoded) {
        throw std::runtime_error("failed to encode durable test record");
    }
    const auto appended = commit_immediately ? file.append(*encoded) : file.append_record(*encoded);
    if (!appended.committed()) {
        throw std::runtime_error("failed to append durable test record");
    }
}

struct CompactionHistoryOperation {
    std::uint64_t sequence{};
    glifistore::Opcode opcode{glifistore::Opcode::put};
    std::size_t key_index{};
    std::string value;
    std::uint64_t expire_at_ns{};
};

struct CompactionHistory {
    std::vector<std::string> keys;
    std::vector<std::vector<CompactionHistoryOperation>> segments;
    std::vector<std::optional<std::string>> expected;
};

[[nodiscard]] auto generated_compaction_history() -> CompactionHistory {
    CompactionHistory history;
    history.keys.reserve(kCompactionHistoryKeyCount);
    history.segments.resize(3);
    history.expected.resize(kCompactionHistoryKeyCount);
    for (std::size_t index = 0; index < kCompactionHistoryKeyCount; ++index) {
        history.keys.push_back("crash-history-key-" + std::to_string(index));
    }

    std::uint64_t next_sequence{1};
    const auto append = [&](const std::size_t segment_index, const std::size_t key_index,
                            const glifistore::Opcode opcode, std::string value,
                            const std::uint64_t expire_at_ns) {
        history.segments[segment_index].push_back({.sequence = next_sequence++,
                                                   .opcode = opcode,
                                                   .key_index = key_index,
                                                   .value = std::move(value),
                                                   .expire_at_ns = expire_at_ns});
        if (opcode == glifistore::Opcode::erase ||
            (expire_at_ns != 0 && expire_at_ns <= kCompactionHistoryNowNs)) {
            history.expected[key_index].reset();
        } else {
            history.expected[key_index] = history.segments[segment_index].back().value;
        }
    };
    for (std::size_t key_index = 0; key_index < kCompactionHistoryKeyCount; ++key_index) {
        append(0, key_index, glifistore::Opcode::put, "baseline-" + std::to_string(key_index), 0);
    }

    std::mt19937_64 random{0xC2A5'4F17'5EED'2026ULL};
    const auto append_generated = [&](const std::size_t segment_index, const std::size_t count,
                                      const std::size_t key_space) {
        for (std::size_t operation = 0; operation < count; ++operation) {
            const auto key_index = static_cast<std::size_t>(random() % key_space);
            const auto choice = random() % 6U;
            if (choice == 0) {
                append(segment_index, key_index, glifistore::Opcode::erase, {}, 0);
            } else {
                const auto expiry = choice == 1 ? kCompactionHistoryNowNs - 1U : 0U;
                append(segment_index, key_index, glifistore::Opcode::put,
                       "history-value-" + std::to_string(next_sequence) + "-" + std::to_string(random()),
                       expiry);
            }
        }
    };
    append_generated(0, 6, kCompactionHistoryKeyCount);
    append_generated(1, 10, kCompactionHistoryKeyCount);
    append_generated(2, 6, 3);
    return history;
}

void seed_put_store(const std::filesystem::path& data_dir) {
    std::filesystem::create_directories(data_dir.parent_path());
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                           .concurrency = glifistore::StoreConcurrencyMode::legacy_mutex,
                                           .storage_mode = glifistore::StorageMode::durable_sync,
                                           .data_directory = data_dir,
                                           .durable_open_mode = glifistore::DurableOpenMode::create_new});
    if (!opened) {
        throw std::runtime_error(std::string{"failed to seed durable store: "} + opened.error().message);
    }
    if (!(*opened)->put(kSeedKey, bytes(kSeedValue)).has_value()) {
        throw std::runtime_error("failed to seed durable key");
    }
}

void seed_rotate_store(const std::filesystem::path& data_dir) {
    std::filesystem::create_directories(data_dir.parent_path());
    const auto store_id = recovery_store_id();
    const glifistore::ManifestSegmentEntry active{
        .segment_id = glifistore::SegmentId{1},
        .generation = glifistore::GenerationId{1},
        .owner_worker = glifistore::WorkerId{0},
        .role = glifistore::ManifestSegmentRole::active,
    };
    const std::string fill_key{"fill"};
    const std::string maximum_value(
        glifistore::kMaxNormalRecordSize - glifistore::kEncodedRecordHeaderSize - fill_key.size(), 'x');
    {
        auto directory = glifistore::DataDirectory::open_and_lock(
            data_dir, glifistore::DataDirectoryOpenMode::open_or_create);
        if (!directory) {
            throw std::runtime_error("failed to open data directory for rotate seed");
        }
        auto created =
            glifistore::DurableSegmentFile::create(*directory, {.store_id = store_id,
                                                                .segment_id = active.segment_id,
                                                                .generation = active.generation,
                                                                .owner_worker = active.owner_worker});
        if (!created.durable() || !created.file) {
            throw std::runtime_error("failed to create rotate seed segment");
        }
        for (std::uint64_t sequence = 1; sequence <= 63; ++sequence) {
            append_record(*created.file, sequence, fill_key, maximum_value);
        }
        const glifistore::Manifest manifest{
            .store_id = store_id,
            .manifest_generation = 1,
            .routing_algorithm = glifistore::RoutingAlgorithm::fnv1a64_v1,
            .worker_count = 1,
            .routing_epoch = 1,
            .next_segment_id = glifistore::SegmentId{2},
            .next_segment_generation = glifistore::GenerationId{1},
            .segments = {active},
        };
        if (!directory->publish_manifest(manifest).durable()) {
            throw std::runtime_error("failed to publish rotate seed manifest");
        }
    }
    auto runtime = glifistore::DurableRuntimeCatalog::open_existing(data_dir);
    if (!runtime) {
        throw std::runtime_error("failed to open rotate seed runtime");
    }
    if (!(*runtime)->put(bytes(kSeedKey), bytes(kSeedValue)).committed()) {
        throw std::runtime_error("failed to seed rotate baseline key");
    }
}

void seed_compaction_store(const std::filesystem::path& data_dir) {
    std::filesystem::create_directories(data_dir.parent_path());
    const auto store_id = recovery_store_id();
    const std::vector entries{
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{1},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::sealed},
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{2},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::sealed},
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{3},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::active},
    };
    auto directory =
        glifistore::DataDirectory::open_and_lock(data_dir, glifistore::DataDirectoryOpenMode::open_or_create);
    if (!directory) {
        throw std::runtime_error("failed to open data directory for compaction seed");
    }
    const auto create = [&](const glifistore::ManifestSegmentEntry& entry) {
        auto created =
            glifistore::DurableSegmentFile::create(*directory, {.store_id = store_id,
                                                                .segment_id = entry.segment_id,
                                                                .generation = entry.generation,
                                                                .owner_worker = entry.owner_worker});
        if (!created.durable() || !created.file) {
            throw std::runtime_error("failed to create compaction seed Segment");
        }
        return std::move(*created.file);
    };
    const auto history = generated_compaction_history();
    for (std::size_t segment_index = 0; segment_index < history.segments.size(); ++segment_index) {
        auto segment = create(entries[segment_index]);
        for (const auto& operation : history.segments[segment_index]) {
            append_record(segment, operation.sequence, history.keys[operation.key_index], operation.value,
                          operation.opcode, operation.expire_at_ns);
        }
        if (entries[segment_index].role == glifistore::ManifestSegmentRole::sealed &&
            !segment.seal().committed()) {
            throw std::runtime_error("failed to seal generated compaction seed Segment");
        }
    }
    const glifistore::Manifest manifest{
        .store_id = store_id,
        .manifest_generation = 1,
        .routing_algorithm = glifistore::RoutingAlgorithm::fnv1a64_v1,
        .worker_count = 1,
        .routing_epoch = 1,
        .next_segment_id = glifistore::SegmentId{4},
        .next_segment_generation = glifistore::GenerationId{1},
        .segments = entries,
    };
    if (!directory->publish_manifest(manifest).durable()) {
        throw std::runtime_error("failed to publish compaction seed manifest");
    }
}

[[nodiscard]] auto multi_output_compaction_manifests()
    -> std::pair<glifistore::Manifest, glifistore::Manifest> {
    glifistore::Manifest old{
        .store_id = recovery_store_id(),
        .manifest_generation = 31,
        .routing_algorithm = glifistore::RoutingAlgorithm::fnv1a64_v1,
        .worker_count = 1,
        .routing_epoch = 1,
        .next_segment_id = glifistore::SegmentId{5},
        .next_segment_generation = glifistore::GenerationId{1},
        .segments =
            {
                {.segment_id = glifistore::SegmentId{1},
                 .generation = glifistore::GenerationId{1},
                 .owner_worker = glifistore::WorkerId{0},
                 .role = glifistore::ManifestSegmentRole::sealed},
                {.segment_id = glifistore::SegmentId{2},
                 .generation = glifistore::GenerationId{1},
                 .owner_worker = glifistore::WorkerId{0},
                 .role = glifistore::ManifestSegmentRole::sealed},
                {.segment_id = glifistore::SegmentId{3},
                 .generation = glifistore::GenerationId{1},
                 .owner_worker = glifistore::WorkerId{0},
                 .role = glifistore::ManifestSegmentRole::sealed},
                {.segment_id = glifistore::SegmentId{4},
                 .generation = glifistore::GenerationId{1},
                 .owner_worker = glifistore::WorkerId{0},
                 .role = glifistore::ManifestSegmentRole::active},
            },
    };
    auto next = old;
    ++next.manifest_generation;
    next.segments = {
        {.segment_id = glifistore::SegmentId{1},
         .generation = glifistore::GenerationId{2},
         .owner_worker = glifistore::WorkerId{0},
         .role = glifistore::ManifestSegmentRole::sealed},
        {.segment_id = glifistore::SegmentId{2},
         .generation = glifistore::GenerationId{2},
         .owner_worker = glifistore::WorkerId{0},
         .role = glifistore::ManifestSegmentRole::sealed},
        old.segments.back(),
    };
    return {std::move(old), std::move(next)};
}

[[nodiscard]] auto multi_output_build_key(const std::size_t index) -> std::string {
    return "multi-crash-key-" + std::to_string(1'000U + index).substr(1);
}

[[nodiscard]] auto multi_output_build_value() -> const std::string& {
    static const auto key = multi_output_build_key(0);
    static const std::string value(
        glifistore::kMaxNormalRecordSize - glifistore::kEncodedRecordHeaderSize - key.size(), 'm');
    return value;
}

struct MultiOutputHistoryOperation {
    std::size_t key_index{};
    glifistore::Opcode opcode{glifistore::Opcode::put};
    char value_fill{};
    std::uint64_t expire_at_ns{};
};

struct MultiOutputRandomHistory {
    std::vector<MultiOutputHistoryOperation> operations;
    std::array<std::optional<char>, kMultiOutputHistoryKeyCount> expected{};
};

[[nodiscard]] auto multi_output_random_key(const std::size_t index) -> std::string {
    return "multi-random-key-" + std::to_string(100U + index).substr(1);
}

[[nodiscard]] auto multi_output_random_value_size() -> std::size_t {
    static const auto key = multi_output_random_key(0);
    return glifistore::kMaxNormalRecordSize - glifistore::kEncodedRecordHeaderSize - key.size();
}

[[nodiscard]] auto generated_multi_output_history(const std::uint64_t seed) -> MultiOutputRandomHistory {
    MultiOutputRandomHistory history;
    history.operations.reserve(kMultiOutputRandomOperationCount);
    std::array<std::size_t, kMultiOutputHistoryKeyCount> key_order{};
    for (std::size_t index = 0; index < key_order.size(); ++index) {
        key_order[index] = index;
    }
    std::mt19937_64 random{seed};
    std::shuffle(key_order.begin(), key_order.end(), random);

    const auto append = [&](const std::size_t key_index, const glifistore::Opcode opcode,
                            const std::uint64_t expire_at_ns) {
        const auto fill = static_cast<char>('a' + (random() % 26U));
        history.operations.push_back(
            {.key_index = key_index, .opcode = opcode, .value_fill = fill, .expire_at_ns = expire_at_ns});
        if (opcode == glifistore::Opcode::erase ||
            (expire_at_ns != 0 && expire_at_ns <= kCompactionHistoryNowNs)) {
            history.expected[key_index].reset();
        } else {
            history.expected[key_index] = fill;
        }
    };

    for (const auto key_index : key_order) {
        append(key_index, glifistore::Opcode::put, 0);
    }
    std::shuffle(key_order.begin(), key_order.end(), random);
    for (std::size_t pair = 0; pair < 16; ++pair) {
        const auto key_index = key_order[pair];
        if (pair % 3U == 0) {
            append(key_index, glifistore::Opcode::erase, 0);
        } else if (pair % 3U == 1) {
            append(key_index, glifistore::Opcode::put, kCompactionHistoryNowNs - 1U);
        } else {
            append(key_index, glifistore::Opcode::put, 0);
        }
        append(key_index, glifistore::Opcode::put, 0);
    }
    return history;
}

void seed_multi_output_compaction_build(const std::filesystem::path& data_dir) {
    std::filesystem::create_directories(data_dir.parent_path());
    const auto [old, next] = multi_output_compaction_manifests();
    static_cast<void>(next);
    auto directory =
        glifistore::DataDirectory::open_and_lock(data_dir, glifistore::DataDirectoryOpenMode::open_or_create);
    if (!directory) {
        throw std::runtime_error("failed to open multi-output build seed directory");
    }
    const auto create = [&](const glifistore::ManifestSegmentEntry& entry) {
        auto created =
            glifistore::DurableSegmentFile::create(*directory, {.store_id = old.store_id,
                                                                .segment_id = entry.segment_id,
                                                                .generation = entry.generation,
                                                                .owner_worker = entry.owner_worker});
        if (!created.durable() || !created.file) {
            throw std::runtime_error("failed to create multi-output build seed Segment");
        }
        return std::move(*created.file);
    };
    std::size_t key_index{};
    std::uint64_t sequence{1};
    for (std::size_t segment_index = 0; segment_index < kMultiOutputBuildGroupSizes.size(); ++segment_index) {
        auto segment = create(old.segments[segment_index]);
        for (std::size_t group_index = 0; group_index < kMultiOutputBuildGroupSizes[segment_index];
             ++group_index) {
            const auto key = multi_output_build_key(key_index++);
            append_record(segment, sequence++, key, multi_output_build_value(), glifistore::Opcode::put, 0,
                          false);
        }
        if (!segment.seal().committed()) {
            throw std::runtime_error("failed to seal multi-output build seed Segment");
        }
    }
    static_cast<void>(create(old.segments.back()));
    if (key_index != kMultiOutputBuildKeyCount || !directory->publish_manifest(old).durable()) {
        throw std::runtime_error("failed to publish complete multi-output build seed");
    }
}

void seed_multi_output_random_compaction(const std::filesystem::path& data_dir, const std::uint64_t seed) {
    std::filesystem::create_directories(data_dir.parent_path());
    const auto [old, next] = multi_output_compaction_manifests();
    static_cast<void>(next);
    auto directory =
        glifistore::DataDirectory::open_and_lock(data_dir, glifistore::DataDirectoryOpenMode::open_or_create);
    if (!directory) {
        throw std::runtime_error("failed to open randomized multi-output seed directory");
    }
    const auto create = [&](const glifistore::ManifestSegmentEntry& entry) {
        auto created =
            glifistore::DurableSegmentFile::create(*directory, {.store_id = old.store_id,
                                                                .segment_id = entry.segment_id,
                                                                .generation = entry.generation,
                                                                .owner_worker = entry.owner_worker});
        if (!created.durable() || !created.file) {
            throw std::runtime_error("failed to create randomized multi-output seed Segment");
        }
        return std::move(*created.file);
    };

    const auto history = generated_multi_output_history(seed);
    if (history.operations.size() != kMultiOutputRandomOperationCount) {
        throw std::runtime_error("randomized multi-output history has the wrong operation count");
    }
    std::string value(multi_output_random_value_size(), 'a');
    std::uint64_t sequence{1};
    std::size_t operation_index{};
    for (std::size_t segment_index = 0; segment_index < kMultiOutputRandomSegmentOperations.size();
         ++segment_index) {
        auto segment = create(old.segments[segment_index]);
        const auto segment_end = operation_index + kMultiOutputRandomSegmentOperations[segment_index];
        for (; operation_index < segment_end; ++operation_index) {
            const auto& operation = history.operations[operation_index];
            const auto key = multi_output_random_key(operation.key_index);
            if (operation.opcode == glifistore::Opcode::put) {
                std::fill(value.begin(), value.end(), operation.value_fill);
            }
            append_record(segment, sequence++, key,
                          operation.opcode == glifistore::Opcode::put ? std::string_view{value}
                                                                      : std::string_view{},
                          operation.opcode, operation.expire_at_ns, false);
        }
        if (!segment.seal().committed()) {
            throw std::runtime_error("failed to seal randomized multi-output seed Segment");
        }
    }
    static_cast<void>(create(old.segments.back()));
    if (!directory->publish_manifest(old).durable()) {
        throw std::runtime_error("failed to publish randomized multi-output seed manifest");
    }
}

void seed_multi_output_compaction_recovery(const std::filesystem::path& data_dir, const bool publish_next) {
    std::filesystem::create_directories(data_dir.parent_path());
    const auto [old, next] = multi_output_compaction_manifests();
    auto directory =
        glifistore::DataDirectory::open_and_lock(data_dir, glifistore::DataDirectoryOpenMode::open_or_create);
    if (!directory || !directory->publish_manifest(old).durable()) {
        throw std::runtime_error("failed to publish multi-output compaction seed manifest");
    }
    const auto create = [&](const glifistore::ManifestSegmentEntry& entry) {
        auto created =
            glifistore::DurableSegmentFile::create(*directory, {.store_id = old.store_id,
                                                                .segment_id = entry.segment_id,
                                                                .generation = entry.generation,
                                                                .owner_worker = entry.owner_worker});
        if (!created.durable() || !created.file) {
            throw std::runtime_error("failed to create multi-output compaction seed Segment");
        }
        if (entry.role == glifistore::ManifestSegmentRole::sealed && !created.file->seal().committed()) {
            throw std::runtime_error("failed to seal multi-output compaction seed Segment");
        }
    };
    for (const auto& entry : old.segments) {
        create(entry);
    }
    const glifistore::DurableCompactionIntent intent{
        .worker_id = glifistore::WorkerId{0}, .old_manifest = old, .next_manifest = next};
    if (!directory->publish_compaction_intent(intent).durable()) {
        throw std::runtime_error("failed to publish multi-output compaction intent");
    }
    create(next.segments[0]);
    create(next.segments[1]);
    if (publish_next && !directory->publish_manifest(next).durable()) {
        throw std::runtime_error("failed to publish multi-output compaction next manifest");
    }
}

void run_worker(const Options& options) {
    glifistore::crash::remove_checkpoint_markers(options.checkpoint_dir);
    std::filesystem::create_directories(options.checkpoint_dir);
    glifistore::crash::CheckpointState checkpoint{
        .checkpoint_dir = options.checkpoint_dir,
        .kill_at = options.boundary,
        .action = options.checkpoint_action == "pause" ? glifistore::crash::CheckpointAction::pause
                                                       : glifistore::crash::CheckpointAction::kill,
    };
    const auto hooks = checkpoint.hooks();

    if (options.scenario == "bootstrap") {
        auto directory = glifistore::DataDirectory::open_and_lock(
            options.data_dir, glifistore::DataDirectoryOpenMode::create_new, hooks);
        if (!directory) {
            throw std::runtime_error("bootstrap worker failed to open data directory");
        }
        if (auto prepared = glifistore::prepare_durable_store(
                *directory, glifistore::DurableOpenMode::create_new, 1, std::optional<std::size_t>{1});
            !prepared) {
            throw std::runtime_error("bootstrap worker failed to prepare store");
        }
        auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory), 0,
                                                                      durable_runtime_options(options));
        if (!runtime) {
            throw std::runtime_error("bootstrap worker failed to open runtime");
        }
        if (!(*runtime)->put(bytes(kSeedKey), bytes(kSeedValue)).committed()) {
            throw std::runtime_error("bootstrap worker failed to commit seed put");
        }
        return;
    }

    if (options.scenario == "put") {
        auto runtime = open_runtime_for_worker(options, hooks);
        if (!runtime) {
            throw std::runtime_error("put worker failed to open runtime");
        }
        if (!runtime->put(bytes(kCrashKey), bytes(kCrashValue)).committed()) {
            throw std::runtime_error("put worker failed to commit crash key");
        }
        return;
    }

    if (options.scenario == "rotate") {
        auto runtime = open_runtime_for_worker(options, hooks);
        if (!runtime) {
            throw std::runtime_error("rotate worker failed to open runtime");
        }
        const std::string rotate_value(
            glifistore::kMaxNormalRecordSize - glifistore::kEncodedRecordHeaderSize - kRotateKey.size(), 'r');
        if (!runtime->put(bytes(kRotateKey), bytes(rotate_value)).committed()) {
            throw std::runtime_error("rotate worker failed to commit rotation put");
        }
        return;
    }

    if (options.scenario == "compact") {
        auto runtime = open_runtime_for_worker(options, hooks);
        if (!runtime) {
            throw std::runtime_error("compaction worker failed to open runtime");
        }
        if (!runtime->compact_worker(0, 0).compacted()) {
            throw std::runtime_error("compaction worker failed to compact");
        }
        return;
    }

    if (options.scenario == "compact-multi-build" || options.scenario == "compact-multi-random") {
        auto runtime = open_runtime_for_worker(options, hooks);
        if (!runtime) {
            throw std::runtime_error("multi-output compaction build worker failed to open runtime");
        }
        if (!runtime->compact_worker(0, 0).compacted()) {
            throw std::runtime_error("multi-output compaction build worker failed to compact");
        }
        return;
    }

    if (options.scenario == "compact-multi-rollback" || options.scenario == "compact-multi-retire") {
        if (auto runtime = open_runtime_for_worker(options, hooks); !runtime) {
            throw std::runtime_error("multi-output compaction recovery worker failed to open runtime");
        }
        return;
    }

    throw std::runtime_error("unknown crash scenario");
}

enum class RecoveryExpectation { absent, optional, present };

[[nodiscard]] auto mutation_expectation(const Options& options) -> RecoveryExpectation {
    if (options.boundary == "sync_commit_slot") {
        return RecoveryExpectation::present;
    }
    if (options.boundary == "write_commit_slot") {
        return RecoveryExpectation::optional;
    }
    return RecoveryExpectation::absent;
}

[[nodiscard]] auto verify_expected_value(const glifistore::Result<glifistore::OwnedValue>& value,
                                         const std::string_view expected,
                                         const RecoveryExpectation expectation, const std::string_view label)
    -> bool {
    if (expectation == RecoveryExpectation::absent) {
        if (value) {
            std::cerr << "verify: " << label << " should be absent before the commit point\n";
            return false;
        }
        if (value.error().code != glifistore::ErrorCode::not_found) {
            std::cerr << "verify: " << label << " failed with an unexpected recovery error\n";
            return false;
        }
        return true;
    }
    if (!value) {
        if (expectation == RecoveryExpectation::optional &&
            value.error().code == glifistore::ErrorCode::not_found) {
            return true;
        }
        std::cerr << "verify: " << label << " should be durable after the commit point\n";
        return false;
    }
    if (owned_text(*value) != expected) {
        std::cerr << "verify: " << label << " value mismatch\n";
        return false;
    }
    return true;
}

[[nodiscard]] auto multi_output_next_authority(const std::string_view boundary) -> bool {
    return boundary == "sync_directory#3" || boundary.starts_with("remove_compaction_segment#") ||
           boundary == "sync_directory#4" || boundary == "sync_directory#5";
}

[[nodiscard]] auto verify_recovery(const Options& options) -> bool {
    if (options.scenario == "compact-multi-build") {
        auto runtime = glifistore::DurableRuntimeCatalog::open_existing(options.data_dir);
        if (!runtime || !(*runtime)->verify_index() || !(*runtime)->namespace_audit().clean()) {
            std::cerr << "verify: multi-output compaction build did not reopen cleanly\n";
            return false;
        }
        const auto [old, next] = multi_output_compaction_manifests();
        const auto next_is_authoritative = multi_output_next_authority(options.boundary);
        if ((*runtime)->manifest() != (next_is_authoritative ? next : old)) {
            std::cerr << "verify: multi-output compaction build recovered the wrong authority\n";
            return false;
        }
        const auto& expected_value = multi_output_build_value();
        for (std::size_t key_index = 0; key_index < kMultiOutputBuildKeyCount; ++key_index) {
            const auto key = multi_output_build_key(key_index);
            const auto found = (*runtime)->get(key);
            if (!found || found->bytes.size() != expected_value.size() ||
                found->bytes.front() != std::byte{'m'} || found->bytes.back() != std::byte{'m'}) {
                std::cerr << "verify: multi-output compaction build lost or changed " << key << '\n';
                return false;
            }
        }
        return true;
    }

    if (options.scenario == "compact-multi-random") {
        auto runtime =
            glifistore::DurableRuntimeCatalog::open_existing(options.data_dir, kCompactionHistoryNowNs);
        if (!runtime || !(*runtime)->verify_index() || !(*runtime)->namespace_audit().clean()) {
            std::cerr << "verify: randomized multi-output compaction did not reopen cleanly\n";
            return false;
        }
        const auto [old, next] = multi_output_compaction_manifests();
        if ((*runtime)->manifest() != (multi_output_next_authority(options.boundary) ? next : old)) {
            std::cerr << "verify: randomized multi-output compaction recovered the wrong authority\n";
            return false;
        }
        const auto history = generated_multi_output_history(options.history_seed);
        for (std::size_t key_index = 0; key_index < history.expected.size(); ++key_index) {
            const auto key = multi_output_random_key(key_index);
            const auto found = (*runtime)->get(key, kCompactionHistoryNowNs);
            const auto expected = history.expected[key_index];
            if (!expected || !found || found->bytes.size() != multi_output_random_value_size() ||
                found->bytes.front() != std::byte{static_cast<unsigned char>(*expected)} ||
                found->bytes.back() != std::byte{static_cast<unsigned char>(*expected)}) {
                std::cerr << "verify: randomized multi-output compaction lost or changed " << key
                          << " for seed " << options.history_seed << '\n';
                return false;
            }
        }
        return true;
    }

    if (options.scenario == "compact-multi-rollback" || options.scenario == "compact-multi-retire") {
        auto runtime = glifistore::DurableRuntimeCatalog::open_existing(options.data_dir);
        if (!runtime || !(*runtime)->verify_index() || !(*runtime)->namespace_audit().clean()) {
            std::cerr << "verify: multi-output compaction recovery did not reopen cleanly\n";
            return false;
        }
        const auto [old, next] = multi_output_compaction_manifests();
        const auto& expected = options.scenario == "compact-multi-rollback" ? old : next;
        if ((*runtime)->manifest() != expected) {
            std::cerr << "verify: multi-output compaction recovered the wrong authority\n";
            return false;
        }
        return true;
    }

    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 1},
         .concurrency = glifistore::StoreConcurrencyMode::legacy_mutex,
         .storage_mode = recovery_storage_mode(options),
         .data_directory = options.data_dir,
         .durable_open_mode = glifistore::DurableOpenMode::open_or_create,
         .durable_periodic = {.sync_interval_ms = 60'000},
         .durable_group = {.max_records = 32, .max_bytes = 65536, .max_wait_ms = 60'000}});
    if (!opened) {
        std::cerr << "verify: failed to reopen durable store\n";
        return false;
    }
    if (!(*opened)->verify_index().has_value()) {
        std::cerr << "verify: index verification failed\n";
        return false;
    }

    if (options.scenario == "bootstrap") {
        const auto seed = (*opened)->get(kSeedKey);
        return verify_expected_value(seed, kSeedValue, mutation_expectation(options), "bootstrap seed key");
    }

    if (options.scenario == "put") {
        const auto seed = (*opened)->get(kSeedKey);
        if (!seed || owned_text(*seed) != kSeedValue) {
            std::cerr << "verify: seed key not preserved after crash\n";
            return false;
        }
        const auto crash = (*opened)->get(kCrashKey);
        return verify_expected_value(crash, kCrashValue, mutation_expectation(options), "crash key");
    }

    if (options.scenario == "rotate") {
        const auto seed = (*opened)->get(kSeedKey);
        if (!seed || owned_text(*seed) != kSeedValue) {
            std::cerr << "verify: rotate seed key not preserved\n";
            return false;
        }
        const std::string rotate_value(
            glifistore::kMaxNormalRecordSize - glifistore::kEncodedRecordHeaderSize - kRotateKey.size(), 'r');
        const auto rotated = (*opened)->get(kRotateKey);
        auto expectation = RecoveryExpectation::absent;
        if (options.boundary == "write_commit_slot#2") {
            expectation = RecoveryExpectation::optional;
        } else if (options.boundary == "sync_commit_slot#2") {
            expectation = RecoveryExpectation::present;
        }
        return verify_expected_value(rotated, rotate_value, expectation, "post-rotation key");
    }

    if (options.scenario == "compact") {
        const auto history = generated_compaction_history();
        for (std::size_t key_index = 0; key_index < history.keys.size(); ++key_index) {
            const auto found = (*opened)->get(history.keys[key_index]);
            const auto expectation =
                history.expected[key_index] ? RecoveryExpectation::present : RecoveryExpectation::absent;
            const auto expected = history.expected[key_index].value_or(std::string{});
            if (!verify_expected_value(found, expected, expectation, history.keys[key_index])) {
                return false;
            }
        }
        return true;
    }

    std::cerr << "verify: unknown scenario\n";
    return false;
}

[[nodiscard]] auto scenario_boundaries(const std::string_view scenario) -> std::vector<std::string> {
    if (scenario == "bootstrap") {
        return {"create_data_directory", "sync_parent_directory", "write_bootstrap",
                "sync_bootstrap",        "rename_bootstrap",      "sync_directory#1",
                "write_manifest",        "sync_manifest",         "rename_manifest",
                "sync_directory#2",      "preallocate_segment",   "write_segment_header",
                "sync_segment_file",     "rename_segment",        "sync_directory#3",
                "remove_bootstrap",      "sync_directory#4",      "write_record",
                "sync_record",           "write_commit_slot",     "sync_commit_slot"};
    }
    if (scenario == "put") {
        return {"write_record", "sync_record", "write_commit_slot", "sync_commit_slot"};
    }
    if (scenario == "rotate") {
        return {"write_commit_slot#1", "sync_commit_slot#1",  "preallocate_segment", "write_segment_header",
                "sync_segment_file",   "rename_segment",      "sync_directory#1",    "write_manifest",
                "sync_manifest",       "rename_manifest",     "sync_directory#2",    "write_record",
                "sync_record",         "write_commit_slot#2", "sync_commit_slot#2"};
    }
    if (scenario == "compact") {
        return {"preallocate_segment",
                "write_segment_header",
                "write_record#1",
                "write_record#2",
                "sync_record#1",
                "write_commit_slot#1",
                "sync_commit_slot#1",
                "write_commit_slot#2",
                "sync_commit_slot#2",
                "write_compaction_intent",
                "sync_compaction_intent",
                "rename_compaction_intent",
                "sync_directory#1",
                "rename_segment",
                "sync_directory#2",
                "write_manifest",
                "sync_manifest",
                "rename_manifest",
                "sync_directory#3",
                "remove_compaction_segment#1",
                "remove_compaction_segment#2",
                "sync_directory#4",
                "remove_compaction_intent",
                "sync_directory#5"};
    }
    if (scenario == "compact-multi-build") {
        return {"write_record#64",     "preallocate_segment#2", "write_segment_header#2",
                "rename_segment#2",    "sync_directory#2",      "sync_record#2",
                "write_commit_slot#3", "sync_commit_slot#3",    "write_commit_slot#4",
                "sync_commit_slot#4",  "sync_directory#3",      "remove_compaction_segment#3",
                "sync_directory#4",    "sync_directory#5"};
    }
    if (scenario == "compact-multi-rollback") {
        return {"remove_compaction_segment#1", "remove_compaction_segment#2", "sync_directory#1",
                "remove_compaction_intent", "sync_directory#2"};
    }
    if (scenario == "compact-multi-retire") {
        return {"remove_compaction_segment#1", "remove_compaction_segment#2",
                "remove_compaction_segment#3", "sync_directory#1",
                "remove_compaction_intent",    "sync_directory#2"};
    }
    return {};
}

[[nodiscard]] auto compaction_copy_boundaries() -> std::vector<std::string> {
    std::vector<std::string> boundaries;
    boundaries.reserve(63);
    for (std::size_t occurrence = 1; occurrence < 64; ++occurrence) {
        boundaries.push_back("write_record#" + std::to_string(occurrence));
    }
    return boundaries;
}

[[nodiscard]] auto run_single_case(const Options& base_options) -> bool {
    Options options = base_options;
    std::error_code ignored;
    std::filesystem::remove_all(options.data_dir, ignored);
    if (options.scenario == "bootstrap") {
        std::filesystem::create_directories(options.data_dir.parent_path());
    } else if (options.scenario == "put") {
        seed_put_store(options.data_dir);
    } else if (options.scenario == "rotate") {
        seed_rotate_store(options.data_dir);
    } else if (options.scenario == "compact") {
        seed_compaction_store(options.data_dir);
    } else if (options.scenario == "compact-multi-build") {
        seed_multi_output_compaction_build(options.data_dir);
    } else if (options.scenario == "compact-multi-random") {
        seed_multi_output_random_compaction(options.data_dir, options.history_seed);
    } else if (options.scenario == "compact-multi-rollback") {
        seed_multi_output_compaction_recovery(options.data_dir, false);
    } else if (options.scenario == "compact-multi-retire") {
        seed_multi_output_compaction_recovery(options.data_dir, true);
    }

    std::filesystem::create_directories(options.checkpoint_dir);
    glifistore::crash::remove_checkpoint_markers(options.checkpoint_dir);

    const pid_t child = ::fork();
    if (child < 0) {
        std::cerr << "fork failed: " << std::strerror(errno) << '\n';
        return false;
    }
    if (child == 0) {
        try {
            run_worker(options);
            std::exit(0);
        } catch (const std::exception& exception) {
            std::cerr << "worker failed: " << exception.what() << '\n';
            std::exit(1);
        }
    }

    if (!glifistore::crash::wait_for_checkpoint(options.checkpoint_dir, options.boundary)) {
        std::cerr << "timed out waiting for checkpoint " << options.boundary << " in scenario "
                  << options.scenario << '\n';
        ::kill(child, SIGKILL);
        int status = 0;
        ::waitpid(child, &status, 0);
        return false;
    }
    if (options.checkpoint_action == "pause") {
        bool stopped = false;
        const auto stop_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < stop_deadline) {
            int stop_status = 0;
            const auto wait_result = ::waitpid(child, &stop_status, WUNTRACED | WNOHANG);
            if (wait_result == child) {
                if (WIFSTOPPED(stop_status)) {
                    stopped = true;
                    break;
                }
                std::cerr << "worker exited before pause confirmation for boundary " << options.boundary
                          << '\n';
                return false;
            }
            if (wait_result < 0) {
                std::cerr << "waitpid failed while confirming paused worker: " << std::strerror(errno)
                          << '\n';
                return false;
            }
            ::usleep(1'000);
        }
        if (!stopped) {
            std::cerr << "worker did not stop at boundary " << options.boundary << '\n';
            ::kill(child, SIGKILL);
            int status = 0;
            ::waitpid(child, &status, 0);
            return false;
        }
    }
    ::kill(child, SIGKILL);
    int status = 0;
    ::waitpid(child, &status, 0);

    if (!verify_recovery(options)) {
        std::cerr << "recovery verification failed for scenario " << options.scenario << " at boundary "
                  << options.boundary << '\n';
        return false;
    }
    return true;
}

void cleanup_matrix_case(const Options& options) {
    std::error_code ignored;
    std::filesystem::remove_all(options.data_dir.parent_path(), ignored);
    std::filesystem::remove_all(options.checkpoint_dir, ignored);
}

[[nodiscard]] auto run_matrix_cases(const std::string_view mode, const std::string_view scenario,
                                    const std::span<const std::string> boundaries,
                                    const std::uint64_t history_seed = 0) -> bool {
    bool success = true;
    for (const auto& boundary : boundaries) {
        Options options{
            .mode = std::string{mode},
            .scenario = std::string{scenario},
            .boundary = boundary,
            .history_seed = history_seed,
            .data_dir = std::filesystem::temp_directory_path() /
                        ("glifistore-crash-" + crash_run_suffix() + "-" + std::string{scenario} + "-" +
                         std::to_string(history_seed) + "-" + boundary) /
                        "store",
            .checkpoint_dir = std::filesystem::temp_directory_path() /
                              ("glifistore-crash-checkpoints-" + crash_run_suffix() + "-" +
                               std::string{scenario} + "-" + std::to_string(history_seed) + "-" + boundary),
        };
        std::cout << "# crash mode=" << mode << " scenario=" << scenario << " seed=" << history_seed
                  << " boundary=" << boundary << '\n';
        if (!run_single_case(options)) {
            success = false;
        }
        cleanup_matrix_case(options);
    }
    return success;
}

[[nodiscard]] auto run_matrix() -> bool {
    const std::vector<std::string_view> scenarios{
        "bootstrap",           "put", "rotate", "compact", "compact-multi-build", "compact-multi-rollback",
        "compact-multi-retire"};
    bool success = true;
    for (const auto scenario : scenarios) {
        const auto boundaries = scenario_boundaries(scenario);
        if (!run_matrix_cases("matrix", scenario, boundaries)) {
            success = false;
        }
    }
    return success;
}

[[nodiscard]] auto run_copy_matrix() -> bool {
    const auto boundaries = compaction_copy_boundaries();
    return run_matrix_cases("copy-matrix", "compact-multi-build", boundaries);
}

[[nodiscard]] auto run_pause_smoke() -> bool {
    const std::array<std::string, 1> boundaries{"write_record"};
    Options options{.checkpoint_action = "pause"};
    const auto boundary = boundaries.front();
    options.mode = "pause-smoke";
    options.scenario = "put";
    options.boundary = boundary;
    options.data_dir = std::filesystem::temp_directory_path() /
                       ("glifistore-crash-pause-" + crash_run_suffix() + "-put-" + boundary) / "store";
    options.checkpoint_dir =
        std::filesystem::temp_directory_path() /
        ("glifistore-crash-pause-checkpoints-" + crash_run_suffix() + "-put-" + boundary);
    const auto passed = run_single_case(options);
    cleanup_matrix_case(options);
    return passed;
}

[[nodiscard]] auto run_random_matrix() -> bool {
    constexpr std::array<std::uint64_t, 4> kSeeds{
        0xA17E'2026'0000'0001ULL,
        0xA17E'2026'0000'0002ULL,
        0xA17E'2026'0000'0003ULL,
        0xA17E'2026'0000'0004ULL,
    };
    std::vector<std::string> boundaries;
    boundaries.reserve(kRandomCampaignBoundaries.size());
    for (const auto boundary : kRandomCampaignBoundaries) {
        boundaries.emplace_back(boundary);
    }
    bool success = true;
    for (const auto seed : kSeeds) {
        if (!run_matrix_cases("random-matrix", "compact-multi-random", boundaries, seed)) {
            success = false;
        }
    }
    return success;
}

class CampaignPrng {
  public:
    explicit constexpr CampaignPrng(const std::uint64_t seed) noexcept : state_(seed) {}

    [[nodiscard]] constexpr auto next() noexcept -> std::uint64_t {
        state_ += 0x9E37'79B9'7F4A'7C15ULL;
        auto value = state_;
        value = (value ^ (value >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
        value = (value ^ (value >> 27U)) * 0x94D0'49BB'1331'11EBULL;
        return value ^ (value >> 31U);
    }

  private:
    std::uint64_t state_;
};

[[nodiscard]] consteval auto campaign_prng_contract() -> bool {
    CampaignPrng random{42};
    return random.next() == 13'679'457'532'755'275'413ULL && random.next() == 2'949'826'092'126'892'291ULL &&
           random.next() == 5'139'283'748'462'763'858ULL;
}

static_assert(campaign_prng_contract(), "random crash schedules must remain cross-platform reproducible");

[[nodiscard]] auto run_random_campaign(const Options& campaign) -> bool {
    if (!campaign.campaign_seed || campaign.campaign_iterations == 0 ||
        campaign.campaign_iterations > kMaxRandomCampaignIterations) {
        return false;
    }

    std::ofstream report;
    if (!campaign.report_path.empty()) {
        if (std::filesystem::exists(campaign.report_path)) {
            throw std::runtime_error("random campaign report already exists");
        }
        report.open(campaign.report_path, std::ios::out);
        if (!report) {
            throw std::runtime_error("cannot create random campaign report");
        }
        report << "iteration\tcampaign_seed\thistory_seed\tboundary\trecovery_outcome\n";
        report.flush();
    }

    std::cout << "# random-crash-campaign schema=glifistore-random-crash-campaign-v1"
              << " campaign_seed=" << *campaign.campaign_seed
              << " iterations=" << campaign.campaign_iterations << '\n';

    CampaignPrng random{*campaign.campaign_seed};
    bool success = true;
    for (std::size_t iteration = 1; iteration <= campaign.campaign_iterations; ++iteration) {
        const auto history_seed = random.next();
        const auto boundary_index =
            static_cast<std::size_t>(random.next() % kRandomCampaignBoundaries.size());
        const auto boundary = kRandomCampaignBoundaries[boundary_index];
        const std::array<std::string, 1> selected_boundary{std::string{boundary}};

        const auto passed =
            run_matrix_cases("random-campaign", "compact-multi-random", selected_boundary, history_seed);
        success = passed && success;
        const auto outcome = passed ? "passed" : "failed";
        std::cout << "# random-crash-case iteration=" << iteration
                  << " campaign_seed=" << *campaign.campaign_seed << " history_seed=" << history_seed
                  << " boundary=" << boundary << " recovery_outcome=" << outcome << '\n';
        if (report) {
            report << iteration << '\t' << *campaign.campaign_seed << '\t' << history_seed << '\t' << boundary
                   << '\t' << outcome << '\n';
            report.flush();
        }
    }

    std::cout << "# random-crash-summary campaign_seed=" << *campaign.campaign_seed
              << " iterations=" << campaign.campaign_iterations
              << " outcome=" << (success ? "passed" : "failed") << '\n';
    return success;
}

[[nodiscard]] auto run_periodic_matrix() -> bool {
    const std::vector<std::string_view> boundaries{"write_record", "sync_record", "write_commit_slot",
                                                   "sync_commit_slot"};
    bool success = true;
    for (const auto boundary : boundaries) {
        Options options{
            .mode = "periodic-matrix",
            .scenario = "put",
            .boundary = std::string{boundary},
            .storage = "periodic",
            .data_dir =
                std::filesystem::temp_directory_path() /
                ("glifistore-crash-periodic-" + crash_run_suffix() + "-put-" + std::string{boundary}) /
                "store",
            .checkpoint_dir = std::filesystem::temp_directory_path() /
                              ("glifistore-crash-periodic-checkpoints-" + crash_run_suffix() + "-put-" +
                               std::string{boundary}),
        };
        std::cout << "# crash storage=periodic scenario=put boundary=" << boundary << '\n';
        if (!run_single_case(options)) {
            success = false;
        }
        cleanup_matrix_case(options);
    }
    return success;
}

[[nodiscard]] auto run_group_matrix() -> bool {
    const std::vector<std::string_view> boundaries{"write_record", "sync_record", "write_commit_slot",
                                                   "sync_commit_slot"};
    bool success = true;
    for (const auto boundary : boundaries) {
        Options options{
            .mode = "group-matrix",
            .scenario = "put",
            .boundary = std::string{boundary},
            .storage = "group",
            .data_dir = std::filesystem::temp_directory_path() /
                        ("glifistore-crash-group-" + crash_run_suffix() + "-put-" + std::string{boundary}) /
                        "store",
            .checkpoint_dir = std::filesystem::temp_directory_path() /
                              ("glifistore-crash-group-checkpoints-" + crash_run_suffix() + "-put-" +
                               std::string{boundary}),
        };
        std::cout << "# crash storage=group scenario=put boundary=" << boundary << '\n';
        if (!run_single_case(options)) {
            success = false;
        }
        cleanup_matrix_case(options);
    }
    return success;
}

void seed_scenario(const Options& options) {
    std::error_code ignored;
    std::filesystem::remove_all(options.data_dir, ignored);
    if (options.scenario == "bootstrap") {
        std::filesystem::create_directories(options.data_dir.parent_path());
        return;
    }
    if (options.scenario == "put") {
        seed_put_store(options.data_dir);
        return;
    }
    if (options.scenario == "rotate") {
        seed_rotate_store(options.data_dir);
        return;
    }
    if (options.scenario == "compact") {
        seed_compaction_store(options.data_dir);
        return;
    }
    if (options.scenario == "compact-multi-build") {
        seed_multi_output_compaction_build(options.data_dir);
        return;
    }
    if (options.scenario == "compact-multi-random") {
        seed_multi_output_random_compaction(options.data_dir, options.history_seed);
        return;
    }
    if (options.scenario == "compact-multi-rollback") {
        seed_multi_output_compaction_recovery(options.data_dir, false);
        return;
    }
    if (options.scenario == "compact-multi-retire") {
        seed_multi_output_compaction_recovery(options.data_dir, true);
        return;
    }
    throw std::runtime_error("unknown seed scenario");
}

} // namespace

int main(int argc, char** argv) {
    const auto options = parse_options(argc, argv);
    if (!options) {
        print_usage(argv[0]);
        return 2;
    }

    try {
        if (options->mode == "seed") {
            if (options->scenario.empty() || options->data_dir.empty()) {
                print_usage(argv[0]);
                return 2;
            }
            seed_scenario(*options);
            return 0;
        }
        if (options->mode == "worker") {
            run_worker(*options);
            return 0;
        }
        if (options->mode == "verify") {
            return verify_recovery(*options) ? 0 : 1;
        }
        if (options->mode == "matrix") {
            return run_matrix() ? 0 : 1;
        }
        if (options->mode == "copy-matrix") {
            return run_copy_matrix() ? 0 : 1;
        }
        if (options->mode == "random-matrix") {
            return run_random_matrix() ? 0 : 1;
        }
        if (options->mode == "random-campaign") {
            if (!options->campaign_seed || options->campaign_iterations == 0) {
                print_usage(argv[0]);
                return 2;
            }
            return run_random_campaign(*options) ? 0 : 1;
        }
        if (options->mode == "pause-smoke") {
            return run_pause_smoke() ? 0 : 1;
        }
        if (options->mode == "periodic-matrix") {
            return run_periodic_matrix() ? 0 : 1;
        }
        if (options->mode == "group-matrix") {
            return run_group_matrix() ? 0 : 1;
        }
        print_usage(argv[0]);
        return 2;
    } catch (const std::exception& exception) {
        std::cerr << "glifistore_crash_persistence: fatal: " << exception.what() << '\n';
        return 1;
    }
}
