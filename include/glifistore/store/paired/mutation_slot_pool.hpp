#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace glifistore::store::paired {

// Reader-owned allocator for one paired mutation lane. The Reader acquires and
// releases slots in completion order; the Writer only reads an immutable slot
// between the mutation-queue release edge and the completion-queue release
// edge. Consequently the payload path needs no mutex, refcount, or allocation.
class MutationSlotPool final {
  public:
    using SlotId = std::uint32_t;

    enum class AcquireFailure : std::uint8_t {
        none,
        slot_exhausted,
        byte_exhausted,
        payload_too_large,
    };

    struct Lease final {
        SlotId slot{};
        std::size_t admission_bytes{};
    };

    struct AcquireResult final {
        std::optional<Lease> lease{};
        AcquireFailure failure{AcquireFailure::none};
    };

    struct PayloadView final {
        std::string_view key;
        std::span<const std::byte> value;
        std::size_t admission_bytes{};
    };

    MutationSlotPool(std::size_t slot_capacity, std::size_t byte_capacity, std::size_t maximum_payload_bytes);

    MutationSlotPool(const MutationSlotPool&) = delete;
    auto operator=(const MutationSlotPool&) -> MutationSlotPool& = delete;
    MutationSlotPool(MutationSlotPool&&) = delete;
    auto operator=(MutationSlotPool&&) -> MutationSlotPool& = delete;

    // Copies key/value into one stable contiguous extent. admission_bytes is a
    // stable logical charge supplied by the paired runtime and includes task
    // metadata as well as payload bytes.
    [[nodiscard]] auto try_acquire(std::span<const std::byte> key, std::span<const std::byte> value,
                                   std::size_t admission_bytes) noexcept -> AcquireResult;

    // Used only when queue publication fails immediately after acquisition.
    [[nodiscard]] auto rollback(Lease lease) noexcept -> bool;

    // Completion-order release is part of the safety contract. Returning false
    // means internal corruption/order violation and must fail the daemon closed.
    [[nodiscard]] auto release(SlotId slot) noexcept -> bool;

    // Writer-only while the corresponding queue cell is owned by the Writer.
    [[nodiscard]] auto view(SlotId slot) const noexcept -> std::optional<PayloadView>;

    [[nodiscard]] auto slot_capacity() const noexcept -> std::size_t {
        return slot_capacity_;
    }

    [[nodiscard]] auto slots_in_use() const noexcept -> std::size_t {
        return slot_capacity_ - free_slots_.size();
    }

    [[nodiscard]] auto byte_capacity() const noexcept -> std::size_t {
        return byte_capacity_;
    }

    [[nodiscard]] auto storage_bytes() const noexcept -> std::size_t {
        return byte_capacity_ + maximum_payload_bytes_;
    }

    [[nodiscard]] auto payload_bytes_in_use() const noexcept -> std::size_t;

    [[nodiscard]] auto admission_bytes_in_use() const noexcept -> std::size_t {
        return admission_bytes_in_use_;
    }

  private:
    struct Slot final {
        std::uint64_t sequence{};
        std::uint64_t begin_cursor{};
        std::uint64_t end_cursor{};
        std::size_t data_offset{};
        std::size_t key_size{};
        std::size_t value_size{};
        std::size_t admission_bytes{};
        bool occupied{};
    };

    const std::size_t slot_capacity_;
    const std::size_t byte_capacity_;
    const std::size_t maximum_payload_bytes_;
    std::unique_ptr<std::byte[]> storage_;
    std::unique_ptr<Slot[]> slots_;
    std::vector<SlotId> free_slots_;
    std::uint64_t head_cursor_{};
    std::uint64_t tail_cursor_{};
    std::uint64_t next_sequence_{};
    std::uint64_t release_sequence_{};
    std::size_t admission_bytes_in_use_{};
};

} // namespace glifistore::store::paired
