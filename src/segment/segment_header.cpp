#include "glifistore/segment/segment_header.hpp"

#include "glifistore/core/little_endian.hpp"
#include "glifistore/segment/crc32c.hpp"
#include "glifistore/segment/record.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace glifistore {
namespace {

inline constexpr std::size_t kImmutableChecksumOffset = 52;
inline constexpr std::size_t kCommitChecksumOffset = 48;
inline constexpr std::size_t kChecksumBytes = 4;

using le::get_u16;
using le::get_u32;
using le::get_u64;
using le::put_u16;
using le::put_u32;
using le::put_u64;

auto all_zero(std::span<const std::byte> bytes) -> bool {
    return std::ranges::all_of(bytes, [](std::byte value) { return value == std::byte{0}; });
}

template <std::size_t Size>
auto checksum_with_zeroed_field(std::span<const std::byte> bytes, std::size_t checksum_offset)
    -> std::uint32_t {
    std::array<std::byte, Size> copy{};
    std::memcpy(copy.data(), bytes.data(), Size);
    std::fill_n(copy.begin() + static_cast<std::ptrdiff_t>(checksum_offset), kChecksumBytes, std::byte{0});
    return crc32c(copy);
}

auto validate_identity(const SegmentHeaderIdentity& identity) -> Status {
    if (identity.format_version != 1 && identity.format_version != 2) {
        return fail(ErrorCode::invalid_argument, "unsupported Segment header version");
    }
    if (all_zero(identity.store_id)) {
        return fail(ErrorCode::invalid_argument, "segment store id cannot be all zero");
    }
    if (identity.segment_id.value == 0 || identity.generation.value == 0) {
        return fail(ErrorCode::invalid_argument, "segment id and generation must be non-zero");
    }
    return {};
}

auto validate_commit(const SegmentCommit& commit) -> Status {
    if (commit.commit_generation == 0) {
        return fail(ErrorCode::invalid_argument, "commit generation must be non-zero");
    }
    if (commit.committed_end < kSegmentHeaderReservedBytes || commit.committed_end > kSegmentSizeBytes ||
        commit.committed_end % kRecordAlignment != 0) {
        return fail(ErrorCode::invalid_argument, "committed segment extent is invalid");
    }
    const auto state = static_cast<std::uint16_t>(commit.state);
    if (state != static_cast<std::uint16_t>(PersistedSegmentState::active) &&
        state != static_cast<std::uint16_t>(PersistedSegmentState::sealed)) {
        return fail(ErrorCode::invalid_argument, "persisted segment state is invalid");
    }
    if (commit.record_count == 0) {
        if (commit.committed_end != kSegmentHeaderReservedBytes || commit.first_sequence.value != 0 ||
            commit.last_sequence.value != 0) {
            return fail(ErrorCode::invalid_argument, "empty commit metadata is inconsistent");
        }
    } else if (commit.committed_end == kSegmentHeaderReservedBytes || commit.first_sequence.value == 0 ||
               commit.last_sequence.value < commit.first_sequence.value) {
        return fail(ErrorCode::invalid_argument, "non-empty commit metadata is inconsistent");
    }
    return {};
}

auto invalid_slot() -> DecodedCommitSlot {
    return {.validity = CommitSlotValidity::invalid, .commit = std::nullopt};
}

} // namespace

auto encode_segment_commit_slot(const std::span<std::byte> out, const SegmentCommit& commit) -> Status {
    if (out.size() < kSegmentCommitSlotBytes) {
        return fail(ErrorCode::invalid_argument, "commit slot buffer is too small");
    }
    if (auto valid = validate_commit(commit); !valid) {
        return unexpected(valid.error());
    }

    std::array<std::byte, kSegmentCommitSlotBytes> encoded{};
    put_u32(encoded, 0, kSegmentCommitMagic);
    put_u16(encoded, 4, kSegmentCommitFormatVersion);
    put_u16(encoded, 6, static_cast<std::uint16_t>(kSegmentCommitSlotBytes));
    put_u64(encoded, 8, commit.commit_generation);
    put_u32(encoded, 16, commit.committed_end);
    put_u16(encoded, 20, static_cast<std::uint16_t>(commit.state));
    put_u16(encoded, 22, 0);
    put_u64(encoded, 24, commit.record_count);
    put_u64(encoded, 32, commit.first_sequence.value);
    put_u64(encoded, 40, commit.last_sequence.value);
    put_u32(encoded, kCommitChecksumOffset,
            checksum_with_zeroed_field<kSegmentCommitSlotBytes>(encoded, kCommitChecksumOffset));
    std::memcpy(out.data(), encoded.data(), encoded.size());
    return {};
}

