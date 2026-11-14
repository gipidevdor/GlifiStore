#pragma once

// Shared oracle for strict sealed-prefix compaction tests (ADR 0044): a history of PUT, overwrite, ERASE and
// TTL operations spread over the sealed Segments of one Worker plus its active Segment, a sequential model of
// what every key must show, and the live-byte accounting that makes a copy budget select an exact prefix.
// The runtime integration tests and the process-kill crash scenario describe the same Store with it.

#include "glifistore/core/key_hash.hpp"
#include "glifistore/segment/record.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace glifistore::test::compaction_prefix {

// The Store is recovered at kOpenNowNs, so a TTL put expiring at kExpiresAtNs is still an Index-resident
// live reference. Compaction runs at kCompactNowNs, when that put has expired, and recovery verifies there.
inline constexpr std::uint64_t kOpenNowNs = 40;
inline constexpr std::uint64_t kCompactNowNs = 100;
inline constexpr std::uint64_t kExpiresAtNs = 50;
// Expired already when the Store is recovered, so recovery never indexes it.
inline constexpr std::uint64_t kAlreadyExpiredNs = 30;

struct HistoryOperation {
    std::size_t segment{};
    Opcode opcode{Opcode::put};
    std::string key;
    std::string value;
    std::uint64_t expire_at_ns{};
};

// Segments 0 .. sealed_count - 1 are sealed in Manifest order and the last Segment is the active one.
// Operation i carries sequence i + 1, so operations must be listed by non-decreasing Segment index.
struct History {
    explicit History(const std::size_t sealed_segments) : sealed_count(sealed_segments) {}

    std::size_t sealed_count{};
    std::vector<HistoryOperation> operations;
};

using Model = std::map<std::string, std::optional<std::string>>;

inline void add_put(History& history, const std::size_t segment, std::string key, std::string value,
                    const std::uint64_t expire_at_ns = 0) {
    history.operations.push_back({.segment = segment,
                                  .opcode = Opcode::put,
                                  .key = std::move(key),
                                  .value = std::move(value),
                                  .expire_at_ns = expire_at_ns});
}

inline void add_erase(History& history, const std::size_t segment, std::string key) {
    history.operations.push_back(
        {.segment = segment, .opcode = Opcode::erase, .key = std::move(key), .value = {}, .expire_at_ns = 0});
}

[[nodiscard]] inline auto text_bytes(const std::string_view text) -> std::span<const std::byte> {
    return std::as_bytes(std::span{text});
}

// The visible value of every key at now_ns: the newest Record wins, and a tombstone or an expired put hides
// every older Record of the key.
[[nodiscard]] inline auto model_at(const History& history, const std::uint64_t now_ns) -> Model {
    Model model;
    for (const auto& operation : history.operations) {
        const bool hidden = operation.opcode == Opcode::erase ||
                            (operation.expire_at_ns != 0 && operation.expire_at_ns <= now_ns);
        model[operation.key] = hidden ? std::nullopt : std::optional<std::string>{operation.value};
    }
    return model;
}

[[nodiscard]] inline auto record_bytes(const HistoryOperation& operation) -> std::uint64_t {
    const auto size = encoded_record_size({
        .sequence = SequenceNumber{1},
        .opcode = operation.opcode,
        .type = ValueType::bytes,
        .flags = 0,
        .key_hash = hash_key(operation.key),
        .expire_at_ns = operation.expire_at_ns,
        .key = text_bytes(operation.key),
        .value = text_bytes(operation.value),
    });
    if (!size) {
        throw std::runtime_error("cannot size a history Record: " + size.error().message);
    }
    return *size;
}

// Bytes of the Records that the recovered Index references, per Segment. Recovery at open_now_ns omits
// expired puts and tombstoned keys, while a put that expires only later still counts as live.
[[nodiscard]] inline auto live_bytes_per_segment(const History& history, const std::uint64_t open_now_ns)
    -> std::vector<std::uint64_t> {
    struct Winner {
        std::size_t segment{};
        std::uint64_t bytes{};
    };

    std::map<std::string, std::optional<Winner>> latest;
    for (const auto& operation : history.operations) {
        const bool hidden = operation.opcode == Opcode::erase ||
                            (operation.expire_at_ns != 0 && operation.expire_at_ns <= open_now_ns);
        if (hidden) {
            latest[operation.key] = std::nullopt;
            continue;
        }
        latest[operation.key] = Winner{.segment = operation.segment, .bytes = record_bytes(operation)};
    }
    std::vector<std::uint64_t> bytes(history.sealed_count + 1U);
    for (const auto& [key, winner] : latest) {
        if (winner) {
            bytes[winner->segment] += winner->bytes;
        }
    }
    return bytes;
}

// The copy budget that admits exactly the first prefix_length sealed Segments' live bytes (inclusive).
[[nodiscard]] inline auto prefix_budget(const std::vector<std::uint64_t>& live_bytes,
                                        const std::size_t prefix_length) -> std::uint64_t {
    std::uint64_t budget{};
    for (std::size_t index = 0; index < prefix_length; ++index) {
        budget += live_bytes[index];
    }
    return budget;
}

// The closure cases of ADR 0044 section 1, with the oldest two of three sealed Segments as the prefix.
// Tombstones and expired puts sit inside the prefix (S1, S2) and after it (S3); older puts of the same
// keys sit inside it. None of them may reappear when the prefix is retired.
[[nodiscard]] inline auto closure_history() -> History {
    History history{3};
    // S1: inside the prefix.
    add_put(history, 0, "overwritten", "v1");
    add_put(history, 0, "erased_inside", "v1");
    add_put(history, 0, "erased_after", "v1");
    add_put(history, 0, "ttl_inside", "v1", kExpiresAtNs);
    add_put(history, 0, "ttl_masked", "v1");
    add_put(history, 0, "kept", "v1");
    add_put(history, 0, "superseded_active", "v1");
    add_put(history, 0, "overwritten_after", "v1");
    // S2: inside the prefix.
    add_put(history, 1, "overwritten", "v2");
    add_erase(history, 1, "erased_inside");
    add_put(history, 1, "ttl_masked", "v2", kExpiresAtNs);
    add_put(history, 1, "restored", "v1");
    add_erase(history, 1, "restored");
    add_put(history, 1, "restored", "v3");
    // S3: retained after the prefix.
    add_erase(history, 2, "erased_after");
    add_put(history, 2, "after_ttl", "v1", kExpiresAtNs);
    add_put(history, 2, "retained_live", "v1");
    add_put(history, 2, "overwritten_after", "v2");
    // S4: the active Segment.
    add_put(history, 3, "superseded_active", "v2");
    add_put(history, 3, "active_only", "v1");
    return history;
}

} // namespace glifistore::test::compaction_prefix
