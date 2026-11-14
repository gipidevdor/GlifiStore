#include "glifistore/store/paired/generation_slot_pool.hpp"

#include <algorithm>
#include <exception>
#include <new>
#include <utility>

namespace glifistore::store::paired {

GenerationSlotPool::GenerationSlotPool(ConstructionToken,
                                       const GenerationSlotFailureHook failure_hook) noexcept
    : failure_hook_(failure_hook) {}

GenerationSlotPool::~GenerationSlotPool() {
    if (!initialized_) {
        return;
    }
    const bool orderly = shutdown_complete_;
    for (auto& slot : slots_) {
        slot.pins = 0;
        destroy_slot(slot);
    }
    // Construction abort (no incremental publication) may destroy without the
    // Reader shutdown sequence. Once a live publication occurred, non-orderly
    // destruction is an invariant failure.
    if (!orderly && publications_.load(std::memory_order_relaxed) != 0U) {
        std::terminate();
    }
}

auto GenerationSlotPool::create(const WorkerRoutingState routing,
                                const std::span<const DurableRuntimeCatalog::PublishedReadRecord> records,
                                const GenerationSlotFailureHook failure_hook,
                                std::shared_ptr<MemoryReserve> memory_reserve)
    -> Result<std::unique_ptr<GenerationSlotPool>> {
    try {
        auto pool =
            std::unique_ptr<GenerationSlotPool>{new GenerationSlotPool{ConstructionToken{}, failure_hook}};
        Result<const Generation*> initial =
            records.empty() ? PairReadGeneration::empty_direct(routing, pool->slots_[0].storage,
                                                               std::move(memory_reserve))
                            : PairReadGeneration::from_durable_snapshot_direct(
                                  routing, records, pool->slots_[0].storage, std::move(memory_reserve));
        if (!initial) {
            return unexpected(std::move(initial.error()));
        }
        auto& slot = pool->slots_[0];
        slot.generation = *initial;
        slot.epoch = (*initial)->epoch();
        slot.visible_through = (*initial)->visible_through();
        slot.publication_count = 1U;
        slot.state = SlotState::published;
        pool->writer_epoch_.store(slot.epoch, std::memory_order_relaxed);
        pool->publication_.store(GenerationPublicationToken::encode(slot.epoch, 0).raw,
                                 std::memory_order_release);
        pool->live_slots_.store(1U, std::memory_order_relaxed);
        pool->live_high_watermark_.store(1U, std::memory_order_relaxed);
        pool->initialized_ = true;
        return pool;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::resource_exhausted, "generation slot pool allocation failed");
    } catch (...) {
        return fail(ErrorCode::internal_error, "generation slot pool construction failed");
    }
}

