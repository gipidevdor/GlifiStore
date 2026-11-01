# GlifiStore libraries and primary applications (Phase C split).
add_library(glifistore_cli_support
    ${CMAKE_SOURCE_DIR}/src/cli/arguments.cpp
)
target_compile_features(glifistore_cli_support PUBLIC cxx_std_23)
target_include_directories(glifistore_cli_support
    PUBLIC "${CMAKE_SOURCE_DIR}/src"
    PRIVATE "${CMAKE_SOURCE_DIR}/include"
)
target_link_libraries(glifistore_cli_support PRIVATE glifistore_project_options)
glifistore_set_warnings(glifistore_cli_support)
glifistore_enable_hardening(glifistore_cli_support)
set_target_properties(glifistore_cli_support PROPERTIES FOLDER "Libraries/Internal")

set(GLIFISTORE_CORE_SOURCES
    ${CMAKE_SOURCE_DIR}/src/core/fault_injection.cpp
    ${CMAKE_SOURCE_DIR}/src/core/hot_path_phases.cpp
    ${CMAKE_SOURCE_DIR}/src/core/worker_routing.cpp
    ${CMAKE_SOURCE_DIR}/src/index/index.cpp
    ${CMAKE_SOURCE_DIR}/src/index/index_hash_seed.cpp
    ${CMAKE_SOURCE_DIR}/src/index/key_arena.cpp
    ${CMAKE_SOURCE_DIR}/src/index/swiss_table.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/bootstrap.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/compaction/layout.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/compaction/manifest_transition.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/compaction/planner.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/compaction/resource_estimator.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/compaction_builder.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/compaction_intent.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/filesystem.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/filesystem_directory.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/manifest.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/namespace_audit.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/durable_flush_coordinator.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/recovery/recovery.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/recovery/recovery_budget.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/recovery/recovery_catalog.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/recovery/recovery_index_builder.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/recovery/recovery_scanner.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/resource_limits.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/runtime_catalog.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/runtime_catalog_batch.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/runtime_catalog_detail.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/runtime_catalog_read_ops.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/runtime_catalog_rotate_ops.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/runtime_catalog_mutate_ops.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/runtime_catalog_maintenance.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/runtime_catalog_compact.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/segment_file.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/segment_file_io.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/store_verify.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/store_backup.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/store_migrate.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/store_repair.cpp
    ${CMAKE_SOURCE_DIR}/src/segment/crc32c.cpp
    ${CMAKE_SOURCE_DIR}/src/segment/global_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/segment/record.cpp
    ${CMAKE_SOURCE_DIR}/src/segment/segment.cpp
    ${CMAKE_SOURCE_DIR}/src/segment/segment_header.cpp
    ${CMAKE_SOURCE_DIR}/src/store/maintenance.cpp
    ${CMAKE_SOURCE_DIR}/src/store/maintenance_evaluate.cpp
    ${CMAKE_SOURCE_DIR}/src/store/paired/mutation_slot_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/store/paired/mutation_state.cpp
    ${CMAKE_SOURCE_DIR}/src/store/paired/completion_policy.cpp
    ${CMAKE_SOURCE_DIR}/src/store/paired/mutation_recovery.cpp
    ${CMAKE_SOURCE_DIR}/src/store/paired/mutation_execution.cpp
    ${CMAKE_SOURCE_DIR}/src/store/paired/fail_closed_state.cpp
    ${CMAKE_SOURCE_DIR}/src/store/paired/read_generation.cpp
    ${CMAKE_SOURCE_DIR}/src/store/paired/read_generation_publish.cpp
    ${CMAKE_SOURCE_DIR}/src/store/paired/read_generation_merge.cpp
    ${CMAKE_SOURCE_DIR}/src/store/paired/read_generation_direct.cpp
    ${CMAKE_SOURCE_DIR}/src/store/paired/generation_slot_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/store/paired/lane_publication.cpp
    ${CMAKE_SOURCE_DIR}/src/store/paired/volatile_sync_chunk.cpp
    ${CMAKE_SOURCE_DIR}/src/store/paired/shard_pair_runtime.cpp
    ${CMAKE_SOURCE_DIR}/src/store/paired/shard_pair_runtime_writer_loop.cpp
    ${CMAKE_SOURCE_DIR}/src/store/paired/shard_pair_runtime_writer_async.cpp
    ${CMAKE_SOURCE_DIR}/src/store/paired/shard_pair_runtime_writer_sync.cpp
    ${CMAKE_SOURCE_DIR}/src/store/paired/shard_pair_runtime_sync_lane.cpp
    ${CMAKE_SOURCE_DIR}/src/store/store.cpp
    ${CMAKE_SOURCE_DIR}/src/store/store_access.cpp
    ${CMAKE_SOURCE_DIR}/src/store/prepared_cold_read.cpp
    ${CMAKE_SOURCE_DIR}/src/vacuum/vacuum.cpp
    ${CMAKE_SOURCE_DIR}/src/worker/pool.cpp
    ${CMAKE_SOURCE_DIR}/src/worker/topology.cpp
    ${CMAKE_SOURCE_DIR}/src/worker/worker.cpp
)
add_library(glifistore_core ${GLIFISTORE_CORE_SOURCES})
add_library(GlifiStore::core ALIAS glifistore_core)
set_target_properties(glifistore_core PROPERTIES EXPORT_NAME core)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/src/persistence/segment_file.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/segment_file_io.cpp
    PROPERTIES COMPILE_DEFINITIONS _GNU_SOURCE)
