#include "glifistore/client/client.hpp"
#include "glifistore/server/protocol.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace glifistore::client {

auto portable_retryability(const std::string_view category, const bool mutation_sent,
                           const bool indeterminate) -> std::string {
    if (indeterminate) {
        return "reconcile_first";
    }
    if (category == "invalid_argument" && !mutation_sent) {
        return "never";
    }
    if (category == "transport" && !mutation_sent) {
        return "same_request";
    }
    if (category == "overloaded") {
        // Wire OVERLOADED collapses admission pressure and durable capacity exhaustion
        // (including maintenance emergency). Do not advertise success-seeking retry.
        return "never";
    }
    if (category == "permission_denied") {
        return "never";
    }
    if (category == "not_found") {
        return "new_attempt";
    }
    if (category == "unavailable") {
        return "never";
    }
    return mutation_sent ? "reconcile_first" : "new_attempt";
}

auto portable_mutation_outcome(const std::uint16_t wire_status) -> std::string {
    switch (static_cast<server::ResponseStatus>(wire_status)) {
    case server::ResponseStatus::ok:
        return "committed";
    case server::ResponseStatus::internal_error:
        return "indeterminate";
    case server::ResponseStatus::invalid_request:
    case server::ResponseStatus::unsupported:
    case server::ResponseStatus::not_found:
    case server::ResponseStatus::overloaded:
    case server::ResponseStatus::wrong_owner:
    case server::ResponseStatus::not_bound:
    case server::ResponseStatus::permission_denied:
        return "rejected";
    }
    // Unknown numeric statuses: prefer indeterminate for mutations (bytes may have been applied).
    return "indeterminate";
}

auto error_from_wire_status(const std::uint16_t wire_status) -> Error {
    Error error;
    switch (static_cast<server::ResponseStatus>(wire_status)) {
    case server::ResponseStatus::invalid_request:
        error = {ErrorCode::invalid_argument, "server rejected the request as invalid"};
        break;
    case server::ResponseStatus::unsupported:
        error = {ErrorCode::invalid_argument, "server does not support the request"};
        break;
    case server::ResponseStatus::internal_error:
        error = {ErrorCode::internal_error, "server reported an internal error"};
        break;
    case server::ResponseStatus::not_found:
        error = {ErrorCode::not_found, "key was not found"};
        break;
    case server::ResponseStatus::overloaded:
        error = {ErrorCode::resource_exhausted, "server is overloaded"};
        break;
    case server::ResponseStatus::wrong_owner:
        error = {ErrorCode::corrupted_data, "server rejected the client's worker routing"};
        break;
    case server::ResponseStatus::not_bound:
        error = {ErrorCode::unavailable, "server connection is not bound to a worker"};
        break;
    case server::ResponseStatus::permission_denied:
        error = {ErrorCode::invalid_argument, "server denied the request"};
        error.category = "permission_denied";
        error.retryability = "never";
        error.wire_status = wire_status;
        return error;
    case server::ResponseStatus::ok:
        return {ErrorCode::internal_error, "unexpected successful response mapping"};
    }
    // Unknown numeric statuses: codecs reject after the frame is buffered; defensive helpers map to
    // protocol (not INTERNAL_ERROR) and preserve the original wire_status.
    if (wire_status > static_cast<std::uint16_t>(server::ResponseStatus::permission_denied)) {
        error = {ErrorCode::corrupted_data, "server returned an unknown status"};
    }
    if (error.category.empty()) {
        switch (error.code) {
        case ErrorCode::invalid_argument:
        case ErrorCode::record_too_large:
            error.category = "invalid_argument";
            break;
        case ErrorCode::not_found:
            error.category = "not_found";
            break;
        case ErrorCode::resource_exhausted:
            error.category = "overloaded";
            break;
        case ErrorCode::unavailable:
            error.category = "unavailable";
            break;
        case ErrorCode::io_error:
            error.category = "transport";
            break;
        case ErrorCode::corrupted_data:
            error.category = "protocol";
            break;
        default:
            error.category = "internal";
            break;
        }
    }
    error.wire_status = wire_status;
    error.retryability = portable_retryability(error.category, false, false);
    return error;
}

} // namespace glifistore::client
