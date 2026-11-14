#include "glifistore/store/paired/mutation_state.hpp"
#include "test.hpp"

#include <optional>

using glifistore::store::paired::commit_knowledge_from;
using glifistore::store::paired::CommitKnowledge;
using glifistore::store::paired::CompletionDecision;
using glifistore::store::paired::decide_completion;
using glifistore::store::paired::durable_outcome_from;
using glifistore::store::paired::DurableDecision;
using glifistore::store::paired::MutationLifecycle;
using glifistore::store::paired::MutationStage;
using glifistore::store::paired::PublicationDecision;
using glifistore::store::paired::PublicationState;

GLIFI_TEST("mutation_state CommitKnowledge aliases DurableMutationOutcome") {
    GLIFI_REQUIRE(commit_knowledge_from(glifistore::DurableMutationOutcome::committed) ==
                  CommitKnowledge::committed);
    GLIFI_REQUIRE(commit_knowledge_from(glifistore::DurableMutationOutcome::not_committed) ==
                  CommitKnowledge::known_not_committed);
    GLIFI_REQUIRE(commit_knowledge_from(glifistore::DurableMutationOutcome::indeterminate) ==
                  CommitKnowledge::indeterminate);
    GLIFI_REQUIRE(durable_outcome_from(CommitKnowledge::committed) ==
                  glifistore::DurableMutationOutcome::committed);
}

GLIFI_TEST("mutation_state illegal transitions are rejected by table") {
    GLIFI_REQUIRE(
        !MutationLifecycle::transition_allowed(MutationStage::not_admitted, MutationStage::completed));
    GLIFI_REQUIRE(!MutationLifecycle::transition_allowed(MutationStage::published, MutationStage::rejected));
    GLIFI_REQUIRE(!MutationLifecycle::transition_allowed(MutationStage::completed, MutationStage::admitted));
    GLIFI_REQUIRE(
        MutationLifecycle::transition_allowed(MutationStage::not_admitted, MutationStage::admitted));
    GLIFI_REQUIRE(MutationLifecycle::transition_allowed(MutationStage::durable_started,
                                                        MutationStage::authority_committed));
}

GLIFI_TEST("mutation_state happy durable path reaches completed success") {
    MutationLifecycle life;
    GLIFI_REQUIRE(life.admit());
    GLIFI_REQUIRE(life.stage_for_writer());
    GLIFI_REQUIRE(life.mark_durable_started());
    glifistore::DurableMutationResult result{.outcome = glifistore::DurableMutationOutcome::committed,
                                             .sequence = glifistore::SequenceNumber{1},
                                             .error = std::nullopt};
    GLIFI_REQUIRE(life.apply_durable_result(result));
    GLIFI_REQUIRE(life.stage() == MutationStage::publication_required);
    GLIFI_REQUIRE(life.mark_publication_staged());
    GLIFI_REQUIRE(life.mark_published());
    const auto decided = decide_completion(life.durable(), life.publication());
    GLIFI_REQUIRE(decided.kind == CompletionDecision::Kind::success);
    GLIFI_REQUIRE(!decided.fail_closed_required);
    GLIFI_REQUIRE(life.decide(decided));
    GLIFI_REQUIRE(life.mark_completed());
    GLIFI_REQUIRE(life.stage() == MutationStage::completed);
}

GLIFI_TEST("mutation_state known-not-committed never becomes success") {
    MutationLifecycle life;
    GLIFI_REQUIRE(life.admit());
    GLIFI_REQUIRE(life.stage_for_writer());
    GLIFI_REQUIRE(life.mark_durable_started());
    glifistore::DurableMutationResult result{
        .outcome = glifistore::DurableMutationOutcome::not_committed,
        .sequence = std::nullopt,
        .error = glifistore::Error{glifistore::ErrorCode::segment_full, "full"}};
    GLIFI_REQUIRE(life.apply_durable_result(result));
    GLIFI_REQUIRE(life.stage() == MutationStage::completion_decided);
    GLIFI_REQUIRE(life.completion().kind == CompletionDecision::Kind::known_not_committed);
    GLIFI_REQUIRE(!life.mark_published());
}

GLIFI_TEST("mutation_state committed cannot decide known_not_committed") {
    MutationLifecycle life;
    GLIFI_REQUIRE(life.admit());
    GLIFI_REQUIRE(life.stage_for_writer());
    GLIFI_REQUIRE(life.mark_durable_started());
    glifistore::DurableMutationResult result{.outcome = glifistore::DurableMutationOutcome::committed,
                                             .sequence = glifistore::SequenceNumber{2},
                                             .error = std::nullopt};
    GLIFI_REQUIRE(life.apply_durable_result(result));
    GLIFI_REQUIRE(life.mark_published());
    CompletionDecision illegal{.kind = CompletionDecision::Kind::known_not_committed};
    GLIFI_REQUIRE(!life.decide(illegal));
}