endif()

function(glifistore_configure_core_target target)
    target_compile_features(${target} PUBLIC cxx_std_23)
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            "${CMAKE_SOURCE_DIR}/src"
    )
    target_link_libraries(${target}
        PUBLIC Threads::Threads
        PRIVATE $<BUILD_INTERFACE:glifistore_project_options>
    )
    if(CMAKE_SYSTEM_NAME MATCHES "FreeBSD|OpenBSD")
        # FindThreads may legitimately resolve pthread symbols from libc and
        # leave the flag empty, while BSD still requires explicit mode.
        target_compile_options(${target} PUBLIC -pthread)
        target_link_options(${target} PUBLIC -pthread)
    endif()
    if(GLIFISTORE_FAULT_INJECTION)
        target_compile_definitions(${target} PUBLIC GLIFISTORE_FAULT_INJECTION=1)
    endif()
    if(GLIFISTORE_HOT_PATH_PHASES)
        target_compile_definitions(${target} PUBLIC GLIFISTORE_HOT_PATH_PHASES=1)
    endif()
    glifistore_set_warnings(${target})
    glifistore_enable_hardening(${target})
    glifistore_enable_ipo(${target})
    set_target_properties(${target} PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
    )
endfunction()

glifistore_configure_core_target(glifistore_core)
if(GLIFISTORE_FAULT_INJECTION)
    message(STATUS "GlifiStore fault injection: ENABLED (debug-only adverse scheduling hooks)")
else()
    message(STATUS "GlifiStore fault injection: disabled (production default)")
endif()
if(GLIFISTORE_HOT_PATH_PHASES)
    message(STATUS "GlifiStore hot-path phases: ENABLED (lab attribution counters)")
else()
    message(STATUS "GlifiStore hot-path phases: disabled (default)")
endif()
set_target_properties(glifistore_core PROPERTIES FOLDER "Libraries")

# ELF shared libraries require PIC objects, but forcing the installed static
# core to PIC would also constrain glifistored's optimized direct link. Build
# a private ABI-only copy on ELF; Darwin's object model needs no such duplicate.
set(GLIFISTORE_ABI_CORE_TARGET glifistore_core)
if(UNIX AND NOT APPLE)
    add_library(glifistore_abi_core STATIC ${GLIFISTORE_CORE_SOURCES})
    glifistore_configure_core_target(glifistore_abi_core)
    set_target_properties(glifistore_abi_core PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        FOLDER "Libraries/Internal"
    )
    set(GLIFISTORE_ABI_CORE_TARGET glifistore_abi_core)
