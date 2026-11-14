#include "glifistore/index/index.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > 4096) {
        return 0;
    }
    auto segment = std::make_shared<glifistore::Segment>(glifistore::SegmentId{1});
    const auto input = std::span<const std::byte>{reinterpret_cast<const std::byte*>(data), size};
    std::uint64_t sequence{1};
    for (std::size_t offset = 0; offset < size && sequence <= 64; ++sequence) {
        const auto remaining = size - offset;
        const auto key_size = std::min<std::size_t>(remaining, 1U + data[offset] % 32U);
        const auto key = input.subspan(offset, key_size);
        offset += key_size;
        const auto value_size = std::min<std::size_t>(size - offset, 16U);
        const auto value = input.subspan(offset, value_size);
        offset += value_size;
        const auto opcode = data[offset == 0 ? 0 : offset - 1] % 5U == 0U ? glifistore::Opcode::erase
                                                                          : glifistore::Opcode::put;
        static_cast<void>(segment->append({
            .sequence = glifistore::SequenceNumber{sequence},
            .opcode = opcode,
            .key = key,
            .value = opcode == glifistore::Opcode::erase ? std::span<const std::byte>{} : value,
        }));
    }
    const std::vector<glifistore::SegmentPtr> segments{segment};
    static_cast<void>(glifistore::rebuild_index_from_segments(segments));
    return 0;
}
