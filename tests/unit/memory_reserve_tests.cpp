#include "glifistore/core/key_hash.hpp"
#include "glifistore/core/memory_reserve.hpp"
#include "glifistore/core/pageable_array.hpp"
#include "glifistore/index/index.hpp"
#include "test.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

constexpr std::uint64_t kMiB = 1024U * 1024U;

class SpillDirectory final {
  public:
    SpillDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "glifistore-spill-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* created = ::mkdtemp(writable.data());
        GLIFI_REQUIRE(created != nullptr);
        path_ = created;
    }

    SpillDirectory(const SpillDirectory&) = delete;
    auto operator=(const SpillDirectory&) -> SpillDirectory& = delete;

    ~SpillDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] auto path() const -> const std::filesystem::path& {
        return path_;
    }

    [[nodiscard]] auto entry_count() const -> std::size_t {
        return static_cast<std::size_t>(
            std::distance(std::filesystem::directory_iterator{path_}, std::filesystem::directory_iterator{}));
    }

  private:
    std::filesystem::path path_;
};

auto all_zero(const std::span<const std::byte> bytes) -> bool {
    return std::ranges::all_of(bytes, [](const std::byte value) { return value == std::byte{0}; });
}

} // namespace

GLIFI_TEST("memory reserve charges the Index first and admits images only in what remains") {
    glifistore::MemoryReserve reserve{100U * kMiB, {}};
    GLIFI_REQUIRE(reserve.charge_index(60U * kMiB));
    GLIFI_REQUIRE(reserve.image_budget_bytes() == 40U * kMiB);
    GLIFI_REQUIRE(reserve.try_charge_image(30U * kMiB));
    GLIFI_REQUIRE(!reserve.try_charge_image(20U * kMiB));
    // A compaction output may borrow the bytes of the images it replaces.
    GLIFI_REQUIRE(reserve.try_charge_image(20U * kMiB, 10U * kMiB));
    // Index growth is never refused, even beyond the reserve; images become excess.
    GLIFI_REQUIRE(!reserve.charge_index(50U * kMiB));
    auto stats = reserve.stats();
    GLIFI_REQUIRE(stats.index_charged_bytes == 110U * kMiB);
    GLIFI_REQUIRE(stats.image_budget_bytes == 0U);
    GLIFI_REQUIRE(reserve.image_excess_bytes() == 50U * kMiB);
    GLIFI_REQUIRE(stats.image_admission_failures == 1U);
    reserve.release_index(110U * kMiB, glifistore::PageableBacking::anonymous);
    reserve.release_image(50U * kMiB);
    stats = reserve.stats();
    GLIFI_REQUIRE(stats.index_charged_bytes == 0U);
    GLIFI_REQUIRE(stats.image_charged_bytes == 0U);
    GLIFI_REQUIRE(stats.image_budget_bytes == 100U * kMiB);
}

GLIFI_TEST("pageable regions choose heap, anonymous and spill backing from the reserve") {
    const SpillDirectory spill;
    auto reserve = std::make_shared<glifistore::MemoryReserve>(4U * kMiB, spill.path());
    using Region = glifistore::PageableRegion;
    {
        auto small = Region::allocate(4096U, Region::Use::index, reserve);
        GLIFI_REQUIRE(small.backing() == glifistore::PageableBacking::heap);
        GLIFI_REQUIRE(all_zero({small.data(), small.size()}));

        auto within = Region::allocate(2U * kMiB, Region::Use::index, reserve);
        GLIFI_REQUIRE(within.backing() == glifistore::PageableBacking::anonymous);
        GLIFI_REQUIRE(all_zero({within.data(), within.size()}));

        auto beyond = Region::allocate(8U * kMiB, Region::Use::index, reserve);
        GLIFI_REQUIRE(beyond.backing() == glifistore::PageableBacking::spill_file);
        GLIFI_REQUIRE(all_zero({beyond.data(), beyond.size()}));
        beyond.data()[beyond.size() - 1U] = std::byte{0x5a};
        GLIFI_REQUIRE(beyond.data()[beyond.size() - 1U] == std::byte{0x5a});
        // The spill file is unlinked before use: nothing persists in the directory.
        GLIFI_REQUIRE(spill.entry_count() == 0U);

        const auto stats = reserve->stats();
        GLIFI_REQUIRE(stats.index_spilled_bytes >= 8U * kMiB);
        GLIFI_REQUIRE(stats.index_charged_bytes >= 10U * kMiB);

        // An image is refused once the Index has consumed the reserve.
        auto image = Region::allocate(kMiB, Region::Use::image, reserve);
        GLIFI_REQUIRE(image.empty());
    }
    const auto released = reserve->stats();
    GLIFI_REQUIRE(released.index_charged_bytes == 0U);
    GLIFI_REQUIRE(released.index_spilled_bytes == 0U);

    // Without a spill directory, growth beyond the reserve stays anonymous (swap-backed).
    auto no_spill = std::make_shared<glifistore::MemoryReserve>(0U, std::filesystem::path{});
    auto anonymous = Region::allocate(2U * kMiB, Region::Use::index, no_spill);
    GLIFI_REQUIRE(anonymous.backing() == glifistore::PageableBacking::anonymous);
}

