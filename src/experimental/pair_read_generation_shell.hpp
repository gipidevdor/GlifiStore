#pragma once

// ADR 0036 lab-only generation-shell backing storage. This header is available
// only to repository tests and dedicated benchmarks; it is not installed.

#include "experimental/generation_slot_pool.hpp"
#include "glifistore/core/error.hpp"
#include "glifistore/core/worker_routing.hpp"
#include "glifistore/store/paired/generation_direct_storage.hpp"
#include "glifistore/store/paired/read_generation.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <span>
#include <utility>

namespace glifistore::experimental {

// Owns exactly one allocate_shared allocation at a time. Keeping this object in
// a fixed generation slot removes the generation shell/control-block malloc
// after pool initialization. The allocator itself retains shared ownership, so
// the backing bytes cannot disappear before shared_ptr destroys its control
// block.
class PairReadGenerationShellStorage final {
  public:
    static constexpr std::size_t kBytes = 512U;
    static constexpr std::size_t kAlignment = 64U;

    [[nodiscard]] static auto create() -> Result<std::shared_ptr<PairReadGenerationShellStorage>> try {
        return std::make_shared<PairReadGenerationShellStorage>();
    } catch (const std::bad_alloc&) {

        return fail(ErrorCode::resource_exhausted, "generation shell backing-storage allocation failed");
    }

    PairReadGenerationShellStorage() = default;
    PairReadGenerationShellStorage(const PairReadGenerationShellStorage&) = delete;
    auto operator=(const PairReadGenerationShellStorage&) -> PairReadGenerationShellStorage& = delete;

    [[nodiscard]] auto occupied() const noexcept -> bool {
        return occupied_.load(std::memory_order_acquire);
    }

    [[nodiscard]] auto allocation_count() const noexcept -> std::uint64_t {
        return allocation_count_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] auto reuse_count() const noexcept -> std::uint64_t {
        return reuse_count_.load(std::memory_order_relaxed);
    }

  private:
    [[nodiscard]] auto allocate(const std::size_t bytes, const std::size_t alignment) -> void* {
        if (bytes > storage_.size() || alignment > kAlignment ||
            occupied_.exchange(true, std::memory_order_acq_rel)) {
            throw std::bad_alloc{};
        }
        const auto count = allocation_count_.fetch_add(1U, std::memory_order_relaxed);
        if (count != 0U) {
            reuse_count_.fetch_add(1U, std::memory_order_relaxed);
        }
        return storage_.data();
    }

    void deallocate(void* pointer) noexcept {
        if (pointer == storage_.data()) {
            occupied_.store(false, std::memory_order_release);
        }
    }

    alignas(kAlignment) std::array<std::byte, kBytes> storage_{};
    std::atomic_bool occupied_{};
    std::atomic_uint64_t allocation_count_{};
    std::atomic_uint64_t reuse_count_{};

    template <typename T> friend class PairReadGenerationShellAllocator;
};

// Writer-only storage nested in PairReadGenerationInlineSlotPool. Unlike the
// independently shareable storage above, occupancy and telemetry need no
// atomics: the wrapper never lets the constructed shared_ptr escape.
class PairReadGenerationInlineShellStorage final {
  public:
    static constexpr std::size_t kBytes = PairReadGenerationShellStorage::kBytes;
    static constexpr std::size_t kAlignment = PairReadGenerationShellStorage::kAlignment;

    PairReadGenerationInlineShellStorage() = default;

    ~PairReadGenerationInlineShellStorage() {
        if (occupied_) {
            std::terminate();
        }
    }

    PairReadGenerationInlineShellStorage(const PairReadGenerationInlineShellStorage&) = delete;
    auto operator=(const PairReadGenerationInlineShellStorage&)
        -> PairReadGenerationInlineShellStorage& = delete;

    [[nodiscard]] auto allocation_count() const noexcept -> std::uint64_t {
        return allocation_count_;
    }

    [[nodiscard]] auto reuse_count() const noexcept -> std::uint64_t {
        return reuse_count_;
    }

