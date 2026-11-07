#include "glifistore/core/key_hash.hpp"
#include "glifistore/persistence/compaction_builder.hpp"
#include "glifistore/persistence/runtime_catalog.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "glifistore/segment/record.hpp"
#include "glifistore/store/store.hpp"
#include "store/store_internal.hpp"
#include "test.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
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

class ManualStoreClock final : public glifistore::StoreClock {
  public:
    explicit ManualStoreClock(const std::uint64_t initial_now_ns) : now_ns_(initial_now_ns) {}

    [[nodiscard]] auto now_ns() const noexcept -> std::uint64_t override {
        return now_ns_.load(std::memory_order_acquire);
    }

    void set(const std::uint64_t now_ns) noexcept {
        now_ns_.store(now_ns, std::memory_order_release);
    }

  private:
    std::atomic_uint64_t now_ns_;
};

class CompactionBuildDirectory final {
  public:
    CompactionBuildDirectory() {
        auto pattern =
            (std::filesystem::temp_directory_path() / "glifistore-compaction-build-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* created = ::mkdtemp(writable.data());
        GLIFI_REQUIRE(created != nullptr);
        path_ = created;
    }

    ~CompactionBuildDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] auto path() const -> const std::filesystem::path& {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

struct CopyWriteFailure {
    bool enabled{};
    bool fired{};

    static auto before(void* context, const glifistore::FilesystemOperation operation) -> glifistore::Status {
        auto& failure = *static_cast<CopyWriteFailure*>(context);
        if (!failure.enabled || operation != glifistore::FilesystemOperation::write_record) {
            return {};
        }
        failure.fired = true;
        return glifistore::fail(glifistore::ErrorCode::io_error, "injected durable compaction copy failure");
    }
};

struct GeneratedCompactionFailure {
    glifistore::FilesystemOperation target{};
    std::size_t fail_on_matching_call{1};
    std::size_t matching_calls{};
    bool enabled{};
    bool fired{};

    static auto before(void* context, const glifistore::FilesystemOperation operation) -> glifistore::Status {
        auto& failure = *static_cast<GeneratedCompactionFailure*>(context);
        if (!failure.enabled || operation != failure.target) {
            return {};
        }
        ++failure.matching_calls;
        if (failure.matching_calls != failure.fail_on_matching_call) {
            return {};
        }
        failure.fired = true;
        return glifistore::fail(glifistore::ErrorCode::io_error,
                                "injected generated-history compaction failure");
    }
};

auto bytes(const std::string_view value) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

auto key_for_worker(const std::size_t worker, const std::size_t worker_count, const std::string_view prefix)
    -> std::string {
    for (std::size_t suffix = 0; suffix < 10'000; ++suffix) {
        auto candidate = std::string{prefix} + std::to_string(suffix);
        if (glifistore::route_worker(candidate, worker_count) == worker) {
            return candidate;
        }
    }
    throw std::runtime_error("failed to construct a routed compaction test key");
}

auto build_manifest() -> glifistore::Manifest {
    return {
        .store_id = {std::byte{0x41}, std::byte{0x42}, std::byte{0x43}},
        .manifest_generation = 21,
        .worker_count = 1,
        .routing_epoch = 1,
        .next_segment_id = glifistore::SegmentId{4},
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
                 .role = glifistore::ManifestSegmentRole::active},
            },
    };
}

auto identity(const glifistore::Manifest& manifest, const glifistore::ManifestSegmentEntry& entry)
    -> glifistore::SegmentHeaderIdentity {
    return {.store_id = manifest.store_id,
            .segment_id = entry.segment_id,
            .generation = entry.generation,
            .owner_worker = entry.owner_worker};
}

struct TestRecord {
    std::uint64_t sequence{};
    glifistore::Opcode opcode{glifistore::Opcode::put};
    std::string_view key;
    std::string_view value;
    std::uint64_t expire_at_ns{};
    glifistore::ValueType type{glifistore::ValueType::bytes};
    std::uint32_t flags{};
};

auto create_records(glifistore::DataDirectory& directory, const glifistore::Manifest& manifest,
                    const glifistore::ManifestSegmentEntry& entry, const std::span<const TestRecord> records)
    -> std::vector<glifistore::RecordRef> {
    auto created = glifistore::DurableSegmentFile::create(directory, identity(manifest, entry));
    GLIFI_REQUIRE(created.durable());
    std::vector<glifistore::RecordRef> references;
    references.reserve(records.size());
    for (const auto& record : records) {
        const glifistore::RecordInput input{
            .sequence = glifistore::SequenceNumber{record.sequence},
            .opcode = record.opcode,
            .type = record.type,
            .flags = record.flags,
            .key_hash = glifistore::hash_key(record.key),
            .expire_at_ns = record.expire_at_ns,
            .key = bytes(record.key),
            .value = bytes(record.value),
        };
        const auto encoded = glifistore::encode_record(input);
        GLIFI_REQUIRE(encoded.has_value());
        const auto offset = created.file->selected_commit().commit.committed_end;
        GLIFI_REQUIRE(created.file->append_record(*encoded).committed());
        references.push_back({.segment_id = entry.segment_id,
                              .offset = glifistore::RecordOffset{offset},
                              .size = glifistore::RecordSize{static_cast<std::uint32_t>(encoded->size())},
                              .sequence = input.sequence,
                              .generation = entry.generation});
    }
    if (entry.role == glifistore::ManifestSegmentRole::sealed) {
        GLIFI_REQUIRE(created.file->seal().committed());
    } else {
        GLIFI_REQUIRE(
            created.file->flush_pending_commit(glifistore::SegmentCommitSync::immediate).committed());
    }
    return references;
}

struct BuildFixture {
    glifistore::Manifest manifest;
    glifistore::Index index;
    glifistore::RecordRef live_a;
    glifistore::RecordRef expired;
    glifistore::RecordRef replacement;
    glifistore::RecordRef active;
};

auto create_build_fixture(glifistore::DataDirectory& directory) -> BuildFixture {
    auto manifest = build_manifest();
    GLIFI_REQUIRE(directory.publish_manifest(manifest).durable());
    const std::vector<TestRecord> first{
        {.sequence = 1, .key = "replacement", .value = "old"},
        {.sequence = 2, .key = "live-a", .value = "alpha", .type = glifistore::ValueType::map, .flags = 17},
        {.sequence = 3, .key = "expired", .value = "stale", .expire_at_ns = 50},
    };
    const std::vector<TestRecord> second{
        {.sequence = 4, .key = "replacement", .value = "new"},
        {.sequence = 5, .key = "deleted", .value = "gone"},
        {.sequence = 6, .opcode = glifistore::Opcode::erase, .key = "deleted", .value = {}},
    };
    const std::vector<TestRecord> active_records{
        {.sequence = 7, .key = "active", .value = "current"},
    };
    const auto first_refs = create_records(directory, manifest, manifest.segments[0], first);
    const auto second_refs = create_records(directory, manifest, manifest.segments[1], second);
    const auto active_refs = create_records(directory, manifest, manifest.segments[2], active_records);

    glifistore::Index index;
    GLIFI_REQUIRE(index.insert_or_assign("live-a", first_refs[1]).has_value());
    GLIFI_REQUIRE(index.insert_or_assign("expired", first_refs[2]).has_value());
    GLIFI_REQUIRE(index.insert_or_assign("replacement", second_refs[0]).has_value());
    GLIFI_REQUIRE(index.insert_or_assign("active", active_refs[0]).has_value());
    return {.manifest = std::move(manifest),
            .index = std::move(index),
            .live_a = first_refs[1],
            .expired = first_refs[2],
            .replacement = second_refs[0],
            .active = active_refs[0]};
}

auto text(const glifistore::OwnedValue& value) -> std::string {
    return {reinterpret_cast<const char*>(value.bytes.data()), value.bytes.size()};
}

