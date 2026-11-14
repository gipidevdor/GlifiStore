/**
 * @file
 * @brief Resident, byte-exact memory image of one durable Segment's Record region (ADR 0045/0046).
 *
 * Internal header (not installed). An image is derived state: it holds at every published offset
 * exactly the bytes the Segment file holds there, so a read may decode either source with the same
 * validation. It is never written to disk and never consulted by recovery.
 */
#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/core/memory_reserve.hpp"
#include "glifistore/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace glifistore {

class DurableSegmentFile;

/**
 * @brief One anonymous, lazily touched mapping of kSegmentSizeBytes with Segment-identical offsets.
 *
 * Ownership: shared by every runtime generation of the same Segment file. Only the single Writer
 * that owns the Segment's append position calls mirror(), and only beyond every published Record;
 * readers reach a Record only through a later release/acquire publication of its reference, so
 * they never observe bytes still being written. The mapping and its reserve charge are released
 * when the last generation pin drops.
 */
class SegmentImage final {
  public:
    /**
     * @brief Allocate an image charged to @p reserve for @p charge_bytes (whole Segment by default).
     *
     * @p overdraft_bytes is forwarded to MemoryReserve::try_charge_image.
     * @return null when the reserve does not admit it or the system refuses memory; the Segment is
     * then simply read from its file.
     */
    [[nodiscard]] static auto create(const std::shared_ptr<MemoryReserve>& reserve,
                                     std::size_t charge_bytes = kSegmentSizeBytes,
                                     std::uint64_t overdraft_bytes = 0) -> std::shared_ptr<SegmentImage>;

    /**
     * @brief Allocate an image and fill it with the committed Records of @p file.
     * @return null when not admitted; an error when the file read fails.
     */
    [[nodiscard]] static auto load(const std::shared_ptr<MemoryReserve>& reserve,
                                   const DurableSegmentFile& file) -> Result<std::shared_ptr<SegmentImage>>;

    explicit SegmentImage(PageableRegion region) noexcept;

    /// Copy encoded Record bytes to @p offset. Out-of-range requests are ignored: the Segment file
    /// rejects the same append, so no reference can ever point there.
    void mirror(std::uint64_t offset, std::span<const std::byte> encoded_record) noexcept;

    /// Whole Segment-sized view; only published Record extents are meaningful.
    [[nodiscard]] auto bytes() const noexcept -> std::span<const std::byte>;

    /// Keep the reserve charge only for @p committed_end bytes once the Segment can no longer grow.
    void seal(std::uint64_t committed_end) noexcept;

    /// Stop charging the reserve for this image (eviction); its pages return when it is destroyed.
    void release_charge() noexcept;

    [[nodiscard]] auto charged_bytes() const noexcept -> std::uint64_t {
        return region_.charged_bytes();
    }

    /// Monotonic creation order; eviction removes the oldest images first.
    [[nodiscard]] auto birth_stamp() const noexcept -> std::uint64_t {
        return birth_stamp_;
    }

  private:
    PageableRegion region_;
    std::uint64_t birth_stamp_;
};

} // namespace glifistore
