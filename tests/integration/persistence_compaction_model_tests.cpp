// The runtime is a refinement of the durable-compaction model (docs/architecture/durable-compaction-model.md,
// section 10). The model decides each key independently: its visibility at a time, which Records a
// transaction selects, drops and copies, and how a foreground mutation during the build interacts with the
// remap. These tests give the real runtime one store whose keys are every history of at most three Records
// over three sealed Segments and the active one, so a single transaction exercises all 2,770 shapes at once,
// and then require, for every key and every time, exactly what the model predicts: the decision, the deciding
// Record, and every counter the transaction reports. A runtime that is wrong about any shape fails; so does a
// model that is wrong about the runtime.
#include "glifistore/core/key_hash.hpp"
#include "glifistore/persistence/compaction.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "glifistore/segment/record.hpp"
#include "persistence_recovery_test_support.hpp"
#include "support/compaction_model.hpp"
#include "support/file_identity.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace model = glifistore::test::compaction_model;
using glifistore::test::file_identity;
using glifistore::test::FileIdentity;

constexpr std::size_t kSealedSegments = 3;
constexpr auto kActiveSegment = static_cast<std::uint8_t>(kSealedSegments);

struct Placed {
    std::size_t key{};
    std::size_t op{};
    std::uint64_t sequence{};
};

// One key per history (and per foreground behavior): its history, the value of each of its Records, where
// every Record lives in sequence order, and how many encoded bytes it takes.
struct Population {
    std::vector<model::Pattern> patterns;
    std::vector<model::Foreground> foreground;
    std::vector<std::string> keys;
    std::vector<std::vector<std::string>> values;
    std::vector<std::vector<std::uint64_t>> bytes;
    std::vector<std::vector<Placed>> placed;
};

auto padded(const std::uint64_t number, const std::size_t width) -> std::string {
    auto text = std::to_string(number);
    return std::string(width > text.size() ? width - text.size() : 0U, '0') + text;
}

auto record_input(const Population& population, const Placed& placed) -> glifistore::RecordInput {
    const auto& op = population.patterns[placed.key][placed.op];
    const auto& key = population.keys[placed.key];
    const auto& value = population.values[placed.key][placed.op];
    return {
        .sequence = glifistore::SequenceNumber{placed.sequence},
        .opcode = op.kind == model::Kind::erase ? glifistore::Opcode::erase : glifistore::Opcode::put,
        .type = glifistore::ValueType::bytes,
        .flags = 0,
        .key_hash = glifistore::hash_key(key),
        .expire_at_ns = model::expiry_ns(op.kind),
        .key = std::as_bytes(std::span{key}),
        .value = std::as_bytes(std::span{value}),
    };
}

// Every non-empty history of at most max_length Records that `keep` admits, each under every foreground
// behavior in `foregrounds`.
auto make_population(const std::size_t max_length, const std::function<bool(const model::Pattern&)>& keep,
                     const std::vector<model::Foreground>& foregrounds) -> Population {
    Population population;
    model::for_each_pattern(kSealedSegments + 1U, max_length, [&](const model::Pattern& pattern) {
        if (pattern.empty() || !keep(pattern)) {
            return;
        }
        for (const auto foreground : foregrounds) {
            population.patterns.push_back(pattern);
            population.foreground.push_back(foreground);
        }
    });
    population.placed.resize(kSealedSegments + 1U);
    std::uint64_t sequence{};
    for (std::size_t segment = 0; segment <= kSealedSegments; ++segment) {
        for (std::size_t key = 0; key < population.patterns.size(); ++key) {
            for (std::size_t op = 0; op < population.patterns[key].size(); ++op) {
                if (population.patterns[key][op].segment == segment) {
                    population.placed[segment].push_back({.key = key, .op = op, .sequence = ++sequence});
                }
            }
        }
    }
    population.keys.reserve(population.patterns.size());
    population.values.resize(population.patterns.size());
    population.bytes.resize(population.patterns.size());
    for (std::size_t key = 0; key < population.patterns.size(); ++key) {
        population.keys.push_back("key-" + padded(key, 7));
        population.values[key].resize(population.patterns[key].size());
        population.bytes[key].resize(population.patterns[key].size());
    }
    for (const auto& segment : population.placed) {
        for (const auto& placed : segment) {
            const auto kind = population.patterns[placed.key][placed.op].kind;
            population.values[placed.key][placed.op] =
                kind == model::Kind::erase ? std::string{} : "value-" + padded(placed.sequence, 10);
            const auto size = glifistore::encoded_record_size(record_input(population, placed));
            GLIFI_REQUIRE(size.has_value());
            population.bytes[placed.key][placed.op] = *size;
        }
    }
    return population;
}

