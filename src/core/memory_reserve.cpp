#include "glifistore/core/memory_reserve.hpp"

#include "glifistore/core/fault_injection.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <limits>
#include <new>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace glifistore {
namespace {

auto page_size_bytes() noexcept -> std::size_t {
    static const std::size_t page_size = [] {
        const auto reported = ::sysconf(_SC_PAGESIZE);
        return reported > 0 ? static_cast<std::size_t>(reported) : std::size_t{4096};
    }();
    return page_size;
}

auto round_up_to_page(const std::size_t bytes) -> std::size_t {
    const auto page = page_size_bytes();
    if (bytes > std::numeric_limits<std::size_t>::max() - (page - 1U)) {
        throw std::bad_alloc{};
    }
    return (bytes + page - 1U) / page * page;
}

// Reserves `payload_bytes` plus one PROT_NONE guard page on each side, then maps the payload over
// the middle with MAP_FIXED. Overruns and underruns fault instead of touching a neighbour.
auto reserve_guarded(const std::size_t payload_bytes) noexcept -> std::byte* {
    const auto page = page_size_bytes();
    if (payload_bytes > std::numeric_limits<std::size_t>::max() - 2U * page) {
        return nullptr;
    }
    void* const reservation =
        ::mmap(nullptr, payload_bytes + 2U * page, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return reservation == MAP_FAILED ? nullptr : static_cast<std::byte*>(reservation) + page;
}

void unmap_guarded(std::byte* const payload, const std::size_t payload_bytes) noexcept {
    const auto page = page_size_bytes();
    static_cast<void>(::munmap(payload - page, payload_bytes + 2U * page));
}

auto map_anonymous(const std::size_t mapped_bytes) noexcept -> std::byte* {
    auto* const payload = reserve_guarded(mapped_bytes);
    if (payload == nullptr) {
        return nullptr;
    }
    void* const mapping =
        ::mmap(payload, mapped_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (mapping == MAP_FAILED) {
        unmap_guarded(payload, mapped_bytes);
        return nullptr;
    }
    return payload;
}

class ScopedDescriptor final {
  public:
    explicit ScopedDescriptor(const int descriptor) noexcept : descriptor_(descriptor) {}

    ScopedDescriptor(const ScopedDescriptor&) = delete;
    auto operator=(const ScopedDescriptor&) -> ScopedDescriptor& = delete;

    ~ScopedDescriptor() {
        if (descriptor_ >= 0) {
            static_cast<void>(::close(descriptor_));
        }
    }

    [[nodiscard]] auto get() const noexcept -> int {
        return descriptor_;
    }

  private:
    int descriptor_;
};

// Writes zeros over the whole file so that every block exists before a page is mapped. Used where
// the platform has no reservation call or the filesystem refuses it.
auto zero_fill(const int descriptor, const std::size_t file_bytes) noexcept -> bool {
    static constexpr std::array<std::byte, 64U * 1024U> kZeros{};
    std::size_t written_bytes{};
    while (written_bytes < file_bytes) {
        const auto chunk_bytes = std::min(kZeros.size(), file_bytes - written_bytes);
        const auto result =
            ::pwrite(descriptor, kZeros.data(), chunk_bytes, static_cast<off_t>(written_bytes));
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return false;
        }
        written_bytes += static_cast<std::size_t>(result);
    }
    return true;
}

// Sizes the spill file and reserves its blocks, so that a later page write cannot meet a full
// filesystem and raise SIGBUS (HAZ-043). A file whose blocks cannot be reserved is refused, never
// mapped sparse; the caller then keeps the region in anonymous memory. Copy-on-write filesystems
// may still need new blocks on a page write; that residual stays under HAZ-043.
auto size_and_reserve(const int descriptor, const std::size_t file_bytes) noexcept -> bool {
    if (fault::consume_fail(fault::Site::spill_reserve)) {
        return false;
    }
#if defined(__APPLE__)
    // F_PREALLOCATE reserves blocks beyond the physical end of file, so it precedes the resize.
    fstore_t store{};
    store.fst_flags = F_ALLOCATECONTIG | F_ALLOCATEALL;
    store.fst_posmode = F_PEOFPOSMODE;
    store.fst_offset = 0;
    store.fst_length = static_cast<off_t>(file_bytes);
    bool reserved{};
    if (fault::consume_fail(fault::Site::spill_contiguous_reserve)) {
        errno = ENOSPC;
    } else {
        reserved = ::fcntl(descriptor, F_PREALLOCATE, &store) != -1;
    }
    if (!reserved) {
        // ENOSPC may mean that no contiguous extent fits, not that the filesystem is full.
        // Try fragmented blocks before deciding to keep the region in anonymous memory.
        store.fst_flags = F_ALLOCATEALL;
        reserved = ::fcntl(descriptor, F_PREALLOCATE, &store) != -1;
    }
    if (!reserved && errno == ENOSPC) {
        return false;
    }
    if (::ftruncate(descriptor, static_cast<off_t>(file_bytes)) != 0) {
        return false;
    }
    return reserved || zero_fill(descriptor, file_bytes);
#elif defined(__linux__) || defined(__FreeBSD__)
    if (::ftruncate(descriptor, static_cast<off_t>(file_bytes)) != 0) {
        return false;
    }
    const auto result = ::posix_fallocate(descriptor, 0, static_cast<off_t>(file_bytes));
    if (result == 0) {
        return true;
    }
    // EINVAL and EOPNOTSUPP mean the filesystem cannot reserve; any other error is a refusal.
    return (result == EINVAL || result == EOPNOTSUPP) && zero_fill(descriptor, file_bytes);
#else
    if (::ftruncate(descriptor, static_cast<off_t>(file_bytes)) != 0) {
        return false;
    }
    return zero_fill(descriptor, file_bytes);
#endif
}

// Shared mapping of an unlinked scratch file. The name never outlives this function, the file is
// never synchronized, and its blocks are returned to the filesystem when the mapping is removed.
auto map_spill_file(const std::filesystem::path& directory, const std::size_t mapped_bytes) noexcept
    -> std::byte* {
    if (mapped_bytes > static_cast<std::size_t>(std::numeric_limits<off_t>::max())) {
        return nullptr;
    }
    try {
        const auto pattern = (directory / "glifistore-index-spill-XXXXXX").string();
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');
        const ScopedDescriptor descriptor{::mkstemp(name.data())};
        if (descriptor.get() < 0) {
            return nullptr;
        }
        static_cast<void>(::unlink(name.data()));
        static_cast<void>(::fcntl(descriptor.get(), F_SETFD, FD_CLOEXEC));
        if (!size_and_reserve(descriptor.get(), mapped_bytes)) {
            return nullptr;
        }
        auto* const payload = reserve_guarded(mapped_bytes);
        if (payload == nullptr) {
            return nullptr;
        }
        void* const mapping = ::mmap(payload, mapped_bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                                     descriptor.get(), 0);
        if (mapping == MAP_FAILED) {
            unmap_guarded(payload, mapped_bytes);
            return nullptr;
        }
        return payload;
    } catch (...) {
        return nullptr;
    }
}

auto saturating_subtract(const std::uint64_t left, const std::uint64_t right) noexcept -> std::uint64_t {
    return left > right ? left - right : 0U;
}

void atomic_saturating_release(std::atomic_uint64_t& counter, const std::uint64_t bytes) noexcept {
    auto current = counter.load(std::memory_order_relaxed);
    while (!counter.compare_exchange_weak(current, saturating_subtract(current, bytes),
                                          std::memory_order_relaxed)) {
    }
}

} // namespace

MemoryReserve::MemoryReserve(const std::uint64_t reserve_bytes, std::filesystem::path spill_directory)
    : reserve_bytes_(reserve_bytes), spill_directory_(std::move(spill_directory)) {}

auto MemoryReserve::charge_index(const std::uint64_t bytes) noexcept -> bool {
    const auto previous = index_charged_bytes_.fetch_add(bytes, std::memory_order_relaxed);
    return previous <= reserve_bytes_ && bytes <= reserve_bytes_ - previous;
}

void MemoryReserve::release_index(const std::uint64_t bytes, const PageableBacking backing) noexcept {
    atomic_saturating_release(index_charged_bytes_, bytes);
    if (backing == PageableBacking::spill_file) {
        atomic_saturating_release(index_spilled_bytes_, bytes);
    }
}

void MemoryReserve::note_index_spilled(const std::uint64_t bytes) noexcept {
    index_spilled_bytes_.fetch_add(bytes, std::memory_order_relaxed);
}

void MemoryReserve::note_spill_failure() noexcept {
    spill_mapping_failures_.fetch_add(1U, std::memory_order_relaxed);
}

auto MemoryReserve::try_charge_image(const std::uint64_t bytes, const std::uint64_t overdraft_bytes) noexcept
    -> bool {
    auto current = image_charged_bytes_.load(std::memory_order_relaxed);
    while (true) {
        const auto base_budget = image_budget_bytes();
        const auto budget = overdraft_bytes > std::numeric_limits<std::uint64_t>::max() - base_budget
                                ? std::numeric_limits<std::uint64_t>::max()
                                : base_budget + overdraft_bytes;
        if (current > budget || bytes > budget - current) {
            image_admission_failures_.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }
        if (image_charged_bytes_.compare_exchange_weak(current, current + bytes, std::memory_order_relaxed)) {
            return true;
        }
    }
}

void MemoryReserve::release_image(const std::uint64_t bytes) noexcept {
    atomic_saturating_release(image_charged_bytes_, bytes);
}

void MemoryReserve::note_image_eviction() noexcept {
    image_evictions_.fetch_add(1U, std::memory_order_relaxed);
}

auto MemoryReserve::image_budget_bytes() const noexcept -> std::uint64_t {
    return saturating_subtract(reserve_bytes_, index_charged_bytes_.load(std::memory_order_relaxed));
}

auto MemoryReserve::image_excess_bytes() const noexcept -> std::uint64_t {
    return saturating_subtract(image_charged_bytes_.load(std::memory_order_relaxed), image_budget_bytes());
}

auto MemoryReserve::stats() const noexcept -> MemoryReserveStats {
    return {
        .reserve_bytes = reserve_bytes_,
        .index_charged_bytes = index_charged_bytes_.load(std::memory_order_relaxed),
        .index_spilled_bytes = index_spilled_bytes_.load(std::memory_order_relaxed),
        .image_charged_bytes = image_charged_bytes_.load(std::memory_order_relaxed),
        .image_budget_bytes = image_budget_bytes(),
        .image_admission_failures = image_admission_failures_.load(std::memory_order_relaxed),
        .image_evictions = image_evictions_.load(std::memory_order_relaxed),
        .spill_mapping_failures = spill_mapping_failures_.load(std::memory_order_relaxed),
    };
}

PageableRegion::PageableRegion(PageableRegion&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0U)),
      mapped_bytes_(std::exchange(other.mapped_bytes_, 0U)),
      charged_bytes_(std::exchange(other.charged_bytes_, 0U)),
      backing_(std::exchange(other.backing_, PageableBacking::heap)), use_(other.use_),
      heap_(std::move(other.heap_)), reserve_(std::move(other.reserve_)) {}

