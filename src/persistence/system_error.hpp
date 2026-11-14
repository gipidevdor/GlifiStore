#pragma once

#include "glifistore/core/error.hpp"

#include <cerrno>
#include <string>
#include <string_view>
#include <system_error>

namespace glifistore {

[[nodiscard]] inline auto persistence_system_error(const std::string_view operation,
                                                   const int error_number = errno) -> Unexpected {
    auto code = ErrorCode::io_error;
    switch (error_number) {
    case ENOSPC:
#if defined(EDQUOT)
    case EDQUOT:
#endif
        code = ErrorCode::storage_exhausted;
        break;
    case EFBIG:
        code = ErrorCode::file_too_large;
        break;
    case EMFILE:
    case ENFILE:
        code = ErrorCode::descriptor_exhausted;
        break;
    case EROFS:
        code = ErrorCode::read_only_filesystem;
        break;
    default:
        break;
    }
    return fail(code, std::string{operation} + ": " +
                          std::error_code{error_number, std::system_category()}.message());
}

} // namespace glifistore