  private:
    [[nodiscard]] auto allocate(const std::size_t bytes, const std::size_t alignment) -> void* {
        if (bytes > storage_.size() || alignment > kAlignment || occupied_) {
            throw std::bad_alloc{};
        }
        occupied_ = true;
        if (allocation_count_ != 0U) {
            ++reuse_count_;
        }
        ++allocation_count_;
        return storage_.data();
    }

    void deallocate(void* pointer) noexcept {
        if (pointer == storage_.data()) {
            occupied_ = false;
        }
    }

    alignas(kAlignment) std::array<std::byte, kBytes> storage_{};
    std::uint64_t allocation_count_{};
    std::uint64_t reuse_count_{};
    bool occupied_{};

    template <typename T> friend class PairReadGenerationBorrowedShellAllocator;
};

// Direct-object storage lives in the production paired namespace. Lab code keeps
// the historical name as an alias so DirectRing / DirectSlotPool stay unchanged.
using PairReadGenerationDirectStorage = store::paired::GenerationDirectStorage;

template <std::size_t Capacity> class PairReadGenerationShellBank final {
  public:
    static_assert(Capacity >= 2U);

    [[nodiscard]] static auto create() -> Result<std::unique_ptr<PairReadGenerationShellBank>> try {
        auto bank = std::make_unique<PairReadGenerationShellBank>();
        for (auto& storage : bank->storage_) {
            auto created = PairReadGenerationShellStorage::create();
            if (!created) {
                return unexpected(std::move(created.error()));
            }
            storage = std::move(*created);
        }
        return bank;
    } catch (const std::bad_alloc&) {

        return fail(ErrorCode::resource_exhausted, "generation shell bank allocation failed");
    }

    PairReadGenerationShellBank() = default;
    PairReadGenerationShellBank(const PairReadGenerationShellBank&) = delete;
    auto operator=(const PairReadGenerationShellBank&) -> PairReadGenerationShellBank& = delete;

    [[nodiscard]] auto at(const std::size_t index) const noexcept
        -> std::shared_ptr<PairReadGenerationShellStorage> {
        return index < Capacity ? storage_[index] : nullptr;
    }

  private:
    std::array<std::shared_ptr<PairReadGenerationShellStorage>, Capacity> storage_{};
};

template <typename T> class PairReadGenerationShellAllocator final {
  public:
    using value_type = T;

    explicit PairReadGenerationShellAllocator(
        std::shared_ptr<PairReadGenerationShellStorage> storage) noexcept
        : storage_(std::move(storage)) {}

    template <typename U>
    PairReadGenerationShellAllocator(const PairReadGenerationShellAllocator<U>& other) noexcept
        : storage_(other.storage_) {}

    [[nodiscard]] auto allocate(const std::size_t count) -> T* {
        // This fixed shell allocator represents exactly one T; accepting any
        // other count would violate the backing-storage contract.
        if (count != 1U) {
            throw std::bad_alloc{};
        }
        return static_cast<T*>(storage_->allocate(sizeof(T), alignof(T)));
    }

    void deallocate(T* pointer, std::size_t) noexcept {
        storage_->deallocate(pointer);
    }

    template <typename U>
    [[nodiscard]] auto operator==(const PairReadGenerationShellAllocator<U>& other) const noexcept -> bool {
        return storage_.get() == other.storage_.get();
    }

  private:
    std::shared_ptr<PairReadGenerationShellStorage> storage_;

    template <typename U> friend class PairReadGenerationShellAllocator;
};

template <typename T> class PairReadGenerationBorrowedShellAllocator final {
  public:
    using value_type = T;

    explicit PairReadGenerationBorrowedShellAllocator(PairReadGenerationInlineShellStorage& storage) noexcept
        : storage_(&storage) {}

    template <typename U>
    PairReadGenerationBorrowedShellAllocator(
        const PairReadGenerationBorrowedShellAllocator<U>& other) noexcept
        : storage_(other.storage_) {}

    [[nodiscard]] auto allocate(const std::size_t count) -> T* {
        // This fixed shell allocator represents exactly one T; accepting any
        // other count would violate the backing-storage contract.
        if (count != 1U) {
            throw std::bad_alloc{};
        }
        return static_cast<T*>(storage_->allocate(sizeof(T), alignof(T)));
    }

    void deallocate(T* pointer, std::size_t) noexcept {
        storage_->deallocate(pointer);
    }

    template <typename U>
    [[nodiscard]] auto operator==(const PairReadGenerationBorrowedShellAllocator<U>& other) const noexcept
        -> bool {
        return storage_ == other.storage_;
    }

  private:
    PairReadGenerationInlineShellStorage* storage_{};

    template <typename U> friend class PairReadGenerationBorrowedShellAllocator;
};

// Narrow friend bridge into PairReadGeneration. It exists only in this
// non-installed header, and cannot be selected by glifistored.
struct PairReadGenerationShellAccess final {
    [[nodiscard]] static auto
    publish_incremental(std::shared_ptr<const store::paired::PairReadGeneration> previous,
                        std::span<const store::paired::ReadMutation> mutations,
                        std::shared_ptr<PairReadGenerationShellStorage> storage,
                        store::paired::PairReadMerge* merge = nullptr)
        -> Result<std::shared_ptr<const store::paired::PairReadGeneration>>;

