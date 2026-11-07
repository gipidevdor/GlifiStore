/**
 * @file
 * @brief Mutation completion classification from commit and publication knowledge.
 */
#pragma once

// Completion polarity helpers for the paired Writer (behavior-neutral extraction).
// Normative: docs/spec/mutation-lifecycle.md §4, docs/spec/error-taxonomy-v1.md
//
// Pure decision → Status/ErrorCode mapping. Does not perform Store I/O.

#include "glifistore/store/paired/mutation_state.hpp"

#include <string_view>

namespace glifistore::store::paired {

/// Wire-facing code for a decided non-success completion (Reactor maps these further).
/// Success completions must not call this — they produce an empty Status.
[[nodiscard]] constexpr auto wire_error_code_for(const CompletionDecision::Kind kind) noexcept -> ErrorCode {
    switch (kind) {
    case CompletionDecision::Kind::known_not_committed:
        return ErrorCode::resource_exhausted;
    case CompletionDecision::Kind::indeterminate:
        return ErrorCode::unavailable;
    case CompletionDecision::Kind::success:
    case CompletionDecision::Kind::undecided:
        return ErrorCode::internal_error;
    }
    return ErrorCode::internal_error;
}

/// Build a Status from a completion decision. Success → empty Status.
[[nodiscard]] auto status_from_completion(const CompletionDecision& decision,
                                          std::string_view known_not_committed_message,
                                          std::string_view indeterminate_message) -> Status;

} // namespace glifistore::store::paired