auto GenerationSlotPool::try_reserve() noexcept -> std::optional<Reservation> {
    if (!accepting_.load(std::memory_order_acquire)) {
        shutdown_reservation_rejections_.fetch_add(1U, std::memory_order_relaxed);
        return std::nullopt;
    }
    for (std::size_t index = 0; index < kCapacity; ++index) {
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

auto GenerationSlotPool::publish_incremental(Reservation& reservation,
                                             const std::span<const ReadMutation> mutations,
                                             PairReadMerge* merge) -> GenerationSlotPublishStatus {
    if (reservation.owner_ != this || !reservation.store_linearized_ || reservation.slot_ >= kCapacity ||
        slots_[reservation.slot_].state != SlotState::building || writer_generation() == nullptr) {
        return GenerationSlotPublishStatus::invalid_generation;
    }
    auto& slot = slots_[reservation.slot_];
    const Generation* previous = writer_generation();
    std::shared_ptr<const Generation> previous_owner;
    if (merge != nullptr) {
        previous_owner = std::shared_ptr<const Generation>(previous, [](const Generation*) {});
    }
    const PairReadGeneration* direct_result{};
    auto built = PairReadGeneration::publish_incremental_construct(
        *previous, std::move(previous_owner), mutations, merge, &slot.storage, &direct_result);
    if (!built || direct_result == nullptr) {
        reservation.reset();
        return GenerationSlotPublishStatus::invalid_generation;
    }
    return commit_published(reservation, direct_result);
}

auto GenerationSlotPool::publish_direct(Reservation& reservation, const Generation* generation) noexcept
    -> GenerationSlotPublishStatus {
    if (reservation.owner_ != this || !reservation.store_linearized_ || reservation.slot_ >= kCapacity ||
        slots_[reservation.slot_].state != SlotState::building || generation == nullptr) {
        return GenerationSlotPublishStatus::invalid_generation;
    }
    return commit_published(reservation, generation);
}

auto GenerationSlotPool::commit_published(Reservation& reservation, const Generation* generation) noexcept
    -> GenerationSlotPublishStatus {
    const auto next_epoch = generation->epoch();
    if (next_epoch <= writer_epoch_.load(std::memory_order_relaxed)) {
        PairReadGeneration::destroy_direct(generation, slots_[reservation.slot_].storage);
        reservation.reset();
        return GenerationSlotPublishStatus::invalid_generation;
    }
    if (next_epoch > GenerationPublicationToken::kMaximumEpoch) {
        PairReadGeneration::destroy_direct(generation, slots_[reservation.slot_].storage);
        reservation.reset();
        return GenerationSlotPublishStatus::epoch_exhausted;
    }

    auto& slot = slots_[reservation.slot_];
    if (slot.publication_count != 0U) {
        slot_reuses_.fetch_add(1U, std::memory_order_relaxed);
    }
    slot.generation = generation;
    slot.epoch = next_epoch;
    slot.visible_through = generation->visible_through();
    ++slot.publication_count;
    slot.state = SlotState::published;
    const auto live = live_slots_.fetch_add(1U, std::memory_order_relaxed) + 1U;
    update_high_watermark(live);

    const auto previous_slot = writer_slot_;
    writer_slot_ = reservation.slot_;
    writer_epoch_.store(slot.epoch, std::memory_order_relaxed);
    publication_.store(GenerationPublicationToken::encode(slot.epoch, writer_slot_).raw,
                       std::memory_order_release);
    slots_[previous_slot].state = SlotState::retired;
    reserved_slots_.fetch_sub(1U, std::memory_order_release);
    reservation.consume();
    publications_.fetch_add(1U, std::memory_order_relaxed);
    return GenerationSlotPublishStatus::published;
}

void GenerationSlotPool::pin(const std::size_t slot_index) noexcept {
    if (slot_index < kCapacity) {
        ++slots_[slot_index].pins;
    }
}

void GenerationSlotPool::unpin(const std::size_t slot_index) noexcept {
    if (slot_index >= kCapacity) {
        return;
    }
    auto& slot = slots_[slot_index];
    if (slot.pins == 0U) {
        std::terminate();
    }
    --slot.pins;
}

auto GenerationSlotPool::adopt(const std::uint64_t minimum_borrowed_epoch) noexcept -> const Generation* {
    if (reader_quiescent_.load(std::memory_order_acquire)) {
        invalid_adoptions_.fetch_add(1U, std::memory_order_relaxed);
        return nullptr;
    }
    const auto token = GenerationPublicationToken{.raw = publication_.load(std::memory_order_acquire)};
    const auto* generation = decode_published(token);
    if (generation == nullptr) {
        invalid_adoptions_.fetch_add(1U, std::memory_order_relaxed);
        return nullptr;
    }
    const auto epoch = token.epoch();
    const auto safe_epoch = std::min(epoch, minimum_borrowed_epoch);
    const auto previous_safe_epoch = reader_safe_epoch_.load(std::memory_order_relaxed);
    if (safe_epoch < previous_safe_epoch) {
        invalid_adoptions_.fetch_add(1U, std::memory_order_relaxed);
        return nullptr;
    }
    reader_generation_ = generation;
    if (epoch != reader_epoch_.load(std::memory_order_relaxed)) {
        reader_epoch_.store(epoch, std::memory_order_relaxed);
    }
    if (safe_epoch != previous_safe_epoch) {
        reader_safe_epoch_.store(safe_epoch, std::memory_order_release);
    }
    return reader_generation_;
}

auto GenerationSlotPool::decode_published(const GenerationPublicationToken token) const noexcept
    -> const Generation* {
    if (token.empty()) {
        return nullptr;
    }
    const auto index = token.slot_index();
    const auto epoch = token.epoch();
    if (index >= kCapacity || slots_[index].generation == nullptr || slots_[index].epoch != epoch) {
        return nullptr;
    }
    return slots_[index].generation;
}

void GenerationSlotPool::reclaim(const std::uint64_t safe_epoch) noexcept {
    if (safe_epoch == 0U) {
        return;
    }
    for (auto& slot : slots_) {
        if (slot.state != SlotState::retired || slot.epoch >= safe_epoch || slot.pins != 0U) {
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

void GenerationSlotPool::stop_admission() noexcept {
    if (accepting_.exchange(false, std::memory_order_acq_rel)) {
        shutdown_starts_.fetch_add(1U, std::memory_order_relaxed);
    }
}

auto GenerationSlotPool::accepting() const noexcept -> bool {
    return accepting_.load(std::memory_order_acquire);
}

auto GenerationSlotPool::mark_reader_quiescent() noexcept -> bool {
    if (accepting_.load(std::memory_order_acquire) || reserved_slots_.load(std::memory_order_acquire) != 0U) {
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

auto GenerationSlotPool::try_finish_shutdown() noexcept -> bool {
    if (accepting_.load(std::memory_order_acquire) || reserved_slots_.load(std::memory_order_acquire) != 0U ||
        !reader_quiescent_.load(std::memory_order_acquire)) {
        return false;
    }
    reclaim(reader_safe_epoch_.load(std::memory_order_acquire));
    shutdown_complete_ = live_slots_.load(std::memory_order_relaxed) == 1U;
    return shutdown_complete_;
}

void GenerationSlotPool::revoke_publication() noexcept {
    publication_.store(0U, std::memory_order_release);
}

auto GenerationSlotPool::stats() const noexcept -> GenerationSlotPoolStats {
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

auto GenerationSlotPool::writer_generation() const noexcept -> const Generation* {
    return slots_[writer_slot_].generation;
}

auto GenerationSlotPool::writer_slot() const noexcept -> std::size_t {
    return writer_slot_;
}

auto GenerationSlotPool::retired_count() const noexcept -> std::size_t {
    std::size_t count{};
    for (const auto& slot : slots_) {
        if (slot.state == SlotState::retired) {
            ++count;
        }
    }
    return count;
}

auto GenerationSlotPool::publication_token() const noexcept -> GenerationPublicationToken {
    return GenerationPublicationToken{.raw = publication_.load(std::memory_order_acquire)};
}

auto GenerationSlotPool::storage_at(const std::size_t slot_index) noexcept -> GenerationDirectStorage& {
    return slots_[slot_index].storage;
}

auto GenerationSlotPool::shell_allocation_count(const std::size_t index) const noexcept -> std::uint64_t {
    return index < kCapacity ? slots_[index].storage.allocation_count() : 0U;
}

auto GenerationSlotPool::shell_reuse_count(const std::size_t index) const noexcept -> std::uint64_t {
    return index < kCapacity ? slots_[index].storage.reuse_count() : 0U;
}

void GenerationSlotPool::set_failure_hook(const GenerationSlotFailureHook failure_hook) noexcept {
    failure_hook_ = failure_hook;
}

void GenerationSlotPool::destroy_slot(Slot& slot) noexcept {
    if (slot.generation != nullptr) {
        PairReadGeneration::destroy_direct(slot.generation, slot.storage);
        slot.generation = nullptr;
    }
}

void GenerationSlotPool::update_high_watermark(const std::size_t value) noexcept {
    auto observed = live_high_watermark_.load(std::memory_order_relaxed);
    while (value > observed && !live_high_watermark_.compare_exchange_weak(
                                   observed, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

void GenerationSlotPool::cancel_reservation(const std::size_t index, const bool linearized) noexcept {
    if (index >= kCapacity || slots_[index].state != SlotState::building) {
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

} // namespace glifistore::store::paired
