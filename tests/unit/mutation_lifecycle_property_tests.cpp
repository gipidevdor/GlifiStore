#include "glifistore/store/paired/completion_policy.hpp"
#include "glifistore/store/paired/lane_state.hpp"
#include "glifistore/store/paired/mutation_state.hpp"
#include "test.hpp"

#include <array>

using glifistore::store::paired::CommitKnowledge;
using glifistore::store::paired::CompletionDecision;
using glifistore::store::paired::decide_completion;
using glifistore::store::paired::DurableDecision;
using glifistore::store::paired::MutationLifecycle;
using glifistore::store::paired::MutationStage;
using glifistore::store::paired::PublicationDecision;
using glifistore::store::paired::PublicationState;
using glifistore::store::paired::wire_error_code_for;

GLIFI_TEST("property OVERLOADED wire code iff known_not_committed completion") {
    const std::array knowledge{
        CommitKnowledge::known_not_committed,
        CommitKnowledge::committed,
        CommitKnowledge::indeterminate,
    };
    const std::array publications{
        PublicationState::not_required, PublicationState::required, PublicationState::staged,
        PublicationState::published,    PublicationState::failed,
    };
    for (const auto commit : knowledge) {
        for (const auto publication : publications) {
            const DurableDecision durable{.knowledge = commit, .mutate_entered = true};
            const PublicationDecision pub{.state = publication};
            const auto decided = decide_completion(durable, pub);
            if (decided.kind == CompletionDecision::Kind::undecided) {
                continue;
            }
            const auto code = wire_error_code_for(decided.kind);
            if (decided.kind == CompletionDecision::Kind::known_not_committed) {
                GLIFI_REQUIRE(code == glifistore::ErrorCode::resource_exhausted);
            } else if (decided.kind == CompletionDecision::Kind::indeterminate) {
                GLIFI_REQUIRE(code == glifistore::ErrorCode::unavailable);
            } else if (decided.kind == CompletionDecision::Kind::success) {
                // success must not be routed through wire_error_code_for by callers;
                // the helper returns internal_error as a guard rail.
                GLIFI_REQUIRE(code == glifistore::ErrorCode::internal_error);
            }
        }
    }
}

GLIFI_TEST("property decided completion cannot change outcome") {
    MutationLifecycle life;
    GLIFI_REQUIRE(life.admit());
    GLIFI_REQUIRE(life.expire_pre_store());
    GLIFI_REQUIRE(life.stage() == MutationStage::completion_decided);
    CompletionDecision again{.kind = CompletionDecision::Kind::success};
    GLIFI_REQUIRE(!life.decide(again));
    GLIFI_REQUIRE(life.stage() == MutationStage::completion_decided);
}

GLIFI_TEST("lane_state aggregates keep cache-line alignment contracts") {
    static_assert(alignof(glifistore::store::paired::AsyncLaneState) >= 128);
    static_assert(alignof(glifistore::store::paired::GenerationState) >= 128);
    static_assert(alignof(glifistore::store::paired::SyncLaneState) >= 128);
    static_assert(alignof(glifistore::store::paired::ReclamationState) >= 128);
}
