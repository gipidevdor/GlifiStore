#include "crash_persistence_prefix.hpp"

#include "glifistore/core/key_hash.hpp"
#include "glifistore/persistence/compaction.hpp"
#include "glifistore/persistence/runtime_catalog.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "glifistore/segment/record.hpp"
#include "support/compaction_prefix_history.hpp"
#include "support/file_identity.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace glifistore::crash::prefix_scenario {
namespace {

namespace oracle = glifistore::test::compaction_prefix;

// The oldest two of the three sealed Segments become one output; the third sealed Segment is retained.
constexpr DurableCompactionShape kShape{.source_count = 2, .replacement_count = 1};
constexpr std::size_t kRetainedSealedSegmentIndex = 2;

// Boundaries at or after which the next Manifest is authoritative. rename_manifest is the atomic switch:
// a process kill keeps a completed rename, so every later checkpoint observes the next authority.
constexpr std::array<std::string_view, 7> kNextAuthorityBoundaries{
    "rename_manifest",
    "sync_directory#3",
    "remove_compaction_segment#1",
    "remove_compaction_segment#2",
    "sync_directory#4",
    "remove_compaction_intent",
    "sync_directory#5",
};

[[nodiscard]] auto store_id() -> StoreId {
    return {std::byte{0x20}, std::byte{0x21}, std::byte{0x22}, std::byte{0x23},
            std::byte{0x24}, std::byte{0x25}, std::byte{0x26}, std::byte{0x27},
            std::byte{0x28}, std::byte{0x29}, std::byte{0x2A}, std::byte{0x2B},
            std::byte{0x2C}, std::byte{0x2D}, std::byte{0x2E}, std::byte{0x2F}};
}

[[nodiscard]] auto old_manifest(const oracle::History& history) -> Manifest {
    Manifest manifest{
        .store_id = store_id(),
        .manifest_generation = 1,
        .routing_algorithm = RoutingAlgorithm::fnv1a64_v1,
        .worker_count = 1,
        .routing_epoch = 1,
        .next_segment_id = SegmentId{history.sealed_count + 2U},
        .next_segment_generation = GenerationId{1},
    };
    for (std::size_t index = 0; index <= history.sealed_count; ++index) {
        manifest.segments.push_back({.segment_id = SegmentId{index + 1U},
                                     .generation = GenerationId{1},
                                     .owner_worker = WorkerId{0},
                                     .role = index < history.sealed_count ? ManifestSegmentRole::sealed
                                                                          : ManifestSegmentRole::active});
    }
    return manifest;
}

[[nodiscard]] auto next_manifest(const Manifest& old) -> Manifest {
    auto next = canonical_durable_compaction_manifest(old, WorkerId{0}, kShape);
    if (!next) {
        throw std::runtime_error("cannot derive the strict-prefix next Manifest: " + next.error().message);
    }
    return std::move(*next);
}

[[nodiscard]] auto segment_identity(const Manifest& manifest, const ManifestSegmentEntry& entry)
    -> SegmentHeaderIdentity {
    return {.store_id = manifest.store_id,
            .segment_id = entry.segment_id,
            .generation = entry.generation,
            .owner_worker = entry.owner_worker};
}

[[nodiscard]] auto segment_path(const std::filesystem::path& data_dir, const Manifest& manifest,
                                const ManifestSegmentEntry& entry) -> std::filesystem::path {
    return data_dir / segment_filename(segment_identity(manifest, entry));
}

using Identities = std::map<std::uint64_t, glifistore::test::FileIdentity>;

// File identities captured when this process seeded a Store, keyed by data directory. A verify-only
// process has none and skips the byte-level check; the matrix seeds and verifies in one process.
[[nodiscard]] auto seeded_identities() -> std::map<std::string, Identities>& {
    static std::map<std::string, Identities> identities;
    return identities;
}

[[nodiscard]] auto fail_verification(const std::string_view reason) -> bool {
    std::cerr << "verify: strict-prefix compaction " << reason << '\n';
    return false;
}

} // namespace

void seed(const std::filesystem::path& data_dir) {
    std::filesystem::create_directories(data_dir.parent_path());
    const auto history = oracle::closure_history();
    const auto manifest = old_manifest(history);
    {
        auto directory = DataDirectory::open_and_lock(data_dir, DataDirectoryOpenMode::open_or_create);
        if (!directory) {
            throw std::runtime_error("failed to open data directory for the strict-prefix compaction seed");
        }
        std::size_t next_operation{};
        for (const auto& entry : manifest.segments) {
            auto created = DurableSegmentFile::create(*directory, segment_identity(manifest, entry));
            if (!created.durable() || !created.file) {
                throw std::runtime_error("failed to create a strict-prefix compaction seed Segment");
            }
            const auto segment_index = static_cast<std::size_t>(entry.segment_id.value - 1U);
            for (; next_operation < history.operations.size() &&
                   history.operations[next_operation].segment == segment_index;
                 ++next_operation) {
                const auto& operation = history.operations[next_operation];
                const auto encoded = encode_record({
                    .sequence = SequenceNumber{static_cast<std::uint64_t>(next_operation) + 1U},
                    .opcode = operation.opcode,
                    .type = ValueType::bytes,
                    .flags = 0,
                    .key_hash = hash_key(operation.key),
                    .expire_at_ns = operation.expire_at_ns,
                    .key = oracle::text_bytes(operation.key),
                    .value = oracle::text_bytes(operation.value),
                });
                if (!encoded || !created.file->append(*encoded).committed()) {
                    throw std::runtime_error("failed to append a strict-prefix compaction seed Record");
                }
            }
            if (entry.role == ManifestSegmentRole::sealed && !created.file->seal().committed()) {
                throw std::runtime_error("failed to seal a strict-prefix compaction seed Segment");
            }
        }
        if (next_operation != history.operations.size() || !directory->publish_manifest(manifest).durable()) {
            throw std::runtime_error("failed to publish the strict-prefix compaction seed Manifest");
        }
    }
    Identities identities;
    for (const auto& entry : manifest.segments) {
        identities.emplace(entry.segment_id.value,
                           glifistore::test::file_identity(segment_path(data_dir, manifest, entry)));
    }
    seeded_identities()[data_dir.string()] = std::move(identities);
}

