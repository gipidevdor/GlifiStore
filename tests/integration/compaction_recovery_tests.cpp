#include "glifistore/core/key_hash.hpp"
#include "glifistore/persistence/compaction_intent.hpp"
#include "glifistore/persistence/runtime_catalog.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "glifistore/segment/record.hpp"
#include "support/compaction_model.hpp"
#include "support/file_identity.hpp"
#include "test.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

class CompactionRecoveryDirectory final {
  public:
    CompactionRecoveryDirectory() {
        auto pattern =
            (std::filesystem::temp_directory_path() / "glifistore-compaction-recovery-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* created = ::mkdtemp(writable.data());
        GLIFI_REQUIRE(created != nullptr);
        path_ = created;
    }

    ~CompactionRecoveryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] auto path() const -> const std::filesystem::path& {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

struct OneShotRecoveryFailure {
    glifistore::FilesystemOperation target{};
    std::size_t fail_on_matching_call{1};
    std::size_t matching_calls{};
    bool fired{};

    static auto before(void* context, const glifistore::FilesystemOperation operation) -> glifistore::Status {
        auto& failure = *static_cast<OneShotRecoveryFailure*>(context);
        if (operation != failure.target) {
            return {};
        }
        ++failure.matching_calls;
        if (failure.matching_calls != failure.fail_on_matching_call) {
            return {};
        }
        failure.fired = true;
        return glifistore::fail(glifistore::ErrorCode::io_error, "injected compaction recovery failure");
    }
};

auto compaction_manifests() -> std::pair<glifistore::Manifest, glifistore::Manifest> {
    glifistore::Manifest old{
        .store_id = {std::byte{0x71}, std::byte{0x72}, std::byte{0x73}},
        .manifest_generation = 11,
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
    auto next = old;
    ++next.manifest_generation;
    next.segments = {
        {.segment_id = glifistore::SegmentId{1},
         .generation = glifistore::GenerationId{2},
         .owner_worker = glifistore::WorkerId{0},
         .role = glifistore::ManifestSegmentRole::sealed},
        old.segments.back(),
    };
    return {std::move(old), std::move(next)};
}

auto multi_output_compaction_manifests() -> std::pair<glifistore::Manifest, glifistore::Manifest> {
    glifistore::Manifest old{
        .store_id = {std::byte{0x81}, std::byte{0x82}, std::byte{0x83}},
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

auto identity(const glifistore::Manifest& manifest, const glifistore::ManifestSegmentEntry& entry)
    -> glifistore::SegmentHeaderIdentity {
    return {.store_id = manifest.store_id,
            .segment_id = entry.segment_id,
            .generation = entry.generation,
            .owner_worker = entry.owner_worker};
}

void create_segment(glifistore::DataDirectory& directory, const glifistore::Manifest& manifest,
                    const glifistore::ManifestSegmentEntry& entry) {
    auto created = glifistore::DurableSegmentFile::create(directory, identity(manifest, entry));
    GLIFI_REQUIRE(created.durable());
    if (entry.role == glifistore::ManifestSegmentRole::sealed) {
        const auto sealed = created.file->seal();
        GLIFI_REQUIRE(sealed.committed());
    }
}

void prepare_interrupted_compaction(const std::filesystem::path& path, const glifistore::Manifest& old,
                                    const glifistore::Manifest& next, const bool publish_next) {
    auto directory = glifistore::DataDirectory::open_and_lock(path);
    GLIFI_REQUIRE(directory.has_value());
    GLIFI_REQUIRE(directory->publish_manifest(old).durable());
    for (const auto& entry : old.segments) {
        create_segment(*directory, old, entry);
    }
    const glifistore::DurableCompactionIntent intent{
        .worker_id = glifistore::WorkerId{0}, .old_manifest = old, .next_manifest = next};
    GLIFI_REQUIRE(directory->publish_compaction_intent(intent).durable());
    for (const auto& entry : next.segments) {
        if (std::find(old.segments.begin(), old.segments.end(), entry) == old.segments.end()) {
            create_segment(*directory, next, entry);
        }
    }
    if (publish_next) {
        GLIFI_REQUIRE(directory->publish_manifest(next).durable());
    }
}

void prepare_interrupted_compaction(const std::filesystem::path& path, const bool publish_next) {
    const auto [old, next] = compaction_manifests();
    prepare_interrupted_compaction(path, old, next, publish_next);
}

auto segment_path(const std::filesystem::path& directory, const glifistore::Manifest& manifest,
                  const glifistore::ManifestSegmentEntry& entry) -> std::filesystem::path {
    return directory / glifistore::segment_filename(identity(manifest, entry));
}

auto segment_temporary_path(const std::filesystem::path& directory, const glifistore::Manifest& manifest,
                            const glifistore::ManifestSegmentEntry& entry) -> std::filesystem::path {
    return directory / ('.' + glifistore::segment_filename(identity(manifest, entry)) + ".tmp");
}

// Strict sealed-prefix transactions (ADR 0044). Unlike the empty Segments above these carry Records, so
// the model that recovery must reproduce under each authority is observable, and so is the identity of
// the sealed Segments after the prefix, which no recovery path may touch.
struct PrefixRecord {
    std::uint64_t sequence{};
    glifistore::Opcode opcode{glifistore::Opcode::put};
    std::string key;
    std::string value;
};

struct PrefixSegment {
    glifistore::ManifestSegmentEntry entry;
    std::vector<PrefixRecord> records;
};

struct PrefixExpectation {
    std::string key;
    std::optional<std::string> value;
};

struct PrefixTransaction {
    glifistore::Manifest old;
    glifistore::Manifest next;
    std::vector<PrefixSegment> old_segments;
    std::vector<PrefixSegment> outputs;
    std::vector<PrefixExpectation> model;
};

auto prefix_entry(const std::uint64_t segment_id, const std::uint32_t generation,
                  const glifistore::ManifestSegmentRole role) -> glifistore::ManifestSegmentEntry {
    return {.segment_id = glifistore::SegmentId{segment_id},
            .generation = glifistore::GenerationId{generation},
            .owner_worker = glifistore::WorkerId{0},
            .role = role};
}

auto prefix_old_manifest(const std::size_t sealed_count) -> glifistore::Manifest {
    glifistore::Manifest manifest{
        .store_id = {std::byte{0x91}, std::byte{0x92}, std::byte{0x93}},
        .manifest_generation = 21,
        .worker_count = 1,
        .routing_epoch = 1,
        .next_segment_id = glifistore::SegmentId{sealed_count + 2U},
        .next_segment_generation = glifistore::GenerationId{1},
    };
    for (std::size_t index = 1; index <= sealed_count; ++index) {
        manifest.segments.push_back(prefix_entry(index, 1, glifistore::ManifestSegmentRole::sealed));
    }
    manifest.segments.push_back(prefix_entry(sealed_count + 1U, 1, glifistore::ManifestSegmentRole::active));
    return manifest;
}

auto prefix_record(const std::uint64_t sequence, std::string key, std::string value) -> PrefixRecord {
    return {.sequence = sequence,
            .opcode = glifistore::Opcode::put,
            .key = std::move(key),
            .value = std::move(value)};
}

auto prefix_erase(const std::uint64_t sequence, std::string key) -> PrefixRecord {
    return {.sequence = sequence, .opcode = glifistore::Opcode::erase, .key = std::move(key), .value = {}};
}

// k = 2 of n = 3 sealed Segments become m = 1. Prefix history: alpha is overwritten and beta erased inside
// the prefix; gamma's put sits inside the prefix while its tombstone sits in the retained Segment 3, so
// the older put must never resurrect after the prefix is retired.
auto three_to_one_transaction() -> PrefixTransaction {
    PrefixTransaction transaction;
    transaction.old = prefix_old_manifest(3);
    transaction.old_segments = {
        {transaction.old.segments[0],
         {prefix_record(1, "alpha", "a1"), prefix_record(2, "beta", "b1"), prefix_record(3, "gamma", "g1")}},
        {transaction.old.segments[1], {prefix_record(4, "alpha", "a2"), prefix_erase(5, "beta")}},
        {transaction.old.segments[2], {prefix_record(6, "delta", "d1"), prefix_erase(7, "gamma")}},
        {transaction.old.segments[3], {prefix_record(8, "epsilon", "e1")}},
    };
    transaction.next = transaction.old;
    ++transaction.next.manifest_generation;
    transaction.next.segments = {prefix_entry(1, 2, glifistore::ManifestSegmentRole::sealed),
                                 transaction.old.segments[2], transaction.old.segments[3]};
    // The only prefix Record still referenced by the Index is alpha's overwrite.
    transaction.outputs = {{transaction.next.segments[0], {prefix_record(4, "alpha", "a2")}}};
    transaction.model = {
        {"alpha", "a2"}, {"beta", std::nullopt}, {"gamma", std::nullopt}, {"delta", "d1"}, {"epsilon", "e1"}};
    return transaction;
}

// k = 3 of n = 4 sealed Segments become m = 2; Segment 4 is retained and holds a tombstone (for c)
// whose older put sits inside the prefix.
auto four_to_two_transaction() -> PrefixTransaction {
    PrefixTransaction transaction;
    transaction.old = prefix_old_manifest(4);
    transaction.old_segments = {
        {transaction.old.segments[0], {prefix_record(1, "a", "a1"), prefix_record(2, "b", "b1")}},
        {transaction.old.segments[1], {prefix_record(3, "a", "a2"), prefix_record(4, "c", "c1")}},
        {transaction.old.segments[2], {prefix_erase(5, "b"), prefix_record(6, "d", "d1")}},
        {transaction.old.segments[3], {prefix_record(7, "e", "e1"), prefix_erase(8, "c")}},
        {transaction.old.segments[4], {prefix_record(9, "f", "f1")}},
    };
    transaction.next = transaction.old;
    ++transaction.next.manifest_generation;
    transaction.next.segments = {prefix_entry(1, 2, glifistore::ManifestSegmentRole::sealed),
                                 prefix_entry(2, 2, glifistore::ManifestSegmentRole::sealed),
                                 transaction.old.segments[3], transaction.old.segments[4]};
    transaction.outputs = {{transaction.next.segments[0], {prefix_record(3, "a", "a2")}},
                           {transaction.next.segments[1], {prefix_record(6, "d", "d1")}}};
    transaction.model = {{"a", "a2"}, {"b", std::nullopt}, {"c", std::nullopt},
                         {"d", "d1"}, {"e", "e1"},         {"f", "f1"}};
    return transaction;
}

// k = 1 of n = 2 sealed Segments become m = 0: Segment 1 is wholly superseded and retires without output.
auto one_to_zero_transaction() -> PrefixTransaction {
    PrefixTransaction transaction;
    transaction.old = prefix_old_manifest(2);
    transaction.old_segments = {
        {transaction.old.segments[0], {prefix_record(1, "a", "a1"), prefix_record(2, "b", "b1")}},
        {transaction.old.segments[1], {prefix_record(3, "a", "a2"), prefix_erase(4, "b")}},
        {transaction.old.segments[2], {prefix_record(5, "c", "c1")}},
    };
    transaction.next = transaction.old;
    ++transaction.next.manifest_generation;
    transaction.next.segments = {transaction.old.segments[1], transaction.old.segments[2]};
    transaction.model = {{"a", "a2"}, {"b", std::nullopt}, {"c", "c1"}};
    return transaction;
}

void write_prefix_segment(glifistore::DataDirectory& directory, const glifistore::Manifest& manifest,
                          const PrefixSegment& segment) {
    auto created = glifistore::DurableSegmentFile::create(directory, identity(manifest, segment.entry));
    GLIFI_REQUIRE(created.durable());
    for (const auto& record : segment.records) {
        const auto encoded = glifistore::encode_record({
            .sequence = glifistore::SequenceNumber{record.sequence},
            .opcode = record.opcode,
            .type = glifistore::ValueType::bytes,
            .flags = 0,
            .key_hash = glifistore::hash_key(record.key),
            .expire_at_ns = 0,
            .key = std::as_bytes(std::span{record.key}),
            .value = std::as_bytes(std::span{record.value}),
        });
        GLIFI_REQUIRE(encoded.has_value());
        GLIFI_REQUIRE(created.file->append(*encoded).committed());
    }
    if (segment.entry.role == glifistore::ManifestSegmentRole::sealed) {
        GLIFI_REQUIRE(created.file->seal().committed());
    }
}

// The same persistent state an interrupted transaction leaves: Mold published, every old Segment present,
// the intent durable and every output promoted to its canonical name; optionally Mnext published too.
void prepare_interrupted_prefix(const std::filesystem::path& path, const PrefixTransaction& transaction,
                                const bool publish_next) {
    auto directory = glifistore::DataDirectory::open_and_lock(path);
    GLIFI_REQUIRE(directory.has_value());
    GLIFI_REQUIRE(directory->publish_manifest(transaction.old).durable());
    for (const auto& segment : transaction.old_segments) {
        write_prefix_segment(*directory, transaction.old, segment);
    }
    const glifistore::DurableCompactionIntent intent{.worker_id = glifistore::WorkerId{0},
                                                     .old_manifest = transaction.old,
                                                     .next_manifest = transaction.next};
    GLIFI_REQUIRE(directory->publish_compaction_intent(intent).durable());
    for (const auto& output : transaction.outputs) {
        write_prefix_segment(*directory, transaction.next, output);
    }
    if (publish_next) {
        GLIFI_REQUIRE(directory->publish_manifest(transaction.next).durable());
    }
}

void require_prefix_model(glifistore::DurableRuntimeCatalog& runtime, const PrefixTransaction& transaction) {
    for (const auto& expected : transaction.model) {
        const auto found = runtime.get(expected.key);
        if (expected.value) {
            GLIFI_REQUIRE(found.has_value());
            GLIFI_REQUIRE(std::string(reinterpret_cast<const char*>(found->bytes.data()),
                                      found->bytes.size()) == *expected.value);
        } else {
            GLIFI_REQUIRE(!found.has_value());
            GLIFI_REQUIRE(found.error().code == glifistore::ErrorCode::not_found);
        }
    }
    GLIFI_REQUIRE(runtime.verify_index().has_value());
}

using glifistore::test::file_identity;

void require_present(const std::filesystem::path& directory, const glifistore::Manifest& manifest,
                     const glifistore::ManifestSegmentEntry& entry) {
    GLIFI_REQUIRE(std::filesystem::exists(segment_path(directory, manifest, entry)));
}

void require_absent(const std::filesystem::path& directory, const glifistore::Manifest& manifest,
                    const glifistore::ManifestSegmentEntry& entry) {
    GLIFI_REQUIRE(!std::filesystem::exists(segment_path(directory, manifest, entry)));
}

void require_no_intent(const std::filesystem::path& directory) {
    GLIFI_REQUIRE(!std::filesystem::exists(directory / glifistore::kCompactionIntentFilename));
}

void require_intent(const std::filesystem::path& directory) {
    GLIFI_REQUIRE(std::filesystem::exists(directory / glifistore::kCompactionIntentFilename));
}

} // namespace

GLIFI_TEST("compaction recovery rolls back exact replacements when the old manifest wins") {
    CompactionRecoveryDirectory temporary;
    const auto [old, next] = compaction_manifests();
    prepare_interrupted_compaction(temporary.path(), false);

    auto runtime = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
    GLIFI_REQUIRE(runtime.has_value());
    GLIFI_REQUIRE((*runtime)->manifest() == old);
    GLIFI_REQUIRE((*runtime)->namespace_audit().recovery_safe());
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
    GLIFI_REQUIRE(!std::filesystem::exists(segment_path(temporary.path(), next, next.segments.front())));
    GLIFI_REQUIRE(std::filesystem::exists(segment_path(temporary.path(), old, old.segments[0])));
    GLIFI_REQUIRE(std::filesystem::exists(segment_path(temporary.path(), old, old.segments[1])));
}

GLIFI_TEST("compaction recovery retires exact sources when the next manifest wins") {
    CompactionRecoveryDirectory temporary;
    const auto [old, next] = compaction_manifests();
    prepare_interrupted_compaction(temporary.path(), true);

    auto runtime = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
    GLIFI_REQUIRE(runtime.has_value());
    GLIFI_REQUIRE((*runtime)->manifest() == next);
    GLIFI_REQUIRE((*runtime)->namespace_audit().recovery_safe());
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
    GLIFI_REQUIRE(!std::filesystem::exists(segment_path(temporary.path(), old, old.segments[0])));
    GLIFI_REQUIRE(!std::filesystem::exists(segment_path(temporary.path(), old, old.segments[1])));
    GLIFI_REQUIRE(std::filesystem::exists(segment_path(temporary.path(), next, next.segments.front())));
}

GLIFI_TEST("multi-output compaction rollback resumes after a partial replacement unlink") {
    CompactionRecoveryDirectory temporary;
    const auto [old, next] = multi_output_compaction_manifests();
    prepare_interrupted_compaction(temporary.path(), old, next, false);
    OneShotRecoveryFailure failure{.target = glifistore::FilesystemOperation::remove_compaction_segment,
                                   .fail_on_matching_call = 2};

    const auto interrupted = glifistore::DurableRuntimeCatalog::open_existing(
        temporary.path(), 0, {.context = &failure, .before = &OneShotRecoveryFailure::before});
    GLIFI_REQUIRE(!interrupted.has_value());
    GLIFI_REQUIRE(failure.fired);
    GLIFI_REQUIRE(std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
    GLIFI_REQUIRE(!std::filesystem::exists(segment_path(temporary.path(), next, next.segments[0])));
    GLIFI_REQUIRE(std::filesystem::exists(segment_path(temporary.path(), next, next.segments[1])));
    for (std::size_t index = 0; index < 3; ++index) {
        GLIFI_REQUIRE(std::filesystem::exists(segment_path(temporary.path(), old, old.segments[index])));
    }

    auto recovered = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
    GLIFI_REQUIRE(recovered.has_value());
    GLIFI_REQUIRE((*recovered)->manifest() == old);
    GLIFI_REQUIRE((*recovered)->namespace_audit().clean());
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
    GLIFI_REQUIRE(!std::filesystem::exists(segment_path(temporary.path(), next, next.segments[0])));
    GLIFI_REQUIRE(!std::filesystem::exists(segment_path(temporary.path(), next, next.segments[1])));
}

GLIFI_TEST("multi-output compaction rollback removes a partially created second replacement") {
    CompactionRecoveryDirectory temporary;
    const auto [old, next] = multi_output_compaction_manifests();
    prepare_interrupted_compaction(temporary.path(), old, next, false);
    const auto second_final = segment_path(temporary.path(), next, next.segments[1]);
    const auto second_temporary = segment_temporary_path(temporary.path(), next, next.segments[1]);
    std::filesystem::rename(second_final, second_temporary);

    auto recovered = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
    GLIFI_REQUIRE(recovered.has_value());
    GLIFI_REQUIRE((*recovered)->manifest() == old);
    GLIFI_REQUIRE((*recovered)->namespace_audit().clean());
    GLIFI_REQUIRE(!std::filesystem::exists(segment_path(temporary.path(), next, next.segments[0])));
    GLIFI_REQUIRE(!std::filesystem::exists(second_final));
    GLIFI_REQUIRE(!std::filesystem::exists(second_temporary));
}

GLIFI_TEST("multi-output compaction retirement resumes after a partial source unlink") {
    CompactionRecoveryDirectory temporary;
    const auto [old, next] = multi_output_compaction_manifests();
    prepare_interrupted_compaction(temporary.path(), old, next, true);
    OneShotRecoveryFailure failure{.target = glifistore::FilesystemOperation::remove_compaction_segment,
                                   .fail_on_matching_call = 3};

    const auto interrupted = glifistore::DurableRuntimeCatalog::open_existing(
        temporary.path(), 0, {.context = &failure, .before = &OneShotRecoveryFailure::before});
    GLIFI_REQUIRE(!interrupted.has_value());
    GLIFI_REQUIRE(failure.fired);
    GLIFI_REQUIRE(std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
    GLIFI_REQUIRE(!std::filesystem::exists(segment_path(temporary.path(), old, old.segments[0])));
    GLIFI_REQUIRE(!std::filesystem::exists(segment_path(temporary.path(), old, old.segments[1])));
    GLIFI_REQUIRE(std::filesystem::exists(segment_path(temporary.path(), old, old.segments[2])));
    GLIFI_REQUIRE(std::filesystem::exists(segment_path(temporary.path(), next, next.segments[0])));
    GLIFI_REQUIRE(std::filesystem::exists(segment_path(temporary.path(), next, next.segments[1])));

    auto recovered = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
    GLIFI_REQUIRE(recovered.has_value());
    GLIFI_REQUIRE((*recovered)->manifest() == next);
    GLIFI_REQUIRE((*recovered)->namespace_audit().clean());
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
    for (std::size_t index = 0; index < 3; ++index) {
        GLIFI_REQUIRE(!std::filesystem::exists(segment_path(temporary.path(), old, old.segments[index])));
    }
    GLIFI_REQUIRE(std::filesystem::exists(segment_path(temporary.path(), next, next.segments[0])));
    GLIFI_REQUIRE(std::filesystem::exists(segment_path(temporary.path(), next, next.segments[1])));
}

GLIFI_TEST("compaction recovery rejects a manifest outside both intent authorities") {
    CompactionRecoveryDirectory temporary;
    auto [old, next] = compaction_manifests();
    prepare_interrupted_compaction(temporary.path(), false);
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        old.manifest_generation = next.manifest_generation + 1U;
        GLIFI_REQUIRE(directory->publish_manifest(old).durable());
    }
    const auto runtime = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
    GLIFI_REQUIRE(!runtime.has_value());
    GLIFI_REQUIRE(runtime.error().code == glifistore::ErrorCode::corrupted_data);
    GLIFI_REQUIRE(std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
}

GLIFI_TEST("compaction recovery rejects an unrelated unlisted Segment") {
    CompactionRecoveryDirectory temporary;
    const auto [old, next] = compaction_manifests();
    prepare_interrupted_compaction(temporary.path(), false);
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        const glifistore::ManifestSegmentEntry unrelated{
            .segment_id = glifistore::SegmentId{4},
            .generation = glifistore::GenerationId{1},
            .owner_worker = glifistore::WorkerId{0},
            .role = glifistore::ManifestSegmentRole::sealed,
        };
        create_segment(*directory, old, unrelated);
    }

    const auto runtime = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
    GLIFI_REQUIRE(!runtime.has_value());
    GLIFI_REQUIRE(runtime.error().code == glifistore::ErrorCode::corrupted_data);
    GLIFI_REQUIRE(std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
    GLIFI_REQUIRE(std::filesystem::exists(segment_path(temporary.path(), next, next.segments.front())));
}

GLIFI_TEST("compaction recovery preserves all files when retirement fails before its first unlink") {
    CompactionRecoveryDirectory temporary;
    const auto [old, next] = compaction_manifests();
    prepare_interrupted_compaction(temporary.path(), false);
    OneShotRecoveryFailure failure{.target = glifistore::FilesystemOperation::remove_compaction_segment};

    const auto interrupted = glifistore::DurableRuntimeCatalog::open_existing(
        temporary.path(), 0, {.context = &failure, .before = &OneShotRecoveryFailure::before});
    GLIFI_REQUIRE(!interrupted.has_value());
    GLIFI_REQUIRE(failure.fired);
    GLIFI_REQUIRE(std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
    GLIFI_REQUIRE(std::filesystem::exists(segment_path(temporary.path(), next, next.segments.front())));
    GLIFI_REQUIRE(std::filesystem::exists(segment_path(temporary.path(), old, old.segments[0])));
    GLIFI_REQUIRE(std::filesystem::exists(segment_path(temporary.path(), old, old.segments[1])));

    auto recovered = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
    GLIFI_REQUIRE(recovered.has_value());
    GLIFI_REQUIRE((*recovered)->manifest() == old);
    GLIFI_REQUIRE(!std::filesystem::exists(segment_path(temporary.path(), next, next.segments.front())));
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
}

GLIFI_TEST("compaction recovery resumes an indeterminate partially retired source set") {
    CompactionRecoveryDirectory temporary;
    const auto [old, next] = compaction_manifests();
    prepare_interrupted_compaction(temporary.path(), true);
    OneShotRecoveryFailure failure{.target = glifistore::FilesystemOperation::remove_compaction_segment,
                                   .fail_on_matching_call = 2};

    const auto interrupted = glifistore::DurableRuntimeCatalog::open_existing(
        temporary.path(), 0, {.context = &failure, .before = &OneShotRecoveryFailure::before});
    GLIFI_REQUIRE(!interrupted.has_value());
    GLIFI_REQUIRE(failure.fired);
    GLIFI_REQUIRE(std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
    GLIFI_REQUIRE(!std::filesystem::exists(segment_path(temporary.path(), old, old.segments[0])));
    GLIFI_REQUIRE(std::filesystem::exists(segment_path(temporary.path(), old, old.segments[1])));

    auto recovered = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
    GLIFI_REQUIRE(recovered.has_value());
    GLIFI_REQUIRE((*recovered)->manifest() == next);
    GLIFI_REQUIRE(!std::filesystem::exists(segment_path(temporary.path(), old, old.segments[0])));
    GLIFI_REQUIRE(!std::filesystem::exists(segment_path(temporary.path(), old, old.segments[1])));
    GLIFI_REQUIRE(std::filesystem::exists(segment_path(temporary.path(), next, next.segments.front())));
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
}

GLIFI_TEST("compaction recovery resumes after retirement directory sync is indeterminate") {
    CompactionRecoveryDirectory temporary;
    const auto [old, next] = compaction_manifests();
    prepare_interrupted_compaction(temporary.path(), true);
    OneShotRecoveryFailure failure{.target = glifistore::FilesystemOperation::sync_directory};

    const auto interrupted = glifistore::DurableRuntimeCatalog::open_existing(
        temporary.path(), 0, {.context = &failure, .before = &OneShotRecoveryFailure::before});
    GLIFI_REQUIRE(!interrupted.has_value());
    GLIFI_REQUIRE(failure.fired);
    GLIFI_REQUIRE(std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
    GLIFI_REQUIRE(!std::filesystem::exists(segment_path(temporary.path(), old, old.segments[0])));
    GLIFI_REQUIRE(!std::filesystem::exists(segment_path(temporary.path(), old, old.segments[1])));

    auto recovered = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
    GLIFI_REQUIRE(recovered.has_value());
    GLIFI_REQUIRE((*recovered)->manifest() == next);
    GLIFI_REQUIRE(std::filesystem::exists(segment_path(temporary.path(), next, next.segments.front())));
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
}

GLIFI_TEST("compaction recovery resumes when intent removal fails before unlink") {
    CompactionRecoveryDirectory temporary;
    const auto [old, next] = compaction_manifests();
    prepare_interrupted_compaction(temporary.path(), true);
    OneShotRecoveryFailure failure{.target = glifistore::FilesystemOperation::remove_compaction_intent};

    const auto interrupted = glifistore::DurableRuntimeCatalog::open_existing(
        temporary.path(), 0, {.context = &failure, .before = &OneShotRecoveryFailure::before});
    GLIFI_REQUIRE(!interrupted.has_value());
    GLIFI_REQUIRE(failure.fired);
    GLIFI_REQUIRE(std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
    GLIFI_REQUIRE(!std::filesystem::exists(segment_path(temporary.path(), old, old.segments[0])));
    GLIFI_REQUIRE(!std::filesystem::exists(segment_path(temporary.path(), old, old.segments[1])));

    auto recovered = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
    GLIFI_REQUIRE(recovered.has_value());
    GLIFI_REQUIRE((*recovered)->manifest() == next);
    GLIFI_REQUIRE(std::filesystem::exists(segment_path(temporary.path(), next, next.segments.front())));
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
}

GLIFI_TEST("compaction recovery reopens ordinarily after intent removal sync is indeterminate") {
    CompactionRecoveryDirectory temporary;
    const auto [old, next] = compaction_manifests();
    prepare_interrupted_compaction(temporary.path(), true);
    OneShotRecoveryFailure failure{.target = glifistore::FilesystemOperation::sync_directory,
                                   .fail_on_matching_call = 2};

    const auto interrupted = glifistore::DurableRuntimeCatalog::open_existing(
        temporary.path(), 0, {.context = &failure, .before = &OneShotRecoveryFailure::before});
    GLIFI_REQUIRE(!interrupted.has_value());
    GLIFI_REQUIRE(failure.fired);
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
    GLIFI_REQUIRE(!std::filesystem::exists(segment_path(temporary.path(), old, old.segments[0])));
    GLIFI_REQUIRE(!std::filesystem::exists(segment_path(temporary.path(), old, old.segments[1])));

    auto recovered = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
    GLIFI_REQUIRE(recovered.has_value());
    GLIFI_REQUIRE((*recovered)->manifest() == next);
    GLIFI_REQUIRE((*recovered)->namespace_audit().recovery_safe());
    GLIFI_REQUIRE(std::filesystem::exists(segment_path(temporary.path(), next, next.segments.front())));
}

GLIFI_TEST(
    "strict-prefix compaction recovery rolls back to the old manifest and keeps the retained Segments") {
    CompactionRecoveryDirectory temporary;
    const auto transaction = three_to_one_transaction();
    prepare_interrupted_prefix(temporary.path(), transaction, false);
    const auto& old = transaction.old;
    const auto retained = file_identity(segment_path(temporary.path(), old, old.segments[2]));
    const auto active = file_identity(segment_path(temporary.path(), old, old.segments[3]));

    {
        auto runtime = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
        GLIFI_REQUIRE(runtime.has_value());
        GLIFI_REQUIRE((*runtime)->manifest() == old);
        GLIFI_REQUIRE((*runtime)->namespace_audit().clean());
        require_prefix_model(**runtime, transaction);
    }
    require_no_intent(temporary.path());
    // Rollback removed the one replacement and nothing else.
    require_absent(temporary.path(), transaction.next, transaction.next.segments[0]);
    for (const auto& entry : old.segments) {
        require_present(temporary.path(), old, entry);
    }
    GLIFI_REQUIRE(file_identity(segment_path(temporary.path(), old, old.segments[2])) == retained);
    GLIFI_REQUIRE(file_identity(segment_path(temporary.path(), old, old.segments[3])) == active);

    // Recovery is idempotent: a second ordinary open changes nothing.
    auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE((*reopened)->manifest() == old);
    require_prefix_model(**reopened, transaction);
}

GLIFI_TEST("strict-prefix compaction recovery retires exactly the prefix when the next manifest wins") {
    CompactionRecoveryDirectory temporary;
    const auto transaction = three_to_one_transaction();
    prepare_interrupted_prefix(temporary.path(), transaction, true);
    const auto& old = transaction.old;
    const auto& next = transaction.next;
    const auto retained = file_identity(segment_path(temporary.path(), old, old.segments[2]));
    const auto output = file_identity(segment_path(temporary.path(), next, next.segments[0]));

    {
        auto runtime = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
        GLIFI_REQUIRE(runtime.has_value());
        GLIFI_REQUIRE((*runtime)->manifest() == next);
        GLIFI_REQUIRE((*runtime)->namespace_audit().clean());
        require_prefix_model(**runtime, transaction);
    }
    require_no_intent(temporary.path());
    // Exactly the two prefix sources are gone. The retained sealed Segment and the active one are not.
    require_absent(temporary.path(), old, old.segments[0]);
    require_absent(temporary.path(), old, old.segments[1]);
    require_present(temporary.path(), next, next.segments[0]);
    require_present(temporary.path(), old, old.segments[2]);
    require_present(temporary.path(), old, old.segments[3]);
    GLIFI_REQUIRE(file_identity(segment_path(temporary.path(), old, old.segments[2])) == retained);
    GLIFI_REQUIRE(file_identity(segment_path(temporary.path(), next, next.segments[0])) == output);

    auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE((*reopened)->manifest() == next);
    GLIFI_REQUIRE((*reopened)->namespace_audit().clean());
    require_prefix_model(**reopened, transaction);
}

GLIFI_TEST("strict-prefix compaction retirement resumes after each partial source unlink") {
    // The prefix has two sources. Failing the first unlink leaves every source; failing the second leaves
    // exactly one. Either way the next ordinary open finishes the cleanup and never touches Segment 3.
    for (const std::size_t failing_call : {std::size_t{1}, std::size_t{2}}) {
        CompactionRecoveryDirectory temporary;
        const auto transaction = three_to_one_transaction();
        prepare_interrupted_prefix(temporary.path(), transaction, true);
        const auto& old = transaction.old;
        const auto& next = transaction.next;
        const auto retained = file_identity(segment_path(temporary.path(), old, old.segments[2]));
        OneShotRecoveryFailure failure{.target = glifistore::FilesystemOperation::remove_compaction_segment,
                                       .fail_on_matching_call = failing_call};

        const auto interrupted = glifistore::DurableRuntimeCatalog::open_existing(
            temporary.path(), 0, {.context = &failure, .before = &OneShotRecoveryFailure::before});
        GLIFI_REQUIRE(!interrupted.has_value());
        GLIFI_REQUIRE(failure.fired);
        require_intent(temporary.path());
        GLIFI_REQUIRE(std::filesystem::exists(segment_path(temporary.path(), old, old.segments[0])) ==
                      (failing_call == 1));
        require_present(temporary.path(), old, old.segments[1]);
        require_present(temporary.path(), next, next.segments[0]);
        require_present(temporary.path(), old, old.segments[2]);

        auto recovered = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
        GLIFI_REQUIRE(recovered.has_value());
        GLIFI_REQUIRE((*recovered)->manifest() == next);
        GLIFI_REQUIRE((*recovered)->namespace_audit().clean());
        require_prefix_model(**recovered, transaction);
        require_no_intent(temporary.path());
        require_absent(temporary.path(), old, old.segments[0]);
        require_absent(temporary.path(), old, old.segments[1]);
        require_present(temporary.path(), next, next.segments[0]);
        require_present(temporary.path(), old, old.segments[2]);
        GLIFI_REQUIRE(file_identity(segment_path(temporary.path(), old, old.segments[2])) == retained);
    }
}

GLIFI_TEST("strict-prefix multi-output rollback resumes after a partial replacement unlink") {
    CompactionRecoveryDirectory temporary;
    const auto transaction = four_to_two_transaction();
    prepare_interrupted_prefix(temporary.path(), transaction, false);
    const auto& old = transaction.old;
    const auto& next = transaction.next;
    const auto retained = file_identity(segment_path(temporary.path(), old, old.segments[3]));
    OneShotRecoveryFailure failure{.target = glifistore::FilesystemOperation::remove_compaction_segment,
                                   .fail_on_matching_call = 2};

    const auto interrupted = glifistore::DurableRuntimeCatalog::open_existing(
        temporary.path(), 0, {.context = &failure, .before = &OneShotRecoveryFailure::before});
    GLIFI_REQUIRE(!interrupted.has_value());
    GLIFI_REQUIRE(failure.fired);
    require_intent(temporary.path());
    require_absent(temporary.path(), next, next.segments[0]);
    require_present(temporary.path(), next, next.segments[1]);
    for (const auto& entry : old.segments) {
        require_present(temporary.path(), old, entry);
    }

    auto recovered = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
    GLIFI_REQUIRE(recovered.has_value());
    GLIFI_REQUIRE((*recovered)->manifest() == old);
    GLIFI_REQUIRE((*recovered)->namespace_audit().clean());
    require_prefix_model(**recovered, transaction);
    require_no_intent(temporary.path());
    require_absent(temporary.path(), next, next.segments[0]);
    require_absent(temporary.path(), next, next.segments[1]);
    for (const auto& entry : old.segments) {
        require_present(temporary.path(), old, entry);
    }
    GLIFI_REQUIRE(file_identity(segment_path(temporary.path(), old, old.segments[3])) == retained);
}

GLIFI_TEST("strict-prefix multi-output rollback removes a partially created second replacement") {
    CompactionRecoveryDirectory temporary;
    const auto transaction = four_to_two_transaction();
    prepare_interrupted_prefix(temporary.path(), transaction, false);
    const auto& old = transaction.old;
    const auto& next = transaction.next;
    const auto second_final = segment_path(temporary.path(), next, next.segments[1]);
    const auto second_temporary = segment_temporary_path(temporary.path(), next, next.segments[1]);
    std::filesystem::rename(second_final, second_temporary);
    const auto retained = file_identity(segment_path(temporary.path(), old, old.segments[3]));

    auto recovered = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
    GLIFI_REQUIRE(recovered.has_value());
    GLIFI_REQUIRE((*recovered)->manifest() == old);
    GLIFI_REQUIRE((*recovered)->namespace_audit().clean());
    require_prefix_model(**recovered, transaction);
    require_absent(temporary.path(), next, next.segments[0]);
    GLIFI_REQUIRE(!std::filesystem::exists(second_final));
    GLIFI_REQUIRE(!std::filesystem::exists(second_temporary));
    GLIFI_REQUIRE(file_identity(segment_path(temporary.path(), old, old.segments[3])) == retained);
}

GLIFI_TEST("strict-prefix multi-output retirement resumes and keeps the sealed Segment after the prefix") {
    for (const std::size_t failing_call : {std::size_t{1}, std::size_t{2}, std::size_t{3}}) {
        CompactionRecoveryDirectory temporary;
        const auto transaction = four_to_two_transaction();
        prepare_interrupted_prefix(temporary.path(), transaction, true);
        const auto& old = transaction.old;
        const auto& next = transaction.next;
        const auto retained = file_identity(segment_path(temporary.path(), old, old.segments[3]));
        OneShotRecoveryFailure failure{.target = glifistore::FilesystemOperation::remove_compaction_segment,
                                       .fail_on_matching_call = failing_call};

        const auto interrupted = glifistore::DurableRuntimeCatalog::open_existing(
            temporary.path(), 0, {.context = &failure, .before = &OneShotRecoveryFailure::before});
        GLIFI_REQUIRE(!interrupted.has_value());
        GLIFI_REQUIRE(failure.fired);
        require_intent(temporary.path());
        // Sources are retired in Manifest order, and only the three prefix sources are candidates.
        for (std::size_t index = 0; index < 3; ++index) {
            GLIFI_REQUIRE(std::filesystem::exists(segment_path(temporary.path(), old, old.segments[index])) ==
                          (index + 1U >= failing_call));
        }
        require_present(temporary.path(), old, old.segments[3]);
        require_present(temporary.path(), next, next.segments[0]);
        require_present(temporary.path(), next, next.segments[1]);

        auto recovered = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
        GLIFI_REQUIRE(recovered.has_value());
        GLIFI_REQUIRE((*recovered)->manifest() == next);
        GLIFI_REQUIRE((*recovered)->namespace_audit().clean());
        require_prefix_model(**recovered, transaction);
        require_no_intent(temporary.path());
        for (std::size_t index = 0; index < 3; ++index) {
            require_absent(temporary.path(), old, old.segments[index]);
        }
        require_present(temporary.path(), old, old.segments[3]);
        require_present(temporary.path(), old, old.segments[4]);
        GLIFI_REQUIRE(file_identity(segment_path(temporary.path(), old, old.segments[3])) == retained);
    }
}

GLIFI_TEST("strict-prefix zero-output compaction recovers under both authorities") {
    for (const bool publish_next : {false, true}) {
        CompactionRecoveryDirectory temporary;
        const auto transaction = one_to_zero_transaction();
        prepare_interrupted_prefix(temporary.path(), transaction, publish_next);
        const auto& old = transaction.old;
        const auto retained = file_identity(segment_path(temporary.path(), old, old.segments[1]));

        auto runtime = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
        GLIFI_REQUIRE(runtime.has_value());
        GLIFI_REQUIRE((*runtime)->manifest() == (publish_next ? transaction.next : transaction.old));
        GLIFI_REQUIRE((*runtime)->namespace_audit().clean());
        require_prefix_model(**runtime, transaction);
        require_no_intent(temporary.path());
        // No replacement exists, so the old authority only removes the intent and the next authority
        // retires the one obsolete source.
        GLIFI_REQUIRE(std::filesystem::exists(segment_path(temporary.path(), old, old.segments[0])) ==
                      !publish_next);
        require_present(temporary.path(), old, old.segments[1]);
        require_present(temporary.path(), old, old.segments[2]);
        GLIFI_REQUIRE(file_identity(segment_path(temporary.path(), old, old.segments[1])) == retained);
    }
}

GLIFI_TEST("strict-prefix compaction recovery fails closed without adopting or cleaning on bad authority") {
    // The retained sealed Segment is listed by both authorities. Without it neither can be adopted.
    for (const bool publish_next : {false, true}) {
        CompactionRecoveryDirectory temporary;
        const auto transaction = three_to_one_transaction();
        prepare_interrupted_prefix(temporary.path(), transaction, publish_next);
        const auto& old = transaction.old;
        const auto& next = transaction.next;
        std::filesystem::remove(segment_path(temporary.path(), old, old.segments[2]));

        const auto runtime = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
        GLIFI_REQUIRE(!runtime.has_value());
        GLIFI_REQUIRE(runtime.error().code == glifistore::ErrorCode::corrupted_data);
        require_intent(temporary.path());
        require_present(temporary.path(), old, old.segments[0]);
        require_present(temporary.path(), old, old.segments[1]);
        require_present(temporary.path(), next, next.segments[0]);
    }

    // A Manifest equal to neither embedded authority is corruption even when it lists the retained Segment.
    CompactionRecoveryDirectory temporary;
    auto transaction = three_to_one_transaction();
    prepare_interrupted_prefix(temporary.path(), transaction, false);
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        auto foreign = transaction.old;
        foreign.manifest_generation = transaction.next.manifest_generation + 1U;
        GLIFI_REQUIRE(directory->publish_manifest(foreign).durable());
    }
    const auto runtime = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
    GLIFI_REQUIRE(!runtime.has_value());
    GLIFI_REQUIRE(runtime.error().code == glifistore::ErrorCode::corrupted_data);
    require_intent(temporary.path());
    require_present(temporary.path(), transaction.next, transaction.next.segments[0]);
    for (const auto& entry : transaction.old.segments) {
        require_present(temporary.path(), transaction.old, entry);
    }
}

GLIFI_TEST("ordinary recovery removes recognized private temporaries and leaves the Store unchanged") {
    // Characterizes src/persistence/recovery/recovery.cpp: with no compaction intent, the stale compaction
    // and Manifest temporaries are reported by the namespace audit, never adopted, and then removed after
    // validation. recovery-state-matrix-v1 section 6 and 7 currently say "ignore" and "report; do not
    // adopt"; this test records the behavior the proposed erratum describes.
    CompactionRecoveryDirectory temporary;
    const auto transaction = one_to_zero_transaction();
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        GLIFI_REQUIRE(directory->publish_manifest(transaction.old).durable());
        for (const auto& segment : transaction.old_segments) {
            write_prefix_segment(*directory, transaction.old, segment);
        }
    }
    const std::array<const char*, 2> names{glifistore::kCompactionTemporaryFilename,
                                           glifistore::kManifestTemporaryFilename};
    for (const auto* name : names) {
        const auto descriptor =
            ::open((temporary.path() / name).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        GLIFI_REQUIRE(descriptor >= 0);
        GLIFI_REQUIRE(::close(descriptor) == 0);
    }
    const auto retained =
        file_identity(segment_path(temporary.path(), transaction.old, transaction.old.segments[1]));

    auto runtime = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
    GLIFI_REQUIRE(runtime.has_value());
    GLIFI_REQUIRE((*runtime)->manifest() == transaction.old);
    GLIFI_REQUIRE((*runtime)->namespace_audit().clean());
    require_prefix_model(**runtime, transaction);
    for (const auto* name : names) {
        GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / name));
    }
    for (const auto& entry : transaction.old.segments) {
        require_present(temporary.path(), transaction.old, entry);
    }
    require_no_intent(temporary.path());
    GLIFI_REQUIRE(file_identity(segment_path(temporary.path(), transaction.old,
                                             transaction.old.segments[1])) == retained);
}

namespace {

namespace model = glifistore::test::compaction_model;

// A transaction of the oldest `source_count` of `sealed_count` sealed Segments into `output_count` outputs in
// which every Record that must survive is in an output: source j <= output_count holds the live Record
// "live-j", which output j copies unchanged; every other prefix Record is dead, erased by a tombstone in the
// active Segment, which is newer than the prefix. Retained sealed Segments hold "retained-i".
auto lattice_transaction(const std::size_t sealed_count, const std::size_t source_count,
                         const std::size_t output_count) -> PrefixTransaction {
    PrefixTransaction transaction;
    transaction.old = prefix_old_manifest(sealed_count);
    std::uint64_t sequence{};
    std::vector<std::uint64_t> live_sequence(output_count + 1U);
    for (std::size_t index = 1; index <= sealed_count; ++index) {
        PrefixSegment segment{transaction.old.segments[index - 1U], {}};
        if (index <= source_count) {
            segment.records.push_back(prefix_record(++sequence, "dead-" + std::to_string(index), "d"));
            if (index <= output_count) {
                live_sequence[index] = ++sequence;
                segment.records.push_back(prefix_record(live_sequence[index], "live-" + std::to_string(index),
                                                        "l" + std::to_string(index)));
            }
        } else {
            segment.records.push_back(prefix_record(++sequence, "retained-" + std::to_string(index), "r"));
        }
        transaction.old_segments.push_back(std::move(segment));
    }
    PrefixSegment active{transaction.old.segments[sealed_count], {}};
    for (std::size_t index = 1; index <= source_count; ++index) {
        active.records.push_back(prefix_erase(++sequence, "dead-" + std::to_string(index)));
    }
    active.records.push_back(prefix_record(++sequence, "active", "a"));
    transaction.old_segments.push_back(std::move(active));

    transaction.next = transaction.old;
    ++transaction.next.manifest_generation;
    transaction.next.segments.clear();
    for (std::size_t index = 1; index <= output_count; ++index) {
        transaction.next.segments.push_back(prefix_entry(index, 2, glifistore::ManifestSegmentRole::sealed));
    }
    for (std::size_t index = source_count + 1U; index <= sealed_count; ++index) {
        transaction.next.segments.push_back(transaction.old.segments[index - 1U]);
    }
    transaction.next.segments.push_back(transaction.old.segments[sealed_count]);
    for (std::size_t index = 1; index <= output_count; ++index) {
        transaction.outputs.push_back({transaction.next.segments[index - 1U],
                                       {prefix_record(live_sequence[index], "live-" + std::to_string(index),
                                                      "l" + std::to_string(index))}});
        transaction.model.push_back({"live-" + std::to_string(index), "l" + std::to_string(index)});
    }
    for (std::size_t index = source_count + 1U; index <= sealed_count; ++index) {
        transaction.model.push_back({"retained-" + std::to_string(index), "r"});
    }
    transaction.model.push_back({"active", "a"});
    for (std::size_t index = 1; index <= source_count; ++index) {
        transaction.model.push_back({"dead-" + std::to_string(index), std::nullopt});
    }
    return transaction;
}

// A representative of every class of the namespace lattice of a transaction (model theorem 6): the states
// that the protocol and a crash during recovery can produce, and one-step departures from them.
auto lattice_states(const std::size_t source_count, const std::size_t output_count)
    -> std::vector<model::NamespaceState> {
    using model::AuthorityChoice;
    using model::NamespaceState;
    using model::OutputName;
    const std::vector<bool> all_sources(source_count, true);
    const std::vector<bool> no_sources(source_count, false);
    const std::vector<OutputName> all_final(output_count, OutputName::final_name);
    const std::vector<OutputName> all_absent(output_count, OutputName::absent);
    std::vector<NamespaceState> states;
    const auto outputs_from = [&](std::size_t code, const std::size_t radix) {
        std::vector<OutputName> outputs;
        for (std::size_t index = 0; index < output_count; ++index, code /= radix) {
            // radix 3: absent / final / temporary. radix 2: absent / temporary.
            const auto digit = code % radix;
            outputs.push_back(radix == 3 ? static_cast<OutputName>(digit)
                                         : (digit == 0 ? OutputName::absent : OutputName::temporary));
        }
        return outputs;
    };
    std::size_t three_power = 1;
    std::size_t two_power = 1;
    for (std::size_t index = 0; index < output_count; ++index) {
        three_power *= 3;
        two_power *= 2;
    }
    // Old authority, intent: every combination of replacement names, all sources present...
    for (std::size_t code = 0; code < three_power; ++code) {
        states.push_back({AuthorityChoice::old_manifest, true, all_sources, outputs_from(code, 3)});
    }
    // ...and each single missing source (refused).
    for (std::size_t index = 0; index < source_count; ++index) {
        auto sources = all_sources;
        sources[index] = false;
        states.push_back({AuthorityChoice::old_manifest, true, sources, all_absent});
    }
    // Next authority, intent: every subset of the sources still present, all replacements final...
    for (std::size_t bits = 0; bits < (std::size_t{1} << source_count); ++bits) {
        std::vector<bool> sources(source_count);
        for (std::size_t index = 0; index < source_count; ++index) {
            sources[index] = ((bits >> index) & 1U) != 0;
        }
        states.push_back({AuthorityChoice::next_manifest, true, sources, all_final});
    }
    // ...and each replacement missing or only temporary (refused).
    for (std::size_t index = 0; index < output_count; ++index) {
        for (const auto name : {OutputName::absent, OutputName::temporary}) {
            auto outputs = all_final;
            outputs[index] = name;
            states.push_back({AuthorityChoice::next_manifest, true, no_sources, outputs});
        }
    }
    // Old authority, no intent: stale temporaries only (recovered), and each final replacement (refused).
    for (std::size_t code = 0; code < two_power; ++code) {
        states.push_back({AuthorityChoice::old_manifest, false, all_sources, outputs_from(code, 2)});
    }
    for (std::size_t index = 0; index < output_count; ++index) {
        auto outputs = all_absent;
        outputs[index] = OutputName::final_name;
        states.push_back({AuthorityChoice::old_manifest, false, all_sources, outputs});
    }
    // Next authority, no intent: finished (recovered), and each source left over (refused).
    states.push_back({AuthorityChoice::next_manifest, false, no_sources, all_final});
    for (std::size_t index = 0; index < source_count; ++index) {
        auto sources = no_sources;
        sources[index] = true;
        states.push_back({AuthorityChoice::next_manifest, false, sources, all_final});
    }
    return states;
}

auto describe_state(const model::NamespaceState& state) -> std::string {
    std::string text = state.authority == model::AuthorityChoice::old_manifest ? "Mold" : "Mnext";
    text += state.intent ? " intent sources=" : " no-intent sources=";
    for (const bool present : state.sources) {
        text += present ? '1' : '0';
    }
    text += " outputs=";
    for (const auto name : state.outputs) {
        text += name == model::OutputName::absent ? '-' : (name == model::OutputName::final_name ? 'F' : 't');
    }
    return text;
}

auto directory_names(const std::filesystem::path& directory) -> std::vector<std::string> {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        names.push_back(entry.path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

// Builds exactly the persistent state: only the files that the state has, the Manifest that is authoritative,
// and the intent if it exists. Creating a Segment costs several syncs, so nothing is created and then removed.
struct UnsyncedIo {
    static auto read_some_at(void*, const int descriptor, const std::span<std::byte> bytes,
                             const std::uint64_t offset) -> std::ptrdiff_t {
        return ::pread(descriptor, bytes.data(), bytes.size(), static_cast<off_t>(offset));
    }

    static auto write_some_at(void*, const int descriptor, const std::span<const std::byte> bytes,
                              const std::uint64_t offset) -> std::ptrdiff_t {
        return ::pwrite(descriptor, bytes.data(), bytes.size(), static_cast<off_t>(offset));
    }

    static auto sync_file(void*, int, glifistore::FileSyncMode) -> int {
        return 0;
    }
};

void build_lattice_state(const std::filesystem::path& path, const PrefixTransaction& transaction,
                         const model::NamespaceState& state) {
    auto directory = glifistore::DataDirectory::open_and_lock(
        path, {.file_io = {.read_some_at = &UnsyncedIo::read_some_at,
                           .write_some_at = &UnsyncedIo::write_some_at,
                           .sync_file = &UnsyncedIo::sync_file}});
    GLIFI_REQUIRE(directory.has_value());
    GLIFI_REQUIRE(directory->publish_manifest(transaction.old).durable());
    const std::size_t source_count = state.sources.size();
    for (std::size_t index = 0; index < transaction.old_segments.size(); ++index) {
        // Sources are the oldest `source_count` Segments; the retained and active ones are always present.
        if (index < source_count && !state.sources[index]) {
            continue;
        }
        write_prefix_segment(*directory, transaction.old, transaction.old_segments[index]);
    }
    if (state.intent) {
        const glifistore::DurableCompactionIntent intent{.worker_id = glifistore::WorkerId{0},
                                                         .old_manifest = transaction.old,
                                                         .next_manifest = transaction.next};
        GLIFI_REQUIRE(directory->publish_compaction_intent(intent).durable());
    }
    for (std::size_t index = 0; index < state.outputs.size(); ++index) {
        if (state.outputs[index] == model::OutputName::absent) {
            continue;
        }
        write_prefix_segment(*directory, transaction.next, transaction.outputs[index]);
        if (state.outputs[index] == model::OutputName::temporary) {
            std::filesystem::rename(
                segment_path(path, transaction.next, transaction.next.segments[index]),
                segment_temporary_path(path, transaction.next, transaction.next.segments[index]));
        }
    }
    if (state.authority == model::AuthorityChoice::next_manifest) {
        GLIFI_REQUIRE(directory->publish_manifest(transaction.next).durable());
    }
}

} // namespace

GLIFI_TEST("recovery refines the model's total function on the namespace lattice of a transaction") {
    // For every shape (n sealed Segments, the oldest k into m) and every representative state of the lattice, the
    // recovery of the real directory does what the model's function R says: it recovers exactly the namespace R
    // names and the model of the keys, or it refuses and removes nothing. A recovered store is a fixpoint: a
    // second open changes nothing. The states include every state that a crash in the middle of recovery leaves.
    struct Shape {
        std::size_t sealed;
        std::size_t sources;
        std::size_t outputs;
    };

    const std::vector<Shape> shapes{{2, 1, 0}, {3, 2, 1}, {3, 3, 2}};
    std::size_t recovered_states{};
    std::size_t refused_states{};
    for (const auto& shape : shapes) {
        const auto transaction = lattice_transaction(shape.sealed, shape.sources, shape.outputs);
        const auto states = lattice_states(shape.sources, shape.outputs);
        for (const auto& state : states) {
            CompactionRecoveryDirectory temporary;
            build_lattice_state(temporary.path(), transaction, state);
            const auto names_before = directory_names(temporary.path());
            const auto expected = model::recover(state);
            const auto label = "shape " + std::to_string(shape.sealed) + "/" + std::to_string(shape.sources) +
                               "/" + std::to_string(shape.outputs) + " " + describe_state(state);

            auto runtime = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
            if (!expected) {
                if (runtime.has_value()) {
                    throw std::runtime_error(label +
                                             ": the model refuses this state but recovery accepted it");
                }
                if (directory_names(temporary.path()) != names_before) {
                    throw std::runtime_error(label + ": a refused recovery changed the namespace");
                }
                ++refused_states;
                continue;
            }
            if (!runtime.has_value()) {
                throw std::runtime_error(label + ": the model recovers this state but recovery refused: " +
                                         runtime.error().message);
            }
            ++recovered_states;
            const auto& authority = expected->authority == model::AuthorityChoice::old_manifest
                                        ? transaction.old
                                        : transaction.next;
            GLIFI_REQUIRE((*runtime)->manifest() == authority);
            GLIFI_REQUIRE((*runtime)->namespace_audit().clean());
            require_prefix_model(**runtime, transaction);
            require_no_intent(temporary.path());
            for (std::size_t index = 0; index < shape.sources; ++index) {
                GLIFI_REQUIRE(std::filesystem::exists(segment_path(temporary.path(), transaction.old,
                                                                   transaction.old.segments[index])) ==
                              expected->sources[index]);
            }
            for (std::size_t index = 0; index < shape.outputs; ++index) {
                GLIFI_REQUIRE(std::filesystem::exists(segment_path(temporary.path(), transaction.next,
                                                                   transaction.next.segments[index])) ==
                              (expected->outputs[index] == model::OutputName::final_name));
                GLIFI_REQUIRE(!std::filesystem::exists(segment_temporary_path(
                    temporary.path(), transaction.next, transaction.next.segments[index])));
            }
            // Every Segment that both authorities list is still there.
            for (const auto& entry : transaction.old.segments) {
                if (std::ranges::find(transaction.next.segments, entry) != transaction.next.segments.end()) {
                    require_present(temporary.path(), transaction.old, entry);
                }
            }
            // Fixpoint: recovery of a recovered directory changes nothing.
            runtime->reset();
            const auto names_after = directory_names(temporary.path());
            auto again = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
            GLIFI_REQUIRE(again.has_value());
            GLIFI_REQUIRE((*again)->manifest() == authority);
            GLIFI_REQUIRE(directory_names(temporary.path()) == names_after);
        }
    }
    // Both regions of the lattice are exercised.
    GLIFI_REQUIRE(recovered_states == 5 + 10 + 22);
    GLIFI_REQUIRE(refused_states == 2 + 7 + 12);
}
