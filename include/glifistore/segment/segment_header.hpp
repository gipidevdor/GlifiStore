/**
 * @file
 * @brief Immutable Segment identity and independently validated alternating commit slots.
 */
#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/core/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace glifistore {

inline constexpr std::uint32_t kSegmentHeaderMagic = 0x48594C47U;
inline constexpr std::uint32_t kSegmentCommitMagic = 0x43594C47U;
inline constexpr std::uint16_t kSegmentHeaderFormatVersion = 1;
inline constexpr std::uint16_t kSegmentFormatVersion = 1;
inline constexpr std::uint16_t kSegmentCommitFormatVersion = 1;
inline constexpr std::size_t kSegmentImmutableHeaderBytes = 128;
inline constexpr std::size_t kSegmentCommitSlotBytes = 128;
inline constexpr std::size_t kSegmentCommitSlotCount = 2;
inline constexpr std::size_t kSegmentCommitSlotsOffset = kSegmentImmutableHeaderBytes;

using StoreId = std::array<std::byte, 16>;

/**
 * @brief Encoded active/sealed authority; values are part of the Segment format.
 */
enum class PersistedSegmentState : std::uint16_t { active = 1, sealed = 2 };

/**
 * @brief Immutable Store/Segment/owner identity encoded independently of commit slots.
 */
struct SegmentHeaderIdentity {
    StoreId store_id{};
    SegmentId segment_id{};
    GenerationId generation{};
    WorkerId owner_worker{};
    std::uint16_t format_version{1};

    auto operator<=>(const SegmentHeaderIdentity&) const = default;
};

/**
 * @brief Committed extent, state, sequence range, and monotonic commit-slot generation.
 */
struct SegmentCommit {
    std::uint64_t commit_generation{};
    std::uint32_t committed_end{};
    PersistedSegmentState state{PersistedSegmentState::active};
    std::uint64_t record_count{};
    SequenceNumber first_sequence{};
    SequenceNumber last_sequence{};

    auto operator<=>(const SegmentCommit&) const = default;
};

/**
 * @brief Keep an absent slot distinct from a present invalid slot during authority selection.
 */
enum class CommitSlotValidity { empty, valid, invalid };

/**
 * @brief Independent slot validity and optional decoded commit; an invalid slot is not an empty one.
 */
struct DecodedCommitSlot {
    CommitSlotValidity validity{CommitSlotValidity::empty};
    std::optional<SegmentCommit> commit{};
};

/**
 * @brief Encoding input for immutable identity and the two optional commit authorities.
 */
struct SegmentHeader {
    SegmentHeaderIdentity identity;
    std::array<std::optional<SegmentCommit>, kSegmentCommitSlotCount> commits;
};

/**
 * @brief Decoded identity and independent validation results for both commit slots.
 */
struct DecodedSegmentHeader {
    SegmentHeaderIdentity identity{};
    std::array<DecodedCommitSlot, kSegmentCommitSlotCount> slots{};
};

/**
 * @brief Selected slot index and its owning decoded commit metadata.
 */
struct SelectedSegmentCommit {
    std::size_t slot_index{};
    SegmentCommit commit;

    auto operator==(const SelectedSegmentCommit&) const -> bool = default;
};

/**
 * @brief Validate and explicitly encode canonical little-endian bytes; C++ object layout is never persisted.
 */
[[nodiscard]] auto encode_segment_commit_slot(std::span<std::byte> out, const SegmentCommit& commit)
    -> Status;
/**
 * @brief Validate encoded size, identity/version, and integrity before returning decoded metadata; input is
 * not retained.
 */
[[nodiscard]] auto decode_segment_commit_slot(std::span<const std::byte> bytes) -> Result<DecodedCommitSlot>;
/**
 * @brief Validate and explicitly encode canonical little-endian bytes; C++ object layout is never persisted.
 */
[[nodiscard]] auto encode_segment_header(std::span<std::byte> out, const SegmentHeader& header) -> Status;
/**
 * @brief Validate encoded size, identity/version, and integrity before returning decoded metadata; input is
 * not retained.
 */
[[nodiscard]] auto decode_segment_header(std::span<const std::byte> bytes) -> Result<DecodedSegmentHeader>;
/**
 * @brief Select the newest valid commit slot; reject missing or contradictory authority rather than guessing.
 */
[[nodiscard]] auto select_newest_segment_commit(const DecodedSegmentHeader& header)
    -> Result<SelectedSegmentCommit>;

} // namespace glifistore
