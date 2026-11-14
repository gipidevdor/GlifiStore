// Strict-prefix durable compaction (ADR 0044): the canonical (k, m) transition, its validator, the
// planner under a prefix, the copy-budget prefix selection and the prefix restriction of an Index
// snapshot. Every fixture here has k < n for at least one case; the whole-set (k = n) behavior is
// pinned beside it so a regression cannot trade one for the other.
#include "glifistore/core/types.hpp"
#include "glifistore/persistence/compaction.hpp"
#include "glifistore/persistence/compaction_builder.hpp"
#include "test.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kWorkerZeroSealedCount = 4;
constexpr std::uint64_t kSegmentPayloadBytes =
    glifistore::kSegmentSizeBytes - glifistore::kSegmentHeaderReservedBytes;

auto segment_entry(const std::uint64_t segment_id, const std::uint32_t worker, const std::uint32_t generation,
                   const glifistore::ManifestSegmentRole role) -> glifistore::ManifestSegmentEntry {
    return {.segment_id = glifistore::SegmentId{segment_id},
            .generation = glifistore::GenerationId{generation},
            .owner_worker = glifistore::WorkerId{worker},
            .role = role};
}

// Worker 0 owns four sealed Segments (IDs 1, 3, 5, 6) interleaved with Worker 1 entries and followed by
// its own active Segment (ID 7), so a prefix must be found by Worker ownership and not by position.
auto prefix_manifest() -> glifistore::Manifest {
    constexpr auto sealed = glifistore::ManifestSegmentRole::sealed;
    constexpr auto active = glifistore::ManifestSegmentRole::active;
    return {
        .store_id = {std::byte{0x52}},
        .manifest_generation = 11,
        .worker_count = 2,
        .routing_epoch = 1,
        .next_segment_id = glifistore::SegmentId{8},
        .next_segment_generation = glifistore::GenerationId{1},
        .segments =
            {
                segment_entry(1, 0, 2, sealed),
                segment_entry(2, 1, 1, sealed),
                segment_entry(3, 0, 4, sealed),
                segment_entry(4, 1, 3, active),
                segment_entry(5, 0, 1, sealed),
                segment_entry(6, 0, 3, sealed),
                segment_entry(7, 0, 1, active),
            },
    };
}

auto prefix_limits() -> glifistore::DurableResourceLimits {
    auto limits = glifistore::DurableResourceLimits{};
    limits.max_store_bytes = 16ULL * glifistore::kSegmentSizeBytes;
    limits.max_segment_count = 16;
    limits.max_temporary_compaction_bytes = 8ULL * glifistore::kSegmentSizeBytes;
    limits.max_write_amplification = 4;
    return limits;
}

// Independent statement of the ADR 0044 rule: outputs reuse the earliest source IDs with the next
// generation, surplus sources disappear, and every other entry (retained sealed Segments, the active
// Segment, other Workers) keeps its Manifest position and identity.
auto expected_transition(const glifistore::Manifest& current, const glifistore::WorkerId worker,
                         const std::size_t source_count, const std::size_t replacement_count)
    -> glifistore::Manifest {
    std::vector<glifistore::ManifestSegmentEntry> sealed;
    for (const auto& entry : current.segments) {
        if (entry.owner_worker == worker && entry.role == glifistore::ManifestSegmentRole::sealed) {
            sealed.push_back(entry);
        }
    }
    auto expected = current;
    ++expected.manifest_generation;
    expected.segments.clear();
    for (const auto& entry : current.segments) {
        const auto found = std::ranges::find(sealed, entry);
        const auto position = static_cast<std::size_t>(found - sealed.begin());
        if (found == sealed.end() || position >= source_count) {
            expected.segments.push_back(entry);
            continue;
        }
        if (position < replacement_count) {
            auto output = entry;
            ++output.generation.value;
            expected.segments.push_back(output);
        }
    }
    return expected;
}

auto canonical(const glifistore::Manifest& current, const std::size_t source_count,
               const std::size_t replacement_count) -> glifistore::Manifest {
    const auto next = glifistore::canonical_durable_compaction_manifest(
        current, glifistore::WorkerId{0},
        {.source_count = source_count, .replacement_count = replacement_count});
    GLIFI_REQUIRE(next.has_value());
    return *next;
}

void require_rejected(const glifistore::Manifest& current, const glifistore::Manifest& next) {
    const auto shape = glifistore::derive_durable_compaction_shape(current, next, glifistore::WorkerId{0});
    GLIFI_REQUIRE(!shape.has_value());
    GLIFI_REQUIRE(shape.error().code == glifistore::ErrorCode::invalid_argument);
    const auto replacements =
        glifistore::validate_durable_compaction_transition(current, next, glifistore::WorkerId{0});
    GLIFI_REQUIRE(!replacements.has_value());
    GLIFI_REQUIRE(replacements.error().code == glifistore::ErrorCode::invalid_argument);
}