auto amp_reject_manifest() -> glifistore::Manifest {
    return {
        .store_id = {std::byte{0x41}, std::byte{0x42}, std::byte{0x43}},
        .manifest_generation = 31,
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
}

auto fill_sealed_with_max_records(glifistore::DataDirectory& directory, const glifistore::Manifest& manifest,
                                  const glifistore::ManifestSegmentEntry& entry,
                                  const std::uint64_t first_sequence, const std::size_t record_count,
                                  const std::string_view key_prefix) -> std::uint64_t {
    auto created = glifistore::DurableSegmentFile::create(directory, identity(manifest, entry));
    GLIFI_REQUIRE(created.durable());
    // Near-max Records so a few dozen live copies exceed one Segment payload and force
    // multi-output compaction (needed for amp=1 with three sealed sources).
    constexpr auto kValueBytes = glifistore::kMaxNormalRecordSize - 256U;
    const std::string value(kValueBytes, 'v');
    std::uint64_t sequence = first_sequence;
    for (std::size_t index = 0; index < record_count; ++index) {
        const auto key = std::string{key_prefix} + std::to_string(index);
        const glifistore::RecordInput input{
            .sequence = glifistore::SequenceNumber{sequence},
            .opcode = glifistore::Opcode::put,
            .type = glifistore::ValueType::bytes,
            .key_hash = glifistore::hash_key(key),
            .key = bytes(key),
            .value = bytes(value),
        };
        const auto encoded = glifistore::encode_record(input);
        GLIFI_REQUIRE(encoded.has_value());
        GLIFI_REQUIRE(encoded->size() <= glifistore::kMaxNormalRecordSize);
        GLIFI_REQUIRE(created.file->append_record(*encoded).committed());
        ++sequence;
    }
    GLIFI_REQUIRE(created.file->seal().committed());
    return sequence;
}

} // namespace

GLIFI_TEST("durable compaction builder copies exact visible Records and preserves v1 sequences") {
    CompactionBuildDirectory temporary;
    glifistore::Manifest next;
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        auto fixture = create_build_fixture(*directory);
        auto built = glifistore::build_durable_worker_compaction(*directory, fixture.manifest,
                                                                 glifistore::WorkerId{0}, fixture.index, 100);
        GLIFI_REQUIRE(built.succeeded());
        GLIFI_REQUIRE(built.prepared->plan.sources.size() == 2);
        GLIFI_REQUIRE(built.prepared->plan.replacements.size() == 1);
        GLIFI_REQUIRE(built.prepared->replacement_commits.size() == 1);
        GLIFI_REQUIRE(built.prepared->stats.source_index_records_verified == 3);
        GLIFI_REQUIRE(built.prepared->stats.records_copied == 2);
        GLIFI_REQUIRE(built.prepared->stats.expired_records_dropped == 1);
        GLIFI_REQUIRE(built.prepared->stats.pacing_delay_ns == 0);
        GLIFI_REQUIRE(built.prepared->stats.pacing_sleep_count == 0);
        GLIFI_REQUIRE(built.prepared->stats.pacing_burst_bytes == 0);
        GLIFI_REQUIRE(!built.prepared->index.find("expired").has_value());
        GLIFI_REQUIRE(built.prepared->index.find("active") == fixture.active);
        GLIFI_REQUIRE(built.prepared->active_live_record_bytes == fixture.active.size.value);
        const auto compacted_a = built.prepared->index.find("live-a");
        const auto compacted_replacement = built.prepared->index.find("replacement");
        GLIFI_REQUIRE(compacted_a.has_value());
        GLIFI_REQUIRE(compacted_replacement.has_value());
        GLIFI_REQUIRE(compacted_a->sequence == fixture.live_a.sequence);
        GLIFI_REQUIRE(compacted_replacement->sequence == fixture.replacement.sequence);
        GLIFI_REQUIRE(compacted_a->generation == glifistore::GenerationId{2});
        GLIFI_REQUIRE(compacted_replacement->generation == glifistore::GenerationId{2});
        GLIFI_REQUIRE(directory->read_manifest().value() == fixture.manifest);
        GLIFI_REQUIRE(directory->read_compaction_intent().has_value());
        next = built.prepared->plan.next_manifest;
        GLIFI_REQUIRE(directory->publish_manifest(next).durable());
    }

    auto runtime = glifistore::DurableRuntimeCatalog::open_existing(temporary.path(), 100);
    GLIFI_REQUIRE(runtime.has_value());
    GLIFI_REQUIRE((*runtime)->manifest() == next);
    const auto live_a = (*runtime)->get("live-a", 100);
    const auto replacement = (*runtime)->get("replacement", 100);
    const auto active = (*runtime)->get("active", 100);
    GLIFI_REQUIRE(live_a.has_value());
    GLIFI_REQUIRE(replacement.has_value());
    GLIFI_REQUIRE(active.has_value());
    GLIFI_REQUIRE(text(*live_a) == "alpha");
    GLIFI_REQUIRE(text(*replacement) == "new");
    GLIFI_REQUIRE(text(*active) == "current");
    GLIFI_REQUIRE(live_a->sequence == 2);
    GLIFI_REQUIRE(replacement->sequence == 4);
    GLIFI_REQUIRE(active->sequence == 7);
    GLIFI_REQUIRE(!(*runtime)->get("expired", 100).has_value());
    GLIFI_REQUIRE(!(*runtime)->get("deleted", 100).has_value());
}

GLIFI_TEST("durable compaction builder prepares zero-output retirement without fabricating Records") {
    CompactionBuildDirectory temporary;
    glifistore::Manifest next;
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        auto fixture = create_build_fixture(*directory);
        glifistore::Index active_only;
        GLIFI_REQUIRE(active_only.insert_or_assign("active", fixture.active).has_value());
        auto built = glifistore::build_durable_worker_compaction(*directory, fixture.manifest,
                                                                 glifistore::WorkerId{0}, active_only, 100);
        GLIFI_REQUIRE(built.succeeded());
        GLIFI_REQUIRE(built.prepared->plan.replacements.empty());
        GLIFI_REQUIRE(built.prepared->replacement_commits.empty());
        GLIFI_REQUIRE(built.prepared->stats.records_copied == 0);
        GLIFI_REQUIRE(built.prepared->index.find("active") == fixture.active);
        GLIFI_REQUIRE(built.prepared->active_live_record_bytes == fixture.active.size.value);
        next = built.prepared->plan.next_manifest;
        GLIFI_REQUIRE(directory->publish_manifest(next).durable());
    }

    auto runtime = glifistore::DurableRuntimeCatalog::open_existing(temporary.path(), 100);
    GLIFI_REQUIRE(runtime.has_value());
    GLIFI_REQUIRE((*runtime)->manifest() == next);
    GLIFI_REQUIRE((*runtime)->get("active", 100).has_value());
    GLIFI_REQUIRE(!(*runtime)->get("live-a", 100).has_value());
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
}

GLIFI_TEST("durable compaction rejected intent gate leaves the namespace untouched") {
    CompactionBuildDirectory temporary;
    auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
    GLIFI_REQUIRE(directory.has_value());
    auto fixture = create_build_fixture(*directory);
    bool gate_called{};
    const glifistore::DurableCompactionIntentGate gate{
        .context = &gate_called,
        .acquire = [](void* opaque) -> glifistore::Status {
            *static_cast<bool*>(opaque) = true;
            return glifistore::fail(glifistore::ErrorCode::sequence_conflict,
                                    "injected stale compaction authority");
        },
    };
    auto entries = fixture.index.entries();
    auto built = glifistore::build_durable_worker_compaction(
        *directory, fixture.manifest, glifistore::WorkerId{0}, std::move(entries), 100, {}, gate);
    GLIFI_REQUIRE(gate_called);
    GLIFI_REQUIRE(!built.succeeded());
    GLIFI_REQUIRE(built.outcome == glifistore::DurableCompactionBuildOutcome::not_started);
    GLIFI_REQUIRE(built.error.has_value());
    GLIFI_REQUIRE(built.error->code == glifistore::ErrorCode::sequence_conflict);
    GLIFI_REQUIRE(directory->read_manifest().value() == fixture.manifest);
    GLIFI_REQUIRE(!directory->read_compaction_intent().has_value());
    const glifistore::ManifestSegmentEntry replacement{
        .segment_id = glifistore::SegmentId{1},
        .generation = glifistore::GenerationId{2},
        .owner_worker = glifistore::WorkerId{0},
        .role = glifistore::ManifestSegmentRole::sealed,
    };
    GLIFI_REQUIRE(!std::filesystem::exists(
        temporary.path() / glifistore::segment_filename(identity(fixture.manifest, replacement))));
}