  private:
    [[nodiscard]] static auto empty_direct(WorkerRoutingState routing,
                                           PairReadGenerationDirectStorage& storage)
        -> Result<const store::paired::PairReadGeneration*>;
    [[nodiscard]] static auto
    publish_incremental_borrowed(std::shared_ptr<const store::paired::PairReadGeneration> previous,
                                 std::span<const store::paired::ReadMutation> mutations,
                                 PairReadGenerationInlineShellStorage& storage)
        -> Result<std::shared_ptr<const store::paired::PairReadGeneration>>;
    [[nodiscard]] static auto
    publish_incremental_direct(const store::paired::PairReadGeneration& previous,
                               std::span<const store::paired::ReadMutation> mutations,
                               PairReadGenerationDirectStorage& storage)
        -> Result<const store::paired::PairReadGeneration*>;
    static void destroy_direct(const store::paired::PairReadGeneration* generation,
                               PairReadGenerationDirectStorage& storage) noexcept;

    template <std::size_t Capacity> friend class PairReadGenerationInlineSlotPool;
    template <std::size_t Capacity> friend class PairReadGenerationDirectRing;
    template <std::size_t Capacity> friend class PairReadGenerationDirectSlotPool;
};

// Construction/performance lab only. There is no Reader reclamation protocol:
// each new generation retires the previous one synchronously on the same
// Writer thread. It cannot be used by glifistored.
template <std::size_t Capacity> class PairReadGenerationDirectRing final {
  public:
    static_assert(Capacity >= 2U);
    using Generation = store::paired::PairReadGeneration;

    explicit PairReadGenerationDirectRing(std::shared_ptr<const Generation> initial) noexcept
        : initial_owner_(std::move(initial)), current_(initial_owner_.get()) {}

    ~PairReadGenerationDirectRing() {
        if (current_slot_) {
            PairReadGenerationShellAccess::destroy_direct(current_, storage_[*current_slot_]);
        }
    }

    PairReadGenerationDirectRing(const PairReadGenerationDirectRing&) = delete;
    auto operator=(const PairReadGenerationDirectRing&) -> PairReadGenerationDirectRing& = delete;

    [[nodiscard]] auto publish(std::span<const store::paired::ReadMutation> mutations)
        -> Result<const Generation*> {
        if (current_ == nullptr) {
            return fail(ErrorCode::invalid_argument, "direct generation ring has no current view");
        }
        const auto next_slot = current_slot_ ? (*current_slot_ + 1U) % Capacity : 0U;
        auto next = PairReadGenerationShellAccess::publish_incremental_direct(*current_, mutations,
                                                                              storage_[next_slot]);
        if (!next) {
            return unexpected(std::move(next.error()));
        }
        if (current_slot_) {
            PairReadGenerationShellAccess::destroy_direct(current_, storage_[*current_slot_]);
        } else {
            initial_owner_.reset();
        }
        current_slot_ = next_slot;
        current_ = *next;
        return current_;
    }

    [[nodiscard]] auto current() const noexcept -> const Generation* {
        return current_;
    }

    [[nodiscard]] auto allocation_count(const std::size_t index) const noexcept -> std::uint64_t {
        return index < Capacity ? storage_[index].allocation_count() : 0U;
    }

    [[nodiscard]] auto reuse_count(const std::size_t index) const noexcept -> std::uint64_t {
        return index < Capacity ? storage_[index].reuse_count() : 0U;
    }

  private:
    std::array<PairReadGenerationDirectStorage, Capacity> storage_{};
    std::shared_ptr<const Generation> initial_owner_;
    const Generation* current_{};
    std::optional<std::size_t> current_slot_;
};

// Lab-only direct-object form of the ADR 0036 SPSC/QSBR protocol. Every
// generation, including epoch zero, lives in one bounded inline slot. The
// release token is the sole publication authority; the Reader announces one
// monotonically advancing safe frontier and never touches a shared_ptr.
template <std::size_t Capacity> class PairReadGenerationDirectSlotPool final {
  public:
    static_assert(Capacity >= 2U);
    static_assert(Capacity <= 65'535U);
    static_assert(std::atomic_uint64_t::is_always_lock_free);
    using Generation = store::paired::PairReadGeneration;

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

        void mark_store_linearized() noexcept {
            store_linearized_ = true;
        }

        [[nodiscard]] auto store_linearized() const noexcept -> bool {
            return store_linearized_;
        }

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
        Reservation(PairReadGenerationDirectSlotPool* owner, const std::size_t slot) noexcept
            : owner_(owner), slot_(slot) {}

        void consume() noexcept {
            owner_ = nullptr;
        }

        PairReadGenerationDirectSlotPool* owner_{};
        std::size_t slot_{};
        bool store_linearized_{};
        friend class PairReadGenerationDirectSlotPool;
    };

    struct ConstructionToken final {
      private:
        ConstructionToken() = default;
        friend class PairReadGenerationDirectSlotPool;
    };

    [[nodiscard]] static auto create(WorkerRoutingState routing,
                                     const GenerationSlotFailureHook failure_hook = {})
        -> Result<std::unique_ptr<PairReadGenerationDirectSlotPool>> {
        try {
            auto pool = std::make_unique<PairReadGenerationDirectSlotPool>(ConstructionToken{}, failure_hook);
            auto initial = PairReadGenerationShellAccess::empty_direct(routing, pool->slots_[0].storage);
            if (!initial) {
                return unexpected(std::move(initial.error()));
            }
            auto& slot = pool->slots_[0];
            slot.generation = *initial;
            slot.publication_count = 1U;
            slot.state = SlotState::published;
            pool->publication_.store(encode(0, 0), std::memory_order_release);
            pool->live_slots_.store(1U, std::memory_order_relaxed);
            pool->live_high_watermark_.store(1U, std::memory_order_relaxed);
            pool->initialized_ = true;
            return pool;
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::resource_exhausted, "direct generation slot pool allocation failed");
        } catch (...) {
            return fail(ErrorCode::internal_error, "direct generation slot pool construction failed");
        }
    }

