include_guard(GLOBAL)

include(CheckCXXSourceCompiles)

function(glifistore_enable_standard_library_hardening target)
    if(NOT GLIFISTORE_ENABLE_STDLIB_HARDENING)
        return()
    endif()

    set(saved_required_definitions "${CMAKE_REQUIRED_DEFINITIONS}")

    set(CMAKE_REQUIRED_DEFINITIONS
        -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG)
    check_cxx_source_compiles(
        "#include <version>
         #if !defined(_LIBCPP_VERSION)
         #error not libc++
         #endif
         #if _LIBCPP_HARDENING_MODE != _LIBCPP_HARDENING_MODE_DEBUG
         #error libc++ debug hardening was not selected
         #endif
         int main() { return 0; }"
        GLIFISTORE_HAS_LIBCPP_DEBUG_HARDENING
    )

    set(CMAKE_REQUIRED_DEFINITIONS -D_GLIBCXX_ASSERTIONS)
    check_cxx_source_compiles(
        "#include <version>
         #if !defined(__GLIBCXX__) || !defined(_GLIBCXX_ASSERTIONS)
         #error libstdc++ assertions unavailable
         #endif
         int main() { return 0; }"
        GLIFISTORE_HAS_GLIBCXX_ASSERTIONS
    )

    set(CMAKE_REQUIRED_DEFINITIONS "${saved_required_definitions}")

    if(GLIFISTORE_HAS_LIBCPP_DEBUG_HARDENING)
        target_compile_definitions(
            ${target} INTERFACE _LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG)
        message(STATUS "GlifiStore diagnostic standard library: libc++ debug hardening")
    elseif(GLIFISTORE_HAS_GLIBCXX_ASSERTIONS)
        target_compile_definitions(${target} INTERFACE _GLIBCXX_ASSERTIONS)
        message(STATUS "GlifiStore diagnostic standard library: libstdc++ assertions")
    else()
        message(FATAL_ERROR
            "GLIFISTORE_ENABLE_STDLIB_HARDENING=ON, but the selected standard library "
            "supports neither libc++ debug hardening nor libstdc++ assertions")
    endif()
endfunction()