GLIFI_TEST("durable compaction staged copy failure remains pre-intent and cleans its temporary") {
    CompactionBuildDirectory temporary;
    const auto manifest = build_manifest();
    const glifistore::ManifestSegmentEntry replacement{
        .segment_id = glifistore::SegmentId{1},
        .generation = glifistore::GenerationId{2},
        .owner_worker = glifistore::WorkerId{0},
        .role = glifistore::ManifestSegmentRole::sealed,
    };
    CopyWriteFailure failure;
    {
        auto directory = glifistore::DataDirectory::open_and_lock(
            temporary.path(), {.context = &failure, .before = &CopyWriteFailure::before});
        GLIFI_REQUIRE(directory.has_value());
        auto fixture = create_build_fixture(*directory);
        failure.enabled = true;
        auto built = glifistore::build_durable_worker_compaction(*directory, fixture.manifest,
                                                                 glifistore::WorkerId{0}, fixture.index, 100);
        GLIFI_REQUIRE(!built.succeeded());
        GLIFI_REQUIRE(built.outcome == glifistore::DurableCompactionBuildOutcome::not_started);
        GLIFI_REQUIRE(built.error.has_value());
        GLIFI_REQUIRE(failure.fired);
        GLIFI_REQUIRE(!directory->read_compaction_intent().has_value());
        GLIFI_REQUIRE(!std::filesystem::exists(
            temporary.path() / glifistore::segment_filename(identity(manifest, replacement))));
        GLIFI_REQUIRE(!std::filesystem::exists(
            temporary.path() /
            ('.' + glifistore::segment_filename(identity(manifest, replacement)) + ".tmp")));
        GLIFI_REQUIRE(directory->healthy());
    }

    auto runtime = glifistore::DurableRuntimeCatalog::open_existing(temporary.path(), 100);
    GLIFI_REQUIRE(runtime.has_value());
    GLIFI_REQUIRE((*runtime)->manifest() == manifest);
    GLIFI_REQUIRE((*runtime)->get("live-a", 100).has_value());
    GLIFI_REQUIRE((*runtime)->get("replacement", 100).has_value());
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() /
                                           glifistore::segment_filename(identity(manifest, replacement))));
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
}

GLIFI_TEST("durable compaction promotion failure after intent is rolled back on reopen") {
    CompactionBuildDirectory temporary;
    const auto manifest = build_manifest();
    const glifistore::ManifestSegmentEntry replacement{
        .segment_id = glifistore::SegmentId{1},
        .generation = glifistore::GenerationId{2},
        .owner_worker = glifistore::WorkerId{0},
        .role = glifistore::ManifestSegmentRole::sealed,
    };
    GeneratedCompactionFailure failure{.target = glifistore::FilesystemOperation::rename_segment};
    {
        auto directory = glifistore::DataDirectory::open_and_lock(
            temporary.path(), {.context = &failure, .before = &GeneratedCompactionFailure::before});
        GLIFI_REQUIRE(directory.has_value());
        auto fixture = create_build_fixture(*directory);
        failure.enabled = true;
        auto built = glifistore::build_durable_worker_compaction(*directory, fixture.manifest,
                                                                 glifistore::WorkerId{0}, fixture.index, 100);
        GLIFI_REQUIRE(!built.succeeded());
        GLIFI_REQUIRE(built.outcome == glifistore::DurableCompactionBuildOutcome::recovery_required);
        GLIFI_REQUIRE(built.error.has_value());
        GLIFI_REQUIRE(failure.fired);
        GLIFI_REQUIRE(directory->read_compaction_intent().has_value());
        GLIFI_REQUIRE(!std::filesystem::exists(
            temporary.path() / glifistore::segment_filename(identity(manifest, replacement))));
    }

    auto runtime = glifistore::DurableRuntimeCatalog::open_existing(temporary.path(), 100);
    GLIFI_REQUIRE(runtime.has_value());
    GLIFI_REQUIRE((*runtime)->manifest() == manifest);
    GLIFI_REQUIRE((*runtime)->get("live-a", 100).has_value());
    GLIFI_REQUIRE((*runtime)->get("replacement", 100).has_value());
    GLIFI_REQUIRE((*runtime)->namespace_audit().clean());
}

GLIFI_TEST("durable runtime installs and retires one Worker compaction atomically") {
    CompactionBuildDirectory temporary;
    const auto old = build_manifest();
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        static_cast<void>(create_build_fixture(*directory));
    }

    auto runtime = glifistore::DurableRuntimeCatalog::open_existing(temporary.path(), 100);
    GLIFI_REQUIRE(runtime.has_value());
    const std::string hot_key{"active-hot"};
    const std::string hot_value{"cached"};
    GLIFI_REQUIRE(
        (*runtime)->put(std::as_bytes(std::span{hot_key}), std::as_bytes(std::span{hot_value})).committed());
    GLIFI_REQUIRE((*runtime)->hot_cache_stats()[0].resident_entries == 1);
    const auto result = (*runtime)->compact_worker(0, 100, 0, 1'000);
    GLIFI_REQUIRE(result.compacted());
    GLIFI_REQUIRE(result.stats.source_index_records_verified == 2);
    GLIFI_REQUIRE(result.stats.records_copied == 2);
    GLIFI_REQUIRE(result.stats.expired_records_dropped == 0);
    GLIFI_REQUIRE(result.stats.pre_intent_duration_ns > 0);
    GLIFI_REQUIRE(result.stats.publication_lease_duration_ns > 0);
    GLIFI_REQUIRE(result.stats.pacing_delay_ns > 0);
    GLIFI_REQUIRE(result.stats.pacing_sleep_count > 0);
    GLIFI_REQUIRE(result.stats.pacing_burst_bytes == 10);
    GLIFI_REQUIRE(result.stats.transient_metadata_lower_bound_bytes > 0);
    const auto next = (*runtime)->manifest();
    GLIFI_REQUIRE(next.manifest_generation == old.manifest_generation + 1U);
    GLIFI_REQUIRE(next.segments.size() == 2);
    GLIFI_REQUIRE(next.segments[0].segment_id == glifistore::SegmentId{1});
    GLIFI_REQUIRE(next.segments[0].generation == glifistore::GenerationId{2});
    GLIFI_REQUIRE(next.segments[1] == old.segments[2]);
    GLIFI_REQUIRE((*runtime)->namespace_audit().recovery_safe());
    GLIFI_REQUIRE((*runtime)->verify_index().has_value());
    GLIFI_REQUIRE(text(*(*runtime)->get("live-a", 100)) == "alpha");
    GLIFI_REQUIRE(text(*(*runtime)->get("replacement", 100)) == "new");
    GLIFI_REQUIRE(text(*(*runtime)->get("active", 100)) == "current");
    GLIFI_REQUIRE(text(*(*runtime)->get(hot_key, 100)) == hot_value);
    GLIFI_REQUIRE((*runtime)->hot_cache_stats()[0].resident_entries == 1);
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() /
                                           glifistore::segment_filename(identity(old, old.segments[0]))));
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() /
                                           glifistore::segment_filename(identity(old, old.segments[1]))));
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
    runtime->reset();

    auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path(), 100);
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE((*reopened)->manifest() == next);
    GLIFI_REQUIRE((*reopened)->verify_index().has_value());
    GLIFI_REQUIRE((*reopened)->get("live-a", 100)->sequence == 2);
    GLIFI_REQUIRE((*reopened)->get("replacement", 100)->sequence == 4);
    GLIFI_REQUIRE((*reopened)->get("active", 100)->sequence == 7);
    GLIFI_REQUIRE((*reopened)->get(hot_key, 100)->sequence == 8);
}

