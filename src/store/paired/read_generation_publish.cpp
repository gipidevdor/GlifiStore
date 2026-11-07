#include "glifistore/store/paired/read_generation.hpp"
#include "store/paired/read_generation_impl.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <optional>
#include <utility>

namespace glifistore::store::paired {

auto PairReadGeneration::empty(const WorkerRoutingState routing)
    -> Result<std::shared_ptr<const PairReadGeneration>> try {
    return make_shared_generation(routing, std::make_shared<const ImmutableReadIndex>(),
                                  make_empty_delta(PairReadGeneration::kMaximumIncrementalDeltaEntries), 0,
                                  0);
} catch (const std::bad_alloc&) {

    return fail(ErrorCode::resource_exhausted, "read generation allocation failed");
} catch (...) {
    return fail(ErrorCode::internal_error, "read generation construction failed");
}

auto PairReadGeneration::from_durable_snapshot(
    const WorkerRoutingState routing,
    const std::span<const DurableRuntimeCatalog::PublishedReadRecord> records)
    -> Result<std::shared_ptr<const PairReadGeneration>> {
    return PairReadGeneration::build_durable_snapshot(routing, records, 0, 0);
}

auto PairReadGeneration::replace_durable_snapshot(
    std::shared_ptr<const PairReadGeneration> previous,
    const std::span<const DurableRuntimeCatalog::PublishedReadRecord> records)
    -> Result<std::shared_ptr<const PairReadGeneration>> {
    if (!previous) {
        return fail(ErrorCode::invalid_argument, "durable snapshot replacement has no previous generation");
    }
    if (previous->epoch_ == std::numeric_limits<std::uint64_t>::max()) {
        return fail(ErrorCode::arithmetic_overflow, "read generation epoch exhausted");
    }
    return PairReadGeneration::build_durable_snapshot(previous->routing_, records, previous->epoch_ + 1U,
                                                      previous->visible_through_);
}

auto PairReadGeneration::build_durable_snapshot(
    const WorkerRoutingState routing,
    const std::span<const DurableRuntimeCatalog::PublishedReadRecord> records, const std::uint64_t epoch,
    const std::uint64_t visible_floor) -> Result<std::shared_ptr<const PairReadGeneration>> try {
    auto visible_through = visible_floor;
    for (const auto& record : records) {
        visible_through = std::max(visible_through, record.reference().sequence.value);
    }
    return make_shared_generation(routing, std::make_shared<const ImmutableReadIndex>(records),
                                  make_empty_delta(PairReadGeneration::kMaximumIncrementalDeltaEntries),
                                  epoch, visible_through);
} catch (const std::bad_alloc&) {

    return fail(ErrorCode::resource_exhausted, "durable read generation allocation failed");
} catch (...) {
    return fail(ErrorCode::internal_error, "durable read generation construction failed");
}

auto PairReadGeneration::publish(std::shared_ptr<const PairReadGeneration> previous,
                                 const std::span<const ReadMutation> mutations,
                                 const std::size_t merge_delta_entries)
    -> Result<std::shared_ptr<const PairReadGeneration>> try {
    if (!previous || merge_delta_entries == 0 || mutations.size() > kMaximumPublicationBatch) {
        return fail(ErrorCode::invalid_argument, "invalid read generation publication");
    }
    if (mutations.empty()) {
        return previous;
    }
    if (previous->epoch_ == std::numeric_limits<std::uint64_t>::max()) {
        return fail(ErrorCode::arithmetic_overflow, "read generation epoch exhausted");
    }

    DeltaBuilder builder{*previous->delta_, current_delta_builder_scratch()};
    auto visible_through = previous->visible_through_;
    for (const auto& mutation : mutations) {
        const auto* durable = mutation.durable ? &mutation.durable.value() : nullptr;
        const bool durable_put = mutation.opcode == Opcode::put && durable != nullptr;
        const bool volatile_pinned = same_segment(mutation.segment, mutation.record);
        if (mutation.opcode == Opcode::put && !durable_put && !volatile_pinned) {
            return fail(ErrorCode::invalid_reference,
                        "publication rejected a RecordRef without its exact Segment pin");
        }
        if (durable_put && (durable->key_hash() != mutation.key.hash || durable->key() != mutation.key.key ||
                            durable->reference() != mutation.record)) {
            return fail(ErrorCode::invalid_reference,
                        "durable publication record disagrees with its exact generation pin");
        }
        const auto prepared = builder.prepare(mutation.key.hash, mutation.key.key);
        const auto* stored = builder.store(view_of(mutation));
        builder.commit(prepared, stored);
        visible_through = std::max(visible_through, mutation.record.sequence.value);
    }

    auto next_delta = std::move(builder).freeze();
    auto next_base = previous->base_;
    if (next_delta.size() >= merge_delta_entries) {
        auto merged = next_base->records();
        next_delta.for_each([&](const ReadRecordView record) { apply_record(merged, materialize(record)); });
        next_base = std::make_shared<const ImmutableReadIndex>(std::move(merged));
        next_delta = make_empty_delta(std::max(merge_delta_entries, previous->delta_->maximum_entries()));
    }
    return make_shared_generation(previous->routing_, std::move(next_base), std::move(next_delta),
                                  previous->epoch_ + 1U, visible_through);
} catch (const std::bad_alloc&) {

    return fail(ErrorCode::resource_exhausted, "read generation publication allocation failed");
} catch (...) {
    return fail(ErrorCode::internal_error, "read generation publication failed");
}

auto PairReadGeneration::publish_incremental(std::shared_ptr<const PairReadGeneration> previous,
                                             const std::span<const ReadMutation> mutations,
                                             PairReadMerge* merge)
    -> Result<std::shared_ptr<const PairReadGeneration>> {
    if (!previous) {
        return fail(ErrorCode::invalid_argument, "invalid incremental read publication");
    }
    const auto* previous_view = previous.get();
    return publish_incremental_construct(*previous_view, std::move(previous), mutations, merge, nullptr,
                                         nullptr);
}

auto IncrementalPublicationAccess::prepare(const PairReadGeneration& previous,
                                           const std::shared_ptr<const PairReadGeneration>* previous_owner,
                                           const std::span<const ReadMutation> mutations,
                                           PairReadMerge* merge) -> Result<IncrementalBuildResult> {
    if (mutations.size() > kMaximumPublicationBatch ||
        (merge != nullptr && (previous_owner == nullptr || !*previous_owner || !merge->state_ ||
                              merge->state_->current.get() != &previous))) {
        return fail(ErrorCode::invalid_argument, "invalid incremental read publication");
    }
    if (mutations.empty()) {
        if (previous_owner == nullptr || !*previous_owner) {
            return fail(ErrorCode::invalid_argument, "direct publication requires a non-empty mutation");
        }
        return IncrementalBuildResult{.empty_reuse = *previous_owner};
    }
    if (previous.epoch_ >= GenerationPublicationToken::kMaximumEpoch) {
        return fail(ErrorCode::arithmetic_overflow, "read generation epoch exhausted");
    }
    if (!PairReadGeneration::can_publish_incremental(previous, merge, mutations.size())) {
        return fail(ErrorCode::resource_exhausted, "incremental read delta capacity exhausted");
    }

    std::optional<DeltaBuilder> post_builder;
    if (merge != nullptr) {
        post_builder.emplace(merge->state_->post_delta, post_delta_builder_scratch());
    }
    DeltaBuilder current_builder{*previous.delta_, current_delta_builder_scratch(),
                                 post_builder ? post_builder->allocation_arena()
                                              : std::shared_ptr<DeltaArena>{}};
    auto* post = post_builder ? &post_builder.value() : nullptr;
    auto visible_through = previous.visible_through_;
    for (const auto& mutation : mutations) {
        const auto* durable = mutation.durable ? &mutation.durable.value() : nullptr;
        const bool durable_put = mutation.opcode == Opcode::put && durable != nullptr;
        const bool volatile_pinned = same_segment(mutation.segment, mutation.record);
        if (mutation.opcode == Opcode::put && !durable_put && !volatile_pinned) {
            return fail(ErrorCode::invalid_reference,
                        "publication rejected a RecordRef without its exact Segment pin");
        }
        if (durable_put && (durable->key_hash() != mutation.key.hash || durable->key() != mutation.key.key ||
                            durable->reference() != mutation.record)) {
            return fail(ErrorCode::invalid_reference,
                        "durable publication record disagrees with its exact generation pin");
        }
        const auto current_slot = current_builder.prepare(mutation.key.hash, mutation.key.key);
        DeltaBuilder::PreparedSlot post_slot{};
        if (post != nullptr) {
            post_slot = post->prepare(mutation.key.hash, mutation.key.key);
        }
        const auto* stored = current_builder.store(view_of(mutation));
        current_builder.commit(current_slot, stored);
        if (post != nullptr) {
            post->commit(post_slot, stored);
        }
        visible_through = std::max(visible_through, mutation.record.sequence.value);
    }

    IncrementalBuildResult prepared;
    prepared.next_delta.emplace(std::move(current_builder).freeze());
    if (post_builder) {
        prepared.next_post.emplace(std::move(*post_builder).freeze());
    }
    prepared.visible_through = visible_through;
    return prepared;
}

void IncrementalPublicationAccess::commit_merge(PairReadMerge* merge, IncrementalBuildResult& prepared,
                                                std::shared_ptr<const PairReadGeneration> next) noexcept {
    if (merge == nullptr) {
        return;
    }
    if (!prepared.next_post) {
        std::terminate();
    }
    merge->state_->post_delta = std::move(*prepared.next_post);
    merge->state_->current = std::move(next);
}

auto PairReadGeneration::publish_incremental_construct(
    const PairReadGeneration& previous, std::shared_ptr<const PairReadGeneration> previous_owner,
    const std::span<const ReadMutation> mutations, PairReadMerge* merge,
    GenerationDirectStorage* direct_storage, const PairReadGeneration** direct_result)
    -> Result<std::shared_ptr<const PairReadGeneration>> try {
    if ((direct_storage != nullptr) != (direct_result != nullptr)) {
        return fail(ErrorCode::invalid_argument, "read publication has invalid construction ownership");
    }
    if (direct_result != nullptr) {
        *direct_result = nullptr;
    }
    auto prepared = IncrementalPublicationAccess::prepare(previous, &previous_owner, mutations, merge);
    if (!prepared) {
        return unexpected(std::move(prepared.error()));
    }
    if (prepared->empty_reuse) {
        return std::move(prepared->empty_reuse);
    }
    auto& next_delta = prepared->next_delta;
    if (!next_delta) {
        return fail(ErrorCode::internal_error, "incremental publication produced no next delta");
    }
    auto delta = std::move(next_delta).value();

    std::shared_ptr<const PairReadGeneration> next;
    if (direct_storage != nullptr) {
        static_assert(sizeof(PairReadGenerationEnableShared) <= GenerationDirectStorage::kBytes);
        static_assert(alignof(PairReadGenerationEnableShared) <= GenerationDirectStorage::kAlignment);
        auto* location = direct_storage->claim(sizeof(PairReadGenerationEnableShared),
                                               alignof(PairReadGenerationEnableShared));
        try {
            auto* constructed = std::construct_at(static_cast<PairReadGenerationEnableShared*>(location),
                                                  previous.routing_, previous.base_, std::move(delta),
                                                  previous.epoch_ + 1U, prepared->visible_through);
            *direct_result = constructed;
            // Merge retains a non-owning view; the slot pool owns destruction.
            next = std::shared_ptr<const PairReadGeneration>(constructed, [](const PairReadGeneration*) {});
        } catch (...) {
            direct_storage->release(location);
            throw;
        }
    } else {
        next = make_shared_generation(previous.routing_, previous.base_, std::move(delta),
                                      previous.epoch_ + 1U, prepared->visible_through);
    }
    IncrementalPublicationAccess::commit_merge(merge, *prepared, next);
    return next;
} catch (const std::bad_alloc&) {

    return fail(ErrorCode::resource_exhausted, "incremental read publication allocation failed");
} catch (...) {
    return fail(ErrorCode::internal_error, "incremental read publication failed");
}

} // namespace glifistore::store::paired