    PairReadGenerationDirectSlotPool(ConstructionToken, const GenerationSlotFailureHook failure_hook) noexcept
        : failure_hook_(failure_hook) {}

    ~PairReadGenerationDirectSlotPool() {
        if (!initialized_) {
            return;
        }
        const bool orderly = shutdown_complete_;
        for (auto& slot : slots_) {
            destroy_slot(slot);
        }
        if (!orderly) {
            std::terminate();
        }
    }

    PairReadGenerationDirectSlotPool(const PairReadGenerationDirectSlotPool&) = delete;
    auto operator=(const PairReadGenerationDirectSlotPool&) -> PairReadGenerationDirectSlotPool& = delete;

    [[nodiscard]] auto try_reserve() noexcept -> std::optional<Reservation> {
        if (!accepting_.load(std::memory_order_acquire)) {
            shutdown_reservation_rejections_.fetch_add(1U, std::memory_order_relaxed);
            return std::nullopt;
        }
        reclaim();
        for (std::size_t index = 0; index < Capacity; ++index) {
            auto& slot = slots_[index];
            if (slot.state != SlotState::free) {
                continue;
            }
            slot.state = SlotState::building;
            reservations_.fetch_add(1U, std::memory_order_relaxed);
            reserved_slots_.fetch_add(1U, std::memory_order_relaxed);
            return Reservation{this, index};
        }
        pool_exhaustions_.fetch_add(1U, std::memory_order_relaxed);
        return std::nullopt;
    }