// One IndexEntry per whole Record, each at most the maximum normal Record, attributed to the i-th sealed
// Segment of `worker` in Manifest order. Only the Segment identity and size feed the prefix estimate.
auto live_entries(const glifistore::Manifest& manifest, const glifistore::WorkerId worker,
                  const std::vector<std::uint64_t>& live_bytes_per_sealed_segment)
    -> std::vector<glifistore::IndexEntry> {
    std::vector<glifistore::IndexEntry> entries;
    std::size_t sealed_index{};
    std::uint64_t counter{};
    for (const auto& segment : manifest.segments) {
        if (segment.owner_worker != worker || segment.role != glifistore::ManifestSegmentRole::sealed) {
            continue;
        }
        auto remaining = live_bytes_per_sealed_segment.at(sealed_index++);
        while (remaining > 0) {
            const auto size = std::min<std::uint64_t>(remaining, glifistore::kMaxNormalRecordSize);
            ++counter;
            entries.push_back({.key = "live-" + std::to_string(counter),
                               .record = {.segment_id = segment.segment_id,
                                          .offset = glifistore::RecordOffset{4096},
                                          .size = glifistore::RecordSize{static_cast<std::uint32_t>(size)},
                                          .sequence = glifistore::SequenceNumber{counter},
                                          .generation = segment.generation}});
            remaining -= size;
        }
    }
    return entries;
}

auto select_prefix(const glifistore::Manifest& manifest, const std::vector<glifistore::IndexEntry>& entries,
                   const std::uint64_t max_copy_bytes) -> glifistore::DurableCompactionPrefixChoice {
    const auto choice = glifistore::select_durable_compaction_prefix(manifest, glifistore::WorkerId{0},
                                                                     entries, max_copy_bytes);
    GLIFI_REQUIRE(choice.has_value());
    return *choice;
}

} // namespace

GLIFI_TEST("durable compaction transition is canonical for every prefix length and output count") {
    const auto current = prefix_manifest();
    std::vector<glifistore::Manifest> seen;
    for (std::size_t source_count = 1; source_count <= kWorkerZeroSealedCount; ++source_count) {
        for (std::size_t replacement_count = 0; replacement_count <= source_count; ++replacement_count) {
            const glifistore::DurableCompactionShape shape{.source_count = source_count,
                                                           .replacement_count = replacement_count};
            const auto next =
                glifistore::canonical_durable_compaction_manifest(current, glifistore::WorkerId{0}, shape);
            GLIFI_REQUIRE(next.has_value());
            GLIFI_REQUIRE(*next == expected_transition(current, glifistore::WorkerId{0}, source_count,
                                                       replacement_count));
            GLIFI_REQUIRE(glifistore::encode_manifest(*next).has_value());

            // The validator needs only the two Manifests and recovers the exact (k, m).
            const auto derived =
                glifistore::derive_durable_compaction_shape(current, *next, glifistore::WorkerId{0});
            GLIFI_REQUIRE(derived.has_value());
            GLIFI_REQUIRE(*derived == shape);
            const auto replacements =
                glifistore::validate_durable_compaction_transition(current, *next, glifistore::WorkerId{0});
            GLIFI_REQUIRE(replacements.has_value());
            GLIFI_REQUIRE(*replacements == replacement_count);

            // A transition is determined by (k, m): no two shapes share a next Manifest.
            GLIFI_REQUIRE(std::ranges::find(seen, *next) == seen.end());
            seen.push_back(*next);
        }
    }
    constexpr std::size_t kShapes = (kWorkerZeroSealedCount + 1U) * (kWorkerZeroSealedCount + 2U) / 2U - 1U;
    GLIFI_REQUIRE(seen.size() == kShapes);
}