auto write_population(const std::filesystem::path& path, const Population& population)
    -> glifistore::Manifest {
    const auto store_id = recovery_store_id();
    std::vector<glifistore::ManifestSegmentEntry> entries;
    for (std::size_t index = 0; index <= kSealedSegments; ++index) {
        entries.push_back({.segment_id = glifistore::SegmentId{index + 1U},
                           .generation = glifistore::GenerationId{1},
                           .owner_worker = glifistore::WorkerId{0},
                           .role = index < kSealedSegments ? glifistore::ManifestSegmentRole::sealed
                                                           : glifistore::ManifestSegmentRole::active});
    }
    auto directory = glifistore::DataDirectory::open_and_lock(path);
    GLIFI_REQUIRE(directory.has_value());
    for (std::size_t index = 0; index <= kSealedSegments; ++index) {
        auto file = create_segment(*directory, store_id, entries[index]);
        // Records are appended without an individual commit and made durable once, by the seal or by the
        // flush of the active Segment, so that thousands of Records do not cost thousands of syncs.
        for (const auto& placed : population.placed[index]) {
            const auto encoded = glifistore::encode_record(record_input(population, placed));
            GLIFI_REQUIRE(encoded.has_value());
            GLIFI_REQUIRE(file.append_record(*encoded).committed());
        }
        if (entries[index].role == glifistore::ManifestSegmentRole::sealed) {
            GLIFI_REQUIRE(file.seal().committed());
        } else {
            GLIFI_REQUIRE(file.flush_pending_commit(glifistore::SegmentCommitSync::immediate).committed());
        }
    }
    auto manifest = recovery_manifest(store_id, 1, std::move(entries));
    GLIFI_REQUIRE(directory->publish_manifest(manifest).durable());
    return manifest;
}

// The value that the model names for Record `origin` of key `key`: one of its stored Records, or one of the
// foreground Records that were appended after them.
auto value_of(const Population& population, const std::size_t key, const std::size_t origin) -> std::string {
    const auto& pattern = population.patterns[key];
    if (origin < pattern.size()) {
        return population.values[key][origin];
    }
    return "foreground-" + population.keys[key] + "-" + std::to_string(origin - pattern.size());
}

struct Decision {
    bool visible{};
    std::string value;

    friend auto operator==(const Decision&, const Decision&) -> bool = default;
};

// What the history says before the transaction, with the key's foreground Records appended.
auto decide_before(const Population& population, const std::size_t key, const bool foreground,
                   const std::uint64_t now_ns) -> Decision {
    const auto pattern = model::with_foreground(
        population.patterns[key], foreground ? population.foreground[key] : model::Foreground::none,
        kActiveSegment);
    if (!model::visible_at(pattern, now_ns)) {
        return {};
    }
    return {true, value_of(population, key, pattern.size() - 1U)};
}

// What it says after a transaction over the oldest `prefix_length` sealed Segments.
auto decide_after(const Population& population, const std::size_t key, const bool foreground,
                  const std::size_t prefix_length, const std::uint64_t now_ns) -> Decision {
    const auto& pattern = population.patterns[key];
    const auto after = model::rewrite_with_foreground(
        pattern, model::prefix(prefix_length), model::recovered_index_contains(pattern),
        foreground ? population.foreground[key] : model::Foreground::none);
    if (!model::visible_after(after, now_ns)) {
        return {};
    }
    return {true, value_of(population, key, after.back().origin)};
}

auto read_decision(glifistore::DurableRuntimeCatalog& runtime, const std::string& key,
                   const std::uint64_t now_ns) -> Decision {
    const auto found = runtime.get(key, now_ns);
    if (found) {
        return {true, owned_text(*found)};
    }
    GLIFI_REQUIRE(found.error().code == glifistore::ErrorCode::not_found);
    return {};
}

[[noreturn]] void disagree(const Population& population, const std::size_t key, const std::string& what,
                           const std::uint64_t now_ns, const Decision& expected, const Decision& actual) {
    const auto text = [](const Decision& decision) {
        return decision.visible ? "'" + decision.value + "'" : std::string{"absent"};
    };
    throw std::runtime_error(
        what + ": " + population.keys[key] + " [" + model::describe(population.patterns[key]) +
        "] foreground " + std::to_string(static_cast<int>(population.foreground[key])) +
        " at t=" + std::to_string(now_ns) + " expected " + text(expected) + ", runtime " + text(actual));
}

