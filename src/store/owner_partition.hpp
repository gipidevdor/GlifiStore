/**
 * @file
 * @brief Stable counting partition of a batch by owner.
 */
#pragma once

#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace glifistore::store_detail {

/**
 * @brief Groups the positions `0 .. item_count - 1` of a batch by owner, keeping ascending position
 * order inside every owner.
 *
 * A multi-owner `put_batch` or `erase_batch` must hand every owner its own subsequence in caller order
 * (docs/reference/cpp-api.md, section 6): the batch is equivalent to applying its items one at a time.
 * That is a stable partition by a small integer key, not a sort. A counting pass sizes every owner's
 * group, a prefix sum places the groups, and one scatter pass in ascending position order fills them.
 * Stability is therefore structural: no tie-break key, no comparator and no temporary buffer exist
 * that a later edit could break.
 *
 * Cost: `O(item_count + owner_count)` time, one allocation of `item_count + owner_count + 1` words,
 * and no comparisons. `owner_count` is at most `kMaximumWorkerCount`, so the owner term is constant.
 *
 * The result separates two concerns. `group(owner)` lists the positions of one owner in caller order,
 * and the Store submits that list as one FIFO sub-batch. Owners are visited in ascending order, so a
 * batch is always submitted in the same owner order.
 */
class OwnerPartition final {
  public:
    /**
     * @brief Builds the partition.
     * @param item_count Number of batch positions.
     * @param owner_count Number of owners; every owner index is below it.
     * @param owner_of Callable `owner_of(position) -> std::size_t`. It must be pure: it runs twice per
     * position and both calls must agree.
     * @throws std::invalid_argument when `owner_count` is zero.
     * @throws std::out_of_range when `owner_of` returns an index at or above `owner_count`.
     * @throws std::length_error when the table size would overflow.
     * @throws std::bad_alloc when the table cannot be allocated.
     */
    template <typename OwnerOf>
    OwnerPartition(const std::size_t item_count, const std::size_t owner_count, OwnerOf&& owner_of)
        : owner_count_(owner_count) {
        if (owner_count == 0) {
            throw std::invalid_argument("owner partition requires at least one owner");
        }
        if (owner_count >= std::numeric_limits<std::size_t>::max() - item_count) {
            throw std::length_error("owner partition table size overflows");
        }
        // Offsets occupy the first owner_count + 1 words and positions the remaining item_count words.
        table_.assign(owner_count + 1U + item_count, 0);
        std::size_t* const offsets = table_.data();
        std::size_t* const positions = offsets + owner_count + 1U;

        // Pass 1: histogram, shifted by one word so that the prefix sum below leaves offsets[owner] at
        // the first slot of that owner's group.
        for (std::size_t position = 0; position < item_count; ++position) {
            ++offsets[checked_owner(owner_of(position)) + 1U];
        }
        for (std::size_t owner = 0; owner < owner_count; ++owner) {
            if (offsets[owner + 1U] > largest_group_) {
                largest_group_ = offsets[owner + 1U];
            }
            offsets[owner + 1U] += offsets[owner];
        }

        // Pass 2: scatter in ascending position order, using offsets[owner] as that owner's write
        // cursor. Every owner's positions therefore land in caller order.
        for (std::size_t position = 0; position < item_count; ++position) {
            positions[offsets[checked_owner(owner_of(position))]++] = position;
        }
        // Each cursor now sits at the end of its group, which is the start of the next one: shift the
        // cursors back by one owner to restore the group boundaries.
        for (std::size_t owner = owner_count; owner > 0; --owner) {
            offsets[owner] = offsets[owner - 1U];
        }
        offsets[0] = 0;
    }

    /// Number of owners the partition was built for.
    [[nodiscard]] auto owner_count() const noexcept -> std::size_t {
        return owner_count_;
    }

    /// Number of positions across all groups.
    [[nodiscard]] auto item_count() const noexcept -> std::size_t {
        return table_.size() - owner_count_ - 1U;
    }

    /// Size of the largest group: the capacity one scratch buffer needs to serve every owner.
    [[nodiscard]] auto largest_group() const noexcept -> std::size_t {
        return largest_group_;
    }

    /// Positions of one owner in ascending order; empty when the owner holds no item.
    /// @pre `owner < owner_count()`.
    [[nodiscard]] auto group(const std::size_t owner) const noexcept -> std::span<const std::size_t> {
        const std::size_t* const offsets = table_.data();
        const std::size_t* const positions = offsets + owner_count_ + 1U;
        return {positions + offsets[owner], offsets[owner + 1U] - offsets[owner]};
    }

  private:
    [[nodiscard]] auto checked_owner(const std::size_t owner) const -> std::size_t {
        if (owner >= owner_count_) {
            throw std::out_of_range("owner index is outside the partition");
        }
        return owner;
    }

    std::vector<std::size_t> table_;
    std::size_t owner_count_;
    std::size_t largest_group_{};
};

} // namespace glifistore::store_detail
