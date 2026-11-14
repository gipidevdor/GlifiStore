#include "glifistore/segment/segment.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > 4096) {
        return 0;
    }
    glifistore::Segment segment{glifistore::SegmentId{1}};
    const auto bytes = std::span<const std::byte>{reinterpret_cast<const std::byte*>(data), size};
    auto ref = segment.append({
        .sequence = glifistore::SequenceNumber{1},
        .key = bytes.first(size / 2),
        .value = bytes.subspan(size / 2),
    });
    if (ref && size != 0) {
        const auto mutation = static_cast<std::size_t>(data[0]) % ref->size.value;
        segment.mutable_base()[ref->offset.value + mutation] ^= std::byte{0xFF};
    }
    static_cast<void>(segment.scan());
    return 0;
}