GLIFI_TEST("durable compaction prefix transition matches the ADR 0044 worked example") {
    constexpr auto sealed = glifistore::ManifestSegmentRole::sealed;
    constexpr auto active = glifistore::ManifestSegmentRole::active;
    const auto current = prefix_manifest();
    const auto next = canonical(current, 2, 1);

    GLIFI_REQUIRE(next.manifest_generation == current.manifest_generation + 1U);
    GLIFI_REQUIRE(next.next_segment_id == current.next_segment_id);
    GLIFI_REQUIRE(next.next_segment_generation == current.next_segment_generation);
    GLIFI_REQUIRE(next.store_id == current.store_id);
    GLIFI_REQUIRE(next.worker_count == current.worker_count);
    const std::vector<glifistore::ManifestSegmentEntry> expected{
        // The output reuses the earliest source ID with the next generation.
        segment_entry(1, 0, 3, sealed),
        segment_entry(2, 1, 1, sealed),
        // ID 3 (generation 4) is the surplus source and disappears.
        segment_entry(4, 1, 3, active),
        // Sealed entries after the prefix and the active entry are unchanged.
        segment_entry(5, 0, 1, sealed),
        segment_entry(6, 0, 3, sealed),
        segment_entry(7, 0, 1, active),
    };
    GLIFI_REQUIRE(next.segments == expected);

    // Zero outputs retire the prefix without fabricating a Segment.
    const auto retired = canonical(current, 1, 0);
    GLIFI_REQUIRE(retired.segments.size() == current.segments.size() - 1U);
    GLIFI_REQUIRE(retired.segments.front() == current.segments[1]);
    GLIFI_REQUIRE(std::ranges::find(retired.segments, current.segments[0]) == retired.segments.end());
}

GLIFI_TEST("durable compaction transition rejects reordered gapped and non-prefix manifests") {
    constexpr auto sealed = glifistore::ManifestSegmentRole::sealed;
    const auto current = prefix_manifest();
    const auto base = canonical(current, 2, 1);

    // Reordered: Manifest entries must stay in strictly increasing Segment ID order.
    auto reordered = base;
    std::swap(reordered.segments[3], reordered.segments[4]);
    require_rejected(current, reordered);
    reordered = base;
    std::swap(reordered.segments[0], reordered.segments[3]);
    require_rejected(current, reordered);

    // Reordered identity: a valid, ID-ordered Manifest whose output reuses a later source's ID (3) instead
    // of the earliest one (1).
    auto relabeled = base;
    relabeled.segments.erase(relabeled.segments.begin());
    relabeled.segments.insert(relabeled.segments.begin() + 1, segment_entry(3, 0, 5, sealed));
    require_rejected(current, relabeled);

    // Gapped: ID 1 is replaced and ID 5 vanishes while the older ID 3 is still listed unchanged.
    auto gapped = current;
    ++gapped.manifest_generation;
    ++gapped.segments[0].generation.value;
    gapped.segments.erase(gapped.segments.begin() + 4);
    require_rejected(current, gapped);

    // Non-prefix: only a middle Segment is replaced.
    auto middle = current;
    ++middle.manifest_generation;
    ++middle.segments[2].generation.value;
    require_rejected(current, middle);

    // Non-prefix: only the newest sealed Segment is replaced.
    auto newest = current;
    ++newest.manifest_generation;
    ++newest.segments[5].generation.value;
    require_rejected(current, newest);

    // Non-prefix: a hole inside the replaced set (IDs 1 and 5 replaced, ID 3 untouched).
    auto hole = current;
    ++hole.manifest_generation;
    ++hole.segments[0].generation.value;
    ++hole.segments[4].generation.value;
    require_rejected(current, hole);

    // Non-prefix: a transition that touches nothing is not a compaction.
    auto unchanged = current;
    ++unchanged.manifest_generation;
    require_rejected(current, unchanged);
}

GLIFI_TEST("durable compaction transition rejects wrong generations and foreign changes") {
    const auto current = prefix_manifest();
    const auto base = canonical(current, 2, 1);

    auto skipped = base;
    skipped.segments[0].generation.value += 1;
    require_rejected(current, skipped);

    auto stale = base;
    stale.segments[0].generation.value = current.segments[0].generation.value;
    require_rejected(current, stale);

    auto regressed = base;
    regressed.segments[0].generation.value = current.segments[0].generation.value - 1U;
    require_rejected(current, regressed);

    auto bumped_retained = base;
    ++bumped_retained.segments[3].generation.value;
    require_rejected(current, bumped_retained);

    auto other_worker = base;
    ++other_worker.segments[1].generation.value;
    require_rejected(current, other_worker);

    auto active_changed = base;
    ++active_changed.segments[5].generation.value;
    require_rejected(current, active_changed);

    // Ownership change: Worker 1's sealed ID 2 relabeled as Worker 0's grows its sealed set past the source.
    auto grown = canonical(current, 1, 1);
    grown.segments[1].owner_worker = glifistore::WorkerId{0};
    require_rejected(current, grown);

    auto next_id = base;
    ++next_id.next_segment_id.value;
    require_rejected(current, next_id);

    auto routing = base;
    ++routing.routing_epoch;
    require_rejected(current, routing);

    auto generation_skipped = base;
    ++generation_skipped.manifest_generation;
    require_rejected(current, generation_skipped);

    auto generation_repeated = base;
    --generation_repeated.manifest_generation;
    require_rejected(current, generation_repeated);

    // The transition belongs to Worker 0: Worker 1 sees no prefix change in it, and an unknown Worker is
    // outside the Manifest.
    const auto other = glifistore::derive_durable_compaction_shape(current, base, glifistore::WorkerId{1});
    GLIFI_REQUIRE(!other.has_value());
    GLIFI_REQUIRE(other.error().code == glifistore::ErrorCode::invalid_argument);
    const auto unknown = glifistore::derive_durable_compaction_shape(current, base, glifistore::WorkerId{2});
    GLIFI_REQUIRE(!unknown.has_value());
    GLIFI_REQUIRE(unknown.error().code == glifistore::ErrorCode::invalid_argument);
}

