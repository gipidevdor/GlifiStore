#pragma once

// ADR 0036 lab-only bridge: generation-shell allocators → PairReadGeneration.
// Lives under experimental/; not part of the official paired publication path.

#include "experimental/pair_read_generation_shell.hpp"
#include "store/paired/read_generation_impl.hpp"

#include <memory>
#include <stdexcept>
#include <utility>

namespace glifistore::store::paired {

[[nodiscard]] inline auto
make_shared_generation_in_shell(WorkerRoutingState routing, std::shared_ptr<const ImmutableReadIndex> base,
                                DeltaState delta, const std::uint64_t epoch,
                                const std::uint64_t visible_through,
                                std::shared_ptr<experimental::PairReadGenerationShellStorage> storage)
    -> std::shared_ptr<const PairReadGeneration> {
    if (!storage) {
        throw std::bad_alloc{};
    }
    GS_PHASE_PUT(generation_shell_allocate);
    using Allocator = experimental::PairReadGenerationShellAllocator<PairReadGenerationEnableShared>;
    return std::allocate_shared<PairReadGenerationEnableShared>(
        Allocator{std::move(storage)}, routing, std::move(base), std::move(delta), epoch, visible_through);
}

[[nodiscard]] inline auto
make_shared_generation_in_borrowed_shell(WorkerRoutingState routing,
                                         std::shared_ptr<const ImmutableReadIndex> base, DeltaState delta,
                                         const std::uint64_t epoch, const std::uint64_t visible_through,
                                         experimental::PairReadGenerationInlineShellStorage& storage)
    -> std::shared_ptr<const PairReadGeneration> {
    GS_PHASE_PUT(generation_shell_allocate);
    using Allocator = experimental::PairReadGenerationBorrowedShellAllocator<PairReadGenerationEnableShared>;
    return std::allocate_shared<PairReadGenerationEnableShared>(Allocator{storage}, routing, std::move(base),
                                                                std::move(delta), epoch, visible_through);
}

} // namespace glifistore::store::paired