GLIFI_TEST("durable runtime rechecks the inclusive maintenance copy budget at snapshot") {
    CompactionBuildDirectory temporary;
    const auto old = build_manifest();
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        static_cast<void>(create_build_fixture(*directory));
    }

    auto runtime = glifistore::DurableRuntimeCatalog::open_existing(temporary.path(), 100);
    GLIFI_REQUIRE(runtime.has_value());
    const auto observation = (*runtime)->maintenance_observation();
    GLIFI_REQUIRE(observation.has_value());
    GLIFI_REQUIRE(observation->candidate_live_record_bytes > 0);

    const auto rejected = (*runtime)->compact_worker(0, 100, observation->candidate_live_record_bytes - 1U);
    GLIFI_REQUIRE(!rejected.compacted());
    GLIFI_REQUIRE(rejected.outcome == glifistore::DurableCompactionOutcome::not_compacted);
    GLIFI_REQUIRE(rejected.error.has_value());
    GLIFI_REQUIRE(rejected.error->code == glifistore::ErrorCode::sequence_conflict);
    GLIFI_REQUIRE((*runtime)->manifest() == old);
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));

    const auto accepted = (*runtime)->compact_worker(0, 100, observation->candidate_live_record_bytes);
    GLIFI_REQUIRE(accepted.compacted());
    GLIFI_REQUIRE(accepted.stats.bytes_copied <= observation->candidate_live_record_bytes);
}

