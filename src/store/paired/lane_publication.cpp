#include "glifistore/store/paired/lane_publication.hpp"

#include "glifistore/core/fault_injection.hpp"
#include "store/store_internal.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

namespace glifistore::store::paired {
namespace {

[[nodiscard]] auto non_owning_generation_view(const PairReadGeneration* generation) noexcept
    -> std::shared_ptr<const PairReadGeneration> {
    // The pool owns the generation. An empty-owner alias needs no control block.
    return {std::shared_ptr<const PairReadGeneration>{}, generation};
}

} // namespace

void mirror_slot_pool_publication(GenerationState& generation) noexcept {
    auto* pool = generation.slot_pool.get();
    if (pool == nullptr || pool->writer_generation() == nullptr) {
        return;
    }
    generation.writer_generation = non_owning_generation_view(pool->writer_generation());
    generation.writer_epoch.store(generation.writer_generation->epoch(), std::memory_order_relaxed);
    generation.retired_generation_count.store(pool->retired_count(), std::memory_order_relaxed);
    publish_read_generation_token(generation.published_token, generation.published_generation,
                                  pool->publication_token(), pool->writer_generation());
}

void store_generation_memory_stats(GenerationState& destination,
                                   const ReadGenerationMemoryStats& source) noexcept {
    destination.memory_stats_sequence.fetch_add(1U, std::memory_order_acq_rel);
    destination.memory_base_entries.store(source.base_entries, std::memory_order_relaxed);
    destination.memory_base_capacity.store(source.base_capacity, std::memory_order_relaxed);
    destination.memory_base_record_storage_bytes.store(source.base_record_storage_bytes,
                                                       std::memory_order_relaxed);
    destination.memory_base_record_mapped_storage_bytes.store(source.base_record_mapped_storage_bytes,
                                                              std::memory_order_relaxed);
    destination.memory_base_lookup_storage_bytes.store(source.base_lookup_storage_bytes,
                                                       std::memory_order_relaxed);
    destination.memory_base_key_bytes.store(source.base_key_bytes, std::memory_order_relaxed);
    destination.memory_base_key_storage_bytes.store(source.base_key_storage_bytes, std::memory_order_relaxed);
    destination.memory_base_pin_storage_bytes.store(source.base_pin_storage_bytes, std::memory_order_relaxed);
    destination.memory_base_allocated_lower_bound_bytes.store(source.base_allocated_lower_bound_bytes,
                                                              std::memory_order_relaxed);
    destination.delta_entries.store(source.delta_entries, std::memory_order_relaxed);
    destination.memory_delta_capacity.store(source.delta_capacity, std::memory_order_relaxed);
    destination.delta_record_versions.store(source.delta_record_versions, std::memory_order_relaxed);
    destination.delta_arena_record_bytes.store(source.delta_arena_record_bytes, std::memory_order_relaxed);
    destination.delta_arena_key_bytes.store(source.delta_arena_key_bytes, std::memory_order_relaxed);
    destination.delta_arena_key_storage_bytes.store(source.delta_arena_key_storage_bytes,
                                                    std::memory_order_relaxed);
    destination.memory_delta_lookup_storage_bytes.store(source.delta_lookup_storage_bytes,
                                                        std::memory_order_relaxed);
    destination.memory_delta_allocated_lower_bound_bytes.store(source.delta_allocated_lower_bound_bytes,
                                                               std::memory_order_relaxed);
    destination.memory_generation_shell_bytes.store(source.generation_shell_bytes, std::memory_order_relaxed);
    destination.memory_current_allocated_lower_bound_bytes.store(source.current_allocated_lower_bound_bytes,
                                                                 std::memory_order_relaxed);
    destination.memory_stats_sequence.fetch_add(1U, std::memory_order_release);
}

void store_merge_progress(MergeState& destination) noexcept {
    if (!destination.read_merge) {
        destination.read_merge_active.store(false, std::memory_order_relaxed);
        destination.read_merge_post_entries.store(0U, std::memory_order_relaxed);
        destination.read_merge_remaining_slots.store(0U, std::memory_order_relaxed);
        destination.read_merge_post_capacity_remaining.store(0U, std::memory_order_relaxed);
        return;
    }
    destination.read_merge_active.store(true, std::memory_order_relaxed);
    destination.read_merge_post_entries.store(PairReadGeneration::merge_post_entries(*destination.read_merge),
                                              std::memory_order_relaxed);
    destination.read_merge_remaining_slots.store(
        PairReadGeneration::merge_remaining_slots(*destination.read_merge), std::memory_order_relaxed);
    destination.read_merge_post_capacity_remaining.store(
        PairReadGeneration::merge_post_capacity_remaining(*destination.read_merge),
        std::memory_order_relaxed);
}

[[nodiscard]] auto publish_slot_incremental(LanePublicationContext& context,
                                            GenerationSlotPool::Reservation& reservation,
                                            std::span<const ReadMutation> mutations) -> bool {
    auto* pool = context.generation.slot_pool.get();
    const auto status = pool->publish_incremental(reservation, mutations, context.merge.read_merge.get());
    if (status != GenerationSlotPublishStatus::published) {
        return false;
    }
    mirror_slot_pool_publication(context.generation);
    store_generation_memory_stats(context.generation, context.generation.writer_generation->memory_stats());
    return true;
}

[[nodiscard]] auto publish_slot_direct(LanePublicationContext& context,
                                       GenerationSlotPool::Reservation& reservation,
                                       const PairReadGeneration* generation) -> bool {
    auto* pool = context.generation.slot_pool.get();
    const auto status = pool->publish_direct(reservation, generation);
    if (status != GenerationSlotPublishStatus::published) {
        return false;
    }
    mirror_slot_pool_publication(context.generation);
    store_generation_memory_stats(context.generation, context.generation.writer_generation->memory_stats());
    return true;
}

void reclaim_quiescent_generations(GenerationState& generation, ReclamationState& reclaim,
                                   const bool reader_epoch_lease) noexcept {
    if (generation.uses_slot_pool()) {
        auto* pool = generation.slot_pool.get();
        std::uint64_t quiescent_epoch{};
        if (reader_epoch_lease) {
            quiescent_epoch = generation.reader_safe_epoch.load(std::memory_order_acquire);
        } else {
            std::atomic_thread_fence(std::memory_order_seq_cst);
            quiescent_epoch = reclaim.active_read_leases.load(std::memory_order_acquire) == 0
                                  ? generation.writer_epoch.load(std::memory_order_relaxed)
                                  : std::uint64_t{0};
        }
        if (quiescent_epoch == 0) {
            generation.retired_generation_count.store(pool->retired_count(), std::memory_order_relaxed);
            return;
        }
        const auto before = pool->retired_count();
        pool->reclaim(quiescent_epoch);
        const auto after = pool->retired_count();
        if (before > after) {
            generation.generations_retired.fetch_add(before - after, std::memory_order_relaxed);
        }
        generation.retired_generation_count.store(after, std::memory_order_relaxed);
        return;
    }
    if (generation.retired_generations.empty()) {
        generation.retired_generation_count.store(0U, std::memory_order_relaxed);
        return;
    }
    const auto before = generation.retired_generations.size();
    std::uint64_t quiescent_epoch{};
    if (reader_epoch_lease) {
        quiescent_epoch = generation.reader_safe_epoch.load(std::memory_order_acquire);
    } else {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        quiescent_epoch = reclaim.active_read_leases.load(std::memory_order_acquire) == 0
                              ? generation.writer_epoch.load(std::memory_order_relaxed)
                              : std::uint64_t{0};
    }
    if (quiescent_epoch == 0) {
        generation.retired_generation_count.store(generation.retired_generations.size(),
                                                  std::memory_order_relaxed);
        return;
    }
    std::erase_if(generation.retired_generations,
                  [&](const auto& retired) { return retired->epoch() < quiescent_epoch; });
    const auto retired = before - generation.retired_generations.size();
    if (retired != 0U) {
        generation.generations_retired.fetch_add(retired, std::memory_order_relaxed);
    }
    generation.retired_generation_count.store(generation.retired_generations.size(),
                                              std::memory_order_relaxed);
}

auto start_incremental_merge(GenerationState& generation, const std::size_t merge_maximum_post_entries)
    -> Result<std::unique_ptr<PairReadMerge>> {
    if (!generation.uses_slot_pool()) {
        return PairReadGeneration::start_incremental_merge(generation.writer_generation,
                                                           merge_maximum_post_entries);
    }
    auto* pool = generation.slot_pool.get();
    const auto slot = pool->writer_slot();
    pool->pin(slot);
    auto cut = std::shared_ptr<const PairReadGeneration>(
        pool->writer_generation(), [pool, slot](const PairReadGeneration*) noexcept { pool->unpin(slot); });
    return PairReadGeneration::start_incremental_merge(std::move(cut), merge_maximum_post_entries);
}

auto try_reserve_publication_slot(GenerationState& generation) noexcept
    -> std::optional<GenerationSlotPool::Reservation> {
    if (!generation.uses_slot_pool()) {
        return std::nullopt;
    }
    return generation.slot_pool->try_reserve();
}

void note_catalog_snapshot_installed(LanePublicationContext& context,
                                     const std::uint64_t catalog_revision) noexcept {
    context.generation.published_catalog_revision.store(catalog_revision, std::memory_order_release);
    context.merge.read_merge.reset();
    context.merge.read_merge_active.store(false, std::memory_order_relaxed);
    context.merge.read_merge_post_entries.store(0U, std::memory_order_relaxed);
    store_merge_progress(context.merge);
}

namespace {

void clear_merge_state(MergeState& merge) noexcept {
    merge.read_merge.reset();
    merge.read_merge_active.store(false, std::memory_order_relaxed);
    merge.read_merge_post_entries.store(0U, std::memory_order_relaxed);
    store_merge_progress(merge);
}

} // namespace

auto replace_durable_snapshot_and_publish(
    LanePublicationContext& context,
    const std::span<const DurableRuntimeCatalog::PublishedReadRecord> records) -> DualPathPublishOutcome {
    if (context.generation.uses_slot_pool()) {
        auto reservation = context.generation.slot_pool->try_reserve();
        if (!reservation) {
            return {.status = DualPathPublishStatus::deferred};
        }
        auto next = PairReadGeneration::replace_durable_snapshot_direct(
            *context.generation.writer_generation, records,
            context.generation.slot_pool->storage_at(reservation->slot_index()));
        if (!next) {
            reservation->reset();
            return {.status = DualPathPublishStatus::build_failed, .build_error = next.error().code};
        }
        reservation->mark_store_linearized();
        if (!publish_slot_direct(context, *reservation, *next)) {
            return {.status = DualPathPublishStatus::publish_failed};
        }
        return {.status = DualPathPublishStatus::published};
    }
    auto next = PairReadGeneration::replace_durable_snapshot(context.generation.writer_generation, records);
    if (!next) {
        return {.status = DualPathPublishStatus::build_failed, .build_error = next.error().code};
    }
    install_writer_generation(context.generation.writer_generation, context.generation.retired_generations,
                              context.generation.retired_generation_count, context.generation.writer_epoch,
                              context.maximum_retired_generations, std::move(*next));
    store_generation_memory_stats(context.generation, context.generation.writer_generation->memory_stats());
    publish_read_generation(context.generation.published_generation,
                            context.generation.writer_generation.get());
    return {.status = DualPathPublishStatus::published};
}

auto finish_incremental_merge_and_publish(LanePublicationContext& context) -> DualPathPublishOutcome {
    if (context.generation.uses_slot_pool()) {
        auto reservation = context.generation.slot_pool->try_reserve();
        if (!reservation) {
            return {.status = DualPathPublishStatus::deferred};
        }
        auto next = PairReadGeneration::finish_incremental_merge_direct(
            *context.generation.writer_generation, *context.merge.read_merge,
            context.generation.slot_pool->storage_at(reservation->slot_index()));
        if (!next) {
            reservation->reset();
            context.merge.read_merge_failures.fetch_add(1U, std::memory_order_relaxed);
            clear_merge_state(context.merge);
            return {.status = DualPathPublishStatus::build_failed, .build_error = next.error().code};
        }
        reservation->mark_store_linearized();
        if (!publish_slot_direct(context, *reservation, *next)) {
            return {.status = DualPathPublishStatus::publish_failed};
        }
        return {.status = DualPathPublishStatus::published};
    }
    auto next = PairReadGeneration::finish_incremental_merge(context.generation.writer_generation,
                                                             *context.merge.read_merge);
    if (!next) {
        context.merge.read_merge_failures.fetch_add(1U, std::memory_order_relaxed);
        clear_merge_state(context.merge);
        return {.status = DualPathPublishStatus::build_failed, .build_error = next.error().code};
    }
    install_writer_generation(context.generation.writer_generation, context.generation.retired_generations,
                              context.generation.retired_generation_count, context.generation.writer_epoch,
                              context.maximum_retired_generations, std::move(*next));
    store_generation_memory_stats(context.generation, context.generation.writer_generation->memory_stats());
    publish_read_generation(context.generation.published_generation,
                            context.generation.writer_generation.get());
    return {.status = DualPathPublishStatus::published};
}

auto publish_incremental_read_mutations(
    LanePublicationContext& context, const std::span<const ReadMutation> mutations,
    std::optional<GenerationSlotPool::Reservation>& slot_reservation,
    const std::function<void(std::size_t publication_count)>* prepare_publish_retry) -> bool {
    if (slot_reservation) {
        slot_reservation->mark_store_linearized();
        return publish_slot_incremental(context, *slot_reservation, mutations);
    }
    auto next = PairReadGeneration::publish_incremental(context.generation.writer_generation, mutations,
                                                        context.merge.read_merge.get());
    if (!next && next.error().code == ErrorCode::resource_exhausted && prepare_publish_retry != nullptr &&
        *prepare_publish_retry) {
        (*prepare_publish_retry)(mutations.size());
        next = PairReadGeneration::publish_incremental(context.generation.writer_generation, mutations,
                                                       context.merge.read_merge.get());
    }
    if (!next) {
        return false;
    }
    install_writer_generation(context.generation.writer_generation, context.generation.retired_generations,
                              context.generation.retired_generation_count, context.generation.writer_epoch,
                              context.maximum_retired_generations, std::move(*next));
    store_generation_memory_stats(context.generation, context.generation.writer_generation->memory_stats());
    publish_read_generation(context.generation.published_generation,
                            context.generation.writer_generation.get());
    return true;
}

[[nodiscard]] auto try_drain_durable_snapshot(LanePublicationContext& context, const bool allow_fail_closed,
                                              const std::function<void()>* after_drain) noexcept -> bool {
    try {
        if (glifistore::fault::consume_fail(glifistore::fault::Site::drain_snapshot)) {
            return false;
        }
        auto snapshot =
            detail::StoreAccess::snapshot_durable_reads(context.store, context.shard, allow_fail_closed);
        if (!snapshot) {
            return false;
        }
        const auto outcome = replace_durable_snapshot_and_publish(context, snapshot->records);
        if (outcome.status != DualPathPublishStatus::published) {
            return false;
        }
        note_catalog_snapshot_installed(context, snapshot->catalog_revision);
        if (after_drain != nullptr && *after_drain) {
            (*after_drain)();
        }
        return true;
    } catch (...) {
        return false;
    }
}

auto finalize_generation_reader_shutdown(GenerationState& generation) -> Status {
    if (generation.reader_shutdown_finalized.load(std::memory_order_acquire)) {
        return {};
    }
    if (generation.uses_slot_pool()) {
        auto* pool = generation.slot_pool.get();
        pool->stop_admission();
        pool->revoke_publication();
        generation.published_generation.store(nullptr, std::memory_order_release);
        generation.published_token.store(0U, std::memory_order_release);
        if (!pool->mark_reader_quiescent() || !pool->try_finish_shutdown()) {
            return fail(ErrorCode::unavailable, "paired slot-pool Reader shutdown is incomplete");
        }
        const auto reclaimed = pool->stats().slots_reclaimed +
                               (pool->writer_generation() != nullptr ? std::size_t{1} : std::size_t{0});
        generation.writer_generation.reset();
        generation.slot_pool.reset();
        generation.retired_generation_count.store(0U, std::memory_order_relaxed);
        generation.shutdown_generations_reclaimed.fetch_add(reclaimed, std::memory_order_relaxed);
        generation.reader_shutdown_finalized.store(true, std::memory_order_release);
        return {};
    }
    // No Reader or Writer remains. Stop future raw-pointer adoption before
    // dropping retired ownership, then advance the terminal safe frontier.
    generation.published_generation.store(nullptr, std::memory_order_release);
    const auto writer_epoch = generation.writer_epoch.load(std::memory_order_acquire);
    const auto terminal_epoch =
        writer_epoch == std::numeric_limits<std::uint64_t>::max() ? writer_epoch : writer_epoch + 1U;
    generation.reader_safe_epoch.store(terminal_epoch, std::memory_order_release);

    const auto reclaimed = generation.retired_generations.size() +
                           (generation.writer_generation ? std::size_t{1} : std::size_t{0});
    generation.retired_generations.clear();
    generation.writer_generation.reset();
    generation.retired_generation_count.store(0U, std::memory_order_relaxed);
    generation.shutdown_generations_reclaimed.fetch_add(reclaimed, std::memory_order_relaxed);
    generation.reader_shutdown_finalized.store(true, std::memory_order_release);
    return {};
}

} // namespace glifistore::store::paired
