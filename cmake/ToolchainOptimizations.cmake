include_guard(GLOBAL)

include(CheckCXXCompilerFlag)
include(CheckIPOSupported)

function(glifistore_apply_toolchain_optimizations target)
    if(GLIFISTORE_ENABLE_ASAN OR GLIFISTORE_ENABLE_TSAN OR GLIFISTORE_ENABLE_UBSAN)
        if(GLIFISTORE_ENABLE_LTO OR NOT GLIFISTORE_PGO STREQUAL "OFF")
            message(WARNING "LTO/PGO are disabled because sanitizers are enabled")
        endif()
        return()
    endif()

    if(GLIFISTORE_NATIVE_CPU)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
            target_compile_options(${target} INTERFACE -mcpu=native)
        endif()
    endif()

    if(GLIFISTORE_PGO STREQUAL "OFF")
        return()
    endif()

    if(NOT (CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU"))
        message(FATAL_ERROR "PGO requires Clang or GCC")
    endif()

    if(GLIFISTORE_PGO STREQUAL "GENERATE")
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
            target_compile_options(${target} INTERFACE "-fprofile-instr-generate=${GLIFISTORE_PGO_PROFILE_DIR}")
            target_link_options(${target} INTERFACE "-fprofile-instr-generate=${GLIFISTORE_PGO_PROFILE_DIR}")
        else()
            target_compile_options(${target} INTERFACE "-fprofile-generate=${GLIFISTORE_PGO_PROFILE_DIR}")
            target_link_options(${target} INTERFACE "-fprofile-generate=${GLIFISTORE_PGO_PROFILE_DIR}")
        endif()
        return()
    endif()

    if(GLIFISTORE_PGO STREQUAL "USE")
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
            if(NOT EXISTS "${GLIFISTORE_PGO_PROFILE_FILE}")
                message(FATAL_ERROR
                    "PGO profile data not found at ${GLIFISTORE_PGO_PROFILE_FILE}. "
                    "Build with macos-pgo-generate, run ./scripts/pgo-train.sh, then configure macos-pgo-use.")
            endif()
            target_compile_options(${target} INTERFACE "-fprofile-instr-use=${GLIFISTORE_PGO_PROFILE_FILE}")
            check_cxx_compiler_flag("-Wno-error=profile-instr-unprofiled"
                GLIFISTORE_HAS_PROFILE_INSTR_UNPROFILED_WARNING)
            if(GLIFISTORE_HAS_PROFILE_INSTR_UNPROFILED_WARNING)
                # A training workload need not link or execute every translation unit in a
                # static library. Retain the diagnostic without allowing the global -Werror
                # policy to reject otherwise valid, partially exercised PGO profiles.
                target_compile_options(${target} INTERFACE "-Wno-error=profile-instr-unprofiled")
            endif()
            target_link_options(${target} INTERFACE "-fprofile-instr-use=${GLIFISTORE_PGO_PROFILE_FILE}")
        else()
            if(NOT IS_DIRECTORY "${GLIFISTORE_PGO_PROFILE_DIR}")
                message(FATAL_ERROR
                    "PGO profile directory not found at ${GLIFISTORE_PGO_PROFILE_DIR}. "
                    "Build with unix-pgo-generate, run training, then configure unix-pgo-use.")
            endif()
            target_compile_options(${target} INTERFACE "-fprofile-use=${GLIFISTORE_PGO_PROFILE_DIR}")
            target_link_options(${target} INTERFACE "-fprofile-use=${GLIFISTORE_PGO_PROFILE_DIR}")
        endif()
        return()
    endif()

    message(FATAL_ERROR "GLIFISTORE_PGO must be OFF, GENERATE, or USE")
endfunction()

function(glifistore_enable_ipo target)
    if(NOT GLIFISTORE_ENABLE_LTO)
        return()
    endif()
    if(GLIFISTORE_ENABLE_ASAN OR GLIFISTORE_ENABLE_TSAN OR GLIFISTORE_ENABLE_UBSAN)
        return()
    endif()

    if(NOT DEFINED GLIFISTORE_IPO_CHECKED)
        set(_glifistore_ipo_supported FALSE)
        check_ipo_supported(RESULT _glifistore_ipo_supported OUTPUT _glifistore_ipo_error)
        set(GLIFISTORE_IPO_SUPPORTED ${_glifistore_ipo_supported} CACHE INTERNAL "IPO availability")
        set(GLIFISTORE_IPO_CHECKED TRUE CACHE INTERNAL "IPO check completed")
        if(NOT _glifistore_ipo_supported)
            message(WARNING "LTO requested but unsupported: ${_glifistore_ipo_error}")
        endif()
    endif()

    if(GLIFISTORE_IPO_SUPPORTED)
        if(NOT CMAKE_CXX_COMPILE_OPTIONS_IPO)
            message(FATAL_ERROR "IPO is supported but CMake did not expose its link options")
        endif()
        set(GLIFISTORE_IPO_LINK_OPTIONS "${CMAKE_CXX_COMPILE_OPTIONS_IPO}"
            CACHE INTERNAL "Link options required by installed LTO archives" FORCE)
        set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endfunction()