GLIFI_TEST("durable runtime builds two compaction outputs and reopens every maximum Record") {
    CompactionBuildDirectory temporary;
    const glifistore::Manifest manifest{
        .store_id = {std::byte{0x61}, std::byte{0x62}, std::byte{0x63}},
        .manifest_generation = 41,
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
    constexpr auto kRecordCount = std::size_t{64};
    std::vector<std::string> keys;
    keys.reserve(kRecordCount);
    for (std::size_t index = 0; index < kRecordCount; ++index) {
        keys.push_back("multi-output-key-" + std::to_string(1'000U + index).substr(1));
    }
    GLIFI_REQUIRE(keys.front().size() == keys.back().size());
    const std::string value(
        glifistore::kMaxNormalRecordSize - glifistore::kEncodedRecordHeaderSize - keys.front().size(), 'm');

    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        std::size_t key_index{};
        std::uint64_t sequence{1};
        const std::vector<std::size_t> group_sizes{22, 21, 21};
        for (std::size_t segment_index = 0; segment_index < group_sizes.size(); ++segment_index) {
            const auto group_size = group_sizes[segment_index];
            std::vector<TestRecord> records;
            records.reserve(group_size);
            for (std::size_t group_index = 0; group_index < group_size; ++group_index) {
                records.push_back({.sequence = sequence++, .key = keys[key_index++], .value = value});
            }
            static_cast<void>(
                create_records(*directory, manifest, manifest.segments[segment_index], records));
        }
        const std::vector<TestRecord> no_active_records;
        static_cast<void>(create_records(*directory, manifest, manifest.segments[3], no_active_records));
        GLIFI_REQUIRE(key_index == kRecordCount);
        GLIFI_REQUIRE(directory->publish_manifest(manifest).durable());
    }

    auto runtime = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
    GLIFI_REQUIRE(runtime.has_value());
    const auto compacted = (*runtime)->compact_worker(0, 0);
    GLIFI_REQUIRE(compacted.compacted());
    GLIFI_REQUIRE(compacted.stats.source_index_records_verified == kRecordCount);
    GLIFI_REQUIRE(compacted.stats.records_copied == kRecordCount);
    GLIFI_REQUIRE(compacted.stats.bytes_copied == kRecordCount * glifistore::kMaxNormalRecordSize);
    const auto next = (*runtime)->manifest();
    GLIFI_REQUIRE(next.manifest_generation == manifest.manifest_generation + 1U);
    GLIFI_REQUIRE(next.segments.size() == 3);
    GLIFI_REQUIRE(next.segments[0].segment_id == glifistore::SegmentId{1});
    GLIFI_REQUIRE(next.segments[0].generation == glifistore::GenerationId{2});
    GLIFI_REQUIRE(next.segments[1].segment_id == glifistore::SegmentId{2});
    GLIFI_REQUIRE(next.segments[1].generation == glifistore::GenerationId{2});
    GLIFI_REQUIRE(next.segments[2] == manifest.segments[3]);
    GLIFI_REQUIRE((*runtime)->verify_index().has_value());
    runtime->reset();

    auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE((*reopened)->manifest() == next);
    GLIFI_REQUIRE((*reopened)->verify_index().has_value());
    for (const auto& key : keys) {
        const auto found = (*reopened)->get(key);
        GLIFI_REQUIRE(found.has_value());
        GLIFI_REQUIRE(found->bytes.size() == value.size());
        GLIFI_REQUIRE(found->bytes.front() == std::byte{0x6D});
        GLIFI_REQUIRE(found->bytes.back() == std::byte{0x6D});
    }
}

GLIFI_TEST("multi-seed durable compaction histories match their models before and after reopen") {
    struct FaultCase {
        std::uint64_t seed{};
        glifistore::FilesystemOperation operation{};
        std::size_t occurrence{1};
        bool expects_next_authority{};
    };

    constexpr std::array<std::uint64_t, 4> kSeeds{
        0xC04F'AC71'0A5E'2026ULL,
        0xC04F'AC71'0A5E'2027ULL,
        0x51A7'E001'BADC'0FFEULL,
        0xD15C'A4D0'5EED'0001ULL,
    };
    constexpr std::array<FaultCase, 5> kFaultCases{
        FaultCase{.seed = 0xFA17'0000'0000'0001ULL,
                  .operation = glifistore::FilesystemOperation::write_compaction_intent},
        FaultCase{.seed = 0xFA17'0000'0000'0002ULL,
                  .operation = glifistore::FilesystemOperation::write_record,
                  .occurrence = 3},
        FaultCase{.seed = 0xFA17'0000'0000'0003ULL,
                  .operation = glifistore::FilesystemOperation::sync_manifest},
        FaultCase{.seed = 0xFA17'0000'0000'0004ULL,
                  .operation = glifistore::FilesystemOperation::remove_compaction_segment,
                  .occurrence = 2,
                  .expects_next_authority = true},
        FaultCase{.seed = 0xFA17'0000'0000'0005ULL,
                  .operation = glifistore::FilesystemOperation::remove_compaction_intent,
                  .expects_next_authority = true},
    };
    const auto run_seed = [](const std::uint64_t seed, const std::optional<FaultCase> fault) {
        CompactionBuildDirectory temporary;
        const glifistore::Manifest manifest{
            .store_id = {std::byte{0x71}, std::byte{0x72}, std::byte{0x73}},
            .manifest_generation = 51,
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
        constexpr auto kNowNs = std::uint64_t{1'000};
        constexpr auto kKeyCount = std::size_t{32};
        struct HistoryOperation {
            std::uint64_t sequence{};
            glifistore::Opcode opcode{glifistore::Opcode::put};
            std::size_t key_index{};
            std::string value;
            std::uint64_t expire_at_ns{};
        };
        std::vector<std::string> keys;
        keys.reserve(kKeyCount);
        for (std::size_t index = 0; index < kKeyCount; ++index) {
            keys.push_back("history-key-" + std::to_string(index));
        }
        std::vector<std::vector<HistoryOperation>> history(4);
        std::vector<std::optional<std::string>> expected(kKeyCount);
        std::vector<bool> latest_in_active(kKeyCount);
        std::uint64_t next_sequence{1};
        const auto append = [&](const std::size_t segment_index, const std::size_t key_index,
                                const glifistore::Opcode opcode, std::string value,
                                const std::uint64_t expire_at_ns) {
            history[segment_index].push_back({.sequence = next_sequence++,
                                              .opcode = opcode,
                                              .key_index = key_index,
                                              .value = std::move(value),
                                              .expire_at_ns = expire_at_ns});
            latest_in_active[key_index] = segment_index == 3;
            if (opcode == glifistore::Opcode::erase || (expire_at_ns != 0 && expire_at_ns <= kNowNs)) {
                expected[key_index].reset();
            } else {
                expected[key_index] = history[segment_index].back().value;
            }
        };

        for (std::size_t key_index = 0; key_index < kKeyCount; ++key_index) {
            append(0, key_index, glifistore::Opcode::put, "baseline-" + std::to_string(key_index), 0);
        }
        std::mt19937_64 random{seed};
        const auto append_generated = [&](const std::size_t segment_index, const std::size_t count,
                                          const std::size_t key_space) {
            for (std::size_t operation = 0; operation < count; ++operation) {
                const auto key_index = static_cast<std::size_t>(random() % key_space);
                const auto choice = random() % 6U;
                if (choice == 0) {
                    append(segment_index, key_index, glifistore::Opcode::erase, {}, 0);
                } else {
                    const auto expiry = choice == 1 ? kNowNs - 1U : 0U;
                    append(segment_index, key_index, glifistore::Opcode::put,
                           "value-" + std::to_string(next_sequence) + "-" + std::to_string(random()), expiry);
                }
            }
        };
        append_generated(0, 8, kKeyCount);
        append_generated(1, 40, kKeyCount);
        append_generated(2, 40, kKeyCount);
        append_generated(3, 32, 8);

        {
            auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
            GLIFI_REQUIRE(directory.has_value());
            for (std::size_t segment_index = 0; segment_index < history.size(); ++segment_index) {
                std::vector<TestRecord> records;
                records.reserve(history[segment_index].size());
                for (const auto& operation : history[segment_index]) {
                    records.push_back({.sequence = operation.sequence,
                                       .opcode = operation.opcode,
                                       .key = keys[operation.key_index],
                                       .value = operation.value,
                                       .expire_at_ns = operation.expire_at_ns});
                }
                static_cast<void>(
                    create_records(*directory, manifest, manifest.segments[segment_index], records));
            }
            GLIFI_REQUIRE(directory->publish_manifest(manifest).durable());
        }

        const auto verify_model = [&](glifistore::DurableRuntimeCatalog& runtime) {
            for (std::size_t key_index = 0; key_index < kKeyCount; ++key_index) {
                const auto found = runtime.get(keys[key_index], kNowNs);
                if (expected[key_index]) {
                    GLIFI_REQUIRE(found.has_value());
                    GLIFI_REQUIRE(text(*found) == *expected[key_index]);
                } else {
                    GLIFI_REQUIRE(!found.has_value());
                    GLIFI_REQUIRE(found.error().code == glifistore::ErrorCode::not_found);
                }
            }
        };
        std::size_t expected_copied{};
        for (std::size_t key_index = 0; key_index < kKeyCount; ++key_index) {
            if (expected[key_index] && !latest_in_active[key_index]) {
                ++expected_copied;
            }
        }
        GLIFI_REQUIRE(expected_copied > 0);

        GeneratedCompactionFailure failure;
        glifistore::FilesystemHooks hooks;
        if (fault) {
            failure.target = fault->operation;
            failure.fail_on_matching_call = fault->occurrence;
            hooks = {.context = &failure, .before = &GeneratedCompactionFailure::before};
        }
        auto runtime = glifistore::DurableRuntimeCatalog::open_existing(temporary.path(), kNowNs, hooks);
        GLIFI_REQUIRE(runtime.has_value());
        verify_model(**runtime);
        failure.enabled = fault.has_value();
        const auto compacted = (*runtime)->compact_worker(0, kNowNs);
        if (fault) {
            GLIFI_REQUIRE(failure.fired);
            GLIFI_REQUIRE(!compacted.compacted());
            GLIFI_REQUIRE(compacted.error.has_value());
            GLIFI_REQUIRE(compacted.error->code == glifistore::ErrorCode::io_error);
            GLIFI_REQUIRE(compacted.outcome == glifistore::DurableCompactionOutcome::not_compacted ||
                          compacted.outcome == glifistore::DurableCompactionOutcome::recovery_required);
            GLIFI_REQUIRE((*runtime)->healthy() ==
                          (compacted.outcome == glifistore::DurableCompactionOutcome::not_compacted));
            if ((*runtime)->healthy()) {
                verify_model(**runtime);
            }
            runtime->reset();

            auto recovered = glifistore::DurableRuntimeCatalog::open_existing(temporary.path(), kNowNs);
            GLIFI_REQUIRE(recovered.has_value());
            GLIFI_REQUIRE((*recovered)->namespace_audit().clean());
            GLIFI_REQUIRE((*recovered)->verify_index().has_value());
            verify_model(**recovered);
            if (fault->expects_next_authority) {
                auto expected_next = manifest;
                ++expected_next.manifest_generation;
                ++expected_next.segments[0].generation.value;
                expected_next.segments.erase(expected_next.segments.begin() + 1,
                                             expected_next.segments.end() - 1);
                GLIFI_REQUIRE((*recovered)->manifest() == expected_next);
            } else {
                GLIFI_REQUIRE((*recovered)->manifest() == manifest);
            }
            return;
        }
        GLIFI_REQUIRE(compacted.compacted());
        GLIFI_REQUIRE(compacted.stats.source_index_records_verified == expected_copied);
        GLIFI_REQUIRE(compacted.stats.records_copied == expected_copied);
        verify_model(**runtime);
        const auto next = (*runtime)->manifest();
        GLIFI_REQUIRE(next.manifest_generation == manifest.manifest_generation + 1U);
        GLIFI_REQUIRE(next.segments.size() == 2);
        GLIFI_REQUIRE(next.segments.front().generation == glifistore::GenerationId{2});
        GLIFI_REQUIRE(next.segments.back() == manifest.segments.back());
        runtime->reset();

        auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path(), kNowNs);
        GLIFI_REQUIRE(reopened.has_value());
        GLIFI_REQUIRE((*reopened)->manifest() == next);
        GLIFI_REQUIRE((*reopened)->namespace_audit().clean());
        GLIFI_REQUIRE((*reopened)->verify_index().has_value());
        verify_model(**reopened);
    };
    for (const auto seed : kSeeds) {
        try {
            run_seed(seed, std::nullopt);
        } catch (const std::exception& exception) {
            throw std::runtime_error("generated durable compaction seed " + std::to_string(seed) +
                                     " failed: " + exception.what());
        }
    }
    for (const auto& fault : kFaultCases) {
        try {
            run_seed(fault.seed, fault);
        } catch (const std::exception& exception) {
            throw std::runtime_error("faulted durable compaction seed " + std::to_string(fault.seed) +
                                     " failed: " + exception.what());
        }
    }
}

GLIFI_TEST("durable runtime fails closed when online compaction requires recovery") {
    CompactionBuildDirectory temporary;
    const auto old = build_manifest();
    GeneratedCompactionFailure failure{
        .target = glifistore::FilesystemOperation::rename_segment,
        .fail_on_matching_call = 1,
    };
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        static_cast<void>(create_build_fixture(*directory));
    }
    {
        auto runtime = glifistore::DurableRuntimeCatalog::open_existing(
            temporary.path(), 100, {.context = &failure, .before = &GeneratedCompactionFailure::before});
        GLIFI_REQUIRE(runtime.has_value());
        failure.enabled = true;
        const auto result = (*runtime)->compact_worker(0, 100);
        GLIFI_REQUIRE(!result.compacted());
        GLIFI_REQUIRE(result.outcome == glifistore::DurableCompactionOutcome::recovery_required);
        GLIFI_REQUIRE(result.error.has_value());
        GLIFI_REQUIRE(failure.fired);
        GLIFI_REQUIRE(!(*runtime)->healthy());
        GLIFI_REQUIRE(!(*runtime)->get("active", 100).has_value());
    }

    auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path(), 100);
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE((*reopened)->manifest() == old);
    GLIFI_REQUIRE((*reopened)->verify_index().has_value());
    GLIFI_REQUIRE((*reopened)->get("live-a", 100).has_value());
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
}

GLIFI_TEST("ordinary recovery cleans a pre-intent staged compaction temporary") {
    CompactionBuildDirectory temporary;
    auto manifest = build_manifest();
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        static_cast<void>(create_build_fixture(*directory));
    }

    auto replacement = manifest.segments.front();
    ++replacement.generation.value;
    const auto replacement_identity = identity(manifest, replacement);
    const auto staged_path =
        temporary.path() / ('.' + glifistore::segment_filename(replacement_identity) + ".tmp");
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        auto staged = glifistore::DurableSegmentFile::create_staged(*directory, replacement_identity);
        GLIFI_REQUIRE(staged.has_value());
        GLIFI_REQUIRE(std::filesystem::exists(staged_path));
    }

    auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path(), 100);
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE((*reopened)->manifest() == manifest);
    GLIFI_REQUIRE((*reopened)->verify_index().has_value());
    GLIFI_REQUIRE((*reopened)->namespace_audit().clean());
    GLIFI_REQUIRE(!std::filesystem::exists(staged_path));
    GLIFI_REQUIRE((*reopened)->get("live-a", 100).has_value());
}

