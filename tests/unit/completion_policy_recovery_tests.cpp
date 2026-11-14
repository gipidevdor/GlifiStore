#include "glifistore/store/paired/completion_policy.hpp"
#include "glifistore/store/paired/mutation_recovery.hpp"
#include "test.hpp"

using glifistore::store::paired::CommitKnowledge;
using glifistore::store::paired::CompletionDecision;
using glifistore::store::paired::decide_completion;
using glifistore::store::paired::DurableDecision;
using glifistore::store::paired::plan_sync_durable_exception_recovery;
using glifistore::store::paired::plan_sync_durable_exception_status;
using glifistore::store::paired::PublicationDecision;
using glifistore::store::paired::PublicationState;
using glifistore::store::paired::status_from_completion;
using glifistore::store::paired::SyncDurableExceptionContext;
using glifistore::store::paired::SyncDurableExceptionStatusKind;
using glifistore::store::paired::wire_error_code_for;

GLIFI_TEST("completion_policy wire codes match taxonomy") {
    GLIFI_REQUIRE(wire_error_code_for(CompletionDecision::Kind::known_not_committed) ==
                  glifistore::ErrorCode::resource_exhausted);
    GLIFI_REQUIRE(wire_error_code_for(CompletionDecision::Kind::indeterminate) ==
                  glifistore::ErrorCode::unavailable);
}

GLIFI_TEST("completion_policy status_from_completion polarities") {
    {
        CompletionDecision d{.kind = CompletionDecision::Kind::success};
        const auto status = status_from_completion(d, "k", "i");
        GLIFI_REQUIRE(status.has_value());
    }
    {
        CompletionDecision d{.kind = CompletionDecision::Kind::known_not_committed};
        const auto status = status_from_completion(d, "known", "indet");
        GLIFI_REQUIRE(!status.has_value());
        GLIFI_REQUIRE(status.error().code == glifistore::ErrorCode::resource_exhausted);
        GLIFI_REQUIRE(status.error().message == "known");
    }
    {
        CompletionDecision d{.kind = CompletionDecision::Kind::indeterminate};
        const auto status = status_from_completion(d, "known", "indet");
        GLIFI_REQUIRE(!status.has_value());
        GLIFI_REQUIRE(status.error().code == glifistore::ErrorCode::unavailable);
        GLIFI_REQUIRE(status.error().message == "indet");
    }
    // decide_completion → status_from_completion round-trip for known-not-committed.
    {
        DurableDecision durable{.knowledge = CommitKnowledge::known_not_committed, .mutate_entered = true};
        PublicationDecision publication{};
        const auto decided = decide_completion(durable, publication);
        const auto status = status_from_completion(decided, "overloaded", "sticky");
        GLIFI_REQUIRE(!status.has_value());
        GLIFI_REQUIRE(status.error().code == glifistore::ErrorCode::resource_exhausted);
    }
}

GLIFI_TEST("mutation_recovery sync durable exception: never entered") {
    const SyncDurableExceptionContext ctx{};
    const auto recovery = plan_sync_durable_exception_recovery(ctx);
    GLIFI_REQUIRE(!recovery.drain_if_unpublished);
    GLIFI_REQUIRE(!recovery.fail_closed);
    GLIFI_REQUIRE(!recovery.mark_exception_lifecycle);
    const auto status = plan_sync_durable_exception_status(ctx);
    GLIFI_REQUIRE(status.kind == SyncDurableExceptionStatusKind::resource_exhausted_never_entered);
}

GLIFI_TEST("mutation_recovery sync durable exception: entered unpublished") {
    const SyncDurableExceptionContext ctx{.durable_mutate_entered = true};
    const auto recovery = plan_sync_durable_exception_recovery(ctx);
    GLIFI_REQUIRE(recovery.drain_if_unpublished);
    GLIFI_REQUIRE(recovery.fail_closed);
    GLIFI_REQUIRE(recovery.mark_exception_lifecycle);
    const auto status = plan_sync_durable_exception_status(ctx);
    GLIFI_REQUIRE(status.kind == SyncDurableExceptionStatusKind::unavailable_store_entered);
}

GLIFI_TEST("mutation_recovery sync durable exception: published committed keeps success") {
    const SyncDurableExceptionContext ctx{
        .durable_committed = true, .durable_mutate_entered = true, .generation_published = true};
    const auto recovery = plan_sync_durable_exception_recovery(ctx);
    GLIFI_REQUIRE(!recovery.drain_if_unpublished);
    GLIFI_REQUIRE(recovery.fail_closed);
    const auto status = plan_sync_durable_exception_status(ctx);
    GLIFI_REQUIRE(status.kind == SyncDurableExceptionStatusKind::success_after_visibility);
}

GLIFI_TEST("mutation_recovery sync durable exception: status_resolved sticks") {
    const SyncDurableExceptionContext ctx{.durable_committed = true,
                                          .durable_mutate_entered = true,
                                          .generation_published = true,
                                          .status_resolved = true};
    const auto status = plan_sync_durable_exception_status(ctx);
    GLIFI_REQUIRE(status.kind == SyncDurableExceptionStatusKind::keep_resolved);
}
