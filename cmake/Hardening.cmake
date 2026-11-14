include_guard(GLOBAL)

include(CheckCXXCompilerFlag)
include(CheckCXXSourceCompiles)
include(CheckLinkerFlag)

function(glifistore_enable_hardening target)
    if(NOT GLIFISTORE_ENABLE_HARDENING)
        return()
    endif()

    foreach(flag IN ITEMS -fstack-protector-strong)
        string(MAKE_C_IDENTIFIER "${flag}" flag_id)
        check_cxx_compiler_flag("${flag}" "HAS_${flag_id}")
        if(HAS_${flag_id})
            target_compile_options(${target} PRIVATE "${flag}")
        endif()
    endforeach()

    get_target_property(target_type ${target} TYPE)
    if(target_type STREQUAL "EXECUTABLE")
        check_cxx_compiler_flag(-fPIE HAS_fPIE)
        if(HAS_fPIE)
            target_compile_options(${target} PRIVATE -fPIE)
        endif()

        check_linker_flag(CXX "LINKER:-pie" HAS_LINKER_pie)
        if(HAS_LINKER_pie)
            target_link_options(${target} PRIVATE "LINKER:-pie")
        endif()
    endif()

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
        set(saved_required_definitions "${CMAKE_REQUIRED_DEFINITIONS}")
        string(APPEND CMAKE_REQUIRED_FLAGS " -O2")
        set(CMAKE_REQUIRED_DEFINITIONS -D_FORTIFY_SOURCE=3)
        check_cxx_source_compiles(
            "#include <features.h>
             #if !defined(__GLIBC__) || !defined(__USE_FORTIFY_LEVEL) || __USE_FORTIFY_LEVEL < 3
             #error _FORTIFY_SOURCE=3 is unavailable
             #endif
             int main() { return 0; }"
            HAS_FORTIFY_SOURCE_3
        )
        set(CMAKE_REQUIRED_FLAGS "${saved_required_flags}")
        set(CMAKE_REQUIRED_DEFINITIONS "${saved_required_definitions}")
        if(HAS_FORTIFY_SOURCE_3)
            target_compile_definitions(${target} PRIVATE
                $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:_FORTIFY_SOURCE=3>
            )
        endif()
        if(target_type STREQUAL "EXECUTABLE")
            check_linker_flag(CXX "LINKER:-z,relro" HAS_LINKER_z_relro)
            check_linker_flag(CXX "LINKER:-z,now" HAS_LINKER_z_now)
            if(HAS_LINKER_z_relro)
                # Prefer the portable LINKER: form and also emit -Wl explicitly so
                # hosted CMake/Ninja response-file links remain inspectable.
                target_link_options(${target} PRIVATE "LINKER:-z,relro" "-Wl,-z,relro")
            endif()
            if(HAS_LINKER_z_now)
                target_link_options(${target} PRIVATE "LINKER:-z,now" "-Wl,-z,now")
            endif()
        endif()
    endif()
endfunction()
