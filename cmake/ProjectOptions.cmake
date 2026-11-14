include_guard(GLOBAL)

option(GLIFISTORE_BUILD_BENCHMARKS "Build microbenchmarks" ON)
option(GLIFISTORE_BUILD_LAB "Build ADR 0036 lab targets" OFF)
option(GLIFISTORE_BUILD_FUZZERS "Build libFuzzer targets" OFF)
option(GLIFISTORE_FAULT_INJECTION
       "Enable debug-only adverse scheduling hooks (GLIFISTORE_FAULT_INJECTION); OFF for production"
       OFF)
option(GLIFISTORE_HOT_PATH_PHASES
       "Enable lab-only hot-path phase counters (GLIFISTORE_HOT_PATH_PHASES); OFF by default"
       OFF)
option(GLIFISTORE_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(GLIFISTORE_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(GLIFISTORE_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(GLIFISTORE_ENABLE_POINTER_PAIR_SANITIZER
       "Enable diagnostic pointer compare/subtract instrumentation with ASan" OFF)
option(GLIFISTORE_ENABLE_HARDENING "Enable supported hardening flags" ON)
option(GLIFISTORE_ENABLE_STDLIB_HARDENING
       "Enable diagnostic-only standard-library runtime assertions" OFF)
option(GLIFISTORE_WARNINGS_AS_ERRORS "Treat supported compiler warnings as errors" OFF)
option(GLIFISTORE_ENABLE_CLANG_TIDY "Run clang-tidy while compiling" OFF)
option(GLIFISTORE_ENABLE_LTO "Enable link-time optimization on release-style builds" OFF)
option(GLIFISTORE_NATIVE_CPU "Tune code generation for the host CPU (-mcpu=native)" OFF)
# AUTO: ON when LibreSSL/OpenSSL is found, OFF otherwise. Explicit ON fails configure if missing.
set(GLIFISTORE_ENABLE_TLS "AUTO" CACHE STRING "Enable daemon TLS (ON, OFF, or AUTO)")
set_property(CACHE GLIFISTORE_ENABLE_TLS PROPERTY STRINGS AUTO ON OFF)
set(GLIFISTORE_PGO "OFF" CACHE STRING "PGO mode: OFF, GENERATE, or USE")
set_property(CACHE GLIFISTORE_PGO PROPERTY STRINGS OFF GENERATE USE)
set(GLIFISTORE_PGO_PROFILE_DIR "${CMAKE_SOURCE_DIR}/build/pgo-profiles" CACHE PATH "Directory for raw PGO profile data")
set(GLIFISTORE_PGO_PROFILE_FILE "${CMAKE_SOURCE_DIR}/build/pgo-profiles/merged.profdata" CACHE FILEPATH "Merged Clang PGO profile")

function(glifistore_project_options)
    add_library(glifistore_project_options INTERFACE)
    target_compile_features(glifistore_project_options INTERFACE cxx_std_23)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON PARENT_SCOPE)
    glifistore_enable_sanitizers(glifistore_project_options)
    glifistore_enable_standard_library_hardening(glifistore_project_options)
    include(ToolchainOptimizations)
    glifistore_apply_toolchain_optimizations(glifistore_project_options)
    if(GLIFISTORE_ENABLE_CLANG_TIDY)
        find_program(GLIFISTORE_CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
        set(CMAKE_CXX_CLANG_TIDY
            "${GLIFISTORE_CLANG_TIDY_EXECUTABLE};--config-file=${PROJECT_SOURCE_DIR}/.clang-tidy"
            PARENT_SCOPE
        )
    endif()
endfunction()