endif()

add_library(glifistore_abi SHARED
    ${CMAKE_SOURCE_DIR}/src/abi/glifistore.cpp
)
add_library(GlifiStore::abi ALIAS glifistore_abi)
set_target_properties(glifistore_abi PROPERTIES
    EXPORT_NAME abi
    OUTPUT_NAME glifistore
    VERSION "${GLIFISTORE_ABI_VERSION}"
    SOVERSION "${GLIFISTORE_ABI_VERSION_MAJOR}"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    FOLDER "Libraries"
)
if(CMAKE_SYSTEM_NAME STREQUAL "OpenBSD")
    # OpenBSD packages encode both interface major and minor in the library
    # filename/WANTLIB contract. ABI_VERSION remains the single authority.
    set_target_properties(glifistore_abi PROPERTIES
        VERSION "${GLIFISTORE_ABI_VERSION}"
        SOVERSION "${GLIFISTORE_ABI_VERSION}"
    )
endif()
target_compile_features(glifistore_abi PRIVATE cxx_std_23)
target_compile_definitions(glifistore_abi PRIVATE
    GLIFISTORE_ABI_BUILD=1
    "GLIFISTORE_PRODUCT_VERSION=\"${PROJECT_VERSION}\""
    "GLIFISTORE_PRODUCT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}"
    "GLIFISTORE_PRODUCT_VERSION_MINOR=${PROJECT_VERSION_MINOR}"
    "GLIFISTORE_PRODUCT_VERSION_PATCH=${PROJECT_VERSION_PATCH}"
)
target_include_directories(glifistore_abi
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(glifistore_abi
    PRIVATE ${GLIFISTORE_ABI_CORE_TARGET} $<BUILD_INTERFACE:glifistore_project_options>
)
glifistore_set_warnings(glifistore_abi)
glifistore_enable_hardening(glifistore_abi)
glifistore_enable_ipo(glifistore_abi)

# The text allowlist is the sole symbol authority. Generate the platform linker
# control file instead of maintaining a second, drift-prone list.
file(STRINGS "${CMAKE_SOURCE_DIR}/abi/symbols-v1.txt" GLIFISTORE_ABI_SYMBOLS)
if(APPLE)
    set(GLIFISTORE_ABI_EXPORT_FILE "${CMAKE_CURRENT_BINARY_DIR}/glifistore-abi.exports")
    file(WRITE "${GLIFISTORE_ABI_EXPORT_FILE}" "")
    foreach(symbol IN LISTS GLIFISTORE_ABI_SYMBOLS)
        file(APPEND "${GLIFISTORE_ABI_EXPORT_FILE}" "_${symbol}\n")
    endforeach()
    target_link_options(glifistore_abi PRIVATE
        "LINKER:-exported_symbols_list,${GLIFISTORE_ABI_EXPORT_FILE}")
elseif(UNIX)
    set(GLIFISTORE_ABI_EXPORT_FILE "${CMAKE_CURRENT_BINARY_DIR}/glifistore-abi.map")
    file(WRITE "${GLIFISTORE_ABI_EXPORT_FILE}" "{\n  global:\n")
    foreach(symbol IN LISTS GLIFISTORE_ABI_SYMBOLS)
        file(APPEND "${GLIFISTORE_ABI_EXPORT_FILE}" "    ${symbol};\n")
    endforeach()
    file(APPEND "${GLIFISTORE_ABI_EXPORT_FILE}" "  local: *;\n};\n")
    target_link_options(glifistore_abi PRIVATE
        "LINKER:--version-script=${GLIFISTORE_ABI_EXPORT_FILE}")
endif()

add_library(glifistore_wire
    ${CMAKE_SOURCE_DIR}/src/server/protocol.cpp
)
add_library(GlifiStore::wire ALIAS glifistore_wire)
set_target_properties(glifistore_wire PROPERTIES EXPORT_NAME wire)
target_compile_features(glifistore_wire PUBLIC cxx_std_23)
target_include_directories(glifistore_wire
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(glifistore_wire PRIVATE $<BUILD_INTERFACE:glifistore_project_options>)
glifistore_set_warnings(glifistore_wire)
glifistore_enable_hardening(glifistore_wire)
glifistore_enable_ipo(glifistore_wire)
set_target_properties(glifistore_wire PROPERTIES FOLDER "Libraries")

add_library(glifistore_tls_layer
    ${CMAKE_SOURCE_DIR}/src/server/tls.cpp
)
add_library(GlifiStore::tls_layer ALIAS glifistore_tls_layer)
set_target_properties(glifistore_tls_layer PROPERTIES EXPORT_NAME tls_layer)
target_compile_features(glifistore_tls_layer PUBLIC cxx_std_23)
target_include_directories(glifistore_tls_layer
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(glifistore_tls_layer
    PUBLIC GlifiStore::core
    PRIVATE $<BUILD_INTERFACE:glifistore_project_options>
)
if(GLIFISTORE_TLS_ENABLED)
    target_compile_definitions(glifistore_tls_layer
        PUBLIC GLIFISTORE_HAS_TLS=1
        PRIVATE "GLIFISTORE_TLS_BACKEND_NAME=\"${GLIFISTORE_TLS_BACKEND}\""
    )
    target_link_libraries(glifistore_tls_layer PUBLIC GlifiStore::tls)
else()
    target_compile_definitions(glifistore_tls_layer PUBLIC GLIFISTORE_HAS_TLS=0)
endif()
glifistore_set_warnings(glifistore_tls_layer)
glifistore_enable_hardening(glifistore_tls_layer)
glifistore_enable_ipo(glifistore_tls_layer)
set_target_properties(glifistore_tls_layer PROPERTIES FOLDER "Libraries")

add_library(glifistore_client
    ${CMAKE_SOURCE_DIR}/src/client/client.cpp
    ${CMAKE_SOURCE_DIR}/src/client/client_error.cpp
)
add_library(GlifiStore::client ALIAS glifistore_client)
set_target_properties(glifistore_client PROPERTIES EXPORT_NAME client)
target_compile_features(glifistore_client PUBLIC cxx_std_23)
target_include_directories(glifistore_client
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(glifistore_client
    PRIVATE GlifiStore::wire GlifiStore::tls_layer $<BUILD_INTERFACE:glifistore_project_options>
)
glifistore_set_warnings(glifistore_client)
glifistore_enable_hardening(glifistore_client)
glifistore_enable_ipo(glifistore_client)
set_target_properties(glifistore_client PROPERTIES FOLDER "Libraries")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(GLIFISTORE_POLLER_SOURCE ${CMAKE_SOURCE_DIR}/src/server/poller_epoll.cpp)
elseif(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD" OR CMAKE_SYSTEM_NAME STREQUAL "OpenBSD")
    set(GLIFISTORE_POLLER_SOURCE ${CMAKE_SOURCE_DIR}/src/server/poller_kqueue.cpp)
else()
    message(FATAL_ERROR "glifistored requires epoll on Linux or kqueue on macOS/FreeBSD/OpenBSD")
endif()

add_library(glifistore_server_core
    ${CMAKE_SOURCE_DIR}/src/server/abuse_limits.cpp
    ${CMAKE_SOURCE_DIR}/src/server/authz.cpp
    ${CMAKE_SOURCE_DIR}/src/server/connection_handoff.cpp
    ${CMAKE_SOURCE_DIR}/src/server/crash_test_hooks.cpp
    ${CMAKE_SOURCE_DIR}/src/server/daemon_config.cpp
    ${CMAKE_SOURCE_DIR}/src/server/daemon_config_materialize.cpp
    ${CMAKE_SOURCE_DIR}/src/server/daemon_config_dump.cpp
    ${CMAKE_SOURCE_DIR}/src/server/daemon_log.cpp
    ${CMAKE_SOURCE_DIR}/src/server/disk_read_executor.cpp
    ${CMAKE_SOURCE_DIR}/src/server/pair_writer.cpp
    ${CMAKE_SOURCE_DIR}/src/server/openbsd_sandbox.cpp
    ${CMAKE_SOURCE_DIR}/src/server/peercred.cpp
    ${CMAKE_SOURCE_DIR}/src/server/reactor.cpp
    ${CMAKE_SOURCE_DIR}/src/server/reactor_tls.cpp
    ${CMAKE_SOURCE_DIR}/src/server/reactor_timeouts.cpp
    ${CMAKE_SOURCE_DIR}/src/server/reactor_dispatch.cpp
    ${CMAKE_SOURCE_DIR}/src/server/reactor_execute.cpp
    ${CMAKE_SOURCE_DIR}/src/server/reactor_io.cpp
    ${CMAKE_SOURCE_DIR}/src/server/reactor_factory.cpp
    ${CMAKE_SOURCE_DIR}/src/server/security_audit.cpp
    ${CMAKE_SOURCE_DIR}/src/server/server.cpp
    ${CMAKE_SOURCE_DIR}/src/server/server_builder.cpp
    ${CMAKE_SOURCE_DIR}/src/server/server_health.cpp
    ${CMAKE_SOURCE_DIR}/src/server/server_lifecycle.cpp
    ${CMAKE_SOURCE_DIR}/src/server/server_stats.cpp
    ${CMAKE_SOURCE_DIR}/src/server/socket.cpp
    ${CMAKE_SOURCE_DIR}/src/server/thread_affinity.cpp
    ${CMAKE_SOURCE_DIR}/src/server/wakeup.cpp
    ${GLIFISTORE_POLLER_SOURCE}
)
add_library(GlifiStore::server ALIAS glifistore_server_core)
set_target_properties(glifistore_server_core PROPERTIES EXPORT_NAME server)
target_compile_features(glifistore_server_core PUBLIC cxx_std_23)
target_include_directories(glifistore_server_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
)
target_link_libraries(glifistore_server_core
    PUBLIC GlifiStore::core GlifiStore::wire GlifiStore::tls_layer
    PRIVATE glifistore_cli_support $<BUILD_INTERFACE:glifistore_project_options>
)
target_compile_definitions(glifistore_server_core PRIVATE GLIFISTORE_VERSION="${PROJECT_VERSION}")
glifistore_set_warnings(glifistore_server_core)
glifistore_enable_hardening(glifistore_server_core)
glifistore_enable_ipo(glifistore_server_core)
set_target_properties(glifistore_server_core PROPERTIES FOLDER "Libraries")

add_executable(glifistored ${CMAKE_SOURCE_DIR}/src/server/daemon_main.cpp)
target_link_libraries(glifistored
    PRIVATE GlifiStore::server glifistore_cli_support glifistore_project_options
)
target_compile_definitions(glifistored PRIVATE GLIFISTORE_VERSION="${PROJECT_VERSION}")
glifistore_set_warnings(glifistored)
glifistore_enable_hardening(glifistored)
glifistore_enable_ipo(glifistored)
set_target_properties(glifistored PROPERTIES FOLDER "Applications")
glifistore_xcode_scheme(glifistored ARGUMENTS
    --bind 127.0.0.1 --port 7379 --workers 2
)

add_executable(glifistore_demo ${CMAKE_SOURCE_DIR}/src/main.cpp)
target_link_libraries(glifistore_demo PRIVATE GlifiStore::core glifistore_project_options)
glifistore_set_warnings(glifistore_demo)
glifistore_enable_hardening(glifistore_demo)
glifistore_enable_ipo(glifistore_demo)
set_target_properties(glifistore_demo PROPERTIES FOLDER "Applications")
glifistore_xcode_scheme(glifistore_demo)