GLIFI_TEST("durable compaction generation exhaustion only blocks outputs, not surplus sources") {
    auto current = prefix_manifest();
    constexpr auto kMaxGeneration = std::numeric_limits<std::uint32_t>::max();
    current.segments[2].generation.value = kMaxGeneration;

    // ID 3 is the second sealed source. With one output it is surplus and its generation is never bumped.
    const auto surplus = glifistore::canonical_durable_compaction_manifest(
        current, glifistore::WorkerId{0}, {.source_count = 2, .replacement_count = 1});
    GLIFI_REQUIRE(surplus.has_value());
    const auto derived =
        glifistore::derive_durable_compaction_shape(current, *surplus, glifistore::WorkerId{0});
    GLIFI_REQUIRE(derived.has_value());
    const glifistore::DurableCompactionShape expected_shape{.source_count = 2, .replacement_count = 1};
    GLIFI_REQUIRE(*derived == expected_shape);

    // With two outputs the exhausted generation would have to be incremented.
    const auto exhausted = glifistore::canonical_durable_compaction_manifest(
        current, glifistore::WorkerId{0}, {.source_count = 2, .replacement_count = 2});
    GLIFI_REQUIRE(!exhausted.has_value());
    GLIFI_REQUIRE(exhausted.error().code == glifistore::ErrorCode::arithmetic_overflow);

    // A retained entry's exhausted generation is irrelevant to a shorter prefix.
    current.segments[4].generation.value = kMaxGeneration;
    const auto retained = glifistore::canonical_durable_compaction_manifest(
        current, glifistore::WorkerId{0}, {.source_count = 2, .replacement_count = 1});
    GLIFI_REQUIRE(retained.has_value());
}

GLIFI_TEST("durable compaction planner replaces only the selected sealed prefix") {
    const auto current = prefix_manifest();
    const auto limits = prefix_limits();
    constexpr auto kSegmentBytes = static_cast<std::uint64_t>(glifistore::kSegmentSizeBytes);

    const auto plan =
        glifistore::plan_durable_worker_compaction(current, glifistore::WorkerId{0}, 1, limits, 0, 2);
    GLIFI_REQUIRE(plan.has_value());
    const std::vector<glifistore::ManifestSegmentEntry> sources{current.segments[0], current.segments[2]};
    GLIFI_REQUIRE(plan->sources == sources);
    GLIFI_REQUIRE(plan->replacements.size() == 1);
    GLIFI_REQUIRE(plan->replacements[0] == segment_entry(1, 0, 3, glifistore::ManifestSegmentRole::sealed));
    GLIFI_REQUIRE(plan->next_manifest == expected_transition(current, glifistore::WorkerId{0}, 2, 1));
    GLIFI_REQUIRE(plan->temporary_bytes == kSegmentBytes);
    GLIFI_REQUIRE(plan->reclaimed_bytes == kSegmentBytes);

    // Zero outputs retire a fully obsolete oldest Segment without any temporary space.
    const auto retired =
        glifistore::plan_durable_worker_compaction(current, glifistore::WorkerId{0}, 0, limits, 0, 1);
    GLIFI_REQUIRE(retired.has_value());
    GLIFI_REQUIRE(retired->sources.size() == 1);
    GLIFI_REQUIRE(retired->replacements.empty());
    GLIFI_REQUIRE(retired->temporary_bytes == 0);
    GLIFI_REQUIRE(retired->reclaimed_bytes == kSegmentBytes);
    GLIFI_REQUIRE(retired->next_manifest.segments.size() == current.segments.size() - 1U);

    // A prefix that is not shorter after compaction reclaims nothing and is refused before any intent.
    for (const std::size_t source_count : {1U, 2U, 3U, 4U}) {
        const auto no_gain = glifistore::plan_durable_worker_compaction(
            current, glifistore::WorkerId{0}, source_count, limits, 0, source_count);
        GLIFI_REQUIRE(!no_gain.has_value());
        GLIFI_REQUIRE(no_gain.error().code == glifistore::ErrorCode::storage_exhausted);
    }

    const auto too_many_outputs =
        glifistore::plan_durable_worker_compaction(current, glifistore::WorkerId{0}, 3, limits, 0, 2);
    GLIFI_REQUIRE(!too_many_outputs.has_value());
    GLIFI_REQUIRE(too_many_outputs.error().code == glifistore::ErrorCode::invalid_argument);
    const auto beyond_sealed =
        glifistore::plan_durable_worker_compaction(current, glifistore::WorkerId{0}, 1, limits, 0, 5);
    GLIFI_REQUIRE(!beyond_sealed.has_value());
    GLIFI_REQUIRE(beyond_sealed.error().code == glifistore::ErrorCode::invalid_argument);
}

