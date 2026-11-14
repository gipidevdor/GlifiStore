// The mathematical model of durable compaction (docs/architecture/durable-compaction-model.md), checked by
// exhaustion. Each test names the theorem it proves and the finite scope it exhausts; the written proofs cover
// the unbounded statement. Nothing here touches a file, a clock or the runtime: the model is pure, so these
// tests establish that the claims are true of the model itself. The integration tests then establish that the
// runtime is a refinement of it.
#include "glifistore/core/types.hpp"
#include "glifistore/persistence/compaction.hpp"
#include "glifistore/persistence/compaction_builder.hpp"
#include "support/compaction_model.hpp"
#include "test.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace model = glifistore::test::compaction_model;

[[noreturn]] void counterexample(const std::string& theorem, const model::Pattern& pattern,
                                 const std::string& detail) {
    throw std::runtime_error(theorem + " fails for [" + model::describe(pattern) + "] " + detail);
}

// Every Index state that the model allows for a key (Index invariant J, section 2): a tombstone is never
// indexed, a put that is visible when the Store is recovered always is, and a put that had already expired
// at recovery may or may not still be (a lazy reclaim removes it only on a later read).
auto legal_index_states(const model::Pattern& pattern) -> std::vector<bool> {
    if (pattern.empty() || pattern.back().kind == model::Kind::erase) {
        return {false};
    }
    return model::recovered_index_contains(pattern) ? std::vector<bool>{true}
                                                    : std::vector<bool>{false, true};
}

} // namespace

