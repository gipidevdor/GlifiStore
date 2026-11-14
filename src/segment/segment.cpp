#include "glifistore/segment/segment.hpp"

#include "glifistore/core/checked_math.hpp"
#include "glifistore/core/hot_path_phases.hpp"

#include <cstring>
#include <utility>

namespace glifistore {

Segment::Segment(SegmentId id, WorkerId owner, GenerationId generation)
    : id_(id), owner_(owner), generation_(generation),
      storage_(std::make_unique_for_overwrite<std::byte[]>(kSegmentSizeBytes)) {
    std::memset(storage_.get(), 0, kSegmentHeaderReservedBytes);
}

Segment::~Segment() = default;
Segment::Segment(Segment&&) noexcept = default;
auto Segment::operator=(Segment&&) noexcept -> Segment& = default;

auto Segment::append(const RecordInput& input) -> Result<RecordRef> {
    if (state_ != SegmentState::active) {
        return fail(ErrorCode::segment_sealed, "append requires an active segment");
    }
    const auto encoded_size = encoded_record_size(input);
    if (!encoded_size) {
        return unexpected(encoded_size.error());
    }
    if (!range_contains(kSegmentSizeBytes, write_offset_, *encoded_size)) {
        return fail(ErrorCode::segment_full, "record does not fit in active segment");
    }
    const auto offset = write_offset_;
    const auto destination =
        std::span<std::byte>{storage_.get() + offset, static_cast<std::size_t>(*encoded_size)};
    {
        GS_PHASE_PUT(encode_copy);
        if (auto encoded = encode_record(destination, input, *encoded_size); !encoded) {
            return unexpected(encoded.error());
        }
    }
    write_offset_ += *encoded_size;
    stats_.used_bytes = write_offset_;
    ++stats_.record_count;
    if (stats_.record_count == 1) {
        stats_.first_sequence = input.sequence;
    }
    stats_.last_sequence = input.sequence;
    return RecordRef{
        .segment_id = id_,
        .offset = RecordOffset{static_cast<std::uint32_t>(offset)},
        .size = RecordSize{static_cast<std::uint32_t>(*encoded_size)},
        .sequence = input.sequence,
        .generation = generation_,
    };
}

auto Segment::validate_ref_extent(const RecordRef& ref) const -> Status {
    if (ref.segment_id != id_ || ref.generation != generation_) {
        return fail(ErrorCode::invalid_reference, "record reference targets another segment generation");
    }
    if (ref.offset.value < kSegmentHeaderReservedBytes ||
        !range_contains(static_cast<std::uint32_t>(stats_.used_bytes), ref.offset.value, ref.size.value)) {
        return fail(ErrorCode::invalid_reference, "record reference lies outside written segment bytes");
    }
    return {};
}

auto Segment::read(const RecordRef& ref) const -> Result<RecordView> {
    if (auto valid = validate_ref_extent(ref); !valid) {
        return unexpected(valid.error());
    }
    auto decoded =
        decode_record(std::span<const std::byte>{storage_.get() + ref.offset.value, ref.size.value}, true);
    if (!decoded) {
        return unexpected(decoded.error());
    }
    if (decoded->sequence != ref.sequence || decoded->encoded_size != ref.size.value) {
        return fail(ErrorCode::invalid_reference, "record reference metadata does not match encoded record");
    }
    return decoded;
}

auto Segment::read_trusted(const RecordRef& ref) const -> Result<RecordView> {
    if (!is_trusted()) {
        return fail(ErrorCode::invalid_argument, "trusted read requires a resident trusted segment");
    }
    if (auto valid = validate_ref_extent(ref); !valid) {
        return unexpected(valid.error());
    }
    auto decoded =
        decode_record(std::span<const std::byte>{storage_.get() + ref.offset.value, ref.size.value}, false);
    if (!decoded) {
        return unexpected(decoded.error());
    }
    if (decoded->sequence != ref.sequence || decoded->encoded_size != ref.size.value) {
        return fail(ErrorCode::invalid_reference, "record reference metadata does not match encoded record");
    }
    return decoded;
}

auto Segment::is_trusted() const noexcept -> bool {
    return residency_ == ResidencyState::resident && state_ != SegmentState::active;
}

auto Segment::scan() const -> Result<std::vector<RecordRef>> {
    std::vector<RecordRef> refs;
    refs.reserve(static_cast<std::size_t>(stats_.record_count));
    std::size_t offset = kSegmentHeaderReservedBytes;
    while (offset < stats_.used_bytes) {
        auto decoded =
            decode_record(std::span<const std::byte>{storage_.get() + offset, stats_.used_bytes - offset});
        if (!decoded) {
            return unexpected(decoded.error());
        }
        refs.push_back(RecordRef{
            .segment_id = id_,
            .offset = RecordOffset{static_cast<std::uint32_t>(offset)},
            .size = RecordSize{decoded->encoded_size},
            .sequence = decoded->sequence,
            .generation = generation_,
        });
        offset += decoded->encoded_size;
    }
    if (refs.size() != stats_.record_count) {
        return fail(ErrorCode::corrupted_data, "segment record count does not match scan result");
    }
    return refs;
}

auto Segment::seal() -> Status {
    if (state_ != SegmentState::active) {
        return fail(ErrorCode::invalid_argument, "only an active segment can be sealed");
    }
    state_ = SegmentState::sealed;
    return {};
}

auto Segment::mark_live(const RecordRef& ref) -> Status {
    if (auto valid = validate_ref_extent(ref); !valid) {
        return valid;
    }
    ++stats_.live_records;
    stats_.live_bytes += ref.size.value;
    return {};
}

auto Segment::mark_dead(const RecordRef& ref) -> Status {
    if (auto valid = validate_ref_extent(ref); !valid) {
        return valid;
    }
    if (stats_.live_records == 0 || stats_.live_bytes < ref.size.value) {
        return fail(ErrorCode::invalid_argument, "segment liveness accounting underflow");
    }
    --stats_.live_records;
    stats_.live_bytes -= ref.size.value;
    return {};
}

auto Segment::retire() -> Status {
    if (state_ != SegmentState::sealed || stats_.live_records != 0) {
        return fail(ErrorCode::invalid_argument, "only a sealed segment with zero live records can retire");
    }
    state_ = SegmentState::retired;
    return {};
}

} // namespace glifistore
