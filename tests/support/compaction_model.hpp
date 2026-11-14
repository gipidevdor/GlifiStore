#pragma once

// Executable reference of the durable-compaction mathematical model
// (docs/architecture/durable-compaction-model.md). Pure functions only: no file, no clock, no runtime.
//
// The unit tests prove the model's theorems by exhaustion over a stated finite scope. The integration tests
// then compare the real runtime with these same functions over the same scope, so an implementation that
// disagrees with the model fails, and a model that disagrees with the implementation fails as well.
//
// Section numbers in the comments refer to the model document.

#include "glifistore/core/types.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace glifistore::test::compaction_model {

// ---------------------------------------------------------------------------------------------------------
// Section 2: time, Records and the decision function.
// ---------------------------------------------------------------------------------------------------------

/// Bytes of Record payload that one Segment holds (the payload of a v1 Segment).
inline constexpr std::uint64_t kPayloadBytes = kSegmentSizeBytes - kSegmentHeaderReservedBytes;

/// t_o: the time at which the Store is recovered, so the time at which the Index is built.
inline constexpr std::uint64_t kOpenNs = 40;
/// t_c: the time at which a compaction transaction decides which expired puts to drop.
inline constexpr std::uint64_t kCompactNs = 100;
/// Times at which a key is read. Ascending, because the runtime reclaims expired entries lazily and a later
/// read at an earlier time would no longer see them (assumption A1: the clock never goes backwards).
inline constexpr std::array<std::uint64_t, 5> kProbeNs{40, 99, 100, 101, 200};

/// The five kinds of Record that matter to a key's decision. A TTL put is classified by where its expiry
/// falls relative to t_o and t_c; the equality cases (expiry == t_o and expiry == t_c) are included on purpose.
enum class Kind : std::uint8_t {
    put,                  ///< no expiry
    erase,                ///< tombstone
    ttl_at_open,          ///< expiry == t_o: hidden when the Store is recovered, so never indexed
    ttl_by_compaction,    ///< expiry == t_c: indexed at recovery, hidden when the transaction decides
    ttl_after_compaction, ///< expiry == t_c + 1: still visible to the transaction, hidden one tick later
};
inline constexpr std::array<Kind, 5> kAllKinds{Kind::put, Kind::erase, Kind::ttl_at_open,
                                               Kind::ttl_by_compaction, Kind::ttl_after_compaction};

[[nodiscard]] constexpr auto expiry_ns(const Kind kind) noexcept -> std::uint64_t {
    switch (kind) {
    case Kind::ttl_at_open:
        return kOpenNs;
    case Kind::ttl_by_compaction:
        return kCompactNs;
    case Kind::ttl_after_compaction:
        return kCompactNs + 1U;
    case Kind::put:
    case Kind::erase:
        return 0;
    }
    return 0;
}

/// hidden(r, t): a tombstone, or a put whose expiry has been reached. Time 0 is "no clock" and expires
/// nothing, exactly as RecordView::expired.
[[nodiscard]] constexpr auto hidden_at(const Kind kind, const std::uint64_t now_ns) noexcept -> bool {
    if (kind == Kind::erase) {
        return true;
    }
    const auto expires = expiry_ns(kind);
    return expires != 0 && now_ns != 0 && expires <= now_ns;
}

/// The history of one key: its Records in sequence order, each placed in a Segment. Segment indices
/// 0 .. n-1 are the sealed Segments in Manifest order and n is the active one. Sequence ranges of Segments
/// are ordered (invariant O), so a key's Records have non-decreasing Segment indices.
struct Op {
    std::uint8_t segment{};
    Kind kind{Kind::put};

    friend auto operator==(const Op&, const Op&) -> bool = default;
};

using Pattern = std::vector<Op>;

