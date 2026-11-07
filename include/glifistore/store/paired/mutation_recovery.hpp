/**
 * @file
 * @brief Exception recovery plans derived from durable commit and publication progress.
 */
#pragma once

// Exception recovery plans for the paired Writer (behavior-neutral extraction).
// Normative: docs/spec/mutation-lifecycle.md §3–4
//
// Pure planning: catch blocks call plan_* then apply drain / fail-closed / Status
// in ShardPairRuntime. No Store I/O here.

#include <cstdint>

namespace glifistore::store::paired {

/**
 * @brief Recorded durable-entry/publication progress used to classify a synchronous exception.
 */
struct SyncDurableExceptionContext final {
    bool durable_committed{};
    bool durable_mutate_entered{};
    bool generation_published{};
    bool status_resolved{};
};

/**
 * @brief Planned drain, fail-closed, and lifecycle actions after a synchronous exception.
 */
struct SyncDurableRecoveryActions final {
    /// Drain sibling/Index snapshot when Store was entered and generation not yet published.
    bool drain_if_unpublished{};
    /// Sticky pair close when Store was entered or a commit was observed.
    bool fail_closed{};
    /// Advance MutationLifecycle from durable_started → indeterminate (shadow).
    bool mark_exception_lifecycle{};
};

/**
 * @brief Completion category selected from known synchronous durable progress.
 */
enum class SyncDurableExceptionStatusKind : std::uint8_t {
    keep_resolved = 0,
    success_after_visibility = 1,
    unavailable_store_entered = 2,
    resource_exhausted_never_entered = 3,
};

/**
 * @brief Planned error/containment outcome after a synchronous durable exception.
 */
struct SyncDurableExceptionStatusPlan final {
    SyncDurableExceptionStatusKind kind{SyncDurableExceptionStatusKind::keep_resolved};
};

/// Phase 1 of catch: decide drain / fail-closed / lifecycle advance.
[[nodiscard]] auto plan_sync_durable_exception_recovery(const SyncDurableExceptionContext& ctx) noexcept
    -> SyncDurableRecoveryActions;

/// Phase 2 of catch: decide Status polarity after drain may have published.
/// Uses generation_published from the updated context.
[[nodiscard]] auto plan_sync_durable_exception_status(const SyncDurableExceptionContext& ctx) noexcept
    -> SyncDurableExceptionStatusPlan;

} // namespace glifistore::store::paired