GLIFI_TEST("mutation_state completion_decided cannot change outcome") {
    MutationLifecycle life;
    GLIFI_REQUIRE(life.admit());
    GLIFI_REQUIRE(life.expire_pre_store());
    GLIFI_REQUIRE(life.stage() == MutationStage::completion_decided);
    CompletionDecision again{.kind = CompletionDecision::Kind::success};
    GLIFI_REQUIRE(!life.decide(again));
}

GLIFI_TEST("mutation_state exception after durable_started is indeterminate") {
    MutationLifecycle life;
    GLIFI_REQUIRE(life.admit());
    GLIFI_REQUIRE(life.stage_for_writer());
    GLIFI_REQUIRE(life.mark_durable_started());
    GLIFI_REQUIRE(life.mark_exception_after_durable_start());
    GLIFI_REQUIRE(life.stage() == MutationStage::indeterminate);
    GLIFI_REQUIRE(life.durable().knowledge == CommitKnowledge::indeterminate);
    GLIFI_REQUIRE(life.durable().mutate_entered);
    GLIFI_REQUIRE(life.publication().state == PublicationState::required);
    const auto decided = decide_completion(life.durable(), life.publication());
    GLIFI_REQUIRE(decided.kind == CompletionDecision::Kind::indeterminate);
    GLIFI_REQUIRE(decided.fail_closed_required);
    GLIFI_REQUIRE(life.decide(decided));
    GLIFI_REQUIRE(life.mark_completed());
}

GLIFI_TEST("mutation_state decide_completion characterization table") {
    // known_not_committed → OVERLOADED polarity, no fail-closed.
    {
        DurableDecision d{.knowledge = CommitKnowledge::known_not_committed,
                          .error = glifistore::Error{glifistore::ErrorCode::io_error, {}},
                          .mutate_entered = true};
        PublicationDecision p{};
        const auto c = decide_completion(d, p);
        GLIFI_REQUIRE(c.kind == CompletionDecision::Kind::known_not_committed);
        GLIFI_REQUIRE(!c.fail_closed_required);
    }
    // clean commit + published → success.
    {
        DurableDecision d{.knowledge = CommitKnowledge::committed, .mutate_entered = true};
        PublicationDecision p{.state = PublicationState::published};
        const auto c = decide_completion(d, p);
        GLIFI_REQUIRE(c.kind == CompletionDecision::Kind::success);
        GLIFI_REQUIRE(!c.fail_closed_required);
    }
    // clean commit without publish → indeterminate + fail-closed.
    {
        DurableDecision d{.knowledge = CommitKnowledge::committed, .mutate_entered = true};
        PublicationDecision p{.state = PublicationState::failed};
        const auto c = decide_completion(d, p);
        GLIFI_REQUIRE(c.kind == CompletionDecision::Kind::indeterminate);
        GLIFI_REQUIRE(c.fail_closed_required);
    }
    // committed+error + published → success (ACK-after-visibility), fail-closed.
    {
        DurableDecision d{.knowledge = CommitKnowledge::committed,
                          .error = glifistore::Error{glifistore::ErrorCode::internal_error, {}},
                          .mutate_entered = true};
        PublicationDecision p{.state = PublicationState::published};
        const auto c = decide_completion(d, p);
        GLIFI_REQUIRE(c.kind == CompletionDecision::Kind::success);
        GLIFI_REQUIRE(c.fail_closed_required);
    }
    // indeterminate → indeterminate + drain + fail-closed.
    {
        DurableDecision d{.knowledge = CommitKnowledge::indeterminate, .mutate_entered = true};
        PublicationDecision p{.state = PublicationState::failed};
        const auto c = decide_completion(d, p);
        GLIFI_REQUIRE(c.kind == CompletionDecision::Kind::indeterminate);
        GLIFI_REQUIRE(c.fail_closed_required);
        GLIFI_REQUIRE(c.drain_required);
    }
    // expired pre-store (never entered).
    {
        DurableDecision d{};
        PublicationDecision p{};
        const auto c = decide_completion(d, p);
        GLIFI_REQUIRE(c.kind == CompletionDecision::Kind::known_not_committed);
        GLIFI_REQUIRE(!c.fail_closed_required);
    }
}