GLIFI_TEST("durable compaction budgets are computed from the prefix, not the complete sealed set") {
    const auto current = prefix_manifest();

    // Write amplification is outputs / reclaimed Segments of the prefix. (k=2, m=1) is 1x and (k=3, m=2)
    // is 2x, although the complete set of four would make either look different.
    auto limits = prefix_limits();
    limits.max_write_amplification = 1;
    GLIFI_REQUIRE(
        glifistore::plan_durable_worker_compaction(current, glifistore::WorkerId{0}, 1, limits, 0, 2)
            .has_value());
    const auto amplified =
        glifistore::plan_durable_worker_compaction(current, glifistore::WorkerId{0}, 2, limits, 0, 3);
    GLIFI_REQUIRE(!amplified.has_value());
    GLIFI_REQUIRE(amplified.error().code == glifistore::ErrorCode::storage_exhausted);
    limits.max_write_amplification = 2;
    GLIFI_REQUIRE(
        glifistore::plan_durable_worker_compaction(current, glifistore::WorkerId{0}, 2, limits, 0, 3)
            .has_value());

    // The temporary-space budget is the output count of the prefix.
    limits = prefix_limits();
    limits.max_temporary_compaction_bytes = glifistore::kSegmentSizeBytes;
    GLIFI_REQUIRE(
        glifistore::plan_durable_worker_compaction(current, glifistore::WorkerId{0}, 1, limits, 0, 3)
            .has_value());
    const auto temporary =
        glifistore::plan_durable_worker_compaction(current, glifistore::WorkerId{0}, 2, limits, 0, 3);
    GLIFI_REQUIRE(!temporary.has_value());
    GLIFI_REQUIRE(temporary.error().code == glifistore::ErrorCode::storage_exhausted);
}

GLIFI_TEST("durable compaction whole-set plans are unchanged by an explicit prefix of every Segment") {
    const auto current = prefix_manifest();
    const auto limits = prefix_limits();
    for (std::size_t output_count = 0; output_count <= kWorkerZeroSealedCount; ++output_count) {
        const auto implicit = glifistore::plan_durable_worker_compaction(current, glifistore::WorkerId{0},
                                                                         output_count, limits);
        const auto explicit_whole = glifistore::plan_durable_worker_compaction(
            current, glifistore::WorkerId{0}, output_count, limits, 0, kWorkerZeroSealedCount);
        GLIFI_REQUIRE(implicit.has_value() == explicit_whole.has_value());
        if (!implicit) {
            GLIFI_REQUIRE(implicit.error().code == explicit_whole.error().code);
            continue;
        }
        GLIFI_REQUIRE(implicit->sources.size() == kWorkerZeroSealedCount);
        GLIFI_REQUIRE(implicit->worker_id == explicit_whole->worker_id);
        GLIFI_REQUIRE(implicit->sources == explicit_whole->sources);
        GLIFI_REQUIRE(implicit->replacements == explicit_whole->replacements);
        GLIFI_REQUIRE(implicit->next_manifest == explicit_whole->next_manifest);
        GLIFI_REQUIRE(implicit->temporary_bytes == explicit_whole->temporary_bytes);
        GLIFI_REQUIRE(implicit->reclaimed_bytes == explicit_whole->reclaimed_bytes);
        GLIFI_REQUIRE(implicit->next_manifest == expected_transition(current, glifistore::WorkerId{0},
                                                                     kWorkerZeroSealedCount, output_count));
    }
}

