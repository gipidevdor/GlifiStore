cmake_minimum_required(VERSION 3.25)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" GLIFISTORE_VERSION)
string(STRIP "${GLIFISTORE_VERSION}" GLIFISTORE_VERSION)
if(NOT GLIFISTORE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must contain exactly one semantic version such as 1.2.3")
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/ABI_VERSION" GLIFISTORE_ABI_VERSION)
string(STRIP "${GLIFISTORE_ABI_VERSION}" GLIFISTORE_ABI_VERSION)
if(NOT GLIFISTORE_ABI_VERSION MATCHES "^([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "ABI_VERSION must contain exactly a major.minor version such as 1.0")
endif()
set(GLIFISTORE_ABI_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(GLIFISTORE_ABI_VERSION_MINOR "${CMAKE_MATCH_2}")

project(GlifiStore VERSION "${GLIFISTORE_VERSION}" LANGUAGES C CXX)

find_package(Threads REQUIRED)

if(DEFINED CMAKE_CXX_STANDARD AND CMAKE_CXX_STANDARD LESS 23)
    message(FATAL_ERROR "GlifiStore requires ISO C++23 or newer")
endif()
if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 23)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)
include(ProjectOptions)
include(CompilerWarnings)
include(Sanitizers)
include(Hardening)
include(StandardLibraryHardening)
include(XcodeSupport)
include(FindGlifiStoreTls)

glifistore_project_options()
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(CMAKE_PREDEFINED_TARGETS_FOLDER "CMake")
# Keep executables at the build-tree root so scripts/CI that expect
# build/<preset>/glifistored (and friends) keep working after the Phase C split.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(GLIFISTORE_ENABLE_TLS STREQUAL "ON")
    if(NOT GLIFISTORE_TLS_FOUND)
        message(FATAL_ERROR
            "GLIFISTORE_ENABLE_TLS=ON but no TLS library was found. "
            "On OpenBSD install/use system LibreSSL; elsewhere provide OpenSSL 3.x "
            "(set OPENSSL_ROOT_DIR if needed).")
    endif()
    set(GLIFISTORE_TLS_ENABLED TRUE)
elseif(GLIFISTORE_ENABLE_TLS STREQUAL "OFF")
    set(GLIFISTORE_TLS_ENABLED FALSE)
elseif(GLIFISTORE_ENABLE_TLS STREQUAL "AUTO")
    set(GLIFISTORE_TLS_ENABLED ${GLIFISTORE_TLS_FOUND})
else()
    message(FATAL_ERROR "GLIFISTORE_ENABLE_TLS must be AUTO, ON, or OFF")
endif()

if(GLIFISTORE_TLS_ENABLED)
    message(STATUS "GlifiStore TLS: enabled (${GLIFISTORE_TLS_BACKEND})")
else()
    if(GLIFISTORE_ENABLE_TLS STREQUAL "AUTO")
        message(STATUS
            "GlifiStore TLS: disabled (no LibreSSL/OpenSSL 3.x found; "
            "cleartext daemon only. Set OPENSSL_ROOT_DIR or GLIFISTORE_ENABLE_TLS=ON)")
    else()
        message(STATUS "GlifiStore TLS: disabled (GLIFISTORE_ENABLE_TLS=OFF)")
    endif()
endif()


add_subdirectory(src)
add_subdirectory(tools)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(GLIFISTORE_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(GLIFISTORE_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()

glifistore_add_xcode_project_files()

add_subdirectory(man)

option(GLIFISTORE_BUILD_DOCS "Enable the Doxygen documentation target" OFF)
if(GLIFISTORE_BUILD_DOCS)
    add_subdirectory(docs/doxygen)
endif()

install(TARGETS glifistore_core glifistore_wire glifistore_tls_layer glifistore_client
    EXPORT GlifiStoreTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Runtime
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Runtime NAMELINK_COMPONENT Development
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Development
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(TARGETS glifistore_abi
    EXPORT GlifiStoreTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT AbiRuntime
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT AbiRuntime NAMELINK_COMPONENT Development
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Development
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(TARGETS glifistore_demo glifistored glifistore_inspect_segment glifistore_verify_store
        glifistore_backup_store glifistore_migrate_store glifistore_repair_store glifistore_rebuild_index
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT Runtime
)
install(FILES
    include/glifistore/abi/glifistore.h
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/glifistore/abi"
    COMPONENT Development
)
install(FILES
    include/glifistore/client/client.hpp
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/glifistore/client"
    COMPONENT Development
)

configure_file(cmake/glifistore-abi.pc.in "${CMAKE_CURRENT_BINARY_DIR}/glifistore-abi.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/glifistore-abi.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
    COMPONENT Development
)
install(FILES
    include/glifistore/core/error.hpp
    include/glifistore/core/types.hpp
    include/glifistore/core/worker_routing.hpp
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/glifistore/core"
    COMPONENT Development
)
install(FILES
    include/glifistore/persistence/compaction_intent.hpp
    include/glifistore/persistence/filesystem.hpp
    include/glifistore/persistence/filesystem_hooks.hpp
    include/glifistore/persistence/manifest.hpp
    include/glifistore/persistence/namespace_audit.hpp
    include/glifistore/persistence/resource_limits.hpp
    include/glifistore/persistence/store_backup.hpp
    include/glifistore/persistence/store_verify.hpp
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/glifistore/persistence"
    COMPONENT Development
)
install(FILES
    include/glifistore/segment/segment_header.hpp
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/glifistore/segment"
    COMPONENT Development
)
install(FILES
    include/glifistore/server/protocol.hpp
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/glifistore/server"
    COMPONENT Development
)
install(FILES
    include/glifistore/store/config.hpp
    include/glifistore/store/maintenance_types.hpp
    include/glifistore/store/store.hpp
    include/glifistore/store/value.hpp
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/glifistore/store"
    COMPONENT Development
)

install(
    EXPORT GlifiStoreTargets
    FILE GlifiStoreTargets.cmake
    NAMESPACE GlifiStore::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/GlifiStore"
    COMPONENT Development
)

set(GLIFISTORE_LTO_ENABLED FALSE)
set(GLIFISTORE_LTO_LINK_OPTIONS "")
if(GLIFISTORE_ENABLE_LTO AND GLIFISTORE_IPO_SUPPORTED)
    set(GLIFISTORE_LTO_ENABLED TRUE)
    set(GLIFISTORE_LTO_LINK_OPTIONS "${GLIFISTORE_IPO_LINK_OPTIONS}")
endif()

configure_package_config_file(
    cmake/GlifiStoreConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/GlifiStoreConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/GlifiStore"
)
if(PROJECT_VERSION_MAJOR EQUAL 0)
    set(GLIFISTORE_VERSION_COMPATIBILITY SameMinorVersion)
else()
    set(GLIFISTORE_VERSION_COMPATIBILITY SameMajorVersion)
endif()
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/GlifiStoreConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY "${GLIFISTORE_VERSION_COMPATIBILITY}"
)
install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/GlifiStoreConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/GlifiStoreConfigVersion.cmake"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindGlifiStoreTls.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/GlifiStore"
    COMPONENT Development
)
install(
    FILES LICENSE NOTICE VERSION ABI_VERSION THIRD_PARTY_NOTICES.md
    DESTINATION "${CMAKE_INSTALL_DATADIR}/GlifiStore"
    COMPONENT Runtime
)
set(GLIFISTORE_DEFAULT_DATA_DIR "${CMAKE_INSTALL_FULL_LOCALSTATEDIR}/lib/glifistore")
configure_file(
    config/glifistored.conf.sample.in
    "${CMAKE_CURRENT_BINARY_DIR}/glifistored.conf.sample"
    @ONLY
)
install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/glifistored.conf.sample"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/GlifiStore/examples"
    COMPONENT Runtime
)