auto decode_segment_commit_slot(const std::span<const std::byte> bytes) -> Result<DecodedCommitSlot> {
    if (bytes.size() < kSegmentCommitSlotBytes) {
        return fail(ErrorCode::invalid_record, "commit slot is truncated");
    }
    const auto encoded = bytes.first(kSegmentCommitSlotBytes);
    if (all_zero(encoded)) {
        return DecodedCommitSlot{};
    }
    if (get_u32(encoded, 0) != kSegmentCommitMagic || get_u16(encoded, 6) != kSegmentCommitSlotBytes) {
        return invalid_slot();
    }
    if (get_u32(encoded, kCommitChecksumOffset) !=
        checksum_with_zeroed_field<kSegmentCommitSlotBytes>(encoded, kCommitChecksumOffset)) {
        return invalid_slot();
    }
    if (get_u16(encoded, 4) != kSegmentCommitFormatVersion) {
        return fail(ErrorCode::invalid_record, "unsupported commit slot format version");
    }
    if (get_u16(encoded, 22) != 0 || !all_zero(encoded.subspan(52))) {
        return invalid_slot();
    }

    SegmentCommit commit{
        .commit_generation = get_u64(encoded, 8),
        .committed_end = get_u32(encoded, 16),
        .state = static_cast<PersistedSegmentState>(get_u16(encoded, 20)),
        .record_count = get_u64(encoded, 24),
        .first_sequence = SequenceNumber{get_u64(encoded, 32)},
        .last_sequence = SequenceNumber{get_u64(encoded, 40)},
    };
    if (auto valid = validate_commit(commit); !valid) {
        return invalid_slot();
    }
    return DecodedCommitSlot{.validity = CommitSlotValidity::valid, .commit = commit};
}

auto encode_segment_header(const std::span<std::byte> out, const SegmentHeader& header) -> Status {
    if (out.size() < kSegmentHeaderReservedBytes) {
        return fail(ErrorCode::invalid_argument, "segment header buffer is too small");
    }
    if (auto valid = validate_identity(header.identity); !valid) {
        return unexpected(valid.error());
    }
    if (header.commits[0] && header.commits[1]) {
        const auto first = header.commits[0].value_or(SegmentCommit{});
        const auto second = header.commits[1].value_or(SegmentCommit{});
        if (first.commit_generation == second.commit_generation && first != second) {
            return fail(ErrorCode::invalid_argument, "equal commit generations contain different metadata");
        }
    }

    std::array<std::byte, kSegmentHeaderReservedBytes> encoded{};
    put_u32(encoded, 0, kSegmentHeaderMagic);
    put_u16(encoded, 4, header.identity.format_version);
    put_u16(encoded, 6, static_cast<std::uint16_t>(kSegmentImmutableHeaderBytes));
    put_u16(encoded, 8, kSegmentFormatVersion);
    put_u16(encoded, 10, header.identity.format_version);
    put_u32(encoded, 12, static_cast<std::uint32_t>(kSegmentHeaderReservedBytes));
    std::memcpy(encoded.data() + 16, header.identity.store_id.data(), header.identity.store_id.size());
    put_u64(encoded, 32, header.identity.segment_id.value);
    put_u32(encoded, 40, header.identity.generation.value);
    put_u32(encoded, 44, header.identity.owner_worker.value);
    put_u32(encoded, 48, 0);
    put_u32(encoded, kImmutableChecksumOffset,
            checksum_with_zeroed_field<kSegmentImmutableHeaderBytes>(encoded, kImmutableChecksumOffset));

    for (std::size_t index = 0; index < header.commits.size(); ++index) {
        if (!header.commits[index]) {
            continue;
        }
        const auto offset = kSegmentCommitSlotsOffset + index * kSegmentCommitSlotBytes;
        const auto commit = header.commits[index].value_or(SegmentCommit{});
        if (auto status = encode_segment_commit_slot(
                std::span<std::byte>{encoded}.subspan(offset, kSegmentCommitSlotBytes), commit);
            !status) {
            return unexpected(status.error());
        }
    }
    std::memcpy(out.data(), encoded.data(), encoded.size());
    return {};
}

