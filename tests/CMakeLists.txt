# Test suites, crash harnesses, CLI contract tests, and check target.
if(BUILD_TESTING)
    add_executable(glifistore_c_abi_tests
        ${CMAKE_SOURCE_DIR}/tests/abi/c_abi_tests.c
    )
    set_target_properties(glifistore_c_abi_tests PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
        LINKER_LANGUAGE CXX
        FOLDER "Quality"
    )
    # The ABI consumer remains compiled as strict C11. Project options are also
    # linked, and the C++ driver performs the final link, so sanitizer builds
    # pull every runtime required by the instrumented C++ shared library into
    # this C-owned executable (including UBSan's C++ vptr handlers on Linux).
    target_link_libraries(glifistore_c_abi_tests
        PRIVATE GlifiStore::abi glifistore_project_options
    )
    add_test(NAME glifistore_c_abi_tests COMMAND glifistore_c_abi_tests)
    set_tests_properties(glifistore_c_abi_tests PROPERTIES LABELS "abi")

    add_executable(glifistore_c_abi_durable_tests
        ${CMAKE_SOURCE_DIR}/tests/abi/c_abi_durable_tests.cpp
    )
    target_compile_features(glifistore_c_abi_durable_tests PRIVATE cxx_std_23)
    target_link_libraries(glifistore_c_abi_durable_tests
        PRIVATE GlifiStore::abi Threads::Threads glifistore_project_options
    )
    if(CMAKE_SYSTEM_NAME MATCHES "FreeBSD|OpenBSD")
        # FindThreads can resolve an empty interface on BSD even though this
        # executable directly creates std::threads and therefore needs pthread mode.
        target_compile_options(glifistore_c_abi_durable_tests PRIVATE -pthread)
        target_link_options(glifistore_c_abi_durable_tests PRIVATE -pthread)
    endif()
    glifistore_set_warnings(glifistore_c_abi_durable_tests)
    glifistore_enable_hardening(glifistore_c_abi_durable_tests)
    glifistore_enable_ipo(glifistore_c_abi_durable_tests)
    set_target_properties(glifistore_c_abi_durable_tests PROPERTIES FOLDER "Quality")
    add_test(NAME glifistore_c_abi_durable_tests COMMAND glifistore_c_abi_durable_tests)
    set_tests_properties(glifistore_c_abi_durable_tests PROPERTIES LABELS "abi;durability")

    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(
        NAME glifistore_abi_symbols
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/engineering/tools/check_abi_symbols.py"
                --library $<TARGET_FILE:glifistore_abi>
                --allowlist "${CMAKE_SOURCE_DIR}/abi/symbols-v1.txt"
    )
    set_tests_properties(glifistore_abi_symbols PROPERTIES LABELS "abi")
    add_test(
        NAME glifistore_abi_symbol_gate_rejects_unexpected
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/engineering/tools/check_abi_symbols.py"
                --actual-list "${CMAKE_SOURCE_DIR}/tests/abi/unexpected-symbols.txt"
                --allowlist "${CMAKE_SOURCE_DIR}/abi/symbols-v1.txt"
    )
    set_tests_properties(glifistore_abi_symbol_gate_rejects_unexpected PROPERTIES
        LABELS "abi;negative"
        WILL_FAIL TRUE
    )
    add_test(
        NAME glifistore_release_artifact_tests
        COMMAND "${Python3_EXECUTABLE}" -m unittest -v scripts.tests.test_release_artifacts
    )
    set_tests_properties(glifistore_release_artifact_tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        LABELS "release;negative"
        TIMEOUT 30
    )
    add_test(
        NAME glifistore_bsd_packaging_tests
        COMMAND "${Python3_EXECUTABLE}" -m unittest -v scripts.tests.test_bsd_packaging
    )
    set_tests_properties(glifistore_bsd_packaging_tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        LABELS "release;packaging;negative"
        TIMEOUT 15
    )
    add_test(
        NAME glifistore_artifact_release_workflow_tests
        COMMAND "${Python3_EXECUTABLE}" -m unittest -v scripts.tests.test_artifact_release_workflow
    )
    set_tests_properties(glifistore_artifact_release_workflow_tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        LABELS "release;negative"
        TIMEOUT 15
    )
    add_test(
        NAME glifistore_persistence_fixture_tests
        COMMAND "${Python3_EXECUTABLE}" -m unittest -v scripts.tests.test_persistence_fixture
    )
    set_tests_properties(glifistore_persistence_fixture_tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        LABELS "release;persistence;negative"
        # This test creates and validates several real Git repositories. Under
        # the parallel CI matrix it can legitimately exceed 15 seconds even
        # after every unittest has completed successfully.
        TIMEOUT 30
    )
    add_test(
        NAME glifistore_install_components
        COMMAND "${CMAKE_COMMAND}"
                -DGLIFISTORE_BINARY_DIR=${CMAKE_BINARY_DIR}
                -DGLIFISTORE_TEST_ROOT=${CMAKE_BINARY_DIR}/install-component-test
                -P "${CMAKE_SOURCE_DIR}/tests/cmake/install_components.cmake"
    )
    set_tests_properties(glifistore_install_components PROPERTIES
        LABELS "install;packaging"
        TIMEOUT 30
    )
    if(UNIX)
        find_program(GLIFISTORE_BASH_EXECUTABLE bash REQUIRED)
        glifistore_resolve_sanitizers(glifistore_test_sanitizers)
        list(JOIN glifistore_test_sanitizers "," glifistore_test_sanitizer_flags)
        add_test(
            NAME glifistore_installed_cpp_interop
            COMMAND "${CMAKE_COMMAND}"
                    -DGLIFISTORE_BINARY_DIR=${CMAKE_BINARY_DIR}
                    -DGLIFISTORE_SOURCE_DIR=${CMAKE_SOURCE_DIR}
                    -DGLIFISTORE_TEST_ROOT=${CMAKE_BINARY_DIR}/installed-cpp-interop-test
                    -DGLIFISTORE_BASH=${GLIFISTORE_BASH_EXECUTABLE}
                    -DGLIFISTORE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                    -DGLIFISTORE_SANITIZERS=${glifistore_test_sanitizer_flags}
                    -P "${CMAKE_SOURCE_DIR}/tests/cmake/installed_cpp_interop.cmake"
        )
        set_tests_properties(glifistore_installed_cpp_interop PROPERTIES
            LABELS "install;packaging;sdk"
            TIMEOUT 60
        )
    endif()

    add_executable(glifistore_tests
        ${CMAKE_SOURCE_DIR}/src/experimental/paired_reactor.cpp
        ${CMAKE_SOURCE_DIR}/src/experimental/paired_shard.cpp
        ${CMAKE_SOURCE_DIR}/src/experimental/pair_read_generation_shell_access.cpp
        ${CMAKE_SOURCE_DIR}/tests/test_main.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/compaction_intent_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/compaction_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/byte_rate_limiter_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/adaptive_batch_sizer_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/index_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/index_hash_seed_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/worker_routing_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/hot_record_table_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/paired_shard_prototype_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/generation_slot_pool_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/generation_slot_pool_production_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/pair_read_generation_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/manifest_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/mutation_slot_pool_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/mutation_state_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/completion_policy_recovery_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/mutation_extraction_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/connection_lifecycle_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/mutation_lifecycle_property_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/shard_combining_executor_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/mutation_window_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/maintenance_controller_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/maintenance_controller_pressure_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/maintenance_controller_emergency_gate_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/namespace_audit_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/persistence_filesystem_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/segment_file_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/store_verify_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/store_backup_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/store_migrate_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/store_repair_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/cli_arguments_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/daemon_config_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/daemon_log_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/security_audit_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/openbsd_sandbox_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/authz_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/peercred_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/abuse_limits_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/bounded_mpsc_queue_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/connection_handoff_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/client_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/client_error_taxonomy_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/client_tls_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/benchmark_parse_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/durable_flush_coordinator_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/latency_histogram_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/result_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/record_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/resource_limits_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/server_protocol_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/segment_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/segment_header_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/segment_catalog_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/tls_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/integration/server_security_bounds_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/trusted_read_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/vacuum_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/worker_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/format_compatibility_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/released_artifact_compat_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/integration/compaction_builder_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/integration/compaction_recovery_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/integration/concurrency_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/integration/runtime_equivalence_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/integration/optimization_regression_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/integration/paired_store_core_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/integration/paired_store_litmus_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/integration/paired_store_erase_batch_durable_litmus_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/integration/paired_store_erase_batch_volatile_litmus_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/integration/persistence_recovery_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/integration/persistence_recovery_compaction_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/integration/persistence_recovery_compaction_enumeration_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/integration/persistence_recovery_compaction_fileio_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/integration/persistence_recovery_compaction_writer_hot_path_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/integration/persistence_recovery_crash_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/integration/persistence_recovery_crash_mutex_hold_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/integration/persistence_recovery_test_support.cpp
        ${CMAKE_SOURCE_DIR}/tests/integration/paired_reactor_prototype_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/integration/rebuild_vacuum_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/integration/server_reactor_paired_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/integration/server_reactor_server_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/integration/server_reactor_mutation_window_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/integration/server_reactor_durable_lifecycle_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/integration/server_reactor_durable_wire_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/integration/server_reactor_security_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/integration/server_reactor_limits_timeout_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/integration/store_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/property/history_property_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/property/store_state_machine_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/concurrency/linearizability_tests.cpp
    )
    target_link_libraries(glifistore_tests
        PRIVATE GlifiStore::server GlifiStore::client glifistore_cli_support glifistore_project_options
    )
    if(GLIFISTORE_TLS_ENABLED)
        target_link_libraries(glifistore_tests PRIVATE GlifiStore::tls)
    endif()
    target_include_directories(glifistore_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/benchmarks"
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/tests"
    )
    target_compile_definitions(glifistore_tests
        PRIVATE GLIFISTORE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
    glifistore_set_warnings(glifistore_tests)
    glifistore_enable_hardening(glifistore_tests)
    glifistore_enable_ipo(glifistore_tests)
    add_test(NAME glifistore_tests COMMAND glifistore_tests)
    add_executable(glifistore_security_audit_sink_tests
        ${CMAKE_SOURCE_DIR}/tests/security_audit_sink_tests.cpp
    )
    target_link_libraries(glifistore_security_audit_sink_tests
        PRIVATE GlifiStore::server glifistore_project_options
    )
    target_include_directories(glifistore_security_audit_sink_tests PRIVATE "${CMAKE_SOURCE_DIR}/tests")
    glifistore_set_warnings(glifistore_security_audit_sink_tests)
    glifistore_enable_hardening(glifistore_security_audit_sink_tests)
    glifistore_enable_ipo(glifistore_security_audit_sink_tests)
    foreach(scenario IN ITEMS normal blocked closed nonblocking)
        add_test(NAME glifistore_audit_sink_${scenario}
            COMMAND glifistore_security_audit_sink_tests ${scenario})
        set_tests_properties(glifistore_audit_sink_${scenario} PROPERTIES TIMEOUT 15)
    endforeach()
    add_test(
        NAME glifistore_test_harness_rejects_partial_shard
        COMMAND "${CMAKE_COMMAND}"
                -DGLIFISTORE_TEST_EXECUTABLE=$<TARGET_FILE:glifistore_tests>
                -DSHARD_COUNT=2
                "-DEXPECTED_DIAGNOSTIC=must be set together"
                -P "${CMAKE_SOURCE_DIR}/tests/cmake/test_shard_failure.cmake"
    )
    add_test(
        NAME glifistore_test_harness_rejects_zero_shards
        COMMAND "${CMAKE_COMMAND}"
                -DGLIFISTORE_TEST_EXECUTABLE=$<TARGET_FILE:glifistore_tests>
                -DSHARD_COUNT=0
                -DSHARD_INDEX=0
                "-DEXPECTED_DIAGNOSTIC=must be greater than zero"
                -P "${CMAKE_SOURCE_DIR}/tests/cmake/test_shard_failure.cmake"
    )
    add_test(
        NAME glifistore_test_harness_rejects_malformed_shards
        COMMAND "${CMAKE_COMMAND}"
                -DGLIFISTORE_TEST_EXECUTABLE=$<TARGET_FILE:glifistore_tests>
                -DSHARD_COUNT=2x
                -DSHARD_INDEX=0
                "-DEXPECTED_DIAGNOSTIC=expected an unsigned decimal integer"
                -P "${CMAKE_SOURCE_DIR}/tests/cmake/test_shard_failure.cmake"
    )
    add_test(
        NAME glifistore_test_harness_rejects_excessive_shards
        COMMAND "${CMAKE_COMMAND}"
                -DGLIFISTORE_TEST_EXECUTABLE=$<TARGET_FILE:glifistore_tests>
                -DSHARD_COUNT=999999
                -DSHARD_INDEX=0
                "-DEXPECTED_DIAGNOSTIC=must not exceed the registered test count"
                -P "${CMAKE_SOURCE_DIR}/tests/cmake/test_shard_failure.cmake"
    )
    add_test(
        NAME glifistore_test_harness_rejects_out_of_range_shard
        COMMAND "${CMAKE_COMMAND}"
                -DGLIFISTORE_TEST_EXECUTABLE=$<TARGET_FILE:glifistore_tests>
                -DSHARD_COUNT=2
                -DSHARD_INDEX=2
                "-DEXPECTED_DIAGNOSTIC=must be less than GLIFISTORE_TEST_SHARD_COUNT"
                -P "${CMAKE_SOURCE_DIR}/tests/cmake/test_shard_failure.cmake"
    )
    add_test(
        NAME glifistore_released_artifact_compat
        COMMAND glifistore_tests "released artifact harness"
    )
    set_tests_properties(glifistore_released_artifact_compat PROPERTIES LABELS "compatibility")
    set_target_properties(glifistore_tests PROPERTIES FOLDER "Quality")
    glifistore_xcode_scheme(glifistore_tests)

    if(NOT CMAKE_SYSTEM_NAME STREQUAL "FreeBSD" AND NOT GLIFISTORE_ENABLE_TSAN)
        # FreeBSD libc++ rejects replaced sized/aligned operator delete as non-usual when
        # paired with __builtin_operator_delete (GS residual: allocation fault suite is
        # Linux/macOS primary). TSan's runtime already owns operator new/delete, so the
        # fault-injection overrides cannot link under -fsanitize=thread.
        add_executable(glifistore_allocation_fault_tests
            ${CMAKE_SOURCE_DIR}/tests/quality/allocation_fault_harness.cpp
            ${CMAKE_SOURCE_DIR}/tests/quality/allocation_fault_core_tests.cpp
            ${CMAKE_SOURCE_DIR}/tests/quality/allocation_fault_paired_tests.cpp
            ${CMAKE_SOURCE_DIR}/tests/quality/allocation_fault_paired_async_tests.cpp
            ${CMAKE_SOURCE_DIR}/tests/quality/allocation_fault_main.cpp
        )
        target_link_libraries(glifistore_allocation_fault_tests
            PRIVATE GlifiStore::server glifistore_project_options
        )
        target_include_directories(glifistore_allocation_fault_tests PRIVATE
            "${CMAKE_SOURCE_DIR}/src"
            "${CMAKE_SOURCE_DIR}/tests"
        )
        glifistore_set_warnings(glifistore_allocation_fault_tests)
        glifistore_enable_hardening(glifistore_allocation_fault_tests)
        glifistore_enable_ipo(glifistore_allocation_fault_tests)
        set_target_properties(glifistore_allocation_fault_tests PROPERTIES FOLDER "Quality")
        if(GLIFISTORE_ENABLE_CLANG_TIDY AND APPLE)
            # clang-tidy 21 asks libc++ to resolve __builtin_operator_delete while this harness
            # deliberately supplies every global delete overload. That combination is rejected as
            # a non-usual deallocation function even though the same TU compiles and runs normally.
            # Keep the executable in the build/test matrix; omit only clang-tidy on this test-only
            # replacement-allocation target. Production sources remain covered by the fail-closed
            # compile-database gate.
            set_target_properties(glifistore_allocation_fault_tests PROPERTIES CXX_CLANG_TIDY "")
        endif()
        add_test(NAME glifistore_allocation_fault_tests COMMAND glifistore_allocation_fault_tests)
        set_tests_properties(glifistore_allocation_fault_tests PROPERTIES
            TIMEOUT 180
            LABELS "fault-injection"
        )
        glifistore_xcode_scheme(glifistore_allocation_fault_tests)
    endif()

    add_executable(glifistore_crash_persistence ${CMAKE_SOURCE_DIR}/tests/crash/crash_persistence.cpp)
    target_link_libraries(glifistore_crash_persistence PRIVATE GlifiStore::core glifistore_project_options)
    target_include_directories(glifistore_crash_persistence PRIVATE "${CMAKE_SOURCE_DIR}/tests")
    glifistore_set_warnings(glifistore_crash_persistence)
    glifistore_enable_hardening(glifistore_crash_persistence)
    glifistore_enable_ipo(glifistore_crash_persistence)
    set_target_properties(glifistore_crash_persistence PROPERTIES FOLDER "Quality")
    add_test(NAME glifistore_crash_sync COMMAND glifistore_crash_persistence --mode matrix)
    add_test(NAME glifistore_crash_periodic COMMAND glifistore_crash_persistence --mode periodic-matrix)
    add_test(NAME glifistore_crash_group COMMAND glifistore_crash_persistence --mode group-matrix)
    add_test(NAME glifistore_crash_random_smoke COMMAND glifistore_crash_persistence
        --mode random-campaign --campaign-seed 11636782997727379457 --iterations 2)
    add_test(NAME glifistore_crash_pause_smoke COMMAND glifistore_crash_persistence --mode pause-smoke)
    # Sync matrix is large; TSan instrumentation of recovery/encode paths routinely
    # exceeds 10 minutes on hosted/macOS runners even when making forward progress.
    set_tests_properties(glifistore_crash_sync PROPERTIES TIMEOUT 2400 LABELS "crash")
    set_tests_properties(glifistore_crash_periodic glifistore_crash_group PROPERTIES TIMEOUT 600
                         LABELS "crash")
    set_tests_properties(glifistore_crash_random_smoke PROPERTIES TIMEOUT 600 LABELS "crash")
    set_tests_properties(glifistore_crash_pause_smoke PROPERTIES TIMEOUT 120 LABELS "crash")

    add_executable(glifistore_crash_daemon ${CMAKE_SOURCE_DIR}/tests/crash/crash_daemon.cpp)
    target_link_libraries(glifistore_crash_daemon
        PRIVATE GlifiStore::client GlifiStore::server glifistore_project_options
    )
    glifistore_set_warnings(glifistore_crash_daemon)
    glifistore_enable_hardening(glifistore_crash_daemon)
    glifistore_enable_ipo(glifistore_crash_daemon)
    set_target_properties(glifistore_crash_daemon PROPERTIES FOLDER "Quality")
    add_test(NAME glifistore_crash_daemon_sync
        COMMAND glifistore_crash_daemon --daemon $<TARGET_FILE:glifistored> --storage durable-sync)
    add_test(NAME glifistore_crash_daemon_group
        COMMAND glifistore_crash_daemon --daemon $<TARGET_FILE:glifistored> --storage durable-group)
    add_test(NAME glifistore_crash_daemon_periodic
        COMMAND glifistore_crash_daemon --daemon $<TARGET_FILE:glifistored> --storage durable-periodic)
    add_test(NAME glifistore_crash_daemon_pre_commit
        COMMAND glifistore_crash_daemon --daemon $<TARGET_FILE:glifistored> --storage durable-sync
                --checkpoint pre-commit)
    add_test(NAME glifistore_crash_daemon_erase
        COMMAND glifistore_crash_daemon --daemon $<TARGET_FILE:glifistored> --storage durable-sync
                --checkpoint post-ack-erase)
    set_tests_properties(
        glifistore_crash_daemon_sync
        glifistore_crash_daemon_group
        glifistore_crash_daemon_periodic
        glifistore_crash_daemon_pre_commit
        glifistore_crash_daemon_erase
        PROPERTIES TIMEOUT 120 LABELS "crash"
    )

    add_executable(glifistore_crash_backup ${CMAKE_SOURCE_DIR}/tests/crash/crash_backup.cpp)
    target_link_libraries(glifistore_crash_backup PRIVATE GlifiStore::core glifistore_project_options)
    target_include_directories(glifistore_crash_backup PRIVATE "${CMAKE_SOURCE_DIR}/tests")
    glifistore_set_warnings(glifistore_crash_backup)
    glifistore_enable_hardening(glifistore_crash_backup)
    glifistore_enable_ipo(glifistore_crash_backup)
    set_target_properties(glifistore_crash_backup PROPERTIES FOLDER "Quality")
    add_test(NAME glifistore_crash_backup COMMAND glifistore_crash_backup)
    set_tests_properties(glifistore_crash_backup PROPERTIES TIMEOUT 120 LABELS "crash")

    add_executable(glifistore_crash_backup_wire ${CMAKE_SOURCE_DIR}/tests/crash/crash_backup_wire.cpp)
    target_link_libraries(glifistore_crash_backup_wire
        PRIVATE GlifiStore::client GlifiStore::server glifistore_project_options
    )
    target_include_directories(glifistore_crash_backup_wire PRIVATE "${CMAKE_SOURCE_DIR}/tests")
    glifistore_set_warnings(glifistore_crash_backup_wire)
    glifistore_enable_hardening(glifistore_crash_backup_wire)
    glifistore_enable_ipo(glifistore_crash_backup_wire)
    set_target_properties(glifistore_crash_backup_wire PROPERTIES FOLDER "Quality")
    add_test(NAME glifistore_crash_backup_wire COMMAND glifistore_crash_backup_wire)
    set_tests_properties(glifistore_crash_backup_wire PROPERTIES TIMEOUT 180 LABELS "crash")

    add_executable(glifistore_crash_backup_daemon ${CMAKE_SOURCE_DIR}/tests/crash/crash_backup_daemon.cpp)
    target_link_libraries(glifistore_crash_backup_daemon
        PRIVATE GlifiStore::client GlifiStore::server glifistore_project_options
    )
    target_include_directories(glifistore_crash_backup_daemon PRIVATE "${CMAKE_SOURCE_DIR}/tests")
    glifistore_set_warnings(glifistore_crash_backup_daemon)
    glifistore_enable_hardening(glifistore_crash_backup_daemon)
    glifistore_enable_ipo(glifistore_crash_backup_daemon)
    set_target_properties(glifistore_crash_backup_daemon PROPERTIES FOLDER "Quality")
    add_test(NAME glifistore_crash_backup_daemon
             COMMAND glifistore_crash_backup_daemon --daemon $<TARGET_FILE:glifistored>)
    set_tests_properties(glifistore_crash_backup_daemon PROPERTIES TIMEOUT 180 LABELS "crash"
                         DEPENDS glifistored)
endif()

if(BUILD_TESTING)
    add_test(NAME glifistore_cli_daemon_help COMMAND glifistored --help)
    set_tests_properties(glifistore_cli_daemon_help PROPERTIES
        PASS_REGULAR_EXPRESSION "GlifiStore native binary key-value server"
    )
    add_test(NAME glifistore_cli_daemon_version COMMAND glifistored --version)
    set_tests_properties(glifistore_cli_daemon_version PROPERTIES
        PASS_REGULAR_EXPRESSION "glifistored ${PROJECT_VERSION}"
    )
    add_test(NAME glifistore_cli_inspect_help COMMAND glifistore_inspect_segment --help)
    set_tests_properties(glifistore_cli_inspect_help PROPERTIES
        PASS_REGULAR_EXPRESSION "Inspect a GlifiStore durable Segment file"
    )
    add_test(NAME glifistore_cli_verify_help COMMAND glifistore_verify_store --help)
    set_tests_properties(glifistore_cli_verify_help PROPERTIES
        PASS_REGULAR_EXPRESSION "Verify a GlifiStore durable data directory"
    )
    add_test(NAME glifistore_cli_backup_help COMMAND glifistore_backup_store --help)
    set_tests_properties(glifistore_cli_backup_help PROPERTIES
        PASS_REGULAR_EXPRESSION "Offline backup"
    )
    add_test(NAME glifistore_cli_migrate_help COMMAND glifistore_migrate_store --help)
    set_tests_properties(glifistore_cli_migrate_help PROPERTIES
        PASS_REGULAR_EXPRESSION "Offline Worker reshard"
    )
    add_test(NAME glifistore_cli_repair_help COMMAND glifistore_repair_store --help)
    set_tests_properties(glifistore_cli_repair_help PROPERTIES
        PASS_REGULAR_EXPRESSION "Offline repair"
    )
    add_test(NAME glifistore_cli_rebuild_help COMMAND glifistore_rebuild_index --help)
    set_tests_properties(glifistore_cli_rebuild_help PROPERTIES
        PASS_REGULAR_EXPRESSION "Offline Index rebuild is not supported"
    )
    add_test(NAME glifistore_cli_rebuild_not_implemented
        COMMAND glifistore_rebuild_index -- /tmp/does-not-matter.glifi
    )
    set_tests_properties(glifistore_cli_rebuild_not_implemented PROPERTIES WILL_FAIL TRUE)
    add_test(NAME glifistore_cli_rejects_invalid_workers COMMAND glifistored --workers 257)
    set_tests_properties(glifistore_cli_rejects_invalid_workers PROPERTIES WILL_FAIL TRUE)
    add_test(NAME glifistore_cli_daemon_rejects_missing_data_directory
        COMMAND glifistored --storage-mode durable-sync
    )
    set_tests_properties(glifistore_cli_daemon_rejects_missing_data_directory PROPERTIES WILL_FAIL TRUE)
    add_test(NAME glifistore_cli_daemon_rejects_durable_options_in_volatile_mode
        COMMAND glifistored --data-dir unused
    )
    set_tests_properties(glifistore_cli_daemon_rejects_durable_options_in_volatile_mode
        PROPERTIES WILL_FAIL TRUE
    )
    add_test(NAME glifistore_cli_daemon_rejects_resource_flags_in_volatile_mode
        COMMAND glifistored --max-store-bytes 1GiB
    )
    set_tests_properties(glifistore_cli_daemon_rejects_resource_flags_in_volatile_mode
        PROPERTIES WILL_FAIL TRUE
    )
    add_test(NAME glifistore_cli_daemon_rejects_zero_group_max_records
        COMMAND glifistored --storage-mode durable-group --data-dir /tmp/glifistore-cli-group
                --group-max-records 0
    )
    set_tests_properties(glifistore_cli_daemon_rejects_zero_group_max_records
        PROPERTIES WILL_FAIL TRUE
    )
    add_test(NAME glifistore_cli_daemon_rejects_zero_durable_mutation_queue
        COMMAND glifistored --durable-mutation-queue-capacity 0
    )
    set_tests_properties(glifistore_cli_daemon_rejects_zero_durable_mutation_queue
        PROPERTIES WILL_FAIL TRUE
    )
    add_test(NAME glifistore_cli_daemon_rejects_unknown_config_key
        COMMAND glifistored --config "${CMAKE_SOURCE_DIR}/tests/fixtures/daemon-unknown-key.conf"
    )
    set_tests_properties(glifistore_cli_daemon_rejects_unknown_config_key
        PROPERTIES WILL_FAIL TRUE
    )
    add_test(NAME glifistore_cli_daemon_dump_config
        COMMAND glifistored --dump-config --port 0
    )
    set_tests_properties(glifistore_cli_daemon_dump_config PROPERTIES
        PASS_REGULAR_EXPRESSION "GlifiStore/config.*port=0"
    )
    set_tests_properties(
        glifistore_cli_daemon_help
        glifistore_cli_daemon_version
        glifistore_cli_daemon_rejects_missing_data_directory
        glifistore_cli_daemon_rejects_durable_options_in_volatile_mode
        glifistore_cli_daemon_rejects_resource_flags_in_volatile_mode
        glifistore_cli_daemon_rejects_zero_group_max_records
        glifistore_cli_daemon_rejects_zero_durable_mutation_queue
        glifistore_cli_daemon_rejects_unknown_config_key
        glifistore_cli_daemon_dump_config
        glifistore_cli_inspect_help
        glifistore_cli_rebuild_help
        glifistore_cli_rejects_invalid_workers
        PROPERTIES TIMEOUT 10
    )
endif()

if(BUILD_TESTING)
    set(glifistore_check_dependencies
        glifistore_c_abi_tests
        glifistore_c_abi_durable_tests
        glifistore_tests
        glifistore_crash_persistence
        glifistore_crash_daemon
        glifistore_crash_backup
        glifistore_crash_backup_wire
        glifistore_crash_backup_daemon
        glifistored
        glifistore_inspect_segment
        glifistore_verify_store
        glifistore_backup_store
        glifistore_migrate_store
        glifistore_repair_store
        glifistore_rebuild_index
    )
    if(TARGET glifistore_benchmarks)
        list(APPEND glifistore_check_dependencies
            glifistore_benchmarks
            glifistore_server_benchmarks
        )
    endif()
    add_custom_target(check
        COMMAND "${CMAKE_CTEST_COMMAND}" -C "$<CONFIG>" --output-on-failure
        DEPENDS ${glifistore_check_dependencies}
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        USES_TERMINAL
        COMMENT "Running GlifiStore tests with CTest"
    )
    set_target_properties(check PROPERTIES FOLDER "Quality")
    glifistore_xcode_scheme(check)
endif()