auto PageableRegion::operator=(PageableRegion&& other) noexcept -> PageableRegion& {
    if (this != &other) {
        reset();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0U);
        mapped_bytes_ = std::exchange(other.mapped_bytes_, 0U);
        charged_bytes_ = std::exchange(other.charged_bytes_, 0U);
        backing_ = std::exchange(other.backing_, PageableBacking::heap);
        use_ = other.use_;
        heap_ = std::move(other.heap_);
        reserve_ = std::move(other.reserve_);
    }
    return *this;
}

PageableRegion::~PageableRegion() {
    reset();
}

void PageableRegion::reset() noexcept {
    if (mapped_bytes_ != 0U) {
        unmap_guarded(data_, mapped_bytes_);
    }
    heap_.reset();
    if (reserve_ && charged_bytes_ != 0U) {
        if (use_ == Use::image) {
            reserve_->release_image(charged_bytes_);
        } else {
            reserve_->release_index(charged_bytes_, backing_);
        }
    }
    reserve_.reset();
    data_ = nullptr;
    size_ = 0U;
    mapped_bytes_ = 0U;
    charged_bytes_ = 0U;
    backing_ = PageableBacking::heap;
}

auto PageableRegion::allocate(const std::size_t bytes, const Use use, std::shared_ptr<MemoryReserve> reserve,
                              const std::size_t charge_limit_bytes, const std::uint64_t image_overdraft_bytes)
    -> PageableRegion {
    PageableRegion region;
    if (bytes == 0U) {
        return region;
    }
    region.use_ = use;
    const bool mapped =
        bytes >= kPageableMappingThresholdBytes && (use == Use::image || kPageableIndexMappingEnabled);
    const auto rounded = mapped ? round_up_to_page(bytes) : bytes;
    const auto charged = charge_limit_bytes < rounded
                             ? (mapped ? round_up_to_page(charge_limit_bytes) : charge_limit_bytes)
                             : rounded;

    // Charge first so that concurrent admissions see this region; every failure below releases
    // the charge through reset() before the exception or empty result leaves.
    bool within_reserve = true;
    if (reserve) {
        if (use == Use::image) {
            if (!reserve->try_charge_image(charged, image_overdraft_bytes)) {
                return region;
            }
        } else {
            within_reserve = reserve->charge_index(charged);
        }
        region.reserve_ = std::move(reserve);
        region.charged_bytes_ = charged;
    }

    if (!mapped) {
        region.heap_ = std::make_unique<std::byte[]>(bytes);
        region.data_ = region.heap_.get();
        region.size_ = bytes;
        return region;
    }

    // The whole rounded size is mapped so that size() is always addressable; a smaller image charge
    // only reflects the pages its known content can touch.
    if (use == Use::index && !within_reserve && !region.reserve_->spill_directory().empty()) {
        if (auto* const spilled = map_spill_file(region.reserve_->spill_directory(), rounded)) {
            region.data_ = spilled;
            region.mapped_bytes_ = rounded;
            region.size_ = bytes;
            region.backing_ = PageableBacking::spill_file;
            region.reserve_->note_index_spilled(charged);
            return region;
        }
        region.reserve_->note_spill_failure();
    }
    auto* const anonymous = map_anonymous(rounded);
    if (anonymous == nullptr) {
        throw std::bad_alloc{};
    }
    region.data_ = anonymous;
    region.mapped_bytes_ = rounded;
    region.size_ = bytes;
    region.backing_ = PageableBacking::anonymous;
    return region;
}

void PageableRegion::adopt_index_reserve(const std::shared_ptr<MemoryReserve>& reserve) noexcept {
    if (reserve_ || !reserve || use_ != Use::index || size_ == 0U) {
        return;
    }
    const auto charged = mapped_bytes_ != 0U ? mapped_bytes_ : size_;
    static_cast<void>(reserve->charge_index(charged));
    reserve_ = reserve;
    charged_bytes_ = charged;
}

void PageableRegion::trim_image_charge(const std::size_t bytes) noexcept {
    if (use_ != Use::image || !reserve_) {
        return;
    }
    const auto page = page_size_bytes();
    const auto rounded = bytes > std::numeric_limits<std::size_t>::max() - (page - 1U)
                             ? bytes
                             : (bytes + page - 1U) / page * page;
    const auto retained = std::min<std::uint64_t>(rounded, charged_bytes_);
    reserve_->release_image(charged_bytes_ - retained);
    charged_bytes_ = retained;
}

} // namespace glifistore
