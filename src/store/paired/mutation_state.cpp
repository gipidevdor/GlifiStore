#include "glifistore/store/paired/mutation_state.hpp"

#include <utility>

namespace glifistore::store::paired {
namespace {

[[nodiscard]] auto can_enter_durable(const MutationStage stage) noexcept -> bool {
    return stage == MutationStage::staged || stage == MutationStage::admitted;
}

} // namespace

auto decide_completion(const DurableDecision& durable, const PublicationDecision& publication) noexcept
    -> CompletionDecision {
    CompletionDecision out{};

    if (!durable.mutate_entered && durable.knowledge == CommitKnowledge::known_not_committed &&
        !durable.error.has_value() && publication.state == PublicationState::not_required) {
        // Expired / never started Store work → known-not-committed (OVERLOADED).
        out.kind = CompletionDecision::Kind::known_not_committed;
        return out;
    }

    switch (durable.knowledge) {
    case CommitKnowledge::known_not_committed:
        out.kind = CompletionDecision::Kind::known_not_committed;
        out.fail_closed_required = false;
        out.drain_required = false;
        return out;

    case CommitKnowledge::indeterminate:
        out.kind = CompletionDecision::Kind::indeterminate;
        out.fail_closed_required = true;
        out.drain_required = true;
        return out;

    case CommitKnowledge::committed:
        if (durable.clean_commit()) {
            if (publication.published()) {
                out.kind = CompletionDecision::Kind::success;
                return out;
            }
            // Clean commit without visibility: treat as sticky indeterminate (capture/publish fail).
            out.kind = CompletionDecision::Kind::indeterminate;
            out.fail_closed_required = true;
            out.drain_required = !publication.published();
            return out;
        }
        // Committed with error: sticky; success only after published visibility.
        out.fail_closed_required = true;
        out.drain_required = !publication.published();
        if (publication.published()) {
            out.kind = CompletionDecision::Kind::success;
        } else {
            out.kind = CompletionDecision::Kind::indeterminate;
        }
        return out;
    }
    out.kind = CompletionDecision::Kind::indeterminate;
    out.fail_closed_required = true;
    return out;
}

auto MutationLifecycle::transition_allowed(const MutationStage from, const MutationStage to) noexcept
    -> bool {
    if (from == to) {
        return true;
    }
    switch (from) {
    case MutationStage::not_admitted:
        return to == MutationStage::admitted;
    case MutationStage::admitted:
        return to == MutationStage::staged || to == MutationStage::rejected ||
               to == MutationStage::expired_pre_store;
    case MutationStage::staged:
        return to == MutationStage::durable_started || to == MutationStage::expired_pre_store ||
               to == MutationStage::rejected;
    case MutationStage::expired_pre_store:
        return to == MutationStage::completion_decided || to == MutationStage::rejected ||
               to == MutationStage::completed;
    case MutationStage::durable_started:
        return to == MutationStage::authority_committed || to == MutationStage::rejected ||
               to == MutationStage::indeterminate || to == MutationStage::publication_required;
    case MutationStage::authority_committed:
        return to == MutationStage::publication_required;
    case MutationStage::publication_required:
        return to == MutationStage::published || to == MutationStage::indeterminate;
    case MutationStage::published:
        return to == MutationStage::completion_decided;
    case MutationStage::indeterminate:
        return to == MutationStage::completion_decided || to == MutationStage::publication_required;
    case MutationStage::rejected:
        return to == MutationStage::completion_decided || to == MutationStage::completed;
    case MutationStage::completion_decided:
        return to == MutationStage::completed;
    case MutationStage::completed:
        return false;
    }
    return false;
}

auto MutationLifecycle::admit() noexcept -> bool {
    if (stage_ != MutationStage::not_admitted) {
        return false;
    }
    stage_ = MutationStage::admitted;
    return true;
}

auto MutationLifecycle::stage_for_writer() noexcept -> bool {
    if (stage_ != MutationStage::admitted) {
        return false;
    }
    stage_ = MutationStage::staged;
    return true;
}

auto MutationLifecycle::expire_pre_store() noexcept -> bool {
    if (stage_ != MutationStage::admitted && stage_ != MutationStage::staged) {
        return false;
    }
    stage_ = MutationStage::expired_pre_store;
    durable_ = {};
    publication_ = {};
    completion_ = decide_completion(durable_, publication_);
    stage_ = MutationStage::completion_decided;
    return true;
}

auto MutationLifecycle::mark_durable_started() noexcept -> bool {
    if (!can_enter_durable(stage_)) {
        return false;
    }
    stage_ = MutationStage::durable_started;
    durable_.mutate_entered = true;
    return true;
}

auto MutationLifecycle::apply_durable_result(const DurableMutationResult& result) noexcept -> bool {
    if (stage_ != MutationStage::durable_started) {
        return false;
    }
    durable_.mutate_entered = true;
    durable_.knowledge = commit_knowledge_from(result.outcome);
    if (result.error) {
        // This lifecycle is a control-flow shadow; only presence and code are
        // authoritative here. Avoid copying diagnostic strings in a noexcept
        // transition used from allocation-failure recovery.
        Error shadow_error{};
        shadow_error.code = result.error->code;
        durable_.error.emplace(std::move(shadow_error));
    } else {
        durable_.error.reset();
    }
    switch (durable_.knowledge) {
    case CommitKnowledge::committed:
        stage_ = MutationStage::authority_committed;
        publication_.state = PublicationState::required;
        stage_ = MutationStage::publication_required;
        return true;
    case CommitKnowledge::indeterminate:
        stage_ = MutationStage::indeterminate;
        publication_.state = PublicationState::required;
        return true;
    case CommitKnowledge::known_not_committed:
        stage_ = MutationStage::rejected;
        publication_.state = PublicationState::not_required;
        completion_ = decide_completion(durable_, publication_);
        stage_ = MutationStage::completion_decided;
        return true;
    }
    return false;
}

auto MutationLifecycle::require_publication() noexcept -> bool {
    if (stage_ != MutationStage::authority_committed && stage_ != MutationStage::indeterminate &&
        stage_ != MutationStage::publication_required) {
        return false;
    }
    publication_.state = PublicationState::required;
    stage_ = MutationStage::publication_required;
    return true;
}

auto MutationLifecycle::mark_publication_staged() noexcept -> bool {
    if (stage_ != MutationStage::publication_required) {
        return false;
    }
    publication_.state = PublicationState::staged;
    return true;
}

auto MutationLifecycle::mark_published() noexcept -> bool {
    if (stage_ != MutationStage::publication_required && publication_.state != PublicationState::staged &&
        publication_.state != PublicationState::required) {
        return false;
    }
    // Illegal: published → known_not_committed must stay impossible.
    if (durable_.knowledge == CommitKnowledge::known_not_committed) {
        return false;
    }
    publication_.state = PublicationState::published;
    stage_ = MutationStage::published;
    return true;
}

auto MutationLifecycle::mark_publication_failed() noexcept -> bool {
    if (stage_ != MutationStage::publication_required && publication_.state != PublicationState::staged &&
        publication_.state != PublicationState::required) {
        return false;
    }
    publication_.state = PublicationState::failed;
    stage_ = MutationStage::indeterminate;
    return true;
}

auto MutationLifecycle::mark_exception_after_durable_start() noexcept -> bool {
    if (stage_ != MutationStage::durable_started) {
        return false;
    }
    durable_.mutate_entered = true;
    durable_.knowledge = CommitKnowledge::indeterminate;
    Error shadow_error{};
    shadow_error.code = ErrorCode::unavailable;
    durable_.error.emplace(std::move(shadow_error));
    publication_.state = PublicationState::required;
    stage_ = MutationStage::indeterminate;
    return true;
}

auto MutationLifecycle::decide(CompletionDecision decision) noexcept -> bool {
    if (stage_ == MutationStage::completion_decided || stage_ == MutationStage::completed) {
        // completion_decided → outcome changed is illegal.
        return false;
    }
    if (decision.kind == CompletionDecision::Kind::undecided) {
        return false;
    }
    // committed → overloaded (known_not_committed completion) is illegal.
    if (durable_.knowledge == CommitKnowledge::committed &&
        decision.kind == CompletionDecision::Kind::known_not_committed) {
        return false;
    }
    switch (stage_) {
    case MutationStage::published:
    case MutationStage::indeterminate:
    case MutationStage::rejected:
    case MutationStage::expired_pre_store:
        break;
    default:
        return false;
    }
    completion_ = decision;
    stage_ = MutationStage::completion_decided;
    return true;
}

auto MutationLifecycle::mark_completed() noexcept -> bool {
    if (stage_ != MutationStage::completion_decided) {
        return false;
    }
    stage_ = MutationStage::completed;
    return true;
}

} // namespace glifistore::store::paired
