/**
 * @file
 * @brief One RAM reserve shared by the pageable Index and resident Segment images (ADR 0046).
 *
 * Internal header (not installed). The reserve is a policy, not an allocator limit: Index memory
 * is always admitted, and only its backing changes when the reserve is exhausted. Segment images
 * are optional and admitted only inside what the Index leaves free.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>

namespace glifistore {

/// Physical backing chosen for one pageable region.
enum class PageableBacking : std::uint8_t {
    heap,       ///< Small region on the process heap.
    anonymous,  ///< Anonymous mapping; the kernel may page it to swap.
    spill_file, ///< Shared mapping of an unlinked scratch file; the kernel pages it to that file.
};

/**
 * @brief Point-in-time counters of one reserve; values may be mutually inconsistent by one update.
 */
struct MemoryReserveStats {
    std::uint64_t reserve_bytes{};
    std::uint64_t index_charged_bytes{};      ///< Index bytes, every backing.
    std::uint64_t index_spilled_bytes{};      ///< Subset of index_charged_bytes backed by spill files.
    std::uint64_t image_charged_bytes{};      ///< Resident Segment image bytes.
    std::uint64_t image_budget_bytes{};       ///< reserve_bytes minus index_charged_bytes, floored at zero.
    std::uint64_t image_admission_failures{}; ///< Images that started cold for lack of budget.
    std::uint64_t spill_mapping_failures{};   ///< Spill files that fell back to anonymous mappings.
};

/**
 * @brief Thread-safe accounting for one Store's RAM reserve.
 *
 * Every counter is a relaxed atomic: charges are policy inputs, never synchronization. A region
 * releases exactly what it charged, from whichever thread drops it last.
 */
class MemoryReserve final {
  public:
    /**
     * @param reserve_bytes RAM the Store may keep resident for derived state (Index first).
     * @param spill_directory Directory for Index spill files; empty keeps Index growth anonymous.
     */
    MemoryReserve(std::uint64_t reserve_bytes, std::filesystem::path spill_directory);

    MemoryReserve(const MemoryReserve&) = delete;
    auto operator=(const MemoryReserve&) -> MemoryReserve& = delete;

    [[nodiscard]] auto reserve_bytes() const noexcept -> std::uint64_t {
        return reserve_bytes_;
    }

    [[nodiscard]] auto spill_directory() const noexcept -> const std::filesystem::path& {
        return spill_directory_;
    }

    /**
     * @brief Charge Index bytes unconditionally.
     * @return true when the charge still fits in the reserve, so the region may stay anonymous.
     */
    auto charge_index(std::uint64_t bytes) noexcept -> bool;
    void release_index(std::uint64_t bytes, PageableBacking backing) noexcept;
    void note_index_spilled(std::uint64_t bytes) noexcept;
    void note_spill_failure() noexcept;

    /**
     * @brief Admit image bytes only while Index plus images stay within the reserve.
     *
     * @p overdraft_bytes lets a compaction output be admitted against images it is about to
     * replace; the sources leave the catalog when the transaction commits.
     * @return false when the image must start cold; the failure is counted.
     */
    [[nodiscard]] auto try_charge_image(std::uint64_t bytes, std::uint64_t overdraft_bytes = 0) noexcept
        -> bool;
    void release_image(std::uint64_t bytes) noexcept;

    /// Reserve left for images after the Index, floored at zero.
    [[nodiscard]] auto image_budget_bytes() const noexcept -> std::uint64_t;
    /// Image bytes above the current image budget; eviction drives this to zero.
    [[nodiscard]] auto image_excess_bytes() const noexcept -> std::uint64_t;
    [[nodiscard]] auto stats() const noexcept -> MemoryReserveStats;

  private:
    const std::uint64_t reserve_bytes_;
    const std::filesystem::path spill_directory_;
    std::atomic_uint64_t index_charged_bytes_{};
    std::atomic_uint64_t index_spilled_bytes_{};
    std::atomic_uint64_t image_charged_bytes_{};
    std::atomic_uint64_t image_admission_failures_{};
    std::atomic_uint64_t spill_mapping_failures_{};
};

