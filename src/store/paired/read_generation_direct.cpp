#include "glifistore/store/paired/read_generation.hpp"
#include "store/paired/read_generation_impl.hpp"

#include <algorithm>

namespace glifistore::store::paired {

auto PairReadGeneration::publish_incremental_direct(const PairReadGeneration& previous,
                                                    const std::span<const ReadMutation> mutations,
                                                    GenerationDirectStorage& storage)
    -> Result<const PairReadGeneration*> {
    const PairReadGeneration* direct_result{};
    auto built = publish_incremental_construct(previous, {}, mutations, nullptr, &storage, &direct_result);
    if (!built) {
        return unexpected(std::move(built.error()));
    }
    if (direct_result == nullptr) {
        return fail(ErrorCode::internal_error, "direct generation construction returned no object");
    }
    return direct_result;
}

auto PairReadGeneration::empty_direct(const WorkerRoutingState routing, GenerationDirectStorage& storage,
                                      std::shared_ptr<MemoryReserve> memory_reserve)
    -> Result<const PairReadGeneration*> try {
    static_assert(sizeof(PairReadGenerationEnableShared) <= GenerationDirectStorage::kBytes);
    static_assert(alignof(PairReadGenerationEnableShared) <= GenerationDirectStorage::kAlignment);
    auto base = std::make_shared<const ImmutableReadIndex>(
        std::make_shared<LargeImmutableMappingPool>(std::move(memory_reserve)));
    auto delta = make_empty_delta(PairReadGeneration::kMaximumIncrementalDeltaEntries);
    auto* location =
        storage.claim(sizeof(PairReadGenerationEnableShared), alignof(PairReadGenerationEnableShared));
    try {
        return std::construct_at(static_cast<PairReadGenerationEnableShared*>(location), routing,
                                 std::move(base), std::move(delta), 0, 0);
    } catch (...) {
        storage.release(location);
        throw;
    }
} catch (const std::bad_alloc&) {

    return fail(ErrorCode::resource_exhausted, "direct empty generation allocation failed");
} catch (...) {
    return fail(ErrorCode::internal_error, "direct empty generation construction failed");
}

auto PairReadGeneration::from_durable_snapshot_direct(
    const WorkerRoutingState routing,
    const std::span<const DurableRuntimeCatalog::PublishedReadRecord> records,
    GenerationDirectStorage& storage, std::shared_ptr<MemoryReserve> memory_reserve)
    -> Result<const PairReadGeneration*> try {
    return build_durable_snapshot_direct(
        routing, records, 0, 0, storage,
        std::make_shared<LargeImmutableMappingPool>(std::move(memory_reserve)));
} catch (const std::bad_alloc&) {

    return fail(ErrorCode::resource_exhausted, "direct durable read generation allocation failed");
}

auto PairReadGeneration::replace_durable_snapshot_direct(
    const PairReadGeneration& previous,
    const std::span<const DurableRuntimeCatalog::PublishedReadRecord> records,
    GenerationDirectStorage& storage) -> Result<const PairReadGeneration*> {
    if (previous.epoch_ >= GenerationPublicationToken::kMaximumEpoch) {
        return fail(ErrorCode::arithmetic_overflow, "read generation epoch exhausted");
    }
    return build_durable_snapshot_direct(previous.routing_, records, previous.epoch_ + 1U,
                                         previous.visible_through_, storage, previous.base_->mapping_pool());
}

auto PairReadGeneration::build_durable_snapshot_direct(
    const WorkerRoutingState routing,
    const std::span<const DurableRuntimeCatalog::PublishedReadRecord> records, const std::uint64_t epoch,
    const std::uint64_t visible_floor, GenerationDirectStorage& storage,
    std::shared_ptr<LargeImmutableMappingPool> mapping_pool) -> Result<const PairReadGeneration*> try {
    if (epoch > GenerationPublicationToken::kMaximumEpoch) {
        return fail(ErrorCode::arithmetic_overflow, "read generation epoch exhausted");
    }
    static_assert(sizeof(PairReadGenerationEnableShared) <= GenerationDirectStorage::kBytes);
    static_assert(alignof(PairReadGenerationEnableShared) <= GenerationDirectStorage::kAlignment);
    auto visible_through = visible_floor;
    for (const auto& record : records) {
        visible_through = std::max(visible_through, record.reference().sequence.value);
    }
    auto base = std::make_shared<const ImmutableReadIndex>(records, std::move(mapping_pool));
    auto delta = make_empty_delta(PairReadGeneration::kMaximumIncrementalDeltaEntries);
    auto* location =
        storage.claim(sizeof(PairReadGenerationEnableShared), alignof(PairReadGenerationEnableShared));
    try {
        return std::construct_at(static_cast<PairReadGenerationEnableShared*>(location), routing,
                                 std::move(base), std::move(delta), epoch, visible_through);
    } catch (...) {
        storage.release(location);
        throw;
    }
} catch (const std::bad_alloc&) {

    return fail(ErrorCode::resource_exhausted, "direct durable read generation allocation failed");
} catch (...) {
    return fail(ErrorCode::internal_error, "direct durable read generation construction failed");
}

void PairReadGeneration::destroy_direct(const PairReadGeneration* generation,
                                        GenerationDirectStorage& storage) noexcept {
    if (generation == nullptr) {
        return;
    }
    auto* concrete = const_cast<PairReadGenerationEnableShared*>(
        static_cast<const PairReadGenerationEnableShared*>(generation));
    std::destroy_at(concrete);
    storage.release(concrete);
}

} // namespace glifistore::store::paired