// The counters that a transaction over the oldest `prefix_length` sealed Segments must report, from the model
// alone: Sel, Drop and Cp of section 3, and the outcome of each conditional remap of section 5.
struct Totals {
    std::uint64_t selected{};
    std::uint64_t dropped{};
    std::uint64_t copied{};
    std::uint64_t copied_bytes{};
    std::uint64_t superseded{};
    std::uint64_t remapped{};
    std::uint64_t expired_erased{};
};

auto predict(const Population& population, const std::size_t prefix_length, const bool foreground) -> Totals {
    Totals totals;
    for (std::size_t key = 0; key < population.patterns.size(); ++key) {
        const auto& pattern = population.patterns[key];
        const auto classification =
            model::classify(pattern, model::prefix(prefix_length), model::recovered_index_contains(pattern));
        const bool touched = foreground && model::touches(population.foreground[key]);
        totals.selected += classification.selected ? 1U : 0U;
        totals.dropped += classification.dropped ? 1U : 0U;
        if (classification.copied) {
            ++totals.copied;
            totals.copied_bytes += population.bytes[key][pattern.size() - 1U];
            ++(touched ? totals.superseded : totals.remapped);
        }
        if (classification.dropped && !touched) {
            ++totals.expired_erased;
        }
    }
    return totals;
}

// Bytes of the Records that the recovered Index references, per sealed Segment.
auto live_bytes(const Population& population) -> std::vector<std::uint64_t> {
    std::vector<std::uint64_t> live(kSealedSegments);
    for (std::size_t key = 0; key < population.patterns.size(); ++key) {
        const auto& pattern = population.patterns[key];
        if (model::recovered_index_contains(pattern) && pattern.back().segment < kSealedSegments) {
            live[pattern.back().segment] += population.bytes[key][pattern.size() - 1U];
        }
    }
    return live;
}

auto put_text(glifistore::DurableRuntimeCatalog& runtime, const std::string& key, const std::string& value)
    -> bool {
    return runtime.put(std::as_bytes(std::span{key}), std::as_bytes(std::span{value})).committed();
}

auto segment_path(const std::filesystem::path& directory, const glifistore::Manifest& manifest,
                  const glifistore::ManifestSegmentEntry& entry) -> std::filesystem::path {
    return directory / glifistore::segment_filename(segment_identity(manifest.store_id, entry));
}

