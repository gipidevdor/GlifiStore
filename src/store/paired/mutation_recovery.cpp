#include "glifistore/store/paired/mutation_recovery.hpp"

namespace glifistore::store::paired {

auto plan_sync_durable_exception_recovery(const SyncDurableExceptionContext& ctx) noexcept
    -> SyncDurableRecoveryActions {
    SyncDurableRecoveryActions out{};
    const bool store_entered = ctx.durable_committed || ctx.durable_mutate_entered;
    if (!store_entered) {
        return out;
    }
    out.fail_closed = true;
    out.drain_if_unpublished = !ctx.generation_published;
    // Shadow lifecycle: only advances when still at durable_started (no result applied).
    out.mark_exception_lifecycle = true;
    return out;
}

auto plan_sync_durable_exception_status(const SyncDurableExceptionContext& ctx) noexcept
    -> SyncDurableExceptionStatusPlan {
    SyncDurableExceptionStatusPlan out{};
    if (ctx.status_resolved) {
        out.kind = SyncDurableExceptionStatusKind::keep_resolved;
        return out;
    }
    if (ctx.generation_published && ctx.durable_committed) {
        out.kind = SyncDurableExceptionStatusKind::success_after_visibility;
        return out;
    }
    if (ctx.durable_committed || ctx.durable_mutate_entered) {
        out.kind = SyncDurableExceptionStatusKind::unavailable_store_entered;
        return out;
    }
    out.kind = SyncDurableExceptionStatusKind::resource_exhausted_never_entered;
    return out;
}

} // namespace glifistore::store::paired