auto decode_segment_header(const std::span<const std::byte> bytes) -> Result<DecodedSegmentHeader> {
    if (bytes.size() < kSegmentHeaderReservedBytes) {
        return fail(ErrorCode::invalid_record, "segment header is truncated");
    }
    const auto encoded = bytes.first(kSegmentHeaderReservedBytes);
    if (get_u32(encoded, 0) != kSegmentHeaderMagic ||
        (get_u16(encoded, 4) != 1 && get_u16(encoded, 4) != 2) ||
        get_u16(encoded, 6) != kSegmentImmutableHeaderBytes || get_u16(encoded, 8) != kSegmentFormatVersion ||
        get_u16(encoded, 10) != get_u16(encoded, 4) || get_u32(encoded, 12) != kSegmentHeaderReservedBytes ||
        get_u32(encoded, 48) != 0 || !all_zero(encoded.subspan(56, kSegmentImmutableHeaderBytes - 56)) ||
        !all_zero(
            encoded.subspan(kSegmentCommitSlotsOffset + kSegmentCommitSlotCount * kSegmentCommitSlotBytes))) {
        return fail(ErrorCode::invalid_record, "segment header fields or reserved bytes are invalid");
    }
    if (get_u32(encoded, kImmutableChecksumOffset) !=
        checksum_with_zeroed_field<kSegmentImmutableHeaderBytes>(encoded, kImmutableChecksumOffset)) {
        return fail(ErrorCode::checksum_mismatch, "segment identity checksum mismatch");
    }

    SegmentHeaderIdentity identity{.format_version = get_u16(encoded, 4)};
    std::memcpy(identity.store_id.data(), encoded.data() + 16, identity.store_id.size());
    identity.segment_id = SegmentId{get_u64(encoded, 32)};
    identity.generation = GenerationId{get_u32(encoded, 40)};
    identity.owner_worker = WorkerId{get_u32(encoded, 44)};
    if (auto valid = validate_identity(identity); !valid) {
        return fail(ErrorCode::invalid_record, "segment identity values are invalid");
    }

    DecodedSegmentHeader header{.identity = identity};
    for (std::size_t index = 0; index < header.slots.size(); ++index) {
        const auto offset = kSegmentCommitSlotsOffset + index * kSegmentCommitSlotBytes;
        auto slot = decode_segment_commit_slot(encoded.subspan(offset, kSegmentCommitSlotBytes));
        if (!slot) {
            return unexpected(slot.error());
        }
        header.slots[index] = *slot;
    }
    return header;
}

auto select_newest_segment_commit(const DecodedSegmentHeader& header) -> Result<SelectedSegmentCommit> {
    std::optional<SelectedSegmentCommit> selected;
    for (std::size_t index = 0; index < header.slots.size(); ++index) {
        const auto& slot = header.slots[index];
        if (slot.validity != CommitSlotValidity::valid || !slot.commit) {
            continue;
        }
        if (!selected || slot.commit->commit_generation > selected->commit.commit_generation) {
            selected = SelectedSegmentCommit{.slot_index = index, .commit = *slot.commit};
            continue;
        }
        if (slot.commit->commit_generation == selected->commit.commit_generation &&
            *slot.commit != selected->commit) {
            return fail(ErrorCode::corrupted_data, "equal commit generations contain conflicting metadata");
        }
    }
    if (!selected) {
        return fail(ErrorCode::corrupted_data, "segment header has no valid commit slot");
    }
    return *selected;
}

} // namespace glifistore
