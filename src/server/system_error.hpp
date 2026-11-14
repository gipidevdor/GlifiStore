#pragma once

#include "glifistore/core/error.hpp"

#include <cerrno>
#include <string>
#include <string_view>
#include <system_error>

namespace glifistore::server {

[[nodiscard]] inline auto system_error(const std::string_view operation, const int error_number = errno)
    -> Unexpected {
    return fail(ErrorCode::io_error, std::string{operation} + ": " +
                                         std::error_code{error_number, std::system_category()}.message());
}

} // namespace glifistore::server
