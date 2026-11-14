#include "glifistore/store/paired/mutation_slot_pool.hpp"

#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>

namespace glifistore::store::paired {

MutationSlotPool::MutationSlotPool(const std::size_t slot_capacity, const std::size_t byte_capacity,
                                   const std::size_t maximum_payload_bytes)
    : slot_capacity_(slot_capacity), byte_capacity_(byte_capacity),
      maximum_payload_bytes_(maximum_payload_bytes) {
    if (slot_capacity_ == 0 || slot_capacity_ > std::numeric_limits<SlotId>::max() || byte_capacity_ == 0 ||
        maximum_payload_bytes_ > byte_capacity_ ||
        byte_capacity_ > std::numeric_limits<std::uint64_t>::max() / 2U ||
        maximum_payload_bytes_ > std::numeric_limits<std::size_t>::max() - byte_capacity_) {
        throw std::invalid_argument{"mutation slot-pool capacity is outside supported limits"};
    }
    // Payload bytes are always overwritten before publication. Avoid touching
    // the complete configured arena at startup: large per-pair budgets remain
    // virtual until traffic commits the corresponding pages.
    storage_ = std::make_unique_for_overwrite<std::byte[]>(byte_capacity_ + maximum_payload_bytes_);
    slots_ = std::make_unique<Slot[]>(slot_capacity_);
    free_slots_.reserve(slot_capacity_);
    for (std::size_t slot = slot_capacity_; slot > 0; --slot) {
        free_slots_.push_back(static_cast<SlotId>(slot - 1U));
    }
}

auto MutationSlotPool::payload_bytes_in_use() const noexcept -> std::size_t {
    return static_cast<std::size_t>(head_cursor_ - tail_cursor_);
}

auto MutationSlotPool::try_acquire(const std::span<const std::byte> key,
                                   const std::span<const std::byte> value,
                                   const std::size_t admission_bytes) noexcept -> AcquireResult {
    // Ring cursors and FIFO sequence numbers are logical identities. Reset
    // them only at complete quiescence so a process with unbounded lifetime
    // never wraps a live identity into an apparent ABA match.
    if (free_slots_.size() == slot_capacity_) {
        if (head_cursor_ != tail_cursor_ || next_sequence_ != release_sequence_ ||
            admission_bytes_in_use_ != 0U) [[unlikely]] {
            std::terminate();
        }
        head_cursor_ = 0U;
        tail_cursor_ = 0U;
        next_sequence_ = 0U;
        release_sequence_ = 0U;
    }
    if (key.size() > std::numeric_limits<std::size_t>::max() - value.size()) {
        return {.failure = AcquireFailure::payload_too_large};
    }
    const auto payload_bytes = key.size() + value.size();
    if (payload_bytes > maximum_payload_bytes_ || admission_bytes < payload_bytes ||
        admission_bytes > byte_capacity_) {
        return {.failure = AcquireFailure::payload_too_large};
    }
    if (free_slots_.empty()) {
        return {.failure = AcquireFailure::slot_exhausted};
    }
    if (admission_bytes > byte_capacity_ - admission_bytes_in_use_ ||
        payload_bytes > byte_capacity_ - payload_bytes_in_use()) {
        return {.failure = AcquireFailure::byte_exhausted};
    }
    if (payload_bytes > std::numeric_limits<std::uint64_t>::max() - head_cursor_ ||
        next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        // Existing FIFO owners must drain before the quiescent reset above.
        return {.failure = AcquireFailure::byte_exhausted};
    }

    const auto slot_id = free_slots_.back();
    auto& slot = slots_[slot_id];
    if (slot.occupied) {
        return {.failure = AcquireFailure::slot_exhausted};
    }
    free_slots_.pop_back();
    const auto data_offset = static_cast<std::size_t>(head_cursor_ % byte_capacity_);
    auto* destination = storage_.get() + data_offset;
    if (!key.empty()) {
        std::memcpy(destination, key.data(), key.size());
    }
    if (!value.empty()) {
        std::memcpy(destination + key.size(), value.data(), value.size());
    }

    slot = {.sequence = next_sequence_,
            .begin_cursor = head_cursor_,
            .end_cursor = head_cursor_ + payload_bytes,
            .data_offset = data_offset,
            .key_size = key.size(),
            .value_size = value.size(),
            .admission_bytes = admission_bytes,
            .occupied = true};
    ++next_sequence_;
    head_cursor_ = slot.end_cursor;
    admission_bytes_in_use_ += admission_bytes;
    return {.lease = Lease{.slot = slot_id, .admission_bytes = admission_bytes}};
}

auto MutationSlotPool::rollback(const Lease lease) noexcept -> bool {
    if (lease.slot >= slot_capacity_) {
        return false;
    }
    auto& slot = slots_[lease.slot];
    if (!slot.occupied || slot.admission_bytes != lease.admission_bytes ||
        slot.sequence + 1U != next_sequence_ || slot.end_cursor != head_cursor_) {
        return false;
    }
    head_cursor_ = slot.begin_cursor;
    --next_sequence_;
    admission_bytes_in_use_ -= slot.admission_bytes;
    slot = {};
    if (free_slots_.size() >= free_slots_.capacity()) [[unlikely]] {
        std::terminate();
    }
    free_slots_.push_back(lease.slot);
    return true;
}

auto MutationSlotPool::release(const SlotId slot_id) noexcept -> bool {
    if (slot_id >= slot_capacity_) {
        return false;
    }
    auto& slot = slots_[slot_id];
    if (!slot.occupied || slot.sequence != release_sequence_ || slot.begin_cursor != tail_cursor_ ||
        slot.admission_bytes > admission_bytes_in_use_) {
        return false;
    }
    tail_cursor_ = slot.end_cursor;
    ++release_sequence_;
    admission_bytes_in_use_ -= slot.admission_bytes;
    slot = {};
    if (free_slots_.size() >= free_slots_.capacity()) [[unlikely]] {
        std::terminate();
    }
    free_slots_.push_back(slot_id);
    return true;
}

auto MutationSlotPool::view(const SlotId slot_id) const noexcept -> std::optional<PayloadView> {
    if (slot_id >= slot_capacity_) {
        return std::nullopt;
    }
    const auto& slot = slots_[slot_id];
    if (!slot.occupied || slot.key_size > maximum_payload_bytes_ ||
        slot.value_size > maximum_payload_bytes_ - slot.key_size || slot.data_offset >= byte_capacity_ ||
        slot.key_size + slot.value_size > byte_capacity_ + maximum_payload_bytes_ - slot.data_offset) {
        return std::nullopt;
    }
    const auto* data = storage_.get() + slot.data_offset;
    return PayloadView{
        .key = {reinterpret_cast<const char*>(data), slot.key_size},
        .value = {data + slot.key_size, slot.value_size},
        .admission_bytes = slot.admission_bytes,
    };
}

} // namespace glifistore::store::paired
