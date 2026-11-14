cmake_minimum_required(VERSION 3.25)

foreach(required IN ITEMS GLIFISTORE_BINARY_DIR GLIFISTORE_TEST_ROOT)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

function(install_component component destination)
    file(REMOVE_RECURSE "${destination}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --install "${GLIFISTORE_BINARY_DIR}"
                --prefix "${destination}" --component "${component}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "Installing component ${component} failed (${result})\n${output}\n${error}")
    endif()
endfunction()

function(require_path path description)
    if(NOT EXISTS "${path}" AND NOT IS_SYMLINK "${path}")
        message(FATAL_ERROR "${description} is missing: ${path}")
    endif()
endfunction()

function(reject_path path description)
    if(EXISTS "${path}" OR IS_SYMLINK "${path}")
        message(FATAL_ERROR "${description} leaked into the component: ${path}")
    endif()
endfunction()

set(runtime_root "${GLIFISTORE_TEST_ROOT}/Runtime")
set(abi_root "${GLIFISTORE_TEST_ROOT}/AbiRuntime")
set(development_root "${GLIFISTORE_TEST_ROOT}/Development")

install_component(Runtime "${runtime_root}")
install_component(AbiRuntime "${abi_root}")
install_component(Development "${development_root}")

# Runtime is independently deployable: service binary, operator documentation,
# and legal/version identity. It must not carry SDK headers or pkg-config files.
require_path("${runtime_root}/bin/glifistored" "Runtime daemon")
require_path("${runtime_root}/share/GlifiStore/VERSION" "Runtime product version")
require_path(
    "${runtime_root}/share/GlifiStore/examples/glifistored.conf.sample"
    "Runtime configuration sample"
)
require_path("${runtime_root}/share/man/man8/glifistored.8" "Runtime daemon manual")
reject_path("${runtime_root}/include" "Development headers")
reject_path("${runtime_root}/lib/pkgconfig" "pkg-config metadata")

# AbiRuntime contains only the loadable, versioned ABI object. The exact suffix
# is platform-dependent, so inspect the component inventory without assuming ELF.
file(GLOB_RECURSE abi_inventory RELATIVE "${abi_root}" "${abi_root}/*")
if(abi_inventory STREQUAL "")
    message(FATAL_ERROR "AbiRuntime component is empty")
endif()
set(found_versioned_abi FALSE)
foreach(entry IN LISTS abi_inventory)
    if(entry MATCHES "(^|/)libglifistore\\.[0-9]" OR
       entry MATCHES "(^|/)libglifistore\\.so\\.[0-9]" OR
       entry MATCHES "(^|/)glifistore-[0-9]+\\.dll$")
        set(found_versioned_abi TRUE)
    endif()
endforeach()
if(NOT found_versioned_abi)
    message(FATAL_ERROR
        "AbiRuntime has no versioned C ABI library; inventory: ${abi_inventory}")
endif()
reject_path("${abi_root}/bin/glifistored" "Daemon")
reject_path("${abi_root}/include" "Development headers")
reject_path("${abi_root}/lib/pkgconfig" "pkg-config metadata")

# Development provides both discovery mechanisms and the ABI header. On Unix,
# the unversioned linker name belongs here through NAMELINK_COMPONENT.
require_path("${development_root}/include/glifistore/abi/glifistore.h" "C ABI header")
require_path("${development_root}/lib/pkgconfig/glifistore-abi.pc" "pkg-config metadata")
require_path(
    "${development_root}/lib/cmake/GlifiStore/GlifiStoreTargets.cmake"
    "CMake package targets"
)
reject_path("${development_root}/bin/glifistored" "Daemon")
reject_path("${development_root}/share/man/man8/glifistored.8" "Runtime manual")

if(APPLE)
    require_path("${development_root}/lib/libglifistore.dylib" "ABI linker name")
elseif(UNIX)
    require_path("${development_root}/lib/libglifistore.so" "ABI linker name")
endif()
