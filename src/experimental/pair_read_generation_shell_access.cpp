// ADR 0036 lab-only ShellAccess definitions. Not part of the official paired
// publication path; linked only into tests and generation-shell benchmarks.

#include "experimental/pair_read_generation_shell_bridge.hpp"

#include <utility>

namespace glifistore::experimental {

auto PairReadGenerationShellAccess::publish_incremental(
    std::shared_ptr<const store::paired::PairReadGeneration> previous,
    const std::span<const store::paired::ReadMutation> mutations,
    std::shared_ptr<PairReadGenerationShellStorage> storage, store::paired::PairReadMerge* merge)
    -> Result<std::shared_ptr<const store::paired::PairReadGeneration>> try {
    if (!previous) {
        return fail(ErrorCode::invalid_argument, "invalid incremental read publication");
    }
    if (!storage) {
        return fail(ErrorCode::invalid_argument, "shell publication requires storage");
    }
    const auto& previous_view = *previous;
    auto prepared =
        store::paired::IncrementalPublicationAccess::prepare(previous_view, &previous, mutations, merge);
    if (!prepared) {
        return unexpected(std::move(prepared.error()));
    }
    if (prepared->empty_reuse) {
        return std::move(prepared->empty_reuse);
    }
    auto& next_delta = prepared->next_delta;
    if (!next_delta) {
        return fail(ErrorCode::internal_error, "shell publication produced no next delta");
    }
    auto delta = std::move(next_delta).value();
    auto next = store::paired::make_shared_generation_in_shell(previous_view.routing_, previous_view.base_,
                                                               std::move(delta), previous_view.epoch_ + 1U,
                                                               prepared->visible_through, std::move(storage));
    store::paired::IncrementalPublicationAccess::commit_merge(merge, *prepared, next);
    return next;
} catch (const std::bad_alloc&) {

    return fail(ErrorCode::resource_exhausted, "incremental read publication allocation failed");
} catch (...) {
    return fail(ErrorCode::internal_error, "incremental read publication failed");
}

auto PairReadGenerationShellAccess::empty_direct(const WorkerRoutingState routing,
                                                 PairReadGenerationDirectStorage& storage)
    -> Result<const store::paired::PairReadGeneration*> {
    return store::paired::PairReadGeneration::empty_direct(routing, storage);
}

auto PairReadGenerationShellAccess::publish_incremental_borrowed(
    std::shared_ptr<const store::paired::PairReadGeneration> previous,
    const std::span<const store::paired::ReadMutation> mutations,
    PairReadGenerationInlineShellStorage& storage)
    -> Result<std::shared_ptr<const store::paired::PairReadGeneration>> try {
    if (!previous) {
        return fail(ErrorCode::invalid_argument, "invalid incremental read publication");
    }
    const auto& previous_view = *previous;
    auto prepared =
        store::paired::IncrementalPublicationAccess::prepare(previous_view, &previous, mutations, nullptr);
    if (!prepared) {
        return unexpected(std::move(prepared.error()));
    }
    if (prepared->empty_reuse) {
        return std::move(prepared->empty_reuse);
    }
    auto& next_delta = prepared->next_delta;
    if (!next_delta) {
        return fail(ErrorCode::internal_error, "borrowed shell publication produced no next delta");
    }
    auto delta = std::move(next_delta).value();
    return store::paired::make_shared_generation_in_borrowed_shell(
        previous_view.routing_, previous_view.base_, std::move(delta), previous_view.epoch_ + 1U,
        prepared->visible_through, storage);
} catch (const std::bad_alloc&) {

    return fail(ErrorCode::resource_exhausted, "incremental read publication allocation failed");
} catch (...) {
    return fail(ErrorCode::internal_error, "incremental read publication failed");
}

auto PairReadGenerationShellAccess::publish_incremental_direct(
    const store::paired::PairReadGeneration& previous,
    const std::span<const store::paired::ReadMutation> mutations, PairReadGenerationDirectStorage& storage)
    -> Result<const store::paired::PairReadGeneration*> {
    return store::paired::PairReadGeneration::publish_incremental_direct(previous, mutations, storage);
}

void PairReadGenerationShellAccess::destroy_direct(const store::paired::PairReadGeneration* generation,
                                                   PairReadGenerationDirectStorage& storage) noexcept {
    store::paired::PairReadGeneration::destroy_direct(generation, storage);
}

} // namespace glifistore::experimental
