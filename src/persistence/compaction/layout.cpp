#include "glifistore/persistence/compaction.hpp"
#include "glifistore/segment/record.hpp"
#include "glifistore/segment/segment_header.hpp"

#include <limits>

namespace glifistore {

auto DurableCompactionLayout::add_record(const std::uint32_t encoded_size)
    -> Result<DurableCompactionPlacement> {
    constexpr auto kPayloadBytes =
        static_cast<std::uint32_t>(kSegmentSizeBytes - kSegmentHeaderReservedBytes);
    if (encoded_size < kEncodedRecordHeaderSize || encoded_size > kMaxNormalRecordSize ||
        encoded_size % kRecordAlignment != 0 || encoded_size > kPayloadBytes) {
        return fail(ErrorCode::invalid_record,
                    "durable compaction Record extent cannot fit a v1 Segment payload");
    }
    if (encoded_size > std::numeric_limits<std::uint64_t>::max() - encoded_bytes_) {
        return fail(ErrorCode::arithmetic_overflow, "durable compaction live byte count overflows uint64_t");
    }
    if (segment_count_ == 0 || encoded_size > kPayloadBytes - current_payload_bytes_) {
        if (segment_count_ == std::numeric_limits<std::size_t>::max()) {
            return fail(ErrorCode::arithmetic_overflow,
                        "durable compaction output Segment count overflows size_t");
        }
        ++segment_count_;
        current_payload_bytes_ = 0;
    }
    const DurableCompactionPlacement placement{
        .segment_index = segment_count_ - 1U,
        .offset =
            RecordOffset{static_cast<std::uint32_t>(kSegmentHeaderReservedBytes) + current_payload_bytes_},
    };
    current_payload_bytes_ += encoded_size;
    encoded_bytes_ += encoded_size;
    return placement;
}

auto durable_compaction_output_segments(const std::uint64_t live_encoded_bytes) -> Result<std::size_t> {
    static_assert(kSegmentSizeBytes > kSegmentHeaderReservedBytes);
    constexpr auto kPayloadBytes =
        static_cast<std::uint64_t>(kSegmentSizeBytes - kSegmentHeaderReservedBytes);
    if (live_encoded_bytes == 0) {
        return std::size_t{0};
    }
    const auto quotient = live_encoded_bytes / kPayloadBytes;
    const auto remainder = live_encoded_bytes % kPayloadBytes;
    if (quotient > std::numeric_limits<std::size_t>::max() - (remainder == 0 ? 0U : 1U)) {
        return fail(ErrorCode::arithmetic_overflow,
                    "durable compaction output Segment count overflows size_t");
    }
    return static_cast<std::size_t>(quotient) + (remainder == 0 ? 0U : 1U);
}

} // namespace glifistore