/// A readable form of a history for failure messages, for example "put@S0 erase@S2".
[[nodiscard]] inline auto describe(const Pattern& pattern) -> std::string {
    static constexpr std::array<const char*, 5> kNames{"put", "erase", "ttl@open", "ttl@compaction", "ttl+1"};
    std::string text;
    for (const auto& op : pattern) {
        text +=
            std::string{kNames[static_cast<std::size_t>(op.kind)]} + "@S" + std::to_string(op.segment) + ' ';
    }
    return text.empty() ? std::string{"(empty)"} : text;
}

/// D(key, t): a key is visible when its newest Record is a put that has not expired.
[[nodiscard]] inline auto visible_at(const Pattern& pattern, const std::uint64_t now_ns) -> bool {
    return !pattern.empty() && !hidden_at(pattern.back().kind, now_ns);
}

/// Calls visit(pattern) for every pattern of at most max_length Records over segment_count Segments, the
/// empty pattern included, each exactly once. The count is the sum over l <= max_length of
/// C(l + segment_count - 1, l) * 5^l.
template <typename Visitor>
void for_each_pattern(const std::size_t segment_count, const std::size_t max_length, Visitor&& visit) {
    Pattern pattern;
    const auto extend = [&](const auto& self, const std::uint8_t first_segment) -> void {
        visit(static_cast<const Pattern&>(pattern));
        if (pattern.size() == max_length) {
            return;
        }
        for (auto segment = first_segment; segment < segment_count; ++segment) {
            for (const auto kind : kAllKinds) {
                pattern.push_back({segment, kind});
                self(self, segment);
                pattern.pop_back();
            }
        }
    };
    extend(extend, 0);
}

/// Closed form of the number of patterns for_each_pattern visits.
[[nodiscard]] constexpr auto pattern_count(const std::size_t segment_count, const std::size_t max_length)
    -> std::uint64_t {
    std::uint64_t total{};
    std::uint64_t kinds_power = 1;
    for (std::size_t length = 0; length <= max_length; ++length) {
        // C(length + segment_count - 1, length), computed multiplicatively without leaving the integers.
        std::uint64_t combinations = 1;
        for (std::size_t step = 1; step <= length; ++step) {
            combinations = combinations * (segment_count - 1U + step) / step;
        }
        total += combinations * kinds_power;
        kinds_power *= kAllKinds.size();
    }
    return total;
}

/// The Index at recovery time holds exactly the keys that are visible at t_o (section 2, definition of I_0).
[[nodiscard]] inline auto recovered_index_contains(const Pattern& pattern) -> bool {
    return visible_at(pattern, kOpenNs);
}

// ---------------------------------------------------------------------------------------------------------
// Section 3: the compaction operator.
// ---------------------------------------------------------------------------------------------------------

/// The sealed Segments first .. last-1 are rewritten. A prefix has first == 0 (ADR 0044); any other range
/// is only used to show that the prefix is necessary (Theorem 1b).
struct Range {
    std::size_t first{};
    std::size_t last{};
};

[[nodiscard]] constexpr auto prefix(const std::size_t length) noexcept -> Range {
    return {.first = 0, .last = length};
}

/// What the transaction does with one key (Sel, Drop, Cp of section 3).
struct Classification {
    bool selected{}; ///< indexed, and its newest Record lies in the range
    bool dropped{};  ///< selected and expired when the transaction decides: removed, not copied
    bool copied{};   ///< selected and not expired: copied into an output Segment unchanged
};

[[nodiscard]] inline auto classify(const Pattern& pattern, const Range range, const bool indexed,
                                   const std::uint64_t compact_ns = kCompactNs) -> Classification {
    Classification result;
    if (pattern.empty() || !indexed) {
        return result;
    }
    const auto& winner = pattern.back();
    result.selected = winner.segment >= range.first && winner.segment < range.last;
    result.dropped = result.selected && hidden_at(winner.kind, compact_ns);
    result.copied = result.selected && !result.dropped;
    return result;
}

/// A Record that survives the transaction. `origin` names the Record of the original pattern that it is, or
/// is a copy of; a copy keeps its sequence, kind and expiry.
struct Survivor {
    std::size_t origin{};
    Kind kind{Kind::put};
};

