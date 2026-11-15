/**
 * @file
 * @brief Ordered response slots for pipelined cold reads on one connection.
 */
#pragma once

// Normative: docs/adr/0045-memory-resident-durable-reads.md §3.

#include "glifistore/server/mutation_window.hpp"
#include "glifistore/server/protocol.hpp"
#include "glifistore/store/value.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace glifistore::server {

/// A connection holds at most this many GET responses behind its oldest cold read.
inline constexpr std::uint32_t kMaximumColdReadWindow = kMaximumMutationWindow;

/// One decided response that owns its value while it waits for an earlier cold read.
struct HeldResponse final {
    ResponseStatus status{ResponseStatus::ok};
    std::uint64_t request_id{};
    std::uint32_t owner_worker{kNoWorker};
    std::uint32_t worker_count{};
    std::uint64_t routing_epoch{};
    OwnedValue value{};

    /// The view borrows `value`; keep this object alive while it is used.
    [[nodiscard]] auto view() const noexcept -> ResponseView {
        return ResponseView{.status = status,
                            .request_id = request_id,
                            .owner_worker = owner_worker,
                            .worker_count = worker_count,
                            .routing_epoch = routing_epoch,
                            .value = value.view()};
    }
};

static_assert(std::is_nothrow_move_constructible_v<HeldResponse>);
static_assert(std::is_nothrow_move_assignable_v<HeldResponse>);

/**
 * @brief FIFO of GET responses for one connection while cold reads are in flight.
 *
 * The oldest slot is always an unfinished cold read while the window is open: a decided head is
 * released at once. Every slot carries the bytes it reserved against the connection output
 * watermark, so the output buffer plus all reservations never exceed the watermark that admitted
 * them. Cold reads complete in submission order because each Reactor owns one FIFO disk-read lane;
 * a completion that does not match the oldest unfinished read is an accounting error.
 */
class OrderedResponseWindow final {
  public:
    struct Released final {
        HeldResponse response;
        std::size_t reserved_bytes{};
    };

    [[nodiscard]] auto empty() const noexcept -> bool {
        return count_ == 0;
    }

    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return count_;
    }

    [[nodiscard]] auto full() const noexcept -> bool {
        return count_ >= kMaximumColdReadWindow;
    }

    [[nodiscard]] auto pending_reads() const noexcept -> std::size_t {
        return pending_reads_;
    }

    [[nodiscard]] auto reserved_bytes() const noexcept -> std::size_t {
        return reserved_bytes_;
    }

    /// Allocate the fixed slot storage once per connection slot; it is kept for later windows.
    [[nodiscard]] auto prepare_storage() noexcept -> bool {
        if (slots_.size() == kMaximumColdReadWindow) {
            return true;
        }
        try {
            slots_.resize(kMaximumColdReadWindow);
            return true;
        } catch (const std::bad_alloc&) {
            return false;
        }
    }

    /// Whether one more slot reserving `reserved_bytes` can be appended.
    [[nodiscard]] auto can_append(const std::size_t reserved_bytes) const noexcept -> bool {
        return slots_.size() == kMaximumColdReadWindow && !full() &&
               reserved_bytes <= std::numeric_limits<std::size_t>::max() - reserved_bytes_;
    }

    /// Append an unfinished cold read; its response is decided by `complete_read`.
    [[nodiscard]] auto admit_read(const std::uint64_t request_id, const std::size_t reserved_bytes) noexcept
        -> bool {
        if (!can_append(reserved_bytes)) {
            return false;
        }
        auto& slot = slots_[(head_ + count_) % kMaximumColdReadWindow];
        slot.response = HeldResponse{.request_id = request_id};
        slot.reserved_bytes = reserved_bytes;
        slot.decided = false;
        ++count_;
        ++pending_reads_;
        reserved_bytes_ += reserved_bytes;
        return true;
    }

    /// Append a decided response behind the unfinished reads. Only valid while the window is open.
    [[nodiscard]] auto hold(HeldResponse response, const std::size_t reserved_bytes) noexcept -> bool {
        if (empty() || !can_append(reserved_bytes)) {
            return false;
        }
        auto& slot = slots_[(head_ + count_) % kMaximumColdReadWindow];
        slot.response = std::move(response);
        slot.reserved_bytes = reserved_bytes;
        slot.decided = true;
        ++count_;
        reserved_bytes_ += reserved_bytes;
        return true;
    }

    /// Decide the oldest unfinished read. Fails when no read is pending or the request id differs.
    [[nodiscard]] auto complete_read(const std::uint64_t request_id, HeldResponse response) noexcept -> bool {
        for (std::size_t index = 0; index < count_; ++index) {
            auto& slot = slots_[(head_ + index) % kMaximumColdReadWindow];
            if (slot.decided) {
                continue;
            }
            if (slot.response.request_id != request_id || response.request_id != request_id) {
                return false;
            }
            slot.response = std::move(response);
            slot.decided = true;
            --pending_reads_;
            return true;
        }
        return false;
    }

    /// Remove the oldest response once it is decided; empty while the head still waits for I/O.
    [[nodiscard]] auto pop_decided() noexcept -> std::optional<Released> {
        if (empty()) {
            return std::nullopt;
        }
        auto& slot = slots_[head_];
        if (!slot.decided) {
            return std::nullopt;
        }
        Released released{.response = std::move(slot.response), .reserved_bytes = slot.reserved_bytes};
        slot.response = HeldResponse{};
        slot.reserved_bytes = 0;
        slot.decided = false;
        head_ = (head_ + 1U) % kMaximumColdReadWindow;
        --count_;
        reserved_bytes_ -= released.reserved_bytes;
        if (count_ == 0) {
            head_ = 0;
        }
        return released;
    }

    /// Drop every slot and its owned value; storage is kept for the next connection in this slot.
    void clear() noexcept {
        for (auto& slot : slots_) {
            slot.response = HeldResponse{};
            slot.reserved_bytes = 0;
            slot.decided = false;
        }
        head_ = 0;
        count_ = 0;
        pending_reads_ = 0;
        reserved_bytes_ = 0;
    }

  private:
    struct Slot final {
        HeldResponse response;
        std::size_t reserved_bytes{};
        bool decided{};
    };

    std::vector<Slot> slots_;
    std::size_t head_{};
    std::size_t count_{};
    std::size_t pending_reads_{};
    std::size_t reserved_bytes_{};
};

} // namespace glifistore::server