void run_worker(const std::filesystem::path& data_dir, const FilesystemHooks& hooks) {
    auto directory = DataDirectory::open_and_lock(data_dir, DataDirectoryOpenMode::existing, hooks);
    if (!directory) {
        throw std::runtime_error("strict-prefix compaction worker failed to open the data directory");
    }
    // Recovered at an earlier time, so the TTL puts are still Index-resident when compaction drops them.
    auto runtime = DurableRuntimeCatalog::open_locked(std::move(*directory), oracle::kOpenNowNs);
    if (!runtime) {
        throw std::runtime_error("strict-prefix compaction worker failed to open the runtime");
    }
    const auto history = oracle::closure_history();
    const auto live = oracle::live_bytes_per_segment(history, oracle::kOpenNowNs);
    const auto result = (*runtime)->compact_worker(0, oracle::kCompactNowNs, oracle::prefix_budget(live, 2));
    if (!result.compacted() || result.stats.source_segments != kShape.source_count) {
        throw std::runtime_error("strict-prefix compaction worker did not compact the oldest two Segments");
    }
}

auto verify(const std::filesystem::path& data_dir, const std::string_view boundary) -> bool {
    const auto history = oracle::closure_history();
    const auto old = old_manifest(history);
    const auto next = next_manifest(old);
    const bool next_authority =
        std::ranges::find(kNextAuthorityBoundaries, boundary) != kNextAuthorityBoundaries.end();

    // The first reopen resolves the interrupted transaction. A kill between the Manifest temporary's
    // write and its rename leaves that recognized private temporary behind: intent recovery admits it (the
    // namespace is recovery-safe) and the next ordinary recovery removes it (the namespace is clean).
    for (const bool second_open : {false, true}) {
        auto runtime = DurableRuntimeCatalog::open_existing(data_dir, oracle::kCompactNowNs);
        if (!runtime) {
            return fail_verification("did not reopen: " + runtime.error().message);
        }
        if (!(*runtime)->verify_index()) {
            return fail_verification("rebuilt an Index that does not verify");
        }
        const auto& audit = (*runtime)->namespace_audit();
        if (!audit.recovery_safe() || (second_open && !audit.clean())) {
            return fail_verification(second_open ? "left a dirty namespace after ordinary recovery"
                                                 : "left a namespace that is not recovery-safe");
        }
        if ((*runtime)->manifest() != (next_authority ? next : old)) {
            return fail_verification(next_authority ? "did not recover the next authority"
                                                    : "did not recover the old authority");
        }
        for (const auto& [key, expected] : oracle::model_at(history, oracle::kCompactNowNs)) {
            const auto found = (*runtime)->get(key, oracle::kCompactNowNs);
            if (!expected) {
                if (found || found.error().code != ErrorCode::not_found) {
                    return fail_verification("resurrected or mis-reported " + key);
                }
                continue;
            }
            if (!found || std::string(reinterpret_cast<const char*>(found->bytes.data()),
                                      found->bytes.size()) != *expected) {
                return fail_verification("lost or changed " + key);
            }
        }
    }

    // Exactly the files of the recovered authority, and nothing else, remain.
    const auto& live_manifest = next_authority ? next : old;
    const auto& retired_manifest = next_authority ? old : next;
    for (const auto& entry : live_manifest.segments) {
        if (!std::filesystem::exists(segment_path(data_dir, live_manifest, entry))) {
            return fail_verification("lost a Segment of its authority");
        }
    }
    for (const auto& entry : retired_manifest.segments) {
        const bool listed = std::ranges::find(live_manifest.segments, entry) != live_manifest.segments.end();
        if (!listed && std::filesystem::exists(segment_path(data_dir, retired_manifest, entry))) {
            return fail_verification("kept a Segment of the other authority");
        }
    }

    // The sealed Segment after the prefix is the same file it was, whichever authority won.
    const auto identities = seeded_identities().find(data_dir.string());
    if (identities != seeded_identities().end()) {
        const auto& retained = old.segments[kRetainedSealedSegmentIndex];
        if (glifistore::test::file_identity(segment_path(data_dir, old, retained)) !=
            identities->second.at(retained.segment_id.value)) {
            return fail_verification("modified the sealed Segment after the prefix");
        }
    }
    return true;
}

auto boundaries() -> std::vector<std::string> {
    return {"preallocate_segment",
            "write_segment_header",
            "write_record#1",
            "write_record#2",
            "write_record#3",
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

} // namespace glifistore::crash::prefix_scenario
