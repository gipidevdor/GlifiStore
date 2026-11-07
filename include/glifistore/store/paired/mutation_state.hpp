#pragma once

// Typed mutation lifecycle for the paired Writer (behavior-neutral extraction).
// Normative as-implemented map: docs/spec/mutation-lifecycle.md
//
// CommitKnowledge aliases DurableMutationOutcome. Publication and completion are
// separate authorities. Illegal transitions return false (debug assert optional).

#include "glifistore/core/error.hpp"
#include "glifistore/persistence/runtime_catalog.hpp"

#include <cstdint>
#include <optional>

namespace glifistore::store::paired {

enum class CommitKnowledge : std::uint8_t {
    known_not_committed = 0,
    committed = 1,
    indeterminate = 2,
};

enum class PublicationState : std::uint8_t {
    not_required = 0,
    required = 1,
    staged = 2,
    published = 3,
    failed = 4,
};

enum class MutationStage : std::uint8_t {
    not_admitted = 0,
    admitted = 1,
    staged = 2,
    expired_pre_store = 3,
    durable_started = 4,
    authority_committed = 5,
    publication_required = 6,
    published = 7,
    completion_decided = 8,
    completed = 9,
    rejected = 10,
    indeterminate = 11,
};

[[nodiscard]] constexpr auto commit_knowledge_from(const DurableMutationOutcome outcome) noexcept
    -> CommitKnowledge {
    switch (outcome) {
    case DurableMutationOutcome::committed:
        return CommitKnowledge::committed;
    case DurableMutationOutcome::indeterminate:
        return CommitKnowledge::indeterminate;
    case DurableMutationOutcome::not_committed:
        return CommitKnowledge::known_not_committed;
    }
    return CommitKnowledge::known_not_committed;
}

[[nodiscard]] constexpr auto durable_outcome_from(const CommitKnowledge knowledge) noexcept
    -> DurableMutationOutcome {
    switch (knowledge) {
    case CommitKnowledge::committed:
        return DurableMutationOutcome::committed;
    case CommitKnowledge::indeterminate:
        return DurableMutationOutcome::indeterminate;
    case CommitKnowledge::known_not_committed:
        return DurableMutationOutcome::not_committed;
    }
    return DurableMutationOutcome::not_committed;
}

struct DurableDecision final {
    CommitKnowledge knowledge{CommitKnowledge::known_not_committed};
    std::optional<Error> error{};
    bool mutate_entered{};

    [[nodiscard]] auto committed() const noexcept -> bool {
        return knowledge == CommitKnowledge::committed;
    }

    [[nodiscard]] auto clean_commit() const noexcept -> bool {
        return committed() && !error.has_value();
    }
};

struct PublicationDecision final {
    PublicationState state{PublicationState::not_required};

    [[nodiscard]] auto published() const noexcept -> bool {
        return state == PublicationState::published;
    }

    [[nodiscard]] auto failed() const noexcept -> bool {
        return state == PublicationState::failed;
    }
};

struct CompletionDecision final {
    enum class Kind : std::uint8_t {
        undecided = 0,
        success = 1,
        known_not_committed = 2,
        indeterminate = 3,
    };

    Kind kind{Kind::undecided};
    bool fail_closed_required{};
    bool drain_required{};
};

// Pure characterization of as-implemented completion polarity (mutation-lifecycle.md §4).
// Does not perform Store I/O or wire encoding.
[[nodiscard]] auto decide_completion(const DurableDecision& durable,
                                     const PublicationDecision& publication) noexcept -> CompletionDecision;

class MutationLifecycle final {
  public:
    [[nodiscard]] auto stage() const noexcept -> MutationStage {
        return stage_;
    }

    [[nodiscard]] auto durable() const noexcept -> const DurableDecision& {
        return durable_;
    }

    [[nodiscard]] auto publication() const noexcept -> const PublicationDecision& {
        return publication_;
    }

    [[nodiscard]] auto completion() const noexcept -> const CompletionDecision& {
        return completion_;
    }

    [[nodiscard]] auto admit() noexcept -> bool;
    [[nodiscard]] auto stage_for_writer() noexcept -> bool;
    [[nodiscard]] auto expire_pre_store() noexcept -> bool;
    [[nodiscard]] auto mark_durable_started() noexcept -> bool;
    [[nodiscard]] auto apply_durable_result(const DurableMutationResult& result) noexcept -> bool;
    [[nodiscard]] auto require_publication() noexcept -> bool;
    [[nodiscard]] auto mark_publication_staged() noexcept -> bool;
    [[nodiscard]] auto mark_published() noexcept -> bool;
    [[nodiscard]] auto mark_publication_failed() noexcept -> bool;
    // Exception after durable mutate entered but before a DurableMutationResult.
    [[nodiscard]] auto mark_exception_after_durable_start() noexcept -> bool;
    [[nodiscard]] auto decide(CompletionDecision decision) noexcept -> bool;
    [[nodiscard]] auto mark_completed() noexcept -> bool;

    // Table helper for tests: whether a direct stage jump is legal.
    [[nodiscard]] static auto transition_allowed(MutationStage from, MutationStage to) noexcept -> bool;

  private:
    MutationStage stage_{MutationStage::not_admitted};
    DurableDecision durable_{};
    PublicationDecision publication_{};
    CompletionDecision completion_{};
};

} // namespace glifistore::store::paired