// One transaction over `prefix_length` sealed Segments, compared with the model for every key.
void refine(const Population& population, const std::size_t prefix_length, const bool foreground) {
    RecoveryTemporaryDirectory temporary;
    const auto before = write_population(temporary.path(), population);
    std::map<std::uint64_t, FileIdentity> identities;
    for (const auto& entry : before.segments) {
        identities.emplace(entry.segment_id.value,
                           file_identity(segment_path(temporary.path(), before, entry)));
    }

    // The copy budget admits exactly the first `prefix_length` sealed Segments' live bytes, and the model's
    // own statement of the policy must then name that prefix.
    const auto live = live_bytes(population);
    std::uint64_t budget{};
    for (std::size_t index = 0; index < prefix_length; ++index) {
        budget += live[index];
    }
    const auto selection = model::reference_selection(live, budget, false);
    GLIFI_REQUIRE(selection.source_count == prefix_length);
    const auto totals = predict(population, prefix_length, foreground);
    // The output count is the aggregate bound here: all copies together are far below one payload.
    GLIFI_REQUIRE(totals.copied_bytes <= model::kPayloadBytes);
    const std::size_t outputs = totals.copied_bytes == 0 ? 0U : 1U;
    // A prefix reclaims a Segment only when it has fewer outputs than sources.
    GLIFI_REQUIRE(outputs < prefix_length);

    BlockingRecordRead blocker;
    auto directory = glifistore::DataDirectory::open_and_lock(
        temporary.path(),
        {.file_io = {.context = &blocker, .read_some_at = &BlockingRecordRead::read_some_at}});
    GLIFI_REQUIRE(directory.has_value());
    // The remap does not depend on the sync policy, so the foreground variant defers commits: a thousand
    // synchronous mutations would take a minute and a half. Close flushes them before the reopen below.
    glifistore::DurableRuntimeOptions options;
    if (foreground) {
        options.commit_sync = glifistore::SegmentCommitSync::deferred;
        options.sync_interval_ms = 10;
    }
    auto runtime =
        glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory), model::kOpenNs, options);
    GLIFI_REQUIRE(runtime.has_value());

    // The recovered store decides every key as the history says.
    for (std::size_t key = 0; key < population.keys.size(); ++key) {
        const auto expected = decide_before(population, key, false, model::kOpenNs);
        const auto actual = read_decision(**runtime, population.keys[key], model::kOpenNs);
        if (!(expected == actual)) {
            disagree(population, key, "recovery", model::kOpenNs, expected, actual);
        }
    }

    glifistore::DurableCompactionResult result;
    if (!foreground) {
        result = (*runtime)->compact_worker(0, model::kCompactNs, budget);
    } else {
        // Hold the build on its first source Record read and run every key's foreground behavior while it is
        // blocked: after the snapshot and before the commit, exactly the window of section 5.
        blocker.arm();
        std::thread compactor{[&] { result = (*runtime)->compact_worker(0, model::kCompactNs, budget); }};
        const bool blocked = blocker.wait_until_blocked();
        bool applied = blocked;
        if (blocked) {
            for (std::size_t key = 0; key < population.keys.size() && applied; ++key) {
                const auto& name = population.keys[key];
                const auto indexed = model::recovered_index_contains(population.patterns[key]);
                std::size_t foreground_index = 0;
                for (const auto kind : model::foreground_kinds(population.foreground[key])) {
                    const auto text =
                        value_of(population, key, population.patterns[key].size() + foreground_index);
                    if (kind == model::Kind::put) {
                        applied = put_text(**runtime, name, text) && applied;
                    } else {
                        // The runtime erases only a key that its Index holds. For the key's first foreground
                        // operation that is exactly the model's recovered Index, so the result is predicted.
                        const auto erased = (*runtime)->erase(std::as_bytes(std::span{name}));
                        if (foreground_index == 0) {
                            applied = (erased.committed() == indexed) && applied;
                        }
                    }
                    ++foreground_index;
                }
            }
        }
        blocker.release();
        compactor.join();
        GLIFI_REQUIRE(applied);
    }
    GLIFI_REQUIRE(result.compacted());

    // Every counter the transaction reports is the model's prediction.
    const auto& stats = result.stats;
    GLIFI_REQUIRE(stats.source_segments == prefix_length);
    GLIFI_REQUIRE(stats.retained_sealed_segments == kSealedSegments - prefix_length);
    GLIFI_REQUIRE(stats.source_index_records_verified == totals.selected);
    GLIFI_REQUIRE(stats.records_copied == totals.copied);
    GLIFI_REQUIRE(stats.expired_records_dropped == totals.dropped);
    GLIFI_REQUIRE(stats.bytes_copied == totals.copied_bytes);
    GLIFI_REQUIRE(stats.remapped_records == totals.remapped);
    GLIFI_REQUIRE(stats.superseded_copies == totals.superseded);
    GLIFI_REQUIRE(stats.expired_index_erasures == totals.expired_erased);
    GLIFI_REQUIRE(stats.manifest_rebases == 0);

    // One canonical transition, determined by (k, m).
    const auto after_manifest = (*runtime)->manifest();
    const auto shape =
        glifistore::derive_durable_compaction_shape(before, after_manifest, glifistore::WorkerId{0});
    GLIFI_REQUIRE(shape.has_value());
    GLIFI_REQUIRE(shape->source_count == prefix_length && shape->replacement_count == outputs);
    GLIFI_REQUIRE(after_manifest.segments.size() == before.segments.size() - (prefix_length - outputs));
    GLIFI_REQUIRE((*runtime)->healthy());
    // The Index is verified before any read at a time that would queue deferred TTL reclaims.
    GLIFI_REQUIRE((*runtime)->verify_index().has_value());

    // For every key and every probe time the runtime decides as the model's rewritten history does, and, from
    // t_c on, as the history before the transaction did (theorems 1 and 2).
    for (const auto now_ns : model::kProbeNs) {
        for (std::size_t key = 0; key < population.keys.size(); ++key) {
            const auto expected = decide_after(population, key, foreground, prefix_length, now_ns);
            const auto actual = read_decision(**runtime, population.keys[key], now_ns);
            if (!(expected == actual)) {
                disagree(population, key, "after the transaction", now_ns, expected, actual);
            }
            if (now_ns >= model::kCompactNs) {
                const auto unchanged = decide_before(population, key, foreground, now_ns);
                if (!(unchanged == actual)) {
                    disagree(population, key, "closure", now_ns, unchanged, actual);
                }
            }
        }
    }

    // Only the rewritten prefix's files changed: its sources are gone, its output exists, and every Segment
    // after it (the retained sealed ones and, with no foreground, the active one) is the same file.
    std::size_t sealed_index{};
    for (const auto& entry : before.segments) {
        const bool sealed = entry.role == glifistore::ManifestSegmentRole::sealed;
        if (sealed && sealed_index++ < prefix_length) {
            GLIFI_REQUIRE(!std::filesystem::exists(segment_path(temporary.path(), before, entry)));
            continue;
        }
        if (sealed || !foreground) {
            GLIFI_REQUIRE(file_identity(segment_path(temporary.path(), before, entry)) ==
                          identities.at(entry.segment_id.value));
        }
    }
    if (outputs != 0) {
        auto output = before.segments.front();
        ++output.generation.value;
        GLIFI_REQUIRE(std::filesystem::exists(segment_path(temporary.path(), after_manifest, output)));
    }

    // A fresh recovery at t_c rebuilds the same decisions from the new files alone.
    GLIFI_REQUIRE((*runtime)->close().has_value());
    runtime->reset();
    auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path(), model::kCompactNs);
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE((*reopened)->manifest() == after_manifest);
    GLIFI_REQUIRE((*reopened)->namespace_audit().clean());
    GLIFI_REQUIRE((*reopened)->verify_index().has_value());
    for (const auto now_ns : model::kProbeNs) {
        if (now_ns < model::kCompactNs) {
            continue;
        }
        for (std::size_t key = 0; key < population.keys.size(); ++key) {
            const auto expected = decide_before(population, key, foreground, now_ns);
            const auto actual = read_decision(**reopened, population.keys[key], now_ns);
            if (!(expected == actual)) {
                disagree(population, key, "recovery after the transaction", now_ns, expected, actual);
            }
        }
    }
}

} // namespace

