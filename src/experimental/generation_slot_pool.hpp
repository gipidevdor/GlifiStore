#pragma once

// ADR 0036 lab-only candidate. This header is compiled only by tests and
// dedicated benchmarks; it is not installed or linked into glifistored.

#include "glifistore/core/error.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace glifistore::experimental {

enum class GenerationSlotPublishStatus : std::uint8_t {
    published,
    pool_exhausted,
    invalid_generation,
    epoch_exhausted,
};

struct GenerationSlotFailureHook final {
    void* context{};
    void (*fail_closed)(void* context) noexcept {};
};

// Capacity is one current generation plus MaximumRetired retirement slots. At
// debt MaximumRetired-1, the last unoccupied retirement slot is reserved before
// Store mutation; commit turns the old current into the final retired slot while
// the building slot becomes current, so no +2 margin is needed.
template <std::size_t MaximumRetired> struct GenerationSlotCapacity final {
    static_assert(MaximumRetired <= 65'534U);
    static constexpr std::size_t value = MaximumRetired + 1U;
};

struct GenerationSlotPoolStats final {
    std::uint64_t publications{};
    std::uint64_t slot_reuses{};
    std::uint64_t slots_reclaimed{};
    std::uint64_t pool_exhaustions{};
    std::uint64_t invalid_adoptions{};
    std::uint64_t reservations{};
    std::uint64_t reservation_cancellations{};
    std::uint64_t unpublished_linearizations{};
    std::uint64_t shutdown_starts{};
    std::uint64_t shutdown_reservation_rejections{};
    std::size_t reserved_slots{};
    std::size_t live_slots{};
    std::size_t live_high_watermark{};
    std::uint64_t writer_epoch{};
    std::uint64_t reader_epoch{};
    std::uint64_t reader_safe_epoch{};
    bool accepting{};
    bool reader_quiescent{};
};

// Single-Writer/single-Reader publication protocol over the real immutable
// generation type. Slots own the graph; Readers retain only a raw pointer after
// one acquire adoption per turn. Slow work extends lifetime by passing its
// minimum borrowed epoch to adopt().
template <typename Generation, std::size_t Capacity> class GenerationSlotPool final {
  public:
    static_assert(Capacity >= 2U);
    static_assert(Capacity <= 65'535U);
    static_assert(std::atomic_uint64_t::is_always_lock_free);

    class Reservation final {
      public:
        Reservation(Reservation&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), slot_(other.slot_),
              store_linearized_(other.store_linearized_) {}

        auto operator=(Reservation&& other) noexcept -> Reservation& {
            if (this != &other) {
                reset();
                owner_ = std::exchange(other.owner_, nullptr);
                slot_ = other.slot_;
                store_linearized_ = other.store_linearized_;
            }
            return *this;
        }

        Reservation(const Reservation&) = delete;
        auto operator=(const Reservation&) -> Reservation& = delete;

        ~Reservation() {
            reset();
        }

        // Call only after the Store mutation has crossed its logical
        // linearization point. Abandoning this guard then arms fail-closed.
        void mark_store_linearized() noexcept {
            store_linearized_ = true;
        }

        [[nodiscard]] auto store_linearized() const noexcept -> bool {
            return store_linearized_;
        }

        // Identifies the already reserved backing slot. Candidate generation
        // builders use it to select the matching fixed shell storage before
        // Store linearization; it does not transfer slot ownership.
        [[nodiscard]] auto slot_index() const noexcept -> std::size_t {
            return slot_;
        }

        void reset() noexcept {
            if (owner_ != nullptr) {
                auto* owner = std::exchange(owner_, nullptr);
                owner->cancel_reservation(slot_, store_linearized_);
            }
        }

      private:
        Reservation(GenerationSlotPool* owner, const std::size_t slot) noexcept
            : owner_(owner), slot_(slot) {}

        void consume() noexcept {
            owner_ = nullptr;
        }

        GenerationSlotPool* owner_{};
        std::size_t slot_{};
        bool store_linearized_{};

        friend class GenerationSlotPool;
    };

    [[nodiscard]] static auto create(std::shared_ptr<const Generation> initial,
                                     const GenerationSlotFailureHook failure_hook = {})
        -> Result<std::unique_ptr<GenerationSlotPool>> {
        if (!initial) {
            return fail(ErrorCode::invalid_argument, "generation slot pool requires an initial graph");
        }
        if (initial->epoch() > kMaximumEpoch) {
            return fail(ErrorCode::arithmetic_overflow, "generation slot pool initial epoch is too large");
        }
        try {
            return std::unique_ptr<GenerationSlotPool>{
                new GenerationSlotPool{std::move(initial), failure_hook}};
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::resource_exhausted, "generation slot pool allocation failed");
        }
    }

    GenerationSlotPool(const GenerationSlotPool&) = delete;
    auto operator=(const GenerationSlotPool&) -> GenerationSlotPool& = delete;

    // Writer-only. Reserve before entering Store mutation. Pool exhaustion is
    // therefore a known pre-linearization rejection, never an ambiguous
    // post-mutation publication failure.
    [[nodiscard]] auto try_reserve() noexcept -> std::optional<Reservation> {
        // This acquire load is the admission linearization point. A Writer
        // that observed true is already admitted and must be drained; one
        // that observes false cannot enter Store mutation.
        if (!accepting_.load(std::memory_order_acquire)) {
            shutdown_reservation_rejections_.fetch_add(1U, std::memory_order_relaxed);
            return std::nullopt;
        }
        reclaim();
        std::size_t next_slot = Capacity;
        for (std::size_t index = 0; index < Capacity; ++index) {
            if (slots_[index].state == SlotState::free) {
                next_slot = index;
                break;
            }
        }
        if (next_slot == Capacity) {
            pool_exhaustions_.fetch_add(1U, std::memory_order_relaxed);
            return std::nullopt;
        }

        slots_[next_slot].state = SlotState::building;
        reservations_.fetch_add(1U, std::memory_order_relaxed);
        reserved_slots_.fetch_add(1U, std::memory_order_relaxed);
        return Reservation{this, next_slot};
    }

    // Writer-only. The graph must already be complete and immutable. Failure
    // leaves the reservation owned by the guard; its destructor either cancels
    // safely or arms fail-closed if Store was already linearized.
    [[nodiscard]] auto commit(Reservation& reservation, std::shared_ptr<const Generation> next) noexcept
        -> GenerationSlotPublishStatus {
        if (reservation.owner_ != this || reservation.slot_ >= Capacity ||
            slots_[reservation.slot_].state != SlotState::building || !next ||
            next->epoch() <= writer_epoch_.load(std::memory_order_relaxed)) {
            return GenerationSlotPublishStatus::invalid_generation;
        }
        if (next->epoch() > kMaximumEpoch) {
            return GenerationSlotPublishStatus::epoch_exhausted;
        }

        const auto next_slot = reservation.slot_;
        auto& slot = slots_[next_slot];
        if (slot.publication_count != 0) {
            slot_reuses_.fetch_add(1U, std::memory_order_relaxed);
        }
        slot.generation = std::move(next);
        slot.epoch = slot.generation->epoch();
        slot.visible_through = slot.generation->visible_through();
        ++slot.publication_count;
        slot.state = SlotState::published;
        const auto live = live_slots_.fetch_add(1U, std::memory_order_relaxed) + 1U;
        update_high_watermark(live_high_watermark_, live);

        const auto previous_slot = writer_slot_;
        writer_slot_ = next_slot;
        writer_epoch_.store(slot.epoch, std::memory_order_relaxed);
        publication_.store(encode(slot.epoch, next_slot), std::memory_order_release);
        slots_[previous_slot].state = SlotState::retired;
        reserved_slots_.fetch_sub(1U, std::memory_order_release);
        reservation.consume();
        publications_.fetch_add(1U, std::memory_order_relaxed);
        return GenerationSlotPublishStatus::published;
    }

    // Convenience for publication paths that do not cross a Store mutation.
    [[nodiscard]] auto try_publish(std::shared_ptr<const Generation> next) noexcept
        -> GenerationSlotPublishStatus {
        auto reservation = try_reserve();
        if (!reservation) {
            return GenerationSlotPublishStatus::pool_exhausted;
        }
        return commit(*reservation, std::move(next));
    }

    // Reader-only, once per event-loop turn. minimum_borrowed_epoch is the
    // oldest epoch still used by asynchronous cold I/O/output; omit it when no
    // work crosses the turn. The returned pointer is valid until the next
    // adoption, or longer while its epoch remains the published minimum.
    [[nodiscard]] auto
    adopt(const std::uint64_t minimum_borrowed_epoch = std::numeric_limits<std::uint64_t>::max()) noexcept
        -> const Generation* {
        if (reader_quiescent_.load(std::memory_order_acquire)) {
            invalid_adoptions_.fetch_add(1U, std::memory_order_relaxed);
            return nullptr;
        }
        const auto token = publication_.load(std::memory_order_acquire);
        if (token == 0) {
            invalid_adoptions_.fetch_add(1U, std::memory_order_relaxed);
            return nullptr;
        }
        const auto index = decode_slot(token);
        const auto epoch = decode_epoch(token);
        if (index >= Capacity || slots_[index].epoch != epoch || !slots_[index].generation) {
            invalid_adoptions_.fetch_add(1U, std::memory_order_relaxed);
            return nullptr;
        }

        const auto safe_epoch = minimum_borrowed_epoch < epoch ? minimum_borrowed_epoch : epoch;
        const auto previous_safe_epoch = reader_safe_epoch_.load(std::memory_order_relaxed);
        // A borrow must be announced while its generation is still adopted.
        // Regressing this frontier could attempt to resurrect an epoch already
        // reclaimed by the Writer, so reject without changing Reader state.
        if (safe_epoch < previous_safe_epoch) {
            invalid_adoptions_.fetch_add(1U, std::memory_order_relaxed);
            return nullptr;
        }
        reader_generation_ = slots_[index].generation.get();
        if (epoch != reader_epoch_.load(std::memory_order_relaxed)) {
            reader_epoch_.store(epoch, std::memory_order_relaxed);
        }
        // Repeating the same frontier cannot enable reclaim. Avoid bouncing
        // this cache line between Reader and Writer until the epoch advances.
        if (safe_epoch != previous_safe_epoch) {
            reader_safe_epoch_.store(safe_epoch, std::memory_order_release);
        }
        return reader_generation_;
    }

    // Writer-only. This is the shutdown admission linearization point, called
    // after the Reader has stopped feeding the mutation lane.
    // Reservations that already observed true remain valid and must be
    // committed or cancelled before Reader quiescence.
    void stop_admission() noexcept {
        if (accepting_.exchange(false, std::memory_order_acq_rel)) {
            shutdown_starts_.fetch_add(1U, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] auto accepting() const noexcept -> bool {
        return accepting_.load(std::memory_order_acquire);
    }

    // Reader-only terminal transition. The caller must first complete every
    // cold-I/O/output borrow. The counter check proves that no admitted Writer
    // reservation can still publish after this point.
    [[nodiscard]] auto mark_reader_quiescent() noexcept -> bool {
        if (accepting_.load(std::memory_order_acquire) ||
            reserved_slots_.load(std::memory_order_acquire) != 0U) {
            return false;
        }
        if (reader_quiescent_.load(std::memory_order_relaxed)) {
            return true;
        }

        const auto terminal_epoch = writer_epoch_.load(std::memory_order_acquire);
        reader_generation_ = nullptr;
        reader_epoch_.store(terminal_epoch, std::memory_order_relaxed);
        reader_safe_epoch_.store(terminal_epoch + 1U, std::memory_order_release);
        reader_quiescent_.store(true, std::memory_order_release);
        return true;
    }

    // Writer-only. Success means every retired slot was reclaimed
    // and the pool owns exactly the final published generation.
    [[nodiscard]] auto try_finish_shutdown() noexcept -> bool {
        if (accepting_.load(std::memory_order_acquire) ||
            reserved_slots_.load(std::memory_order_acquire) != 0U ||
            !reader_quiescent_.load(std::memory_order_acquire)) {
            return false;
        }
        reclaim();
        return live_slots_.load(std::memory_order_relaxed) == 1U;
    }

    // Writer-only opportunistic reclaim. The release safe frontier orders all
    // Reader accesses to an older graph before the reset below.
    void reclaim() noexcept {
        const auto safe_epoch = reader_safe_epoch_.load(std::memory_order_acquire);
        if (safe_epoch == 0) {
            return;
        }
        for (auto& slot : slots_) {
            if (slot.state != SlotState::retired || slot.epoch >= safe_epoch) {
                continue;
            }
            slot.generation.reset();
            slot.epoch = 0;
            slot.visible_through = 0;
            slot.state = SlotState::free;
            live_slots_.fetch_sub(1U, std::memory_order_relaxed);
            slots_reclaimed_.fetch_add(1U, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] auto reader_generation() const noexcept -> const Generation* {
        return reader_generation_;
    }

    // Writer-only owner view used by the structurally nested shell candidate.
    // The reference is invalidated by the next successful commit or reclaim.
    [[nodiscard]] auto writer_generation_owner() const noexcept -> const std::shared_ptr<const Generation>& {
        return slots_[writer_slot_].generation;
    }

    [[nodiscard]] auto stats() const noexcept -> GenerationSlotPoolStats {
        return {.publications = publications_.load(std::memory_order_relaxed),
                .slot_reuses = slot_reuses_.load(std::memory_order_relaxed),
                .slots_reclaimed = slots_reclaimed_.load(std::memory_order_relaxed),
                .pool_exhaustions = pool_exhaustions_.load(std::memory_order_relaxed),
                .invalid_adoptions = invalid_adoptions_.load(std::memory_order_relaxed),
                .reservations = reservations_.load(std::memory_order_relaxed),
                .reservation_cancellations = reservation_cancellations_.load(std::memory_order_relaxed),
                .unpublished_linearizations = unpublished_linearizations_.load(std::memory_order_relaxed),
                .shutdown_starts = shutdown_starts_.load(std::memory_order_relaxed),
                .shutdown_reservation_rejections =
                    shutdown_reservation_rejections_.load(std::memory_order_relaxed),
                .reserved_slots = reserved_slots_.load(std::memory_order_relaxed),
                .live_slots = live_slots_.load(std::memory_order_relaxed),
                .live_high_watermark = live_high_watermark_.load(std::memory_order_relaxed),
                .writer_epoch = writer_epoch_.load(std::memory_order_relaxed),
                .reader_epoch = reader_epoch_.load(std::memory_order_relaxed),
                .reader_safe_epoch = reader_safe_epoch_.load(std::memory_order_relaxed),
                .accepting = accepting_.load(std::memory_order_relaxed),
                .reader_quiescent = reader_quiescent_.load(std::memory_order_relaxed)};
    }

  private:
    enum class SlotState : std::uint8_t { free, building, published, retired };

    struct Slot final {
        std::shared_ptr<const Generation> generation;
        std::uint64_t epoch{};
        std::uint64_t visible_through{};
        std::uint64_t publication_count{};
        SlotState state{SlotState::free};
    };

    static constexpr std::uint64_t kSlotBits = 16U;
    static constexpr std::uint64_t kSlotMask = (1ULL << kSlotBits) - 1U;
    static constexpr std::uint64_t kMaximumEpoch = std::numeric_limits<std::uint64_t>::max() >> kSlotBits;

    explicit GenerationSlotPool(std::shared_ptr<const Generation> initial,
                                const GenerationSlotFailureHook failure_hook) noexcept
        : failure_hook_(failure_hook) {
        auto& slot = slots_[0];
        slot.epoch = initial->epoch();
        slot.visible_through = initial->visible_through();
        slot.generation = std::move(initial);
        slot.publication_count = 1;
        slot.state = SlotState::published;
        writer_epoch_.store(slot.epoch, std::memory_order_relaxed);
        publication_.store(encode(slot.epoch, 0), std::memory_order_release);
        live_slots_.store(1U, std::memory_order_relaxed);
        live_high_watermark_.store(1U, std::memory_order_relaxed);
    }

    [[nodiscard]] static constexpr auto encode(const std::uint64_t epoch, const std::size_t slot) noexcept
        -> std::uint64_t {
        return (epoch << kSlotBits) | (static_cast<std::uint64_t>(slot) + 1U);
    }

    [[nodiscard]] static constexpr auto decode_epoch(const std::uint64_t token) noexcept -> std::uint64_t {
        return token >> kSlotBits;
    }

    [[nodiscard]] static constexpr auto decode_slot(const std::uint64_t token) noexcept -> std::size_t {
        return static_cast<std::size_t>((token & kSlotMask) - 1U);
    }

    static void update_high_watermark(std::atomic_size_t& target, const std::size_t value) noexcept {
        auto observed = target.load(std::memory_order_relaxed);
        while (value > observed && !target.compare_exchange_weak(observed, value, std::memory_order_relaxed,
                                                                 std::memory_order_relaxed)) {
        }
    }

    void cancel_reservation(const std::size_t slot_index, const bool store_linearized) noexcept {
        if (slot_index >= Capacity || slots_[slot_index].state != SlotState::building) {
            return;
        }
        slots_[slot_index].state = SlotState::free;
        reserved_slots_.fetch_sub(1U, std::memory_order_release);
        reservation_cancellations_.fetch_add(1U, std::memory_order_relaxed);
        if (!store_linearized) {
            return;
        }
        unpublished_linearizations_.fetch_add(1U, std::memory_order_relaxed);
        if (failure_hook_.fail_closed != nullptr) {
            failure_hook_.fail_closed(failure_hook_.context);
        }
    }

    std::array<Slot, Capacity> slots_{};
    alignas(128) std::atomic_uint64_t publication_{};
    alignas(128) std::atomic_uint64_t reader_safe_epoch_{};
    const Generation* reader_generation_{}; // Reader-only.
    std::atomic_uint64_t reader_epoch_{};
    std::size_t writer_slot_{}; // Writer-only.
    std::atomic_uint64_t writer_epoch_{};
    std::atomic_size_t live_slots_{};
    std::atomic_size_t live_high_watermark_{};
    std::atomic_uint64_t publications_{};
    std::atomic_uint64_t slot_reuses_{};
    std::atomic_uint64_t slots_reclaimed_{};
    std::atomic_uint64_t pool_exhaustions_{};
    std::atomic_uint64_t invalid_adoptions_{};
    std::atomic_uint64_t reservations_{};
    std::atomic_uint64_t reservation_cancellations_{};
    std::atomic_uint64_t unpublished_linearizations_{};
    std::atomic_uint64_t shutdown_starts_{};
    std::atomic_uint64_t shutdown_reservation_rejections_{};
    std::atomic_size_t reserved_slots_{};
    std::atomic_bool accepting_{true};
    std::atomic_bool reader_quiescent_{};
    GenerationSlotFailureHook failure_hook_{};
};

} // namespace glifistore::experimental