GLIFI_TEST("online compaction preserves another Worker's cached Segment after catalog compaction") {
    CompactionBuildDirectory temporary;
    const glifistore::Manifest manifest{
        .store_id = {std::byte{0x51}, std::byte{0x52}, std::byte{0x53}},
        .manifest_generation = 9,
        .worker_count = 2,
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
                 .role = glifistore::ManifestSegmentRole::active},
                {.segment_id = glifistore::SegmentId{4},
                 .generation = glifistore::GenerationId{1},
                 .owner_worker = glifistore::WorkerId{1},
                 .role = glifistore::ManifestSegmentRole::active},
            },
    };
    const auto first_key = key_for_worker(0, 2, "compact-first-");
    const auto second_key = key_for_worker(0, 2, "compact-second-");
    const auto active_key = key_for_worker(0, 2, "compact-active-");
    const auto other_key = key_for_worker(1, 2, "cached-other-");
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        const std::vector<TestRecord> first{{.sequence = 1, .key = first_key, .value = "first"}};
        const std::vector<TestRecord> second{{.sequence = 2, .key = second_key, .value = "second"}};
        const std::vector<TestRecord> active{{.sequence = 3, .key = active_key, .value = "active"}};
        const std::vector<TestRecord> other{{.sequence = 1, .key = other_key, .value = "other"}};
        static_cast<void>(create_records(*directory, manifest, manifest.segments[0], first));
        static_cast<void>(create_records(*directory, manifest, manifest.segments[1], second));
        static_cast<void>(create_records(*directory, manifest, manifest.segments[2], active));
        static_cast<void>(create_records(*directory, manifest, manifest.segments[3], other));
        GLIFI_REQUIRE(directory->publish_manifest(manifest).durable());
    }

    auto runtime = glifistore::DurableRuntimeCatalog::open_existing(temporary.path(), 100);
    GLIFI_REQUIRE(runtime.has_value());
    GLIFI_REQUIRE(text(*(*runtime)->get(other_key, 100)) == "other");
    const auto compacted = (*runtime)->compact_worker(0, 100);
    GLIFI_REQUIRE(compacted.compacted());
    GLIFI_REQUIRE((*runtime)->manifest().segments.size() == 3);
    GLIFI_REQUIRE(text(*(*runtime)->get(other_key, 100)) == "other");
    GLIFI_REQUIRE(text(*(*runtime)->get(first_key, 100)) == "first");
    GLIFI_REQUIRE(text(*(*runtime)->get(second_key, 100)) == "second");
    GLIFI_REQUIRE((*runtime)->verify_index().has_value());
    const auto skipped = (*runtime)->compact_worker(1, 100);
    GLIFI_REQUIRE(!skipped.compacted());
    GLIFI_REQUIRE(skipped.outcome == glifistore::DurableCompactionOutcome::not_compacted);
    GLIFI_REQUIRE(skipped.error.has_value());
    GLIFI_REQUIRE(skipped.error->code == glifistore::ErrorCode::not_found);
    GLIFI_REQUIRE((*runtime)->healthy());
    GLIFI_REQUIRE(text(*(*runtime)->get(other_key, 100)) == "other");
}

GLIFI_TEST("public Store compact drops sealed Index-resident expired puts") {
    CompactionBuildDirectory temporary;
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        static_cast<void>(create_build_fixture(*directory));
    }

    // Recover before expiry so the Index still names the sealed expired put.
    const auto clock = std::make_shared<ManualStoreClock>(40);
    auto store = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = temporary.path(),
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
        .clock = clock,
    });
    GLIFI_REQUIRE(store.has_value());
    GLIFI_REQUIRE((*store)->get("expired").has_value());

    // Advance time without a GET reclaim so sealed compaction owns the TTL drop.
    clock->set(100);
    const auto compacted = (*store)->compact();
    GLIFI_REQUIRE(compacted.has_value());
    GLIFI_REQUIRE(compacted->compacted);
    GLIFI_REQUIRE(compacted->worker_index == 0);
    GLIFI_REQUIRE(compacted->source_records_verified == 3);
    GLIFI_REQUIRE(compacted->records_copied == 2);
    GLIFI_REQUIRE(compacted->expired_records_dropped == 1);
    GLIFI_REQUIRE(text(*(*store)->get("live-a")) == "alpha");
    GLIFI_REQUIRE(text(*(*store)->get("replacement")) == "new");
    GLIFI_REQUIRE(text(*(*store)->get("active")) == "current");
    const auto expired = (*store)->get("expired");
    GLIFI_REQUIRE(!expired.has_value());
    GLIFI_REQUIRE(expired.error().code == glifistore::ErrorCode::not_found);
    GLIFI_REQUIRE((*store)->verify_index().has_value());
    GLIFI_REQUIRE((*store)->close().has_value());

    auto reopened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = temporary.path(),
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
        .clock = clock,
    });
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE(text(*(*reopened)->get("live-a")) == "alpha");
    GLIFI_REQUIRE(!(*reopened)->get("expired").has_value());
}

GLIFI_TEST("maintenance observation counts unread expired sealed TTL puts") {
    CompactionBuildDirectory temporary;
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        static_cast<void>(create_build_fixture(*directory));
    }

    auto runtime = glifistore::DurableRuntimeCatalog::open_existing(temporary.path(), 40);
    GLIFI_REQUIRE(runtime.has_value());

    const auto without_probe = (*runtime)->maintenance_observation(0, 100, false);
    GLIFI_REQUIRE(without_probe.has_value());
    GLIFI_REQUIRE(!without_probe->unread_ttl_probe_performed);
    GLIFI_REQUIRE(without_probe->candidate_unread_expired_sealed_record_count == 0);

    const auto probed = (*runtime)->maintenance_observation(0, 100, true);
    GLIFI_REQUIRE(probed.has_value());
    GLIFI_REQUIRE(probed->unread_ttl_probe_performed);
    GLIFI_REQUIRE(probed->candidate_unread_expired_sealed_record_count == 1);
    GLIFI_REQUIRE(probed->candidate_unread_expired_sealed_record_bytes > 0);
}