    [[nodiscard]] auto publish_incremental(Reservation& reservation,
                                           std::span<const store::paired::ReadMutation> mutations)
        -> GenerationSlotPublishStatus {
        if (reservation.owner_ != this || !reservation.store_linearized_ || reservation.slot_ >= Capacity ||
            slots_[reservation.slot_].state != SlotState::building) {
            return GenerationSlotPublishStatus::invalid_generation;
        }
        auto& slot = slots_[reservation.slot_];
        auto next = PairReadGenerationShellAccess::publish_incremental_direct(
            *slots_[writer_slot_].generation, mutations, slot.storage);
        if (!next) {
            reservation.reset();
            return GenerationSlotPublishStatus::invalid_generation;
        }
        const auto next_epoch = (*next)->epoch();
        if (next_epoch <= writer_epoch_.load(std::memory_order_relaxed) || next_epoch > kMaximumEpoch) {
            PairReadGenerationShellAccess::destroy_direct(*next, slot.storage);
            reservation.reset();
            return next_epoch > kMaximumEpoch ? GenerationSlotPublishStatus::epoch_exhausted
                                              : GenerationSlotPublishStatus::invalid_generation;
        }

        if (slot.publication_count != 0U) {
            slot_reuses_.fetch_add(1U, std::memory_order_relaxed);
        }
        slot.generation = *next;
        slot.epoch = (*next)->epoch();
        slot.visible_through = (*next)->visible_through();
        ++slot.publication_count;
        slot.state = SlotState::published;
        const auto live = live_slots_.fetch_add(1U, std::memory_order_relaxed) + 1U;
        update_high_watermark(live);

        const auto previous_slot = writer_slot_;
        writer_slot_ = reservation.slot_;
        writer_epoch_.store(slot.epoch, std::memory_order_relaxed);
        publication_.store(encode(slot.epoch, writer_slot_), std::memory_order_release);
        slots_[previous_slot].state = SlotState::retired;
        reserved_slots_.fetch_sub(1U, std::memory_order_release);
        reservation.consume();
        publications_.fetch_add(1U, std::memory_order_relaxed);
        return GenerationSlotPublishStatus::published;
    }

    [[nodiscard]] auto
    adopt(const std::uint64_t minimum_borrowed_epoch = std::numeric_limits<std::uint64_t>::max()) noexcept
        -> const Generation* {
        if (reader_quiescent_.load(std::memory_order_acquire)) {
            invalid_adoptions_.fetch_add(1U, std::memory_order_relaxed);
            return nullptr;
        }
        const auto token = publication_.load(std::memory_order_acquire);
        if (token == 0U) {
            invalid_adoptions_.fetch_add(1U, std::memory_order_relaxed);
            return nullptr;
        }
        const auto index = decode_slot(token);
        const auto epoch = decode_epoch(token);
        if (index >= Capacity || slots_[index].generation == nullptr || slots_[index].epoch != epoch) {
            invalid_adoptions_.fetch_add(1U, std::memory_order_relaxed);
            return nullptr;
        }
        const auto safe_epoch = std::min(epoch, minimum_borrowed_epoch);
        const auto previous_safe_epoch = reader_safe_epoch_.load(std::memory_order_relaxed);
        if (safe_epoch < previous_safe_epoch) {
            invalid_adoptions_.fetch_add(1U, std::memory_order_relaxed);
            return nullptr;
        }
        reader_generation_ = slots_[index].generation;
        if (epoch != reader_epoch_.load(std::memory_order_relaxed)) {
            reader_epoch_.store(epoch, std::memory_order_relaxed);
        }
        if (safe_epoch != previous_safe_epoch) {
            reader_safe_epoch_.store(safe_epoch, std::memory_order_release);
        }
        return reader_generation_;
    }

