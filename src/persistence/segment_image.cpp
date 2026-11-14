#include "glifistore/persistence/segment_image.hpp"

#include "glifistore/persistence/segment_file.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <new>
#include <utility>

namespace glifistore {
namespace {

std::atomic_uint64_t next_image_birth_stamp{1};

} // namespace

SegmentImage::SegmentImage(PageableRegion region) noexcept
    : region_(std::move(region)),
      birth_stamp_(next_image_birth_stamp.fetch_add(1U, std::memory_order_relaxed)) {}

auto SegmentImage::create(const std::shared_ptr<MemoryReserve>& reserve, const std::size_t charge_bytes,
                          const std::uint64_t overdraft_bytes) -> std::shared_ptr<SegmentImage> {
    if (!reserve) {
        return {};
    }
    try {
        auto region = PageableRegion::allocate(kSegmentSizeBytes, PageableRegion::Use::image, reserve,
                                               std::max<std::size_t>(charge_bytes, 1U), overdraft_bytes);
        if (region.empty()) {
            return {};
        }
        return std::make_shared<SegmentImage>(std::move(region));
    } catch (const std::bad_alloc&) {
        // An image is an optimization: refusing it leaves the Segment on the cold file path.
        return {};
    }
}

auto SegmentImage::load(const std::shared_ptr<MemoryReserve>& reserve, const DurableSegmentFile& file)
    -> Result<std::shared_ptr<SegmentImage>> {
    // A sealed Segment never grows: charge only its committed extent. An active Segment is charged
    // whole because the Writer will fill it.
    const auto& commit = file.selected_commit().commit;
    const auto charge_bytes = commit.state == PersistedSegmentState::sealed
                                  ? static_cast<std::size_t>(commit.committed_end)
                                  : kSegmentSizeBytes;
    auto image = create(reserve, charge_bytes);
    if (!image) {
        return std::shared_ptr<SegmentImage>{};
    }
    if (auto read = file.read_committed_records_into(
            std::span<std::byte>{image->region_.data(), image->region_.size()});
        !read) {
        return unexpected(read.error());
    }
    return image;
}

void SegmentImage::mirror(const std::uint64_t offset,
                          const std::span<const std::byte> encoded_record) noexcept {
    if (offset < kSegmentHeaderReservedBytes || offset > region_.size() ||
        encoded_record.size() > region_.size() - offset || encoded_record.empty()) {
        return;
    }
    std::memcpy(region_.data() + offset, encoded_record.data(), encoded_record.size());
}

auto SegmentImage::bytes() const noexcept -> std::span<const std::byte> {
    return {region_.data(), region_.size()};
}

void SegmentImage::release_charge() noexcept {
    region_.trim_image_charge(0U);
}

void SegmentImage::seal(const std::uint64_t committed_end) noexcept {
    region_.trim_image_charge(
        static_cast<std::size_t>(std::min<std::uint64_t>(committed_end, region_.size())));
}

void CompactionImageCredit::add_source(const SegmentImage* const image) {
    const auto charged_bytes = image != nullptr ? image->charged_bytes() : 0U;
    const std::lock_guard lock{mutex_};
    sources_.push_back(Source{.image = image, .charged_bytes = charged_bytes});
    credit_bytes_ += charged_bytes;
}

void CompactionImageCredit::retain_sources(const std::size_t count) noexcept {
    const std::lock_guard lock{mutex_};
    while (sources_.size() > count) {
        credit_bytes_ -= std::min(credit_bytes_, sources_.back().charged_bytes);
        sources_.pop_back();
    }
}

auto CompactionImageCredit::create_output(const std::shared_ptr<MemoryReserve>& reserve)
    -> std::shared_ptr<SegmentImage> {
    // The reserve already counts earlier outputs, so the overdraft is exactly what the remaining
    // sources hold: admission keeps (images - sources) within the budget.
    const std::lock_guard lock{mutex_};
    return SegmentImage::create(reserve, kSegmentSizeBytes, credit_bytes_);
}

void CompactionImageCredit::release_evicted(SegmentImage& image) noexcept {
    const std::lock_guard lock{mutex_};
    const auto source = std::ranges::find(sources_, &image, &Source::image);
    if (source != sources_.end()) {
        credit_bytes_ -= std::min(credit_bytes_, source->charged_bytes);
        sources_.erase(source);
    }
    image.release_charge();
}

auto CompactionImageCredit::credit_bytes() const noexcept -> std::uint64_t {
    const std::lock_guard lock{mutex_};
    return credit_bytes_;
}

} // namespace glifistore
