include_guard(GLOBAL)

include(CheckCXXCompilerFlag)
include(CMakePushCheckState)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_FLAGS "-Werror=unknown-warning-option")
    check_cxx_compiler_flag(
        "-Wmissing-designated-field-initializers"
        GLIFISTORE_HAS_MISSING_DESIGNATED_FIELD_INITIALIZERS
    )
    cmake_pop_check_state()
endif()

function(glifistore_set_warnings target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
            -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
            -Wnull-dereference -Wdouble-promotion -Wformat=2
            -Wimplicit-fallthrough -Wmissing-field-initializers
        )
        # Apple Clang does not include partial designated aggregates in
        # -Wmissing-field-initializers, unlike GCC and upstream Clang. Keep the
        # developer build aligned with the Linux and BSD CI gates.
        if(GLIFISTORE_HAS_MISSING_DESIGNATED_FIELD_INITIALIZERS)
            target_compile_options(${target} PRIVATE -Wmissing-designated-field-initializers)
        endif()
        if(GLIFISTORE_WARNINGS_AS_ERRORS OR DEFINED ENV{CI})
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