GLIFI_TEST("durable compaction blob profile keeps the complete sealed set") {
    auto current = prefix_manifest();
    current.format_version = 2;
    const auto limits = prefix_limits();

    // The complete set stays admissible under the blob profile.
    const auto whole = glifistore::plan_durable_worker_compaction(current, glifistore::WorkerId{0}, 2, limits,
                                                                  0, kWorkerZeroSealedCount);
    GLIFI_REQUIRE(whole.has_value());
    const auto next = canonical(current, kWorkerZeroSealedCount, 2);
    const auto derived = glifistore::derive_durable_compaction_shape(current, next, glifistore::WorkerId{0});
    GLIFI_REQUIRE(derived.has_value());
    GLIFI_REQUIRE(derived->source_count == kWorkerZeroSealedCount);

    // A strict prefix is refused by the planner and by the transition validator.
    const auto planned =
        glifistore::plan_durable_worker_compaction(current, glifistore::WorkerId{0}, 1, limits, 0, 2);
    GLIFI_REQUIRE(!planned.has_value());
    GLIFI_REQUIRE(planned.error().code == glifistore::ErrorCode::invalid_argument);
    require_rejected(current, canonical(current, 2, 1));
    require_rejected(current, canonical(current, 3, 0));

    // The blob rule comes before any resource arithmetic: a strict prefix is refused as an invalid
    // argument even when the write-amplification limit would also refuse it.
    auto strict_limits = limits;
    strict_limits.max_write_amplification = 1;
    const auto refused =
        glifistore::plan_durable_worker_compaction(current, glifistore::WorkerId{0}, 2, strict_limits, 0, 3);
    GLIFI_REQUIRE(!refused.has_value());
    GLIFI_REQUIRE(refused.error().code == glifistore::ErrorCode::invalid_argument);
    // Control: the complete set under the same limit is refused by that limit, not by the profile.
    const auto amplified = glifistore::plan_durable_worker_compaction(
        current, glifistore::WorkerId{0}, 3, strict_limits, 0, kWorkerZeroSealedCount);
    GLIFI_REQUIRE(!amplified.has_value());
    GLIFI_REQUIRE(amplified.error().code == glifistore::ErrorCode::storage_exhausted);
}

GLIFI_TEST("durable compaction prefix selection picks the shortest prefix among equal estimated reclaim") {
    const auto manifest = prefix_manifest();
    constexpr std::uint64_t kSmall = 1024;

    // k=1 reclaims one Segment (nothing live, zero outputs). k=2, 3 and 4 each reclaim exactly one as well,
    // because their live bytes need 1, 2 and 3 outputs. The shortest prefix wins every tie.
    const auto entries = live_entries(manifest, glifistore::WorkerId{0},
                                      {0, kSmall, kSegmentPayloadBytes, kSegmentPayloadBytes});
    const auto tie = select_prefix(manifest, entries, 0);
    GLIFI_REQUIRE(tie.source_count == 1);
    GLIFI_REQUIRE(tie.live_bytes == 0);
    GLIFI_REQUIRE(tie.estimated_reclaimed_segments == 1);

    // A strictly better longer prefix wins. Three small Segments reclaim two, and a heavy newest Segment
    // adds an output without adding reclaim, so the strict prefix k=3 beats the complete set.
    const auto heavy_tail =
        live_entries(manifest, glifistore::WorkerId{0}, {kSmall, kSmall, kSmall, kSegmentPayloadBytes});
    const auto prefix = select_prefix(manifest, heavy_tail, 0);
    GLIFI_REQUIRE(prefix.source_count == 3);
    GLIFI_REQUIRE(prefix.live_bytes == 3U * kSmall);
    GLIFI_REQUIRE(prefix.estimated_reclaimed_segments == 2);

    // With only small live data, every extra Segment adds reclaim, so the complete set is the best prefix.
    const auto small = live_entries(manifest, glifistore::WorkerId{0}, {kSmall, kSmall, kSmall, kSmall});
    const auto whole = select_prefix(manifest, small, 0);
    GLIFI_REQUIRE(whole.source_count == kWorkerZeroSealedCount);
    GLIFI_REQUIRE(whole.live_bytes == 4U * kSmall);
    GLIFI_REQUIRE(whole.estimated_reclaimed_segments == 3);

    // A fully dead sealed history reclaims everything with zero outputs.
    const auto dead = live_entries(manifest, glifistore::WorkerId{0}, {0, 0, 0, 0});
    const auto retire = select_prefix(manifest, dead, 0);
    GLIFI_REQUIRE(retire.source_count == kWorkerZeroSealedCount);
    GLIFI_REQUIRE(retire.live_bytes == 0);
    GLIFI_REQUIRE(retire.estimated_reclaimed_segments == kWorkerZeroSealedCount);
}