GLIFI_TEST("public Store compacts one scheduled Worker and preserves restart visibility") {
    CompactionBuildDirectory temporary;
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        static_cast<void>(create_build_fixture(*directory));
    }

    auto store = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = temporary.path(),
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    GLIFI_REQUIRE(store.has_value());
    const auto compacted = (*store)->compact();
    GLIFI_REQUIRE(compacted.has_value());
    GLIFI_REQUIRE(compacted->compacted);
    GLIFI_REQUIRE(compacted->worker_index == 0);
    GLIFI_REQUIRE(compacted->source_records_verified == 2);
    GLIFI_REQUIRE(compacted->records_copied == 2);
    GLIFI_REQUIRE(compacted->expired_records_dropped == 0);
    GLIFI_REQUIRE(text(*(*store)->get("live-a")) == "alpha");
    GLIFI_REQUIRE(text(*(*store)->get("replacement")) == "new");
    GLIFI_REQUIRE((*store)->verify_index().has_value());

    const auto no_gain = (*store)->compact();
    GLIFI_REQUIRE(no_gain.has_value());
    GLIFI_REQUIRE(!no_gain->compacted);
    GLIFI_REQUIRE(no_gain->worker_index == 0);
    GLIFI_REQUIRE(no_gain->source_records_verified == 2);
    GLIFI_REQUIRE(no_gain->source_bytes_verified > 0);
    GLIFI_REQUIRE(no_gain->records_copied == 0);
    GLIFI_REQUIRE(no_gain->bytes_copied == 0);
    GLIFI_REQUIRE(no_gain->expired_records_dropped == 0);
    GLIFI_REQUIRE((*store)->close().has_value());

    auto reopened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = temporary.path(),
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE(text(*(*reopened)->get("live-a")) == "alpha");
    GLIFI_REQUIRE(text(*(*reopened)->get("replacement")) == "new");
}