/**
 * @brief Move-only owner of one contiguous, zero-initialized byte region.
 *
 * Regions below kPageableMappingThresholdBytes live on the heap. Larger Index regions are
 * anonymous mappings while the reserve allows, then spill-file mappings when a spill directory is
 * configured; Image regions are always anonymous. Every mapping has a PROT_NONE guard page on each
 * side, so an overrun faults instead of reaching another allocation. A spill file is unlinked before use, never
 * synchronized and never reopened: the Index is derived state rebuilt at open (ADR 0004).
 * Allocation failure throws std::bad_alloc, matching the container behavior it replaces.
 */
class PageableRegion final {
  public:
    enum class Use : std::uint8_t { index, image };

    PageableRegion() noexcept = default;
    PageableRegion(PageableRegion&& other) noexcept;
    auto operator=(PageableRegion&& other) noexcept -> PageableRegion&;
    PageableRegion(const PageableRegion&) = delete;
    auto operator=(const PageableRegion&) -> PageableRegion& = delete;
    ~PageableRegion();

    /**
     * @brief Allocate @p bytes of zeroed memory charged to @p reserve (may be null).
     *
     * The charge is the page-rounded size, capped by @p charge_limit_bytes for an image whose
     * known content is smaller than its mapping (a sealed Segment).
     * An Index region is always granted. An image region is granted only when the reserve admits
     * it; otherwise the result is empty. Throws std::bad_alloc when the system refuses memory.
     */
    [[nodiscard]] static auto
    allocate(std::size_t bytes, Use use, std::shared_ptr<MemoryReserve> reserve,
             std::size_t charge_limit_bytes = std::numeric_limits<std::size_t>::max(),
             std::uint64_t image_overdraft_bytes = 0) -> PageableRegion;

    [[nodiscard]] auto data() noexcept -> std::byte* {
        return data_;
    }

    [[nodiscard]] auto data() const noexcept -> const std::byte* {
        return data_;
    }

    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return size_;
    }

    [[nodiscard]] auto empty() const noexcept -> bool {
        return size_ == 0;
    }

    [[nodiscard]] auto backing() const noexcept -> PageableBacking {
        return backing_;
    }

    [[nodiscard]] auto charged_bytes() const noexcept -> std::uint64_t {
        return charged_bytes_;
    }

    /**
     * @brief Charge an existing uncharged Index region to @p reserve; its backing is unchanged.
     *
     * Lets storage built before the reserve existed (recovery) count against it.
     */
    void adopt_index_reserve(const std::shared_ptr<MemoryReserve>& reserve) noexcept;

    /// Reduce the charged image bytes to @p bytes (rounded up to a page); the mapping is unchanged.
    void trim_image_charge(std::size_t bytes) noexcept;

  private:
    void reset() noexcept;

    std::byte* data_{};
    std::size_t size_{};
    std::size_t mapped_bytes_{};
    std::uint64_t charged_bytes_{};
    PageableBacking backing_{PageableBacking::heap};
    Use use_{Use::index};
    std::unique_ptr<std::byte[]> heap_;
    std::shared_ptr<MemoryReserve> reserve_;
};

/// Regions smaller than this stay on the heap: mapping them would waste page-table entries.
inline constexpr std::size_t kPageableMappingThresholdBytes = std::size_t{1} << 20U;

/// Under AddressSanitizer every Index region is a heap region, so overflows stay instrumented; the
/// reserve still charges it and no spill file is used. Images stay mapped: they are large, lazily
/// touched and read only through validated extents.
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
inline constexpr bool kPageableIndexMappingEnabled = false;
#else
inline constexpr bool kPageableIndexMappingEnabled = true;
#endif
#elif defined(__SANITIZE_ADDRESS__)
inline constexpr bool kPageableIndexMappingEnabled = false;
#else
inline constexpr bool kPageableIndexMappingEnabled = true;
#endif

} // namespace glifistore
