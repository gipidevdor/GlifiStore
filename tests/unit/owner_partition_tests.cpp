// OwnerPartition is the stable grouping step of put_batch and erase_batch (src/store/owner_partition.hpp).
// Its whole contract is the order it produces: owner-major, ascending position inside every owner. That
// is exactly the result of a stable sort of the positions by owner, so the tests compare against
// std::ranges::stable_sort: exhaustively for every short owner sequence, and with seeded campaigns
// across and beyond the small-range thresholds at which an unstable sort first misorders equal keys.

#include "glifistore/store/config.hpp"
#include "store/owner_partition.hpp"
#include "test.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace {

using glifistore::kMaximumWorkerCount;
using glifistore::store_detail::OwnerPartition;

/// SplitMix64: a fixed seed replays the same campaign on every platform and standard library.
class DeterministicStream final {
  public:
    explicit DeterministicStream(const std::uint64_t seed) noexcept : state_(seed) {}

    auto below(const std::size_t bound) noexcept -> std::size_t {
        state_ += 0x9E3779B97F4A7C15ULL;
        auto mixed = state_;
        mixed = (mixed ^ (mixed >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        mixed = (mixed ^ (mixed >> 27U)) * 0x94D049BB133111EBULL;
        mixed ^= mixed >> 31U;
        return static_cast<std::size_t>(mixed % bound);
    }

  private:
    std::uint64_t state_;
};

auto stable_order(const std::vector<std::size_t>& owners) -> std::vector<std::size_t> {
    std::vector<std::size_t> order(owners.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::stable_sort(order, [&owners](const std::size_t left, const std::size_t right) {
        return owners[left] < owners[right];
    });
    return order;
}

/// Builds the partition of `owners` and checks every property of the contract.
void require_partition_of(const std::vector<std::size_t>& owners, const std::size_t owner_count) {
    const auto owner_of = [&owners](const std::size_t position) { return owners[position]; };
    const OwnerPartition partition{owners.size(), owner_count, owner_of};
    GLIFI_REQUIRE(partition.owner_count() == owner_count);
    GLIFI_REQUIRE(partition.item_count() == owners.size());

    std::vector<std::size_t> concatenated;
    concatenated.reserve(owners.size());
    std::size_t largest = 0;
    for (std::size_t owner = 0; owner < owner_count; ++owner) {
        const auto group = partition.group(owner);
        largest = std::max(largest, group.size());
        for (std::size_t slot = 0; slot < group.size(); ++slot) {
            GLIFI_REQUIRE(owners[group[slot]] == owner);
            // Caller order inside the owner: positions strictly ascend.
            GLIFI_REQUIRE(slot == 0 || group[slot - 1] < group[slot]);
            concatenated.push_back(group[slot]);
        }
    }
    GLIFI_REQUIRE(partition.largest_group() == largest);
    // Owners in ascending order, each in caller order: precisely a stable sort of the positions.
    GLIFI_REQUIRE(concatenated == stable_order(owners));
}

} // namespace

GLIFI_TEST("owner partition equals a stable sort for every short owner sequence") {
    // All sequences of length 0..8 over 1..4 owners: 87,000-odd cases, including every ordering of
    // repeated owners that an unstable sort could reorder.
    for (std::size_t owner_count = 1; owner_count <= 4; ++owner_count) {
        for (std::size_t item_count = 0; item_count <= 8; ++item_count) {
            std::size_t sequences = 1;
            for (std::size_t slot = 0; slot < item_count; ++slot) {
                sequences *= owner_count;
            }
            for (std::size_t code = 0; code < sequences; ++code) {
                std::vector<std::size_t> owners(item_count);
                std::size_t rest = code;
                for (auto& owner : owners) {
                    owner = rest % owner_count;
                    rest /= owner_count;
                }
                require_partition_of(owners, owner_count);
            }
        }
    }
}

GLIFI_TEST("owner partition keeps caller order across the small-range sort thresholds") {
    // Batch sizes from empty to well beyond the insertion-sort range of common standard libraries,
    // with the shapes that expose a missing tie-break: alternating owners, runs, and one hot owner.
    for (const std::size_t owner_count : {std::size_t{2}, std::size_t{4}, std::size_t{7}}) {
        for (std::size_t item_count = 0; item_count <= 260; ++item_count) {
            std::vector<std::size_t> owners(item_count);
            for (std::size_t position = 0; position < item_count; ++position) {
                owners[position] = position % owner_count;
            }
            require_partition_of(owners, owner_count);
            for (std::size_t position = 0; position < item_count; ++position) {
                owners[position] = (position / 5U) % owner_count;
            }
            require_partition_of(owners, owner_count);
            for (std::size_t position = 0; position < item_count; ++position) {
                owners[position] = (position % 9U == 0) ? owner_count - 1U : 0U;
            }
            require_partition_of(owners, owner_count);
        }
    }
}

GLIFI_TEST("owner partition matches a stable sort in seeded random campaigns") {
    DeterministicStream stream{0x5EED0DEC0DE12345ULL};
    for (int round = 0; round < 3'000; ++round) {
        const auto owner_count = 1U + stream.below(kMaximumWorkerCount);
        const auto item_count = stream.below(round % 10 == 0 ? 4'000U : 300U);
        std::vector<std::size_t> owners(item_count);
        const auto shape = stream.below(3);
        for (auto& owner : owners) {
            if (shape == 0) {
                owner = stream.below(owner_count); // uniform
            } else if (shape == 1) {
                owner = stream.below(10) < 8 ? 0U : stream.below(owner_count); // one hot owner
            } else {
                owner = owner_count - 1U; // every item on one owner
            }
        }
        require_partition_of(owners, owner_count);
    }
}

GLIFI_TEST("owner partition handles empty batches, empty owners and the largest owner count") {
    const OwnerPartition empty{0, 3, [](std::size_t) { return std::size_t{0}; }};
    GLIFI_REQUIRE(empty.item_count() == 0);
    GLIFI_REQUIRE(empty.largest_group() == 0);
    for (std::size_t owner = 0; owner < 3; ++owner) {
        GLIFI_REQUIRE(empty.group(owner).empty());
    }

    // One owner among the maximum count holds everything: every other group is empty.
    const OwnerPartition lopsided{10, kMaximumWorkerCount,
                                  [](std::size_t) { return kMaximumWorkerCount - 1U; }};
    GLIFI_REQUIRE(lopsided.largest_group() == 10);
    for (std::size_t owner = 0; owner + 1U < kMaximumWorkerCount; ++owner) {
        GLIFI_REQUIRE(lopsided.group(owner).empty());
    }
    const auto only = lopsided.group(kMaximumWorkerCount - 1U);
    GLIFI_REQUIRE(only.size() == 10);
    for (std::size_t slot = 0; slot < only.size(); ++slot) {
        GLIFI_REQUIRE(only[slot] == slot);
    }

    // A single owner is the identity ordering.
    const OwnerPartition single{5, 1, [](std::size_t) { return std::size_t{0}; }};
    GLIFI_REQUIRE(single.group(0).size() == 5);
    GLIFI_REQUIRE(single.largest_group() == 5);
}

GLIFI_TEST("owner partition reads each position exactly twice") {
    // The linear bound rests on two passes over the owners; a callable that is invoked more often
    // would hide a quadratic or repeated routing cost behind the same interface.
    constexpr std::size_t kItems = 1'000;
    std::size_t calls = 0;
    const OwnerPartition partition{kItems, 16, [&calls](const std::size_t position) {
                                       ++calls;
                                       return position % 16U;
                                   }};
    GLIFI_REQUIRE(calls == 2 * kItems);
    GLIFI_REQUIRE(partition.largest_group() == (kItems + 15U) / 16U);
}

GLIFI_TEST("owner partition rejects an unusable owner table") {
    const auto owner_zero = [](std::size_t) { return std::size_t{0}; };

    bool rejected = false;
    try {
        const OwnerPartition no_owners{3, 0, owner_zero};
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    GLIFI_REQUIRE(rejected);

    // An owner at or above the count is refused instead of indexing outside the table.
    rejected = false;
    try {
        const OwnerPartition out_of_range{4, 2, [](const std::size_t position) { return position + 1U; }};
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    GLIFI_REQUIRE(rejected);

    // The table size is computed with checked arithmetic.
    rejected = false;
    try {
        const OwnerPartition overflow{std::numeric_limits<std::size_t>::max(), 1, owner_zero};
    } catch (const std::length_error&) {
        rejected = true;
    }
    GLIFI_REQUIRE(rejected);
}