/// The key's history after the transaction, in sequence order: the Records before the range, the copy of its
/// newest Record when it was copied, then the Records after the range. Every other Record of the range is
/// dropped.
[[nodiscard]] inline auto rewrite(const Pattern& pattern, const Range range, const bool indexed,
                                  const std::uint64_t compact_ns = kCompactNs) -> std::vector<Survivor> {
    const auto classification = classify(pattern, range, indexed, compact_ns);
    std::vector<Survivor> after;
    bool copy_placed = !classification.copied;
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        const auto segment = pattern[index].segment;
        if (segment >= range.first && !copy_placed) {
            after.push_back({pattern.size() - 1U, pattern.back().kind});
            copy_placed = true;
        }
        if (segment < range.first || segment >= range.last) {
            after.push_back({index, pattern[index].kind});
        }
    }
    if (!copy_placed) {
        after.push_back({pattern.size() - 1U, pattern.back().kind});
    }
    return after;
}

[[nodiscard]] inline auto visible_after(const std::vector<Survivor>& after, const std::uint64_t now_ns)
    -> bool {
    return !after.empty() && !hidden_at(after.back().kind, now_ns);
}

/// Theorem 1 for one key, at one time: the decision, and the Record that decides, are unchanged.
[[nodiscard]] inline auto agrees_at(const Pattern& pattern, const std::vector<Survivor>& after,
                                    const std::uint64_t now_ns) -> bool {
    const bool before = visible_at(pattern, now_ns);
    if (before != visible_after(after, now_ns)) {
        return false;
    }
    return !before || after.back().origin == pattern.size() - 1U;
}

/// Theorem 1 for one key: agreement at every probe time that is not earlier than the transaction.
[[nodiscard]] inline auto closed_under(const Pattern& pattern, const Range range, const bool indexed)
    -> bool {
    const auto after = rewrite(pattern, range, indexed);
    return std::ranges::all_of(kProbeNs, [&](const std::uint64_t now_ns) {
        return now_ns < kCompactNs || agrees_at(pattern, after, now_ns);
    });
}

// ---------------------------------------------------------------------------------------------------------
// Section 5: foreground mutations that run during the build.
// ---------------------------------------------------------------------------------------------------------

/// What a client does to one key between the snapshot and the commit of the transaction. All of it is newer
/// than every existing Record and lands in the active Segment.
enum class Foreground : std::uint8_t { none, put, erase, erase_then_put };
inline constexpr std::array<Foreground, 4> kAllForeground{Foreground::none, Foreground::put,
                                                          Foreground::erase, Foreground::erase_then_put};

[[nodiscard]] constexpr auto touches(const Foreground foreground) noexcept -> bool {
    return foreground != Foreground::none;
}

[[nodiscard]] inline auto foreground_kinds(const Foreground foreground) -> std::vector<Kind> {
    switch (foreground) {
    case Foreground::none:
        return {};
    case Foreground::put:
        return {Kind::put};
    case Foreground::erase:
        return {Kind::erase};
    case Foreground::erase_then_put:
        return {Kind::erase, Kind::put};
    }
    return {};
}

/// The key's history with the foreground operations appended in the active Segment.
[[nodiscard]] inline auto with_foreground(Pattern pattern, const Foreground foreground,
                                          const std::uint8_t active_segment) -> Pattern {
    for (const auto kind : foreground_kinds(foreground)) {
        pattern.push_back({active_segment, kind});
    }
    return pattern;
}

/// The transaction applied to a key that a foreground operation touched: the foreground Records are newer
/// than everything, so they survive unchanged after the rewritten range and after the copy. Their origins
/// continue the numbering of with_foreground(pattern, foreground, ...).
[[nodiscard]] inline auto rewrite_with_foreground(const Pattern& pattern, const Range range,
                                                  const bool indexed, const Foreground foreground)
    -> std::vector<Survivor> {
    auto after = rewrite(pattern, range, indexed);
    std::size_t origin = pattern.size();
    for (const auto kind : foreground_kinds(foreground)) {
        after.push_back({origin++, kind});
    }
    return after;
}