GLIFI_TEST("refinement: a strict prefix of two sealed Segments matches the model for every history of three "
           "Records") {
    // 2,770 keys, one per history of at most three Records over three sealed Segments and the active one. The
    // budget selects the oldest two sealed Segments; the third sealed Segment is retained.
    const auto population =
        make_population(3, [](const model::Pattern&) { return true; }, {model::Foreground::none});
    GLIFI_REQUIRE(population.keys.size() == model::pattern_count(kSealedSegments + 1U, 3) - 1U);
    GLIFI_REQUIRE(population.keys.size() == 2'770);
    refine(population, 2, false);
}

GLIFI_TEST("refinement: the complete sealed set matches the model for every history of three Records") {
    const auto population =
        make_population(3, [](const model::Pattern&) { return true; }, {model::Foreground::none});
    refine(population, 3, false);
}

GLIFI_TEST(
    "refinement: a zero-output prefix of one Segment matches the model for every history of three Records") {
    // The first sealed Segment holds no Record that survives: every newest Record in it is a tombstone, an
    // expired put, or a put that has expired by compaction time. The estimate still counts the last ones as
    // live (the Index carries no expiry), no prefix promises a gain, and the exact layout drops them all.
    const auto population = make_population(
        3,
        [](const model::Pattern& pattern) {
            const auto& newest = pattern.back();
            return !(newest.segment == 0 &&
                     (newest.kind == model::Kind::put || newest.kind == model::Kind::ttl_after_compaction));
        },
        {model::Foreground::none});
    refine(population, 1, false);
}

GLIFI_TEST(
    "refinement: foreground mutations during the build match the model for every history of two Records") {
    // Every history of at most two Records, each under every foreground behavior (none, put, erase, erase then
    // put) applied between the snapshot and the commit. The conditional remap must supersede exactly the copies
    // of the touched keys, remap the others, erase the untouched expired ones, and never change a decision.
    const auto population = make_population(2, [](const model::Pattern&) { return true; },
                                            {model::Foreground::none, model::Foreground::put,
                                             model::Foreground::erase, model::Foreground::erase_then_put});
    // 270 non-empty histories of at most two Records over four Segments, four behaviors each.
    GLIFI_REQUIRE(population.keys.size() == 1'080);
    refine(population, 2, true);
}
