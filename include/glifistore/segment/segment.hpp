#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/core/types.hpp"
#include "glifistore/segment/record.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace glifistore {

enum class SegmentState : std::uint8_t { free, active, sealed, retired, reclaimable, corrupted };
enum class ResidencyState : std::uint8_t { resident, mapped, evicted, loading, evicting };

struct SegmentStats {
    std::size_t used_bytes{};
    std::size_t live_bytes{};
    std::uint64_t record_count{};
    std::uint64_t live_records{};
    SequenceNumber first_sequence{};
    SequenceNumber last_sequence{};
};

class Segment final {
  public:
    explicit Segment(SegmentId id, WorkerId owner = {}, GenerationId generation = GenerationId{1});
    ~Segment();

    Segment(const Segment&) = delete;
    auto operator=(const Segment&) -> Segment& = delete;
    Segment(Segment&&) noexcept;
    auto operator=(Segment&&) noexcept -> Segment&;

    [[nodiscard]] auto append(const RecordInput& input) -> Result<RecordRef>;
    [[nodiscard]] auto read(const RecordRef& ref) const -> Result<RecordView>;
    // Skips CRC verification. Only valid when resident bytes are immutable (sealed/retired).
    // Not used on Store get paths; callers with mutable segment access must use read().
    [[nodiscard]] auto read_trusted(const RecordRef& ref) const -> Result<RecordView>;
    [[nodiscard]] auto validate_ref_extent(const RecordRef& ref) const -> Status;
    // True for sealed or retired resident segments whose append path is closed.
    [[nodiscard]] auto is_trusted() const noexcept -> bool;
    [[nodiscard]] auto scan() const -> Result<std::vector<RecordRef>>;

    auto seal() -> Status;
    auto mark_live(const RecordRef& ref) -> Status;
    auto mark_dead(const RecordRef& ref) -> Status;
    auto retire() -> Status;

    [[nodiscard]] auto id() const noexcept -> SegmentId {
        return id_;
    }

    [[nodiscard]] auto owner() const noexcept -> WorkerId {
        return owner_;
    }

    [[nodiscard]] auto generation() const noexcept -> GenerationId {
        return generation_;
    }

    [[nodiscard]] auto state() const noexcept -> SegmentState {
        return state_;
    }

    [[nodiscard]] auto residency() const noexcept -> ResidencyState {
        return residency_;
    }

    [[nodiscard]] auto stats() const noexcept -> SegmentStats {
        return stats_;
    }

    [[nodiscard]] auto base() const noexcept -> const std::byte* {
        return storage_.get();
    }

    // Exposed for fuzzing and corruption probes; mutating bytes invalidates CRC on read().
    [[nodiscard]] auto mutable_base() noexcept -> std::byte* {
        return storage_.get();
    }

    [[nodiscard]] auto capacity() const noexcept -> std::size_t {
        return kSegmentSizeBytes;
    }

  private:
    SegmentId id_;
    WorkerId owner_;
    GenerationId generation_;
    SegmentState state_{SegmentState::active};
    ResidencyState residency_{ResidencyState::resident};
    std::unique_ptr<std::byte[]> storage_;
    std::size_t write_offset_{kSegmentHeaderReservedBytes};
    SegmentStats stats_{.used_bytes = kSegmentHeaderReservedBytes};
};

using SegmentPtr = std::shared_ptr<Segment>;

} // namespace glifistore