/// The history after a transaction as a pattern again, so that a second transaction can be applied to it.
/// `origins[i]` is the Record of the original pattern that Record i is, or is a copy of.
struct Renumbered {
    Pattern pattern;
    std::vector<std::size_t> origins;
};

/// The `outputs` Segments replace the range, so a Segment that was after the range moves down by
/// (range.last - range.first) and up by `outputs`. A copy needs an output Segment (outputs >= 1) and lands in
/// the first one.
[[nodiscard]] inline auto renumber(const Pattern& pattern, const Range range, const bool indexed,
                                   const std::size_t outputs) -> Renumbered {
    const auto classification = classify(pattern, range, indexed);
    Renumbered after;
    bool copy_placed = !classification.copied;
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        const auto& op = pattern[index];
        if (op.segment >= range.first && !copy_placed) {
            after.pattern.push_back({static_cast<std::uint8_t>(range.first), pattern.back().kind});
            after.origins.push_back(pattern.size() - 1U);
            copy_placed = true;
        }
        if (op.segment < range.first) {
            after.pattern.push_back(op);
            after.origins.push_back(index);
        } else if (op.segment >= range.last) {
            after.pattern.push_back(
                {static_cast<std::uint8_t>(op.segment - (range.last - range.first) + outputs), op.kind});
            after.origins.push_back(index);
        }
    }
    return after;
}

// ---------------------------------------------------------------------------------------------------------
// Section 6: the prefix choice.
// ---------------------------------------------------------------------------------------------------------

struct Selection {
    std::size_t source_count{};      ///< k, or 0 when no prefix is admitted
    std::uint64_t live_bytes{};      ///< L_k
    std::size_t estimated_reclaim{}; ///< rho(k) when it is positive, else 0

    friend auto operator==(const Selection&, const Selection&) -> bool = default;
};

/// ceil(bytes / payload): the fewest output Segments that can hold `bytes` of Records.
[[nodiscard]] constexpr auto aggregate_outputs(const std::uint64_t bytes) noexcept -> std::size_t {
    return static_cast<std::size_t>(bytes / kPayloadBytes + (bytes % kPayloadBytes == 0 ? 0U : 1U));
}

/// The ADR 0044 policy, written straight from its definition rather than as the scan the runtime uses.
/// live[i] is the Index-referenced Record bytes of sealed Segment i; budget 0 means unbounded.
///
///   L_k  = live[0] + ... + live[k-1]            (cumulative live bytes)
///   rho  = k - ceil(L_k / payload)              (Segments freed, estimated)
///   A    = { k : budget == 0 or L_k <= budget } (admitted prefixes)
///
/// When some admitted k has rho >= 1, the choice is the lexicographic optimum: the largest rho and, among
/// those, the smallest k (hence the least copy work, since L_k does not decrease). Otherwise the longest
/// admitted prefix is returned with no estimated gain, so that the exact layout decides. The blob profile
/// admits only the complete sealed set.
[[nodiscard]] inline auto reference_selection(const std::span<const std::uint64_t> live,
                                              const std::uint64_t budget, const bool blob_profile)
    -> Selection {
    const auto n = live.size();
    std::vector<std::uint64_t> cumulative(n + 1U);
    for (std::size_t index = 0; index < n; ++index) {
        cumulative[index + 1U] = cumulative[index] + live[index];
    }
    const auto admitted = [&](const std::size_t k) { return budget == 0 || cumulative[k] <= budget; };
    if (blob_profile) {
        return n != 0 && admitted(n) ? Selection{.source_count = n, .live_bytes = cumulative[n]}
                                     : Selection{};
    }
    Selection best;
    Selection longest;
    for (std::size_t k = 1; k <= n; ++k) {
        if (!admitted(k)) {
            continue;
        }
        longest = {.source_count = k, .live_bytes = cumulative[k], .estimated_reclaim = 0};
        const auto outputs = aggregate_outputs(cumulative[k]);
        if (outputs >= k) {
            continue;
        }
        const auto reclaim = k - outputs;
        if (reclaim > best.estimated_reclaim) {
            best = {.source_count = k, .live_bytes = cumulative[k], .estimated_reclaim = reclaim};
        }
    }
    return best.source_count != 0 ? best : longest;
}