GLIFI_TEST("public Store compaction scheduler advances one Worker per call") {
    CompactionBuildDirectory temporary;
    const glifistore::Manifest manifest{
        .store_id = {std::byte{0x61}, std::byte{0x62}, std::byte{0x63}},
        .manifest_generation = 12,
        .worker_count = 2,
        .routing_epoch = 1,
        .next_segment_id = glifistore::SegmentId{7},
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
                 .role = glifistore::ManifestSegmentRole::active},
                {.segment_id = glifistore::SegmentId{4},
                 .generation = glifistore::GenerationId{1},
                 .owner_worker = glifistore::WorkerId{1},
                 .role = glifistore::ManifestSegmentRole::sealed},
                {.segment_id = glifistore::SegmentId{5},
                 .generation = glifistore::GenerationId{1},
                 .owner_worker = glifistore::WorkerId{1},
                 .role = glifistore::ManifestSegmentRole::sealed},
                {.segment_id = glifistore::SegmentId{6},
                 .generation = glifistore::GenerationId{1},
                 .owner_worker = glifistore::WorkerId{1},
                 .role = glifistore::ManifestSegmentRole::active},
            },
    };
    const auto worker_zero_first = key_for_worker(0, 2, "schedule-zero-first-");
    const auto worker_zero_second = key_for_worker(0, 2, "schedule-zero-second-");
    const auto worker_one_first = key_for_worker(1, 2, "schedule-one-first-");
    const auto worker_one_second = key_for_worker(1, 2, "schedule-one-second-");
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        const std::vector<TestRecord> zero_first{
            {.sequence = 1, .key = worker_zero_first, .value = "zero-first"}};
        const std::vector<TestRecord> zero_second{
            {.sequence = 2, .key = worker_zero_second, .value = "zero-second"}};
        const std::vector<TestRecord> one_first{
            {.sequence = 1, .key = worker_one_first, .value = "one-first"}};
        const std::vector<TestRecord> one_second{
            {.sequence = 2, .key = worker_one_second, .value = "one-second"}};
        static_cast<void>(create_records(*directory, manifest, manifest.segments[0], zero_first));
        static_cast<void>(create_records(*directory, manifest, manifest.segments[1], zero_second));
        static_cast<void>(create_records(*directory, manifest, manifest.segments[2], {}));
        static_cast<void>(create_records(*directory, manifest, manifest.segments[3], one_first));
        static_cast<void>(create_records(*directory, manifest, manifest.segments[4], one_second));
        static_cast<void>(create_records(*directory, manifest, manifest.segments[5], {}));
        GLIFI_REQUIRE(directory->publish_manifest(manifest).durable());
    }

    auto store = glifistore::Store::open({
        .worker_config = {.explicit_count = 2},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = temporary.path(),
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    GLIFI_REQUIRE(store.has_value());
    const auto first = (*store)->compact();
    GLIFI_REQUIRE(first.has_value());
    GLIFI_REQUIRE(first->compacted);
    GLIFI_REQUIRE(first->worker_index == 0);
    const auto second = (*store)->compact();
    GLIFI_REQUIRE(second.has_value());
    GLIFI_REQUIRE(second->compacted);
    GLIFI_REQUIRE(second->worker_index == 1);
    const auto no_gain = (*store)->compact();
    GLIFI_REQUIRE(no_gain.has_value());
    GLIFI_REQUIRE(!no_gain->compacted);
    GLIFI_REQUIRE(no_gain->worker_index.has_value());
    GLIFI_REQUIRE(no_gain->source_records_verified > 0);
    GLIFI_REQUIRE(no_gain->source_bytes_verified > 0);
    GLIFI_REQUIRE(no_gain->records_copied == 0);
    GLIFI_REQUIRE(text(*(*store)->get(worker_zero_first)) == "zero-first");
    GLIFI_REQUIRE(text(*(*store)->get(worker_zero_second)) == "zero-second");
    GLIFI_REQUIRE(text(*(*store)->get(worker_one_first)) == "one-first");
    GLIFI_REQUIRE(text(*(*store)->get(worker_one_second)) == "one-second");
    GLIFI_REQUIRE((*store)->verify_index().has_value());
}

GLIFI_TEST("background reclaim_threshold skip advances past live-only Worker to reclaimable peer") {
    // HAZ-026 end-to-end: Store observe advances the compaction cursor even when normal policy
    // skips reclaim_threshold, so a live-only Worker cannot starve a peer with overwrite debt.
    CompactionBuildDirectory temporary;
    const glifistore::Manifest manifest{
        .store_id = {std::byte{0x71}, std::byte{0x72}, std::byte{0x73}},
        .manifest_generation = 13,
        .worker_count = 2,
        .routing_epoch = 1,
        .next_segment_id = glifistore::SegmentId{7},
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
                 .role = glifistore::ManifestSegmentRole::active},
                {.segment_id = glifistore::SegmentId{4},
                 .generation = glifistore::GenerationId{1},
                 .owner_worker = glifistore::WorkerId{1},
                 .role = glifistore::ManifestSegmentRole::sealed},
                {.segment_id = glifistore::SegmentId{5},
                 .generation = glifistore::GenerationId{1},
                 .owner_worker = glifistore::WorkerId{1},
                 .role = glifistore::ManifestSegmentRole::sealed},
                {.segment_id = glifistore::SegmentId{6},
                 .generation = glifistore::GenerationId{1},
                 .owner_worker = glifistore::WorkerId{1},
                 .role = glifistore::ManifestSegmentRole::active},
            },
    };
    const auto worker_zero_first = key_for_worker(0, 2, "starve-zero-first-");
    const auto worker_zero_second = key_for_worker(0, 2, "starve-zero-second-");
    const auto worker_one_key = key_for_worker(1, 2, "starve-one-");
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        const std::vector<TestRecord> zero_first{
            {.sequence = 1, .key = worker_zero_first, .value = "zero-first"}};
        const std::vector<TestRecord> zero_second{
            {.sequence = 2, .key = worker_zero_second, .value = "zero-second"}};
        // Equal-sized overwrite history ⇒ ~50% dead bytes (meets inclusive 5000 bp threshold).
        const std::vector<TestRecord> one_old{{.sequence = 1, .key = worker_one_key, .value = "old-value"}};
        const std::vector<TestRecord> one_new{{.sequence = 2, .key = worker_one_key, .value = "new-value"}};
        static_cast<void>(create_records(*directory, manifest, manifest.segments[0], zero_first));
        static_cast<void>(create_records(*directory, manifest, manifest.segments[1], zero_second));
        static_cast<void>(create_records(*directory, manifest, manifest.segments[2], {}));
        static_cast<void>(create_records(*directory, manifest, manifest.segments[3], one_old));
        static_cast<void>(create_records(*directory, manifest, manifest.segments[4], one_new));
        static_cast<void>(create_records(*directory, manifest, manifest.segments[5], {}));
        GLIFI_REQUIRE(directory->publish_manifest(manifest).durable());
    }

    auto store = glifistore::Store::open({
        .worker_config = {.explicit_count = 2},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = temporary.path(),
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
        .maintenance =
            {
                .mode = glifistore::MaintenanceMode::background,
                .min_eval_interval_ms = 60'000,
                .max_eval_interval_ms = 60'000,
                .dead_byte_ratio_bp_normal = 5'000,
            },
    });
    GLIFI_REQUIRE(store.has_value());
    auto* controller = glifistore::detail::StoreAccess::maintenance_controller(**store);
    GLIFI_REQUIRE(controller != nullptr);

    const auto skip_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    glifistore::MaintenanceSnapshot after_skip{};
    while (std::chrono::steady_clock::now() < skip_deadline) {
        after_skip = (*store)->maintenance_snapshot();
        if (after_skip.last_skip_reason == glifistore::MaintenanceSkipReason::reclaim_threshold &&
            after_skip.last_observation.compaction_candidate_worker == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(after_skip.last_skip_reason == glifistore::MaintenanceSkipReason::reclaim_threshold);
    GLIFI_REQUIRE(after_skip.last_observation.compaction_candidate_worker == 0);
    GLIFI_REQUIRE(after_skip.compact_attempts == 0);
    GLIFI_REQUIRE(after_skip.useful_compactions == 0);

    controller->request_evaluate();
    const auto compact_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    glifistore::MaintenanceSnapshot after_compact{};
    while (std::chrono::steady_clock::now() < compact_deadline) {
        after_compact = (*store)->maintenance_snapshot();
        if (after_compact.useful_compactions > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(after_compact.useful_compactions > 0);
    GLIFI_REQUIRE(after_compact.compact_attempts > 0);
    GLIFI_REQUIRE(text(*(*store)->get(worker_zero_first)) == "zero-first");
    GLIFI_REQUIRE(text(*(*store)->get(worker_zero_second)) == "zero-second");
    GLIFI_REQUIRE(text(*(*store)->get(worker_one_key)) == "new-value");
    GLIFI_REQUIRE((*store)->verify_index().has_value());
    GLIFI_REQUIRE((*store)->close().has_value());
}

// GS-PERSIST-AMP-001: temporary-space budget rejects before durable intent; Mold
// remains sole authority and no compaction intent residue remains.
GLIFI_TEST("compaction temporary-space budget rejects before intent without residue") {
    CompactionBuildDirectory temporary;
    const auto old = build_manifest();
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        static_cast<void>(create_build_fixture(*directory));
    }

    glifistore::DurableResourceLimits limits{};
    limits.max_write_amplification = 4;
    // Two sealed → one output passes amp=1; force rejection via temporary budget alone.
    limits.max_temporary_compaction_bytes = 1;
    auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
    GLIFI_REQUIRE(directory.has_value());
    auto runtime = glifistore::DurableRuntimeCatalog::open_locked(
        std::move(*directory), 100, glifistore::DurableRuntimeOptions{.limits = limits});
    GLIFI_REQUIRE(runtime.has_value());
    const auto rejected = (*runtime)->compact_worker(0, 100);
    GLIFI_REQUIRE(!rejected.compacted());
    GLIFI_REQUIRE(rejected.outcome == glifistore::DurableCompactionOutcome::not_compacted);
    GLIFI_REQUIRE(rejected.error.has_value());
    GLIFI_REQUIRE(rejected.error->code == glifistore::ErrorCode::storage_exhausted);
    GLIFI_REQUIRE(rejected.error->message.find("temporary-space") != std::string::npos);
    GLIFI_REQUIRE((*runtime)->healthy());
    GLIFI_REQUIRE((*runtime)->manifest() == old);
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
    runtime->reset();

    auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path(), 100);
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE((*reopened)->manifest() == old);
    GLIFI_REQUIRE((*reopened)->namespace_audit().clean());
}

// GS-PERSIST-AMP-001: write-amplification alone rejects before intent (temporary budget
// remains permissive). Three sealed sources + live bytes requiring two outputs yields
// temporary=2·S and reclaimed=1·S, so amp=1 fails while temporary budget does not.
GLIFI_TEST("compaction write-amplification budget rejects before intent without residue") {
    CompactionBuildDirectory temporary;
    const auto old = amp_reject_manifest();
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        GLIFI_REQUIRE(directory->publish_manifest(old).durable());
        // Payload per Segment is ~64MiB-4KiB; ~33 max-size Records per sealed Segment
        // leaves enough live bytes for two outputs across three sealed sources.
        auto next_sequence = fill_sealed_with_max_records(*directory, old, old.segments[0], 1, 33, "amp-a-");
        next_sequence =
            fill_sealed_with_max_records(*directory, old, old.segments[1], next_sequence, 33, "amp-b-");
        next_sequence =
            fill_sealed_with_max_records(*directory, old, old.segments[2], next_sequence, 1, "amp-c-");
        const std::vector<TestRecord> active_records{
            {.sequence = next_sequence, .key = "amp-active", .value = "live"},
        };
        static_cast<void>(create_records(*directory, old, old.segments[3], active_records));
    }

    glifistore::DurableResourceLimits limits{};
    limits.max_write_amplification = 1;
    limits.max_temporary_compaction_bytes = 8ULL * glifistore::kSegmentSizeBytes;
    limits.max_store_bytes = 16ULL * glifistore::kSegmentSizeBytes;
    auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
    GLIFI_REQUIRE(directory.has_value());
    auto runtime = glifistore::DurableRuntimeCatalog::open_locked(
        std::move(*directory), 100, glifistore::DurableRuntimeOptions{.limits = limits});
    GLIFI_REQUIRE(runtime.has_value());
    const auto rejected = (*runtime)->compact_worker(0, 100);
    GLIFI_REQUIRE(!rejected.compacted());
    GLIFI_REQUIRE(rejected.outcome == glifistore::DurableCompactionOutcome::not_compacted);
    GLIFI_REQUIRE(rejected.error.has_value());
    GLIFI_REQUIRE(rejected.error->code == glifistore::ErrorCode::storage_exhausted);
    GLIFI_REQUIRE(rejected.error->message.find("write-amplification") != std::string::npos);
    GLIFI_REQUIRE((*runtime)->healthy());
    GLIFI_REQUIRE((*runtime)->manifest() == old);
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
    runtime->reset();

    auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path(), 100);
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE((*reopened)->manifest() == old);
    GLIFI_REQUIRE((*reopened)->namespace_audit().clean());
}