GLIFI_TEST("image regions trim and release their charge without unmapping") {
    auto reserve = std::make_shared<glifistore::MemoryReserve>(64U * kMiB, std::filesystem::path{});
    using Region = glifistore::PageableRegion;
    auto image = Region::allocate(16U * kMiB, Region::Use::image, reserve);
    GLIFI_REQUIRE(!image.empty());
    GLIFI_REQUIRE(reserve->stats().image_charged_bytes == 16U * kMiB);
    image.trim_image_charge(kMiB + 1U);
    GLIFI_REQUIRE(image.charged_bytes() > kMiB);
    GLIFI_REQUIRE(image.charged_bytes() < 2U * kMiB);
    GLIFI_REQUIRE(reserve->stats().image_charged_bytes == image.charged_bytes());
    image.trim_image_charge(0U);
    GLIFI_REQUIRE(reserve->stats().image_charged_bytes == 0U);
    image.data()[image.size() - 1U] = std::byte{1};
    GLIFI_REQUIRE(image.data()[image.size() - 1U] == std::byte{1});
}

GLIFI_TEST("pageable array preserves contents across heap to mapping growth") {
    auto reserve = std::make_shared<glifistore::MemoryReserve>(64U * kMiB, std::filesystem::path{});
    glifistore::PageableArray<std::uint64_t> values{reserve};
    for (std::uint64_t value = 0; value < 300'000U; ++value) {
        values.resize(values.size() + 1U);
        values[values.size() - 1U] = value * 3U;
    }
    GLIFI_REQUIRE(values.backing() == glifistore::PageableBacking::anonymous);
    GLIFI_REQUIRE(values.capacity() >= values.size());
    for (std::uint64_t value = 0; value < 300'000U; ++value) {
        GLIFI_REQUIRE(values[value] == value * 3U);
    }
    values.assign(16U, 7U);
    GLIFI_REQUIRE(values.size() == 16U);
    GLIFI_REQUIRE(std::ranges::all_of(values.span(), [](const std::uint64_t value) { return value == 7U; }));
    values.clear();
    GLIFI_REQUIRE(values.empty());
    GLIFI_REQUIRE(reserve->stats().index_charged_bytes == 0U);

    // Storage that existed before a reserve is attached is charged on attachment.
    glifistore::PageableArray<std::uint64_t> late;
    late.resize(1024U);
    late.set_memory_reserve(reserve);
    GLIFI_REQUIRE(reserve->stats().index_charged_bytes == 1024U * sizeof(std::uint64_t));
}

GLIFI_TEST("Index beyond a zero reserve spills to a file and stays exact") {
    const SpillDirectory spill;
    auto reserve = std::make_shared<glifistore::MemoryReserve>(0U, spill.path());
    {
        glifistore::Index index;
        index.attach_memory_reserve(reserve);
        constexpr std::uint64_t kKeys = 40'000U;
        for (std::uint64_t key = 0; key < kKeys; ++key) {
            // Long keys exercise the heap-key arena as well as the slot and control arrays.
            const auto text = "spill-key-with-a-long-heap-suffix-" + std::to_string(key);
            const auto inserted = index.insert_or_assign(
                glifistore::HashedKey::compute(text),
                glifistore::RecordRef{.segment_id = glifistore::SegmentId{1},
                                      .offset = glifistore::RecordOffset{static_cast<std::uint32_t>(key)},
                                      .size = glifistore::RecordSize{64},
                                      .sequence = glifistore::SequenceNumber{key + 1U},
                                      .generation = glifistore::GenerationId{1}});
            GLIFI_REQUIRE(inserted.has_value());
        }
        const auto stats = reserve->stats();
        GLIFI_REQUIRE(stats.index_spilled_bytes > 0U);
        GLIFI_REQUIRE(stats.image_budget_bytes == 0U);
        for (std::uint64_t key = 0; key < kKeys; key += 97U) {
            const auto text = "spill-key-with-a-long-heap-suffix-" + std::to_string(key);
            const auto found = index.find(glifistore::HashedKey::compute(text));
            GLIFI_REQUIRE(found.has_value());
            GLIFI_REQUIRE(found->sequence.value == key + 1U);
        }
        GLIFI_REQUIRE(index.stats().size == kKeys);
    }
    GLIFI_REQUIRE(reserve->stats().index_charged_bytes == 0U);
    GLIFI_REQUIRE(spill.entry_count() == 0U);
}
