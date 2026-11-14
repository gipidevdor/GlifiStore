/**
 * @file
 * @brief Shared execution of `Store::put_batch` and `Store::erase_batch`.
 *
 * Both batch calls prepare their items the same way and differ only in the operation they apply, so
 * the grouping, submission and failure policy live here once. Each call prepares its items in caller
 * order, optionally groups them by owner with OwnerPartition, and hands every owner its own
 * subsequence as one FIFO sub-batch.
 *
 * Failure contract (docs/spec/error-taxonomy-v1.md, section 5). A batch has a pre-commit phase and a
 * commit phase. The Store sizes everything it allocates for the batch before the first submission, so
 * an exception in the pre-commit phase leaves nothing applied and the caller reports every item as
 * not committed. `run_sequential_batch` and `submit_paired_batch` are the commit phase and never
 * throw: each position of the result receives the outcome of its own submission. An owner that was
 * admitted keeps the statuses its runtime reported. A runtime can still refuse a sub-batch before
 * admitting its first item, for example when it cannot allocate its nodes; that owner's items and
 * those of owners not yet submitted then report `resource_exhausted`, the known-not-committed
 * polarity.
 */
#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/core/fault_injection.hpp"
#include "glifistore/store/paired/shard_pair_runtime.hpp"
#include "store/owner_partition.hpp"
#include "store/store_impl.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace glifistore::store_detail {

// Handing a runtime status to the caller's vector must not allocate: the commit phase relies on it.
static_assert(std::is_nothrow_move_assignable_v<Status>);

/// Marks items that were never submitted. An empty message keeps the assignment allocation-free.
inline void stamp_not_submitted(const std::span<Status> statuses) noexcept {
    for (auto& status : statuses) {
        status = resource_exhausted();
    }
}

/**
 * @brief Buffers for submitting one owner group to the paired runtime.
 *
 * The buffers are sized once for the largest group of a batch, before the first submission, and reused
 * for every group. Allocating per group would let an allocation failure interleave with owners that
 * were already applied. Groups up to one Writer publication chunk (32 items) fit in the object itself.
 */
class BatchGroupScratch final {
  public:
    using Item = store::paired::ShardPairRuntime::SyncBatchItem;

    /// One Writer publication chunk: the largest group that needs no heap buffer.
    static constexpr std::size_t kInlineCapacity = 32;

    /// @param capacity Size of the largest group the scratch will serve.
    /// @throws std::bad_alloc when a group above kInlineCapacity needs heap buffers.
    explicit BatchGroupScratch(const std::size_t capacity) {
        if (capacity > kInlineCapacity) {
            heap_items_ = std::make_unique<Item[]>(capacity);
            heap_statuses_ = std::make_unique<Status[]>(capacity);
        }
    }

    BatchGroupScratch(const BatchGroupScratch&) = delete;
    auto operator=(const BatchGroupScratch&) -> BatchGroupScratch& = delete;
    BatchGroupScratch(BatchGroupScratch&&) = delete;
    auto operator=(BatchGroupScratch&&) -> BatchGroupScratch& = delete;
    ~BatchGroupScratch() = default;

    /// Input items of a group of `count` positions. @pre `count` is at most the constructed capacity.
    [[nodiscard]] auto items(const std::size_t count) noexcept -> std::span<Item> {
        return {heap_items_ ? heap_items_.get() : inline_items_.data(), count};
    }

    /// Result statuses of a group of `count` positions, reset to success. @pre as for items().
    [[nodiscard]] auto statuses(const std::size_t count) noexcept -> std::span<Status> {
        const std::span<Status> view{heap_statuses_ ? heap_statuses_.get() : inline_statuses_.data(), count};
        for (auto& status : view) {
            status = {};
        }
        return view;
    }

  private:
    std::array<Item, kInlineCapacity> inline_items_{};
    std::array<Status, kInlineCapacity> inline_statuses_{};
    std::unique_ptr<Item[]> heap_items_;
    std::unique_ptr<Status[]> heap_statuses_;
};

/**
 * @brief Applies a prepared batch one item at a time, in caller order, without a paired runtime.
 *
 * A failure stops the batch. The item that failed reports the outcome a single operation would
 * (`resource_exhausted` for an allocation failure, `internal_error` plus fail-closed otherwise), items
 * applied before it keep their own statuses, and later items were never started and report
 * `resource_exhausted`.
 *
 * @tparam Prepared Prepared entry with a `shard` member.
 * @tparam Apply Callable `Status(const Prepared&)` that applies one entry.
 * @param statuses One slot per entry, written exactly once.
 */