/// Next-fit placement of Records of the given encoded sizes, in sequence order, into Segments of
/// kPayloadBytes. A Record never spans two Segments. Returns the number of Segments used.
[[nodiscard]] inline auto next_fit_segments(const std::span<const std::uint64_t> sizes) -> std::size_t {
    std::size_t segments{};
    std::uint64_t used{};
    for (const auto size : sizes) {
        if (segments == 0 || size > kPayloadBytes - used) {
            ++segments;
            used = 0;
        }
        used += size;
    }
    return segments;
}

// ---------------------------------------------------------------------------------------------------------
// Section 8: the recovery function on the namespace lattice.
// ---------------------------------------------------------------------------------------------------------

enum class AuthorityChoice : std::uint8_t { old_manifest, next_manifest };
enum class OutputName : std::uint8_t { absent, final_name, temporary };

/// The persistent state of a transaction that stopped anywhere: which Manifest is authoritative, whether the
/// intent exists, which source Segments still exist under their final names, and which replacement
/// Segments exist under their final or temporary names. Retained and active Segments are not part of the
/// lattice: they are listed by both authorities and are never removed.
struct NamespaceState {
    AuthorityChoice authority{AuthorityChoice::old_manifest};
    bool intent{};
    std::vector<bool> sources{};
    std::vector<OutputName> outputs{};

    friend auto operator==(const NamespaceState&, const NamespaceState&) -> bool = default;
};

/// The namespace after a successful recovery. `intent` is always false and temporaries never remain.
struct RecoveredState {
    AuthorityChoice authority{AuthorityChoice::old_manifest};
    std::vector<bool> sources{};
    std::vector<OutputName> outputs{};

    friend auto operator==(const RecoveredState&, const RecoveredState&) -> bool = default;
};

/// R: the recovery decision table of the persistent protocol as a total function. nullopt means fail closed:
/// recovery returns an error and removes nothing. The authority is chosen only by Manifest equality, and the
/// obsolete names are the other authority's.
///
///   old  : needs every source;           needs no final replacement unless an intent explains it
///   next : needs every replacement final; needs no source unless an intent explains it
///
/// With an intent the obsolete names are removed and the intent last; without one, recognized stale
/// temporaries are removed and nothing else is tolerated.
[[nodiscard]] inline auto recover(const NamespaceState& state) -> std::optional<RecoveredState> {
    const auto present = [](const bool value) { return value; };
    const bool all_sources = std::ranges::all_of(state.sources, present);
    const bool no_sources = std::ranges::none_of(state.sources, present);
    const bool all_final = std::ranges::all_of(
        state.outputs, [](const OutputName name) { return name == OutputName::final_name; });
    const bool no_final = std::ranges::none_of(
        state.outputs, [](const OutputName name) { return name == OutputName::final_name; });

    if (state.authority == AuthorityChoice::old_manifest) {
        if (!all_sources || (!state.intent && !no_final)) {
            return std::nullopt;
        }
        return RecoveredState{.authority = AuthorityChoice::old_manifest,
                              .sources = state.sources,
                              .outputs = std::vector<OutputName>(state.outputs.size(), OutputName::absent)};
    }
    if (!all_final || (!state.intent && !no_sources)) {
        return std::nullopt;
    }
    return RecoveredState{.authority = AuthorityChoice::next_manifest,
                          .sources = std::vector<bool>(state.sources.size(), false),
                          .outputs = state.outputs};
}

} // namespace glifistore::test::compaction_model
