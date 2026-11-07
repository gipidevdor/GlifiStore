/**
 * @file
 * @brief Bounded generation-slot reservation, publication, and quiescent reclamation.
 */
#pragma once

// ADR 0036 Wave 1 production generation slot pool. Opt-in via
// PairedConcurrencyConfig::generation_slot_pool (default false). Alternative A
// shared_ptr publication remains the bit-identical default path.

#include "glifistore/core/error.hpp"
#include "glifistore/core/worker_routing.hpp"
#include "glifistore/persistence/runtime_catalog.hpp"
#include "glifistore/store/paired/generation_direct_storage.hpp"
#include "glifistore/store/paired/read_generation.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace glifistore::store::paired {

/**
 * @brief Publication versus bounded-capacity, identity, or epoch exhaustion failure.
 */
enum class GenerationSlotPublishStatus : std::uint8_t {
    published,
    pool_exhausted,
    invalid_generation,
    epoch_exhausted,
};

/**
 * @brief Non-owning callback used to fail the owning runtime closed on slot-identity failure.
 */
struct GenerationSlotFailureHook final {
    void* context{};
    void (*fail_closed)(void* context) noexcept {};
};

/// Capacity is one current generation plus MaximumRetired retirement slots.
template <std::size_t MaximumRetired> struct GenerationSlotCapacity final {
    static_assert(MaximumRetired <= 65'534U);
    static constexpr std::size_t value = MaximumRetired + 1U;
};

/**
 * @brief Bounded slot occupancy, retirement, publication, and rejection observations.
 */
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

/// Single 64-bit release token: {epoch:48, slot+1:16}. Zero is never published.
struct GenerationPublicationToken final {
    static constexpr std::uint64_t kSlotBits = 16U;
    static constexpr std::uint64_t kSlotMask = (1ULL << kSlotBits) - 1U;
    static constexpr std::uint64_t kMaximumEpoch = std::numeric_limits<std::uint64_t>::max() >> kSlotBits;

    std::uint64_t raw{};

    [[nodiscard]] static constexpr auto encode(const std::uint64_t epoch, const std::size_t slot) noexcept
        -> GenerationPublicationToken {
        return GenerationPublicationToken{.raw =
                                              (epoch << kSlotBits) | (static_cast<std::uint64_t>(slot) + 1U)};
    }

    [[nodiscard]] constexpr auto empty() const noexcept -> bool {
        return raw == 0U;
    }

    [[nodiscard]] constexpr auto epoch() const noexcept -> std::uint64_t {
        return raw >> kSlotBits;
    }

    [[nodiscard]] constexpr auto slot_index() const noexcept -> std::size_t {
        return static_cast<std::size_t>((raw & kSlotMask) - 1U);
    }
};

/// Fixed-capacity direct-object slot pool for PairReadGeneration. Slots own the
/// graph; Readers retain only a raw pointer after one acquire adoption.
class GenerationSlotPool final {
  public:
    static constexpr std::size_t kCapacity = GenerationSlotCapacity<64>::value;
    static_assert(kCapacity == 65U);
    static_assert(std::atomic_uint64_t::is_always_lock_free);

    using Generation = PairReadGeneration;

    /**
     * @brief Move-only Writer reservation obtained before mutation authority changes.
     * @note The pool must outlive this token. Destruction cancels an unused reservation;
     * a marked linearization without publication triggers the pool containment path.
     */
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

    [[nodiscard]] static auto create(WorkerRoutingState routing,
                                     std::span<const DurableRuntimeCatalog::PublishedReadRecord> records = {},
                                     GenerationSlotFailureHook failure_hook = {})
        -> Result<std::unique_ptr<GenerationSlotPool>>;

    GenerationSlotPool(const GenerationSlotPool&) = delete;
    auto operator=(const GenerationSlotPool&) -> GenerationSlotPool& = delete;
    ~GenerationSlotPool();

    /**
     * @brief Reserve a free generation slot before Store entry; nullopt applies bounded backpressure.
     */
    [[nodiscard]] auto try_reserve() noexcept -> std::optional<Reservation>;

    [[nodiscard]] auto publish_incremental(Reservation& reservation, std::span<const ReadMutation> mutations,
                                           PairReadMerge* merge = nullptr) -> GenerationSlotPublishStatus;

    /// Commit a generation already constructed into storage_at(reservation.slot_index()).
    [[nodiscard]] auto publish_direct(Reservation& reservation, const Generation* generation) noexcept
        -> GenerationSlotPublishStatus;

    /**
     * @brief Retain a live slot for a counted lease; balance with unpin() before slot reuse.
     */
    void pin(std::size_t slot_index) noexcept;
    /**
     * @brief Release one counted slot pin; caller must own a matching live pin.
     */
    void unpin(std::size_t slot_index) noexcept;

    [[nodiscard]] auto
    adopt(std::uint64_t minimum_borrowed_epoch = std::numeric_limits<std::uint64_t>::max()) noexcept
        -> const Generation*;
    [[nodiscard]] auto decode_published(GenerationPublicationToken token) const noexcept -> const Generation*;

    /**
     * @brief Writer-only retirement collection bounded by the supplied safe epoch and outstanding pins.
     */
    void reclaim(std::uint64_t safe_epoch) noexcept;
    void stop_admission() noexcept;
    [[nodiscard]] auto accepting() const noexcept -> bool;
    [[nodiscard]] auto mark_reader_quiescent() noexcept -> bool;
    [[nodiscard]] auto try_finish_shutdown() noexcept -> bool;
    void revoke_publication() noexcept;

    [[nodiscard]] auto stats() const noexcept -> GenerationSlotPoolStats;
    [[nodiscard]] auto writer_generation() const noexcept -> const Generation*;
    [[nodiscard]] auto writer_slot() const noexcept -> std::size_t;
    [[nodiscard]] auto retired_count() const noexcept -> std::size_t;
    [[nodiscard]] auto publication_token() const noexcept -> GenerationPublicationToken;
    [[nodiscard]] auto storage_at(std::size_t slot_index) noexcept -> GenerationDirectStorage&;
    [[nodiscard]] auto shell_allocation_count(std::size_t index) const noexcept -> std::uint64_t;
    [[nodiscard]] auto shell_reuse_count(std::size_t index) const noexcept -> std::uint64_t;
    void set_failure_hook(GenerationSlotFailureHook failure_hook) noexcept;

  private:
    enum class SlotState : std::uint8_t { free, building, published, retired };

    struct Slot final {
        GenerationDirectStorage storage;
        const Generation* generation{};
        std::uint64_t epoch{};
        std::uint64_t visible_through{};
        std::uint64_t publication_count{};
        std::uint32_t pins{};
        SlotState state{SlotState::free};
    };

    struct ConstructionToken final {
      private:
        ConstructionToken() = default;
        friend class GenerationSlotPool;
    };

    explicit GenerationSlotPool(ConstructionToken, GenerationSlotFailureHook failure_hook) noexcept;

    void cancel_reservation(std::size_t index, bool linearized) noexcept;
    void destroy_slot(Slot& slot) noexcept;
    void update_high_watermark(std::size_t value) noexcept;
    [[nodiscard]] auto commit_published(Reservation& reservation, const Generation* generation) noexcept
        -> GenerationSlotPublishStatus;

    std::array<Slot, kCapacity> slots_{};
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

} // namespace glifistore::store::paired