GLIFI_TEST("model: the history enumerator visits exactly the closed-form number of histories") {
    for (std::size_t segments = 1; segments <= 5; ++segments) {
        for (std::size_t length = 0; length <= 5; ++length) {
            std::uint64_t visited{};
            model::for_each_pattern(segments, length, [&](const model::Pattern& pattern) {
                ++visited;
                GLIFI_REQUIRE(pattern.size() <= length);
                GLIFI_REQUIRE(std::ranges::is_sorted(pattern, {}, &model::Op::segment));
            });
            GLIFI_REQUIRE(visited == model::pattern_count(segments, length));
        }
    }
    // The scope of the closure theorem below: four sealed Segments and the active one, up to five Records.
    GLIFI_REQUIRE(model::pattern_count(5, 5) == 442'276);
}

GLIFI_TEST("model theorem 1: every oldest-prefix transaction preserves the decision of every key") {
    // Scope: all 442,276 histories of at most five Records over four sealed Segments and the active one,
    // every prefix length k = 1..4, every Index the model allows, every probe time at or after t_c.
    std::uint64_t checked{};
    model::for_each_pattern(5, 5, [&](const model::Pattern& pattern) {
        if (pattern.empty()) {
            return;
        }
        for (const bool indexed : legal_index_states(pattern)) {
            for (std::size_t k = 1; k <= 4; ++k) {
                if (!model::closed_under(pattern, model::prefix(k), indexed)) {
                    counterexample("theorem 1", pattern, "for prefix " + std::to_string(k));
                }
                ++checked;
            }
        }
    });
    // 442,275 non-empty histories, 530,730 (history, Index) pairs, four prefixes each.
    GLIFI_REQUIRE(checked == 2'122'920);
}

GLIFI_TEST("model theorem 1b: a range of Segments is safe only if it is a prefix") {
    // Theorem 1 holds for every prefix (above). For every other contiguous range [a, b) with a >= 1 it fails,
    // and the smallest counterexample is the one ADR 0044 names: an older put outside the range and a
    // tombstone inside it. Dropping the tombstone resurrects the put.
    for (std::size_t first = 1; first <= 3; ++first) {
        for (std::size_t last = first + 1; last <= 4; ++last) {
            std::optional<model::Pattern> smallest;
            model::for_each_pattern(5, 3, [&](const model::Pattern& pattern) {
                if (pattern.empty()) {
                    return;
                }
                for (const bool indexed : legal_index_states(pattern)) {
                    if (!model::closed_under(pattern, {.first = first, .last = last}, indexed) &&
                        (!smallest || pattern.size() < smallest->size())) {
                        smallest = pattern;
                    }
                }
            });
            GLIFI_REQUIRE(smallest.has_value());
            GLIFI_REQUIRE(smallest->size() == 2);
            GLIFI_REQUIRE((*smallest)[0].segment < first && (*smallest)[1].segment >= first &&
                          (*smallest)[1].segment < last);
        }
    }
    const model::Pattern resurrection{{0, model::Kind::put}, {2, model::Kind::erase}};
    GLIFI_REQUIRE(!model::closed_under(resurrection, {.first = 2, .last = 3}, false));
    GLIFI_REQUIRE(model::closed_under(resurrection, model::prefix(3), false));
}

GLIFI_TEST("model theorem 1c: the hypotheses of theorem 1 are tight") {
    // (a) Time. Before the transaction's own time an expired-by-then put is still visible, but the
    // transaction dropped it: agreement is only claimed for t >= t_c (assumption A1).
    const model::Pattern expiring{{1, model::Kind::ttl_by_compaction}};
    const auto after_expiring = model::rewrite(expiring, model::prefix(2), true);
    GLIFI_REQUIRE(model::agrees_at(expiring, after_expiring, model::kCompactNs));
    GLIFI_REQUIRE(model::agrees_at(expiring, after_expiring, 200));
    GLIFI_REQUIRE(!model::agrees_at(expiring, after_expiring, model::kCompactNs - 1U));

    // (b) Index. A visible newest Record inside the prefix that the Index does not reference is not copied,
    // so the key is lost: the Index must reference every visible key (J2).
    const model::Pattern visible{{0, model::Kind::put}};
    GLIFI_REQUIRE(model::closed_under(visible, model::prefix(1), true));
    GLIFI_REQUIRE(!model::closed_under(visible, model::prefix(1), false));
    std::uint64_t lost{};
    model::for_each_pattern(4, 3, [&](const model::Pattern& pattern) {
        if (pattern.empty() || !model::visible_at(pattern, model::kCompactNs)) {
            return;
        }
        const auto& newest = pattern.back();
        if (newest.segment < 2 && !model::closed_under(pattern, model::prefix(2), false)) {
            ++lost;
        }
    });
    GLIFI_REQUIRE(lost > 0);

    // (c) Order. Two Records whose sequence order disagrees with their Segment order (a tombstone of
    // sequence 5 in the first Segment, a put of sequence 3 in the second) break O: the transaction over the
    // first Segment drops the tombstone and the put reappears. In order, the same two are safe.
    const model::Pattern disordered{{1, model::Kind::put}, {0, model::Kind::erase}};
    GLIFI_REQUIRE(!model::closed_under(disordered, model::prefix(1), false));
    const model::Pattern ordered{{0, model::Kind::put}, {1, model::Kind::erase}};
    GLIFI_REQUIRE(model::closed_under(ordered, model::prefix(1), false));

    // (d) An Index entry for a hidden newest Record is harmless: the key is hidden either way.
    for (const auto kind : {model::Kind::erase, model::Kind::ttl_at_open, model::Kind::ttl_by_compaction}) {
        const model::Pattern hidden_key{{0, model::Kind::put}, {1, kind}};
        GLIFI_REQUIRE(model::closed_under(hidden_key, model::prefix(2), true));
        GLIFI_REQUIRE(model::closed_under(hidden_key, model::prefix(2), false));
    }
}

GLIFI_TEST("model theorem 2: foreground mutations during the build commute with the transaction") {
    // Scope: every history of at most four Records over three sealed Segments and the active one, every
    // foreground behavior on the key, every prefix, every legal Index. The foreground Records are newest, so
    // they decide the key in both histories; the remap leaves exactly the touched keys' entries alone.
    constexpr std::uint8_t kActive = 3;
    std::uint64_t checked{};
    std::uint64_t superseded{};
    std::uint64_t remapped{};
    model::for_each_pattern(4, 4, [&](const model::Pattern& pattern) {
        if (pattern.empty()) {
            return;
        }
        for (const bool indexed : legal_index_states(pattern)) {
            for (std::size_t k = 1; k <= 3; ++k) {
                for (const auto foreground : model::kAllForeground) {
                    const auto before = model::with_foreground(pattern, foreground, kActive);
                    const auto after =
                        model::rewrite_with_foreground(pattern, model::prefix(k), indexed, foreground);
                    for (const auto now_ns : model::kProbeNs) {
                        if (now_ns >= model::kCompactNs && !model::agrees_at(before, after, now_ns)) {
                            counterexample("theorem 2", before, "for prefix " + std::to_string(k));
                        }
                    }
                    const auto classification = model::classify(pattern, model::prefix(k), indexed);
                    superseded += classification.copied && model::touches(foreground) ? 1U : 0U;
                    remapped += classification.copied && !model::touches(foreground) ? 1U : 0U;
                    ++checked;
                }
            }
        }
    });
    // 24,645 non-empty histories, 29,574 (history, Index) pairs, three prefixes, four foreground behaviors.
    GLIFI_REQUIRE(checked == 354'888);
    // Both outcomes of the conditional remap occur in the scope.
    GLIFI_REQUIRE(superseded > 0 && remapped > 0);
}

GLIFI_TEST("model corollary: transactions compose") {
    // Scope: every history of at most four Records over four sealed Segments and the active one, a first
    // transaction at t_c with every prefix and output count, then a second one at t_c + 1 with every prefix.
    // The second transaction sees the Index that the first one leaves: the copied keys retargeted, the dropped
    // keys removed. The pair preserves the decision of every key from t_c + 1 on.
    constexpr std::uint64_t kSecondNs = model::kCompactNs + 1U;
    std::uint64_t pairs{};
    model::for_each_pattern(5, 4, [&](const model::Pattern& pattern) {
        if (pattern.empty()) {
            return;
        }
        for (const bool indexed : legal_index_states(pattern)) {
            for (std::size_t k1 = 1; k1 <= 3; ++k1) {
                const auto first = model::classify(pattern, model::prefix(k1), indexed);
                for (const std::size_t outputs : {std::size_t{first.copied ? 1U : 0U}, std::size_t{1}}) {
                    const auto middle = model::renumber(pattern, model::prefix(k1), indexed, outputs);
                    const bool indexed_after = indexed && !first.dropped;
                    const std::size_t sealed_after = 4 - k1 + outputs;
                    for (std::size_t k2 = 1; k2 <= sealed_after; ++k2) {
                        const auto after =
                            model::rewrite(middle.pattern, model::prefix(k2), indexed_after, kSecondNs);
                        for (const auto now_ns : {kSecondNs, std::uint64_t{150}, std::uint64_t{200}}) {
                            const bool before_visible = model::visible_at(pattern, now_ns);
                            const bool same_decision = before_visible == model::visible_after(after, now_ns);
                            // The deciding Record is the original newest one, through both rewrites.
                            const bool same_record =
                                !before_visible || middle.origins[after.back().origin] == pattern.size() - 1U;
                            if (!same_decision || !same_record) {
                                counterexample("composition", pattern,
                                               "for prefixes " + std::to_string(k1) + " then " +
                                                   std::to_string(k2));
                            }
                        }
                        ++pairs;
                    }
                }
            }
        }
    });
    GLIFI_REQUIRE(pairs == 879'562);
}

GLIFI_TEST("model theorem 3: canonical transitions are in bijection with (k, m) and derive inverts them") {
    // The next Manifest of a transaction is a function of (k, m) alone, injective in it, and the validator
    // accepts exactly its image. Part 1 proves injectivity and inversion for one to seven sealed Segments.
    // Part 2 proves that nothing else is accepted, over every Manifest reachable from the old one by one of
    // four edits per entry (keep, drop, bump the generation by one, bump it by two).
    const auto make_manifest = [](const std::size_t sealed_count, const std::uint16_t format_version) {
        glifistore::Manifest manifest{.store_id = {std::byte{0x66}},
                                      .manifest_generation = 20,
                                      .worker_count = 2,
                                      .routing_epoch = 1,
                                      .format_version = format_version};
        for (std::size_t index = 1; index <= sealed_count; ++index) {
            manifest.segments.push_back(
                {.segment_id = glifistore::SegmentId{2U * index - 1U},
                 .generation = glifistore::GenerationId{static_cast<std::uint32_t>(index) + 1U},
                 .owner_worker = glifistore::WorkerId{0},
                 .role = glifistore::ManifestSegmentRole::sealed});
            manifest.segments.push_back({.segment_id = glifistore::SegmentId{2U * index},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{1},
                                         .role = glifistore::ManifestSegmentRole::sealed});
        }
        const auto next_id = 2U * sealed_count + 1U;
        manifest.segments.push_back({.segment_id = glifistore::SegmentId{next_id},
                                     .generation = glifistore::GenerationId{1},
                                     .owner_worker = glifistore::WorkerId{1},
                                     .role = glifistore::ManifestSegmentRole::active});
        manifest.segments.push_back({.segment_id = glifistore::SegmentId{next_id + 1U},
                                     .generation = glifistore::GenerationId{1},
                                     .owner_worker = glifistore::WorkerId{0},
                                     .role = glifistore::ManifestSegmentRole::active});
        manifest.next_segment_id = glifistore::SegmentId{next_id + 2U};
        return manifest;
    };
    const glifistore::WorkerId worker{0};

    for (std::size_t sealed = 1; sealed <= 7; ++sealed) {
        const auto old = make_manifest(sealed, 1);
        std::vector<glifistore::Manifest> images;
        for (std::size_t k = 1; k <= sealed; ++k) {
            for (std::size_t m = 0; m <= k; ++m) {
                const glifistore::DurableCompactionShape shape{.source_count = k, .replacement_count = m};
                const auto next = glifistore::canonical_durable_compaction_manifest(old, worker, shape);
                GLIFI_REQUIRE(next.has_value());
                const auto derived = glifistore::derive_durable_compaction_shape(old, *next, worker);
                GLIFI_REQUIRE(derived.has_value() && *derived == shape);
                GLIFI_REQUIRE(std::ranges::find(images, *next) == images.end());
                images.push_back(*next);
            }
        }
        GLIFI_REQUIRE(images.size() == (sealed + 1U) * (sealed + 2U) / 2U - 1U);
    }

    for (const std::uint16_t format_version : {std::uint16_t{1}, std::uint16_t{2}}) {
        constexpr std::size_t kSealed = 3;
        const auto old = make_manifest(kSealed, format_version);
        const std::size_t entries = old.segments.size();
        std::size_t accepted{};
        std::vector<glifistore::DurableCompactionShape> shapes;
        std::size_t combinations = 1;
        for (std::size_t index = 0; index < entries; ++index) {
            combinations *= 4;
        }
        for (std::size_t code = 0; code < combinations; ++code) {
            auto next = old;
            ++next.manifest_generation;
            next.segments.clear();
            std::size_t digits = code;
            for (std::size_t index = 0; index < entries; ++index, digits /= 4) {
                auto entry = old.segments[index];
                switch (digits % 4) {
                case 0:
                    break;
                case 1:
                    continue; // drop
                case 2:
                    entry.generation.value += 1;
                    break;
                default:
                    entry.generation.value += 2;
                    break;
                }
                next.segments.push_back(entry);
            }
            const auto shape = glifistore::derive_durable_compaction_shape(old, next, worker);
            if (!shape) {
                continue;
            }
            ++accepted;
            // Whatever is accepted is exactly the canonical transition of the shape it reports.
            const auto canonical = glifistore::canonical_durable_compaction_manifest(old, worker, *shape);
            GLIFI_REQUIRE(canonical.has_value() && *canonical == next);
            shapes.push_back(*shape);
        }
        // Version 1 admits every (k, m); the blob profile only the complete sealed set.
        const std::size_t expected =
            format_version == 1 ? (kSealed + 1U) * (kSealed + 2U) / 2U - 1U : kSealed + 1U;
        GLIFI_REQUIRE(accepted == expected);
        if (format_version == 2) {
            GLIFI_REQUIRE(std::ranges::all_of(shapes, [](const glifistore::DurableCompactionShape& shape) {
                return shape.source_count == kSealed;
            }));
        }
        // Metadata that must not change: one deviation at a time on every canonical transition.
        for (const auto& shape : shapes) {
            const auto canonical = glifistore::canonical_durable_compaction_manifest(old, worker, shape);
            GLIFI_REQUIRE(canonical.has_value());
            for (std::size_t deviation = 0; deviation < 5; ++deviation) {
                auto next = *canonical;
                switch (deviation) {
                case 0:
                    ++next.manifest_generation;
                    break;
                case 1:
                    --next.manifest_generation;
                    break;
                case 2:
                    ++next.next_segment_id.value;
                    break;
                case 3:
                    ++next.routing_epoch;
                    break;
                default:
                    next.worker_hash_seed += 1;
                    break;
                }
                GLIFI_REQUIRE(!glifistore::derive_durable_compaction_shape(old, next, worker).has_value());
            }
        }
    }
}

namespace {

// Index entries that put `live_bytes[i]` of Records in the i-th sealed Segment of Worker 0.
auto synthetic_entries(const glifistore::Manifest& manifest, const std::vector<std::uint64_t>& live_bytes)
    -> std::vector<glifistore::IndexEntry> {
    std::vector<glifistore::IndexEntry> entries;
    std::size_t sealed_index{};
    std::uint64_t counter{};
    for (const auto& segment : manifest.segments) {
        if (segment.owner_worker != glifistore::WorkerId{0} ||
            segment.role != glifistore::ManifestSegmentRole::sealed) {
            continue;
        }
        auto remaining = live_bytes[sealed_index++];
        while (remaining > 0) {
            const auto size = std::min<std::uint64_t>(remaining, glifistore::kMaxNormalRecordSize);
            ++counter;
            entries.push_back({.key = "r" + std::to_string(counter),
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

auto selection_manifest(const std::size_t sealed_count, const std::uint16_t format_version)
    -> glifistore::Manifest {
    glifistore::Manifest manifest{.store_id = {std::byte{0x67}},
                                  .manifest_generation = 5,
                                  .worker_count = 1,
                                  .routing_epoch = 1,
                                  .format_version = format_version};
    for (std::size_t index = 1; index <= sealed_count + 1U; ++index) {
        manifest.segments.push_back({.segment_id = glifistore::SegmentId{index},
                                     .generation = glifistore::GenerationId{1},
                                     .owner_worker = glifistore::WorkerId{0},
                                     .role = index <= sealed_count
                                                 ? glifistore::ManifestSegmentRole::sealed
                                                 : glifistore::ManifestSegmentRole::active});
    }
    manifest.next_segment_id = glifistore::SegmentId{sealed_count + 2U};
    return manifest;
}

} // namespace

GLIFI_TEST("model theorem 4: the runtime's prefix scan is the lexicographic optimum of its definition") {
    // Scope: one to five sealed Segments, every live-byte vector over {0, 1 KiB, payload/2, payload,
    // 2 payloads} per Segment, and for each the unbounded budget and, around every cumulative live byte count,
    // the budgets one below, equal to and one above it; for both profiles. The last level is more than a real
    // Segment can hold: the definition (rho may be negative, and only rho >= 1 counts as a gain) must still
    // give the runtime's answer.
    // Part 1: the runtime equals the definition, written without the early exit its scan relies on.
    // Part 2: the properties that make the definition sensible.
    constexpr std::array<std::uint64_t, 5> kLevels{0, 1024, model::kPayloadBytes / 2U, model::kPayloadBytes,
                                                   2U * model::kPayloadBytes};
    std::uint64_t compared{};
    for (std::size_t sealed = 1; sealed <= 5; ++sealed) {
        std::size_t vectors = 1;
        for (std::size_t index = 0; index < sealed; ++index) {
            vectors *= kLevels.size();
        }
        for (std::size_t code = 0; code < vectors; ++code) {
            std::vector<std::uint64_t> live(sealed);
            for (std::size_t index = 0, digits = code; index < sealed; ++index, digits /= kLevels.size()) {
                live[index] = kLevels[digits % kLevels.size()];
            }
            std::vector<std::uint64_t> cumulative(sealed + 1U);
            for (std::size_t index = 0; index < sealed; ++index) {
                cumulative[index + 1U] = cumulative[index] + live[index];
            }
            std::vector<std::uint64_t> budgets{0};
            for (std::size_t k = 1; k <= sealed; ++k) {
                for (const auto delta : {std::int64_t{-1}, std::int64_t{0}, std::int64_t{1}}) {
                    const auto budget = static_cast<std::int64_t>(cumulative[k]) + delta;
                    if (budget > 0) {
                        budgets.push_back(static_cast<std::uint64_t>(budget));
                    }
                }
            }
            std::sort(budgets.begin() + 1, budgets.end());
            for (const std::uint16_t format_version : {std::uint16_t{1}, std::uint16_t{2}}) {
                const auto manifest = selection_manifest(sealed, format_version);
                const auto entries = synthetic_entries(manifest, live);
                std::optional<model::Selection> previous;
                std::optional<model::Selection> unbounded;
                for (const auto budget : budgets) {
                    const auto runtime = glifistore::select_durable_compaction_prefix(
                        manifest, glifistore::WorkerId{0}, entries, budget);
                    GLIFI_REQUIRE(runtime.has_value());
                    const auto expected = model::reference_selection(live, budget, format_version == 2);
                    GLIFI_REQUIRE(runtime->source_count == expected.source_count);
                    GLIFI_REQUIRE(runtime->live_bytes == expected.live_bytes);
                    GLIFI_REQUIRE(runtime->estimated_reclaimed_segments == expected.estimated_reclaim);
                    ++compared;

                    if (format_version != 1) {
                        // Blob profile: all or nothing.
                        GLIFI_REQUIRE(expected.source_count == 0 || expected.source_count == sealed);
                        continue;
                    }
                    // P5: the copy work never exceeds the budget.
                    GLIFI_REQUIRE(budget == 0 || expected.live_bytes <= budget);
                    // P1 and P2: when a gain is promised it is the best, and no shorter prefix matches it.
                    if (expected.estimated_reclaim > 0) {
                        for (std::size_t k = 1; k <= sealed; ++k) {
                            if (budget != 0 && cumulative[k] > budget) {
                                continue;
                            }
                            const auto outputs = model::aggregate_outputs(cumulative[k]);
                            const std::size_t reclaim = outputs >= k ? 0U : k - outputs;
                            GLIFI_REQUIRE(reclaim <= expected.estimated_reclaim);
                            if (reclaim == expected.estimated_reclaim) {
                                GLIFI_REQUIRE(k >= expected.source_count);
                                GLIFI_REQUIRE(cumulative[k] >= expected.live_bytes);
                            }
                        }
                    }
                    if (budget == 0) {
                        // P3: an unbounded budget never promises less than the complete sealed set (ADR 0015).
                        const auto outputs = model::aggregate_outputs(cumulative[sealed]);
                        GLIFI_REQUIRE(expected.estimated_reclaim >=
                                      (outputs >= sealed ? 0U : sealed - outputs));
                        unbounded = expected;
                        continue;
                    }
                    // P4: a larger budget never promises less.
                    if (previous) {
                        GLIFI_REQUIRE(expected.estimated_reclaim >= previous->estimated_reclaim);
                    }
                    previous = expected;
                }
                if (format_version == 1 && previous && unbounded) {
                    GLIFI_REQUIRE(unbounded->estimated_reclaim >= previous->estimated_reclaim);
                }
            }
        }
    }
    GLIFI_REQUIRE(compared == 115'240);
}

GLIFI_TEST("model theorem 5: the output count is bounded by the aggregate and by the next-fit layout") {
    // The runtime's exact layout is next-fit. Its Segment count is at least ceil(L / payload) (the aggregate
    // lower bound the selection uses) and at most 1 + L / (payload - R + 1) for Records of at most R bytes,
    // because every closed Segment holds more than payload - R bytes. The estimate is therefore a lower bound
    // for fragmentation and an upper bound for expiry (the Index cannot see expired puts).
    std::mt19937_64 random{0x1A7075ULL};
    constexpr std::uint64_t kMaximum = glifistore::kMaxNormalRecordSize;
    for (std::size_t round = 0; round < 3000; ++round) {
        const auto count = 1U + random() % 400U;
        std::vector<std::uint64_t> sizes;
        std::uint64_t total{};
        glifistore::DurableCompactionLayout layout;
        for (std::size_t index = 0; index < count; ++index) {
            const std::uint64_t size = 8U * (7U + random() % ((kMaximum - 56U) / 8U + 1U));
            sizes.push_back(size);
            total += size;
            GLIFI_REQUIRE(layout.add_record(static_cast<std::uint32_t>(size)).has_value());
        }
        const auto segments = model::next_fit_segments(sizes);
        GLIFI_REQUIRE(layout.segment_count() == segments);
        GLIFI_REQUIRE(layout.encoded_bytes() == total);
        GLIFI_REQUIRE(segments >= model::aggregate_outputs(total));
        GLIFI_REQUIRE(segments <= 1U + total / (model::kPayloadBytes - kMaximum + 1U));
    }
    // A Record never spans two Segments: 127 maximum Records need three Segments, not the aggregate two.
    const std::vector<std::uint64_t> maxima(127, kMaximum);
    GLIFI_REQUIRE(model::aggregate_outputs(127U * kMaximum) == 2);
    GLIFI_REQUIRE(model::next_fit_segments(maxima) == 3);
}

GLIFI_TEST(
    "model theorem 6: the recovery function is total, idempotent and reaches the same state from any crash") {
    // R maps every namespace state of a transaction to a recovered state or to a refusal. Over the whole
    // lattice of up to four sources and four replacements it satisfies:
    //  (a) a recovered state is a fixpoint: recovering it again changes nothing (idempotence);
    //  (b) crash-monotone: removing any of the obsolete names that recovery itself would remove, in any
    //      order, gives a state that recovers to the same result (a crash during recovery is harmless);
    //  (c) the protocol of the transaction only ever produces states that recover, and recover to the
    //      authority that the Manifest names.
    using model::AuthorityChoice;
    using model::NamespaceState;
    using model::OutputName;
    for (std::size_t k = 1; k <= 4; ++k) {
        for (std::size_t m = 0; m <= k; ++m) {
            std::size_t outputs_states = 1;
            for (std::size_t index = 0; index < m; ++index) {
                outputs_states *= 3;
            }
            for (const auto authority : {AuthorityChoice::old_manifest, AuthorityChoice::next_manifest}) {
                for (const bool intent : {false, true}) {
                    for (std::size_t source_bits = 0; source_bits < (1U << k); ++source_bits) {
                        for (std::size_t output_code = 0; output_code < outputs_states; ++output_code) {
                            NamespaceState state{.authority = authority, .intent = intent};
                            for (std::size_t index = 0; index < k; ++index) {
                                state.sources.push_back(((source_bits >> index) & 1U) != 0);
                            }
                            for (std::size_t index = 0, digits = output_code; index < m;
                                 ++index, digits /= 3) {
                                state.outputs.push_back(static_cast<OutputName>(digits % 3));
                            }
                            const auto recovered = model::recover(state);
                            if (!recovered) {
                                continue;
                            }
                            // (a)
                            const NamespaceState settled{.authority = recovered->authority,
                                                         .intent = false,
                                                         .sources = recovered->sources,
                                                         .outputs = recovered->outputs};
                            const auto again = model::recover(settled);
                            GLIFI_REQUIRE(again.has_value() && *again == *recovered);
                            // (b) only meaningful while the intent still names the transaction.
                            if (!intent) {
                                continue;
                            }
                            for (std::size_t mask = 0; mask < (1U << (k + m)); ++mask) {
                                auto partial = state;
                                for (std::size_t index = 0; index < k + m; ++index) {
                                    if (((mask >> index) & 1U) == 0) {
                                        continue;
                                    }
                                    const bool obsolete_source =
                                        index < k && authority == AuthorityChoice::next_manifest;
                                    const bool obsolete_output =
                                        index >= k && authority == AuthorityChoice::old_manifest;
                                    if (obsolete_source) {
                                        partial.sources[index] = false;
                                    } else if (obsolete_output) {
                                        partial.outputs[index - k] = OutputName::absent;
                                    }
                                }
                                const auto partial_result = model::recover(partial);
                                GLIFI_REQUIRE(partial_result.has_value() && *partial_result == *recovered);
                            }
                        }
                    }
                }
            }

            // (c) The protocol, one persistent step at a time: stage every output under a temporary name,
            // write the intent, promote every output, publish the next Manifest, retire every source in
            // order, remove the intent. Every prefix of that sequence, with a crash after each step, recovers.
            std::vector<NamespaceState> protocol;
            NamespaceState state{.authority = AuthorityChoice::old_manifest,
                                 .intent = false,
                                 .sources = std::vector<bool>(k, true),
                                 .outputs = std::vector<OutputName>(m, OutputName::absent)};
            protocol.push_back(state);
            for (std::size_t index = 0; index < m; ++index) {
                state.outputs[index] = OutputName::temporary;
                protocol.push_back(state);
            }
            state.intent = true;
            protocol.push_back(state);
            for (std::size_t index = 0; index < m; ++index) {
                state.outputs[index] = OutputName::final_name;
                protocol.push_back(state);
            }
            state.authority = AuthorityChoice::next_manifest;
            protocol.push_back(state);
            for (std::size_t index = 0; index < k; ++index) {
                state.sources[index] = false;
                protocol.push_back(state);
            }
            state.intent = false;
            protocol.push_back(state);
            const std::size_t manifest_step = 1U + m + 1U + m;
            for (std::size_t step = 0; step < protocol.size(); ++step) {
                const auto recovered = model::recover(protocol[step]);
                GLIFI_REQUIRE(recovered.has_value());
                GLIFI_REQUIRE((recovered->authority == AuthorityChoice::next_manifest) ==
                              (step >= manifest_step));
            }
        }
    }
}
