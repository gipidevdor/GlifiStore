/**
 * @file
 * @brief Strong identifiers and physical Record references, independent of encoded layouts.
 */
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace glifistore {

inline constexpr std::size_t kSegmentSizeBytes = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kSegmentHeaderReservedBytes = 4096;
inline constexpr std::size_t kRecordAlignment = 8;
inline constexpr std::size_t kMaxNormalRecordSize = 1ULL * 1024ULL * 1024ULL;

/**
 * @brief Compile-time tag separation for identifiers sharing the same integer representation. This is not a
 * serialized layout.
 */
template <typename Tag, typename T> struct StrongId {
    T value{};
    auto operator<=>(const StrongId&) const = default;
};

struct SegmentIdTag;
struct WorkerIdTag;
struct SequenceNumberTag;
struct GenerationIdTag;
struct RecordOffsetTag;
struct RecordSizeTag;

using SegmentId = StrongId<SegmentIdTag, std::uint64_t>;
using WorkerId = StrongId<WorkerIdTag, std::uint32_t>;
using SequenceNumber = StrongId<SequenceNumberTag, std::uint64_t>;
using GenerationId = StrongId<GenerationIdTag, std::uint32_t>;
using RecordOffset = StrongId<RecordOffsetTag, std::uint32_t>;
using RecordSize = StrongId<RecordSizeTag, std::uint32_t>;

/**
 * @brief Physical Record identity including Segment generation, extent, and sequence. It does not pin backing
 * storage.
 */
struct RecordRef {
    SegmentId segment_id{};
    RecordOffset offset{};
    RecordSize size{};
    SequenceNumber sequence{};
    GenerationId generation{};

    auto operator<=>(const RecordRef&) const = default;
};

} // namespace glifistore

namespace std {

/**
 * @brief Hash a StrongId by its underlying integer while retaining type separation at the call site.
 */
template <typename Tag, typename T> struct hash<glifistore::StrongId<Tag, T>> {
    auto operator()(const glifistore::StrongId<Tag, T>& id) const noexcept -> std::size_t {
        return hash<T>{}(id.value);
    }
};

} // namespace std