    void reclaim() noexcept {
        const auto safe_epoch = reader_safe_epoch_.load(std::memory_order_acquire);
        if (safe_epoch == 0U) {
            return;
        }
        for (auto& slot : slots_) {
            if (slot.state != SlotState::retired || slot.epoch >= safe_epoch) {
                continue;
            }
            destroy_slot(slot);
            slot.epoch = 0U;
            slot.visible_through = 0U;
            slot.state = SlotState::free;
            live_slots_.fetch_sub(1U, std::memory_order_relaxed);
            slots_reclaimed_.fetch_add(1U, std::memory_order_relaxed);
        }
    }

    void stop_admission() noexcept {
        if (accepting_.exchange(false, std::memory_order_acq_rel)) {
            shutdown_starts_.fetch_add(1U, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] auto accepting() const noexcept -> bool {
        return accepting_.load(std::memory_order_acquire);
    }

    [[nodiscard]] auto mark_reader_quiescent() noexcept -> bool {
        if (accepting_.load(std::memory_order_acquire) ||
            reserved_slots_.load(std::memory_order_acquire) != 0U) {
            return false;
        }
        if (reader_quiescent_.load(std::memory_order_relaxed)) {
            return true;
        }
        const auto epoch = writer_epoch_.load(std::memory_order_acquire);
        reader_generation_ = nullptr;
        reader_epoch_.store(epoch, std::memory_order_relaxed);
        reader_safe_epoch_.store(epoch + 1U, std::memory_order_release);
        reader_quiescent_.store(true, std::memory_order_release);
        return true;
    }

    [[nodiscard]] auto try_finish_shutdown() noexcept -> bool {
        if (accepting_.load(std::memory_order_acquire) ||
            reserved_slots_.load(std::memory_order_acquire) != 0U ||
            !reader_quiescent_.load(std::memory_order_acquire)) {
            return false;
        }
        reclaim();
        shutdown_complete_ = live_slots_.load(std::memory_order_relaxed) == 1U;
        return shutdown_complete_;
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

    [[nodiscard]] auto shell_allocation_count(const std::size_t index) const noexcept -> std::uint64_t {
        return index < Capacity ? slots_[index].storage.allocation_count() : 0U;
    }

    [[nodiscard]] auto shell_reuse_count(const std::size_t index) const noexcept -> std::uint64_t {
        return index < Capacity ? slots_[index].storage.reuse_count() : 0U;
    }

  private:
    enum class SlotState : std::uint8_t { free, building, published, retired };

    struct Slot final {
        PairReadGenerationDirectStorage storage;
        const Generation* generation{};
        std::uint64_t epoch{};
        std::uint64_t visible_through{};
        std::uint64_t publication_count{};
        SlotState state{SlotState::free};
    };

    static constexpr std::uint64_t kSlotBits = 16U;
    static constexpr std::uint64_t kSlotMask = (1ULL << kSlotBits) - 1U;
    static constexpr std::uint64_t kMaximumEpoch = std::numeric_limits<std::uint64_t>::max() >> kSlotBits;

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

    static void destroy_slot(Slot& slot) noexcept {
        if (slot.generation != nullptr) {
            PairReadGenerationShellAccess::destroy_direct(slot.generation, slot.storage);
            slot.generation = nullptr;
        }
    }

    void update_high_watermark(const std::size_t value) noexcept {
        auto observed = live_high_watermark_.load(std::memory_order_relaxed);
        while (value > observed &&
               !live_high_watermark_.compare_exchange_weak(observed, value, std::memory_order_relaxed,
                                                           std::memory_order_relaxed)) {
        }
    }

    void cancel_reservation(const std::size_t index, const bool linearized) noexcept {
        if (index >= Capacity || slots_[index].state != SlotState::building) {
            return;
        }
        slots_[index].state = SlotState::free;
        reserved_slots_.fetch_sub(1U, std::memory_order_release);
        reservation_cancellations_.fetch_add(1U, std::memory_order_relaxed);
        if (!linearized) {
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
    const Generation* reader_generation_{};
    std::atomic_uint64_t reader_epoch_{};
    std::size_t writer_slot_{};
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
    bool initialized_{};
    bool shutdown_complete_{};
};

// Owns inline shell bytes and the publication pool as one non-movable lifetime
// domain. Member order is normative: pool_ is destroyed before shell_bank_, so
// every allocate_shared control block is gone before its borrowed backing bytes.
// Built shared_ptrs never escape this type.
template <std::size_t Capacity> class PairReadGenerationInlineSlotPool final {
  public:
    using Generation = store::paired::PairReadGeneration;
    using Pool = GenerationSlotPool<Generation, Capacity>;
    using Reservation = typename Pool::Reservation;

    struct ConstructionToken final {
      private:
        ConstructionToken() = default;
        friend class PairReadGenerationInlineSlotPool;
    };

    [[nodiscard]] static auto create(std::shared_ptr<const Generation> initial,
                                     const GenerationSlotFailureHook failure_hook = {})
        -> Result<std::unique_ptr<PairReadGenerationInlineSlotPool>> {
        auto pool = Pool::create(std::move(initial), failure_hook);
        if (!pool) {
            return unexpected(std::move(pool.error()));
        }
        try {
            return std::make_unique<PairReadGenerationInlineSlotPool>(ConstructionToken{}, std::move(*pool));
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::resource_exhausted, "inline generation slot pool allocation failed");
        }
    }

    PairReadGenerationInlineSlotPool(ConstructionToken, std::unique_ptr<Pool> pool) noexcept
        : pool_(std::move(pool)) {}

    PairReadGenerationInlineSlotPool(const PairReadGenerationInlineSlotPool&) = delete;
    auto operator=(const PairReadGenerationInlineSlotPool&) -> PairReadGenerationInlineSlotPool& = delete;

    [[nodiscard]] auto try_reserve() noexcept -> std::optional<Reservation> {
        return pool_->try_reserve();
    }

    [[nodiscard]] auto publish_incremental(Reservation& reservation,
                                           std::span<const store::paired::ReadMutation> mutations)
        -> GenerationSlotPublishStatus {
        if (!reservation.store_linearized() || reservation.slot_index() >= Capacity) {
            return GenerationSlotPublishStatus::invalid_generation;
        }
        auto next = PairReadGenerationShellAccess::publish_incremental_borrowed(
            pool_->writer_generation_owner(), mutations, shell_bank_[reservation.slot_index()]);
        if (!next) {
            reservation.reset();
            return GenerationSlotPublishStatus::invalid_generation;
        }
        const auto status = pool_->commit(reservation, std::move(*next));
        if (status != GenerationSlotPublishStatus::published) {
            reservation.reset();
        }
        return status;
    }

    [[nodiscard]] auto
    adopt(const std::uint64_t minimum_borrowed_epoch = std::numeric_limits<std::uint64_t>::max()) noexcept
        -> const Generation* {
        return pool_->adopt(minimum_borrowed_epoch);
    }

    void reclaim() noexcept {
        pool_->reclaim();
    }

    [[nodiscard]] auto stats() const noexcept -> GenerationSlotPoolStats {
        return pool_->stats();
    }

    [[nodiscard]] auto shell_allocation_count(const std::size_t index) const noexcept -> std::uint64_t {
        return index < Capacity ? shell_bank_[index].allocation_count() : 0U;
    }

    [[nodiscard]] auto shell_reuse_count(const std::size_t index) const noexcept -> std::uint64_t {
        return index < Capacity ? shell_bank_[index].reuse_count() : 0U;
    }

  private:
    // Declared before pool_: reverse destruction destroys pool_ first.
    std::array<PairReadGenerationInlineShellStorage, Capacity> shell_bank_{};
    std::unique_ptr<Pool> pool_;
};

} // namespace glifistore::experimental
