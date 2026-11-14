#include "glifistore/store/paired/completion_policy.hpp"

#include <string>

namespace glifistore::store::paired {

auto status_from_completion(const CompletionDecision& decision,
                            const std::string_view known_not_committed_message,
                            const std::string_view indeterminate_message) -> Status {
    switch (decision.kind) {
    case CompletionDecision::Kind::success:
        return Status{};
    case CompletionDecision::Kind::known_not_committed:
        return Status{fail(ErrorCode::resource_exhausted, std::string{known_not_committed_message})};
    case CompletionDecision::Kind::indeterminate:
        return Status{fail(ErrorCode::unavailable, std::string{indeterminate_message})};
    case CompletionDecision::Kind::undecided:
        return Status{fail(ErrorCode::internal_error, "completion undecided")};
    }
    return Status{fail(ErrorCode::internal_error, "completion undecided")};
}

} // namespace glifistore::store::paired
