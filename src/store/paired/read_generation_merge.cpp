#include "glifistore/store/paired/read_generation.hpp"
#include "store/paired/read_generation_impl.hpp"

namespace glifistore::store::paired {

PairReadMerge::PairReadMerge(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

PairReadMerge::~PairReadMerge() = default;
PairReadMerge::PairReadMerge(PairReadMerge&&) noexcept = default;
auto PairReadMerge::operator=(PairReadMerge&&) noexcept -> PairReadMerge& = default;

auto PairReadGeneration::finish_incremental_merge_direct(const PairReadGeneration& current,
                                                         PairReadMerge& merge,
                                                         GenerationDirectStorage& storage)
    -> Result<const PairReadGeneration*> try {
    if (!merge.state_ || merge.state_->current.get() != &current ||
        merge.state_->phase != PairReadMerge::State::Phase::ready || !merge.state_->builder) {
        return fail(ErrorCode::invalid_argument, "incremental read merge is not ready");
    }
    if (current.epoch_ >= GenerationPublicationToken::kMaximumEpoch) {
        return fail(ErrorCode::arithmetic_overflow, "read generation epoch exhausted");
    }
    static_assert(sizeof(PairReadGenerationEnableShared) <= GenerationDirectStorage::kBytes);
    static_assert(alignof(PairReadGenerationEnableShared) <= GenerationDirectStorage::kAlignment);
    auto next_base = std::move(*merge.state_->builder).freeze();
    merge.state_->builder.reset();
    auto* location =
        storage.claim(sizeof(PairReadGenerationEnableShared), alignof(PairReadGenerationEnableShared));
    try {
        return std::construct_at(static_cast<PairReadGenerationEnableShared*>(location), current.routing_,
                                 std::move(next_base), std::move(merge.state_->post_delta),
                                 current.epoch_ + 1U, current.visible_through_);
    } catch (...) {
        storage.release(location);
        throw;
    }
} catch (const std::bad_alloc&) {

    return fail(ErrorCode::resource_exhausted, "direct incremental merge publication allocation failed");
} catch (...) {
    return fail(ErrorCode::internal_error, "direct incremental merge publication failed");
}

auto PairReadGeneration::start_incremental_merge(std::shared_ptr<const PairReadGeneration> cut,
                                                 const std::size_t maximum_post_entries)
    -> Result<std::unique_ptr<PairReadMerge>> try {
    if (!cut || maximum_post_entries == 0 || cut->delta_->size() == 0 ||
        cut->delta_->size() > cut->delta_->maximum_entries()) {
        return fail(ErrorCode::invalid_argument, "invalid incremental read merge boundary");
    }
    if (cut->base_->size() > std::numeric_limits<std::size_t>::max() - cut->delta_->size()) {
        return fail(ErrorCode::arithmetic_overflow, "incremental read merge size overflows size_t");
    }
    const auto bounded_post_entries =
        std::min(maximum_post_entries, cut->delta_->maximum_entries() - cut->delta_->size());
    auto builder = std::make_unique<IncrementalBaseBuilder>(cut->base_->size() + cut->delta_->size()
#if defined(__unix__) || defined(__APPLE__)
                                                                ,
                                                            cut->base_->mapping_pool()
#endif
    );
    auto post_delta = make_empty_delta(cut->delta_->maximum_entries());
    auto state = std::make_unique<PairReadMerge::State>(cut, std::move(builder), std::move(post_delta),
                                                        bounded_post_entries);
    return std::unique_ptr<PairReadMerge>{new PairReadMerge{std::move(state)}};
} catch (const std::bad_alloc&) {

    return fail(ErrorCode::resource_exhausted, "incremental read merge allocation failed");
} catch (...) {
    return fail(ErrorCode::internal_error, "incremental read merge construction failed");
}

auto PairReadGeneration::advance_incremental_merge(PairReadMerge& merge, const std::size_t maximum_slots)
    -> Result<std::size_t> try {
    if (!merge.state_ || maximum_slots == 0) {
        return fail(ErrorCode::invalid_argument, "invalid incremental read merge quantum");
    }
    auto& state = *merge.state_;
    std::size_t processed{};
    while (state.phase != PairReadMerge::State::Phase::ready) {
        if (state.phase == PairReadMerge::State::Phase::initialize) {
            if (state.builder->initialized()) {
                state.phase = PairReadMerge::State::Phase::base;
                continue;
            }
            if (processed == maximum_slots) {
                break;
            }
            processed += state.builder->initialize_next(maximum_slots - processed);
            continue;
        }
        if (state.phase == PairReadMerge::State::Phase::base) {
            if (state.base_cursor == state.cut->base_->capacity()) {
                state.phase = PairReadMerge::State::Phase::delta;
                continue;
            }
            if (processed == maximum_slots) {
                break;
            }
            auto record = state.cut->base_->record_at(state.base_cursor++);
            ++processed;
            if (!record) {
                continue;
            }
            const HashedKey key{.key = record->key, .hash = record->hash};
            const auto* override = state.cut->delta_->find_handle(key);
            if (override == nullptr) {
                state.builder->insert(*record);
            } else if (override->opcode == Opcode::put) {
                state.builder->insert(delta_record_view(*override, key.hash));
            }
            continue;
        }

        if (state.delta_cursor == state.cut->delta_->capacity()) {
            state.phase = PairReadMerge::State::Phase::ready;
            continue;
        }
        if (processed == maximum_slots) {
            break;
        }
        auto record = state.cut->delta_->record_at(state.delta_cursor++);
        ++processed;
        if (!record || record->opcode == Opcode::erase) {
            continue;
        }
        const HashedKey key{.key = record->key, .hash = record->hash};
        if (!state.builder->contains(key)) {
            state.builder->insert(*record);
        }
    }
    return processed;
} catch (const std::bad_alloc&) {

    return fail(ErrorCode::resource_exhausted, "incremental read merge quantum allocation failed");
} catch (...) {
    return fail(ErrorCode::internal_error, "incremental read merge quantum failed");
}

auto PairReadGeneration::finish_incremental_merge(std::shared_ptr<const PairReadGeneration> current,
                                                  PairReadMerge& merge)
    -> Result<std::shared_ptr<const PairReadGeneration>> try {
    if (!current || !merge.state_ || merge.state_->current.get() != current.get() ||
        merge.state_->phase != PairReadMerge::State::Phase::ready || !merge.state_->builder) {
        return fail(ErrorCode::invalid_argument, "incremental read merge is not ready");
    }
    if (current->epoch_ == std::numeric_limits<std::uint64_t>::max()) {
        return fail(ErrorCode::arithmetic_overflow, "read generation epoch exhausted");
    }
    auto next_base = std::move(*merge.state_->builder).freeze();
    merge.state_->builder.reset();
    return make_shared_generation(current->routing_, std::move(next_base),
                                  std::move(merge.state_->post_delta), current->epoch_ + 1U,
                                  current->visible_through_);
} catch (const std::bad_alloc&) {

    return fail(ErrorCode::resource_exhausted, "incremental read merge publication allocation failed");
} catch (...) {
    return fail(ErrorCode::internal_error, "incremental read merge publication failed");
}

auto PairReadGeneration::merge_ready(const PairReadMerge& merge) noexcept -> bool {
    return merge.state_ && merge.state_->phase == PairReadMerge::State::Phase::ready;
}

auto PairReadGeneration::merge_post_entries(const PairReadMerge& merge) noexcept -> std::size_t {
    return merge.state_ ? merge.state_->post_delta.size() : 0U;
}

auto PairReadGeneration::merge_remaining_slots(const PairReadMerge& merge) noexcept -> std::size_t {
    if (!merge.state_ || !merge.state_->builder) {
        return 0U;
    }
    const auto& state = *merge.state_;
    auto remaining = state.builder->remaining_initialization_slots();
    if (state.phase == PairReadMerge::State::Phase::initialize ||
        state.phase == PairReadMerge::State::Phase::base) {
        remaining = saturating_add(remaining, state.cut->base_->capacity() - state.base_cursor);
    }
    if (state.phase != PairReadMerge::State::Phase::ready) {
        remaining = saturating_add(remaining, state.cut->delta_->capacity() - state.delta_cursor);
    }
    return remaining;
}

auto PairReadGeneration::merge_post_capacity_remaining(const PairReadMerge& merge) noexcept -> std::size_t {
    if (!merge.state_) {
        return 0U;
    }
    const auto& state = *merge.state_;
    if (state.post_delta.size() > state.maximum_post_entries) {
        return 0U;
    }
    return std::min(state.maximum_post_entries - state.post_delta.size(),
                    state.post_delta.available_record_versions());
}

auto PairReadGeneration::merge_advance_budget(const PairReadMerge& merge,
                                              const std::size_t maximum_new_records,
                                              const std::size_t minimum_slots) noexcept -> std::size_t {
    const auto remaining_slots = merge_remaining_slots(merge);
    if (remaining_slots == 0U) {
        return 0U;
    }
    const auto post_capacity = merge_post_capacity_remaining(merge);
    if (maximum_new_records == 0U) {
        return std::min(remaining_slots, minimum_slots);
    }
    if (maximum_new_records >= post_capacity) {
        return remaining_slots;
    }

    // ceil(remaining_slots * maximum_new_records / post_capacity), decomposed
    // before multiplication. maximum_new_records and the remainder are both
    // bounded by kMaximumIncrementalDeltaEntries, so the tail product cannot
    // overflow while the quotient term is at most remaining_slots.
    const auto quotient = remaining_slots / post_capacity;
    const auto remainder = remaining_slots % post_capacity;
    auto proportional = quotient * maximum_new_records;
    const auto tail_product = remainder * maximum_new_records;
    proportional += tail_product / post_capacity;
    if (tail_product % post_capacity != 0U) {
        ++proportional;
    }
    return std::min(remaining_slots, std::max(minimum_slots, proportional));
}

auto PairReadGeneration::can_publish_incremental(const PairReadGeneration& current,
                                                 const PairReadMerge* merge,
                                                 const std::size_t maximum_new_entries) noexcept -> bool {
    const auto delta_size = current.delta_->size();
    if (delta_size > current.delta_->maximum_entries() ||
        maximum_new_entries > current.delta_->maximum_entries() - delta_size ||
        maximum_new_entries > current.delta_->available_record_versions()) {
        return false;
    }
    if (merge == nullptr) {
        return true;
    }
    if (!merge->state_) {
        return false;
    }
    const auto post_size = merge->state_->post_delta.size();
    return post_size <= merge->state_->maximum_post_entries &&
           maximum_new_entries <= merge->state_->maximum_post_entries - post_size &&
           maximum_new_entries <= merge->state_->post_delta.available_record_versions();
}

} // namespace glifistore::store::paired