GLIFI_TEST("durable compaction copy budget bounds the selected prefix inclusively") {
    const auto manifest = prefix_manifest();
    constexpr std::uint64_t kSmall = 1024;
    const auto entries = live_entries(manifest, glifistore::WorkerId{0}, {kSmall, kSmall, kSmall, kSmall});

    GLIFI_REQUIRE(select_prefix(manifest, entries, 0).source_count == 4);
    // The budget is inclusive of the prefix's live bytes.
    GLIFI_REQUIRE(select_prefix(manifest, entries, 4U * kSmall).source_count == 4);
    const auto three = select_prefix(manifest, entries, 4U * kSmall - 1U);
    GLIFI_REQUIRE(three.source_count == 3);
    GLIFI_REQUIRE(three.live_bytes == 3U * kSmall);
    GLIFI_REQUIRE(three.live_bytes <= 4U * kSmall - 1U);
    GLIFI_REQUIRE(select_prefix(manifest, entries, 3U * kSmall).source_count == 3);
    const auto two = select_prefix(manifest, entries, 3U * kSmall - 1U);
    GLIFI_REQUIRE(two.source_count == 2);
    GLIFI_REQUIRE(two.estimated_reclaimed_segments == 1);

    // One Segment of live data alone promises no gain. The longest admissible prefix is still returned so
    // that the exact layout (which drops expired puts the Index cannot see) can decide.
    const auto no_gain = select_prefix(manifest, entries, kSmall);
    GLIFI_REQUIRE(no_gain.source_count == 1);
    GLIFI_REQUIRE(no_gain.live_bytes == kSmall);
    GLIFI_REQUIRE(no_gain.estimated_reclaimed_segments == 0);

    // The oldest Segment alone over the budget admits no prefix at all.
    const auto none = select_prefix(manifest, entries, kSmall - 1U);
    GLIFI_REQUIRE(none.source_count == 0);
    GLIFI_REQUIRE(none.live_bytes == 0);
    GLIFI_REQUIRE(none.estimated_reclaimed_segments == 0);

    // Without any estimated gain anywhere the longest prefix within the budget is returned.
    const auto full = live_entries(
        manifest, glifistore::WorkerId{0},
        {kSegmentPayloadBytes, kSegmentPayloadBytes, kSegmentPayloadBytes, kSegmentPayloadBytes});
    const auto unbounded = select_prefix(manifest, full, 0);
    GLIFI_REQUIRE(unbounded.source_count == 4);
    GLIFI_REQUIRE(unbounded.estimated_reclaimed_segments == 0);
    const auto bounded = select_prefix(manifest, full, 2U * kSegmentPayloadBytes);
    GLIFI_REQUIRE(bounded.source_count == 2);
    GLIFI_REQUIRE(bounded.live_bytes == 2U * kSegmentPayloadBytes);
    GLIFI_REQUIRE(bounded.estimated_reclaimed_segments == 0);
    GLIFI_REQUIRE(select_prefix(manifest, full, kSegmentPayloadBytes - 1U).source_count == 0);
}

GLIFI_TEST("durable compaction prefix selection attributes only this Worker's sealed Segment generations") {
    const auto manifest = prefix_manifest();
    constexpr std::uint64_t kSmall = 1024;
    auto entries = live_entries(manifest, glifistore::WorkerId{0}, {kSmall, kSmall, 0, 0});
    const auto baseline = select_prefix(manifest, entries, 0);

    // References into the active Segment, another Worker's sealed Segment, an unknown Segment and a retired
    // generation of a prefix Segment never count toward the prefix estimate.
    const auto foreign = [](const std::uint64_t segment_id, const std::uint32_t generation) {
        return glifistore::IndexEntry{
            .key = "foreign-" + std::to_string(segment_id) + "-" + std::to_string(generation),
            .record = {
                .segment_id = glifistore::SegmentId{segment_id},
                .offset = glifistore::RecordOffset{4096},
                .size = glifistore::RecordSize{static_cast<std::uint32_t>(glifistore::kMaxNormalRecordSize)},
                .sequence = glifistore::SequenceNumber{900},
                .generation = glifistore::GenerationId{generation}}};
    };
    entries.push_back(foreign(7, 1));
    entries.push_back(foreign(2, 1));
    entries.push_back(foreign(99, 1));
    entries.push_back(foreign(1, 9));
    const auto filtered = select_prefix(manifest, entries, 0);
    GLIFI_REQUIRE(filtered.source_count == baseline.source_count);
    GLIFI_REQUIRE(filtered.live_bytes == baseline.live_bytes);
    GLIFI_REQUIRE(filtered.estimated_reclaimed_segments == baseline.estimated_reclaimed_segments);

    // Worker 1 has one sealed Segment of its own; Worker 0's references do not count for it.
    const auto worker_one =
        glifistore::select_durable_compaction_prefix(manifest, glifistore::WorkerId{1}, entries, 0);
    GLIFI_REQUIRE(worker_one.has_value());
    GLIFI_REQUIRE(worker_one->source_count == 1);
    GLIFI_REQUIRE(worker_one->live_bytes == glifistore::kMaxNormalRecordSize);
    GLIFI_REQUIRE(worker_one->estimated_reclaimed_segments == 0);

    // A Worker without sealed Segments has no prefix.
    auto without_sealed = manifest;
    std::erase_if(without_sealed.segments, [](const glifistore::ManifestSegmentEntry& entry) {
        return entry.owner_worker == glifistore::WorkerId{0} &&
               entry.role == glifistore::ManifestSegmentRole::sealed;
    });
    GLIFI_REQUIRE(select_prefix(without_sealed, entries, 0).source_count == 0);
}