template <typename Impl, typename Prepared, typename Apply>
void run_sequential_batch(Impl& impl, const std::span<const Prepared> prepared,
                          const std::span<Status> statuses, const Apply& apply) noexcept {
    for (std::size_t position = 0; position < prepared.size(); ++position) {
        try {
            const auto& entry = prepared[position];
#if defined(GLIFISTORE_FAULT_INJECTION)
            // Litmus: arm the real emergency gate after the batch-entry check so later siblings
            // exercise the per-item re-check (mid-batch TOCTOU).
            if (fault::consume_fail(fault::Site::put_batch_gate)) {
                if (auto* controller = impl.maintenance.get(); controller != nullptr) {
                    controller->publish_mutations_rejected(true);
                }
            }
#endif
            // Mid-batch / TOCTOU: the gate may arm after the batch-entry probe.
            if (auto rejected = reject_if_maintenance_emergency(impl.maintenance.get()); !rejected) {
                statuses[position] = std::move(rejected);
                continue;
            }
            typename Impl::OperationGuard operation{impl, entry.shard};
            if (!operation) {
                statuses[position] = closed_store();
                continue;
            }
            statuses[position] = apply(entry);
        } catch (const std::bad_alloc&) {
            stamp_not_submitted(statuses.subspan(position));
            return;
        } catch (...) {
            impl.mark_durable_fail_closed();
            statuses[position] = internal_failure();
            stamp_not_submitted(statuses.subspan(position + 1U));
            return;
        }
    }
}

/**
 * @brief Submits one owner's group as a single FIFO sub-batch and writes its statuses back.
 *
 * `ShardPairRuntime::mutate_batch` throws only before it admits the first item, so an exception here
 * means nothing of this group was applied. The group then reports `resource_exhausted` for an
 * allocation failure, or `internal_error` plus fail-closed for anything unexpected.
 *
 * @param position_of Callable mapping a slot of this group to its position in `prepared`; slots are in
 * caller order.
 * @param make_item Callable building the runtime input item of one prepared entry.
 * @return false when the group failed with an exception and later groups must not be submitted.
 */
template <typename Impl, typename Prepared, typename MakeItem, typename PositionOf>
[[nodiscard]] auto submit_owner_group(Impl& impl, const std::size_t owner, const std::size_t count,
                                      const PositionOf& position_of, const std::span<const Prepared> prepared,
                                      const std::span<Status> statuses, BatchGroupScratch& scratch,
                                      const MakeItem& make_item) noexcept -> bool {
    typename Impl::OperationGuard operation{impl, owner};
    if (!operation) {
        for (std::size_t slot = 0; slot < count; ++slot) {
            statuses[position_of(slot)] = closed_store();
        }
        return true;
    }
    try {
        const auto items = scratch.items(count);
        const auto results = scratch.statuses(count);
        for (std::size_t slot = 0; slot < count; ++slot) {
            items[slot] = make_item(prepared[position_of(slot)]);
        }
        const auto batch_status = impl.pair_runtime->mutate_batch(owner, items, results);
        for (std::size_t slot = 0; slot < count; ++slot) {
            // A refused batch leaves untouched slots at success; none of them was applied.
            if (!batch_status && results[slot]) {
                results[slot] = batch_status;
            }
            statuses[position_of(slot)] = std::move(results[slot]);
        }
        return true;
    } catch (const std::bad_alloc&) {
        for (std::size_t slot = 0; slot < count; ++slot) {
            statuses[position_of(slot)] = resource_exhausted();
        }
        return false;
    } catch (...) {
        impl.mark_durable_fail_closed();
        for (std::size_t slot = 0; slot < count; ++slot) {
            statuses[position_of(slot)] = internal_failure();
        }
        return false;
    }
}

/**
 * @brief Submits a prepared batch to the paired runtime, one FIFO sub-batch per owner.
 *
 * @param partition Owner grouping of a multi-owner batch, or null when every item belongs to
 * `only_owner`; the single-owner batch is then submitted as one group in caller order.
 * @param scratch Buffers already sized for the largest group.
 *
 * Owners are visited in ascending order. When an owner's submission fails with an exception, owners
 * after it were never started and report `resource_exhausted`.
 */
template <typename Impl, typename Prepared, typename MakeItem>
void submit_paired_batch(Impl& impl, const std::span<const Prepared> prepared,
                         const OwnerPartition* const partition, const std::size_t only_owner,
                         const std::span<Status> statuses, BatchGroupScratch& scratch,
                         const MakeItem& make_item) noexcept {
    if (partition == nullptr) {
        const auto whole_batch = [](const std::size_t slot) noexcept { return slot; };
        static_cast<void>(submit_owner_group(impl, only_owner, prepared.size(), whole_batch, prepared,
                                             statuses, scratch, make_item));
        return;
    }
    for (std::size_t owner = 0; owner < partition->owner_count(); ++owner) {
        const auto group = partition->group(owner);
        if (group.empty()) {
            continue;
        }
        const auto group_position = [group](const std::size_t slot) noexcept { return group[slot]; };
        if (submit_owner_group(impl, owner, group.size(), group_position, prepared, statuses, scratch,
                               make_item)) {
            continue;
        }
        for (std::size_t later = owner + 1U; later < partition->owner_count(); ++later) {
            for (const auto position : partition->group(later)) {
                statuses[position] = resource_exhausted();
            }
        }
        return;
    }
}

} // namespace glifistore::store_detail