GLIFI_TEST("durable compaction blob profile selection keeps the complete sealed set") {
    auto manifest = prefix_manifest();
    manifest.format_version = 2;
    constexpr std::uint64_t kSmall = 1024;

    // The v1 policy would stop at k=3 here (see the heavy-tail case above); the blob profile never does.
    const auto heavy_tail =
        live_entries(manifest, glifistore::WorkerId{0}, {kSmall, kSmall, kSmall, kSegmentPayloadBytes});
    const auto whole = select_prefix(manifest, heavy_tail, 0);
    GLIFI_REQUIRE(whole.source_count == kWorkerZeroSealedCount);
    GLIFI_REQUIRE(whole.live_bytes == 3U * kSmall + kSegmentPayloadBytes);

    // It is all or nothing under a budget: a budget that cannot admit every Segment admits none.
    GLIFI_REQUIRE(select_prefix(manifest, heavy_tail, whole.live_bytes).source_count ==
                  kWorkerZeroSealedCount);
    GLIFI_REQUIRE(select_prefix(manifest, heavy_tail, whole.live_bytes - 1U).source_count == 0);
    GLIFI_REQUIRE(select_prefix(manifest, heavy_tail, 3U * kSmall).source_count == 0);
}

GLIFI_TEST("durable compaction prefix entries keep only references inside the selected sealed prefix") {
    const auto manifest = prefix_manifest();
    const auto keep = [](const std::vector<glifistore::IndexEntry>& entries) {
        std::vector<std::string> keys;
        keys.reserve(entries.size());
        for (const auto& entry : entries) {
            keys.push_back(entry.key);
        }
        std::ranges::sort(keys);
        return keys;
    };
    std::uint64_t next_sequence{1};
    const auto reference = [&](const std::uint64_t segment_id, const std::uint32_t generation,
                               std::string key) {
        return glifistore::IndexEntry{.key = std::move(key),
                                      .record = {.segment_id = glifistore::SegmentId{segment_id},
                                                 .offset = glifistore::RecordOffset{4096},
                                                 .size = glifistore::RecordSize{64},
                                                 .sequence = glifistore::SequenceNumber{next_sequence++},
                                                 .generation = glifistore::GenerationId{generation}}};
    };
    // Worker 0 sealed IDs/generations: 1/2, 3/4, 5/1, 6/3. The rest is outside every prefix.
    const std::vector<glifistore::IndexEntry> snapshot{
        reference(1, 2, "in-1"),    reference(3, 4, "in-3"),    reference(5, 1, "in-5"),
        reference(6, 3, "in-6"),    reference(7, 1, "active"),  reference(2, 1, "other-worker"),
        reference(1, 1, "stale-1"), reference(3, 5, "stale-3"), reference(99, 1, "unknown"),
    };

    const std::vector<std::string> first_two{"in-1", "in-3"};
    GLIFI_REQUIRE(keep(glifistore::durable_compaction_prefix_entries(manifest, glifistore::WorkerId{0}, 2,
                                                                     snapshot)) == first_two);
    const std::vector<std::string> first_one{"in-1"};
    GLIFI_REQUIRE(keep(glifistore::durable_compaction_prefix_entries(manifest, glifistore::WorkerId{0}, 1,
                                                                     snapshot)) == first_one);
    const std::vector<std::string> first_three{"in-1", "in-3", "in-5"};
    GLIFI_REQUIRE(keep(glifistore::durable_compaction_prefix_entries(manifest, glifistore::WorkerId{0}, 3,
                                                                     snapshot)) == first_three);
    const std::vector<std::string> whole{"in-1", "in-3", "in-5", "in-6"};
    GLIFI_REQUIRE(keep(glifistore::durable_compaction_prefix_entries(manifest, glifistore::WorkerId{0}, 0,
                                                                     snapshot)) == whole);
    GLIFI_REQUIRE(keep(glifistore::durable_compaction_prefix_entries(manifest, glifistore::WorkerId{0}, 4,
                                                                     snapshot)) == whole);
}
