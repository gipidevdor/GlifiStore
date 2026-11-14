# Benchmarks and hosted-CI smoke benches.
if(GLIFISTORE_BUILD_BENCHMARKS)
    find_program(GIT_EXECUTABLE NAMES git)
    set(GLIFISTORE_GIT_SHA "unknown")
    if(GIT_EXECUTABLE)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE GLIFISTORE_GIT_SHA
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        execute_process(
            COMMAND ${GIT_EXECUTABLE} diff-index --quiet HEAD --
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            RESULT_VARIABLE GLIFISTORE_GIT_DIRTY
            ERROR_QUIET
        )
        if(GLIFISTORE_GIT_DIRTY)
            string(APPEND GLIFISTORE_GIT_SHA "-dirty")
        endif()
    endif()
    if(GLIFISTORE_BUILD_LAB)
        add_executable(glifistore_generation_shell_benchmark
            generation_shell_benchmark.cpp
            ${CMAKE_SOURCE_DIR}/src/experimental/pair_read_generation_shell_access.cpp
        )
        target_link_libraries(
            glifistore_generation_shell_benchmark
            PRIVATE GlifiStore::core glifistore_project_options
        )
        target_include_directories(glifistore_generation_shell_benchmark
            PRIVATE "${CMAKE_SOURCE_DIR}/src"
        )
        target_compile_definitions(glifistore_generation_shell_benchmark
            PRIVATE GLIFISTORE_GIT_SHA="${GLIFISTORE_GIT_SHA}"
        )
        glifistore_set_warnings(glifistore_generation_shell_benchmark)
        glifistore_enable_hardening(glifistore_generation_shell_benchmark)
        glifistore_enable_ipo(glifistore_generation_shell_benchmark)
        set_target_properties(glifistore_generation_shell_benchmark PROPERTIES FOLDER "Benchmarks")
        add_executable(glifistore_generation_publication_benchmark
            generation_publication_benchmark.cpp
            ${CMAKE_SOURCE_DIR}/src/experimental/pair_read_generation_shell_access.cpp
        )
        target_link_libraries(
            glifistore_generation_publication_benchmark
            PRIVATE GlifiStore::server glifistore_project_options
        )
        target_include_directories(glifistore_generation_publication_benchmark
            PRIVATE "${CMAKE_SOURCE_DIR}/src"
        )
        target_compile_definitions(glifistore_generation_publication_benchmark
            PRIVATE GLIFISTORE_GIT_SHA="${GLIFISTORE_GIT_SHA}"
        )
        glifistore_set_warnings(glifistore_generation_publication_benchmark)
        glifistore_enable_hardening(glifistore_generation_publication_benchmark)
        glifistore_enable_ipo(glifistore_generation_publication_benchmark)
        set_target_properties(glifistore_generation_publication_benchmark PROPERTIES FOLDER "Benchmarks")
        add_executable(glifistore_generation_runtime_benchmark generation_runtime_benchmark.cpp)
        target_link_libraries(glifistore_generation_runtime_benchmark
            PRIVATE GlifiStore::core glifistore_project_options)
        target_compile_definitions(glifistore_generation_runtime_benchmark
            PRIVATE GLIFISTORE_GIT_SHA="${GLIFISTORE_GIT_SHA}")
        glifistore_set_warnings(glifistore_generation_runtime_benchmark)
        glifistore_enable_hardening(glifistore_generation_runtime_benchmark)
        glifistore_enable_ipo(glifistore_generation_runtime_benchmark)
        set_target_properties(glifistore_generation_runtime_benchmark PROPERTIES FOLDER "Lab")
    endif()
    add_executable(glifistore_benchmarks
        benchmark_main.cpp
        harness.cpp
        suite.cpp
    )
    target_link_libraries(glifistore_benchmarks PRIVATE GlifiStore::core glifistore_project_options)
    target_include_directories(glifistore_benchmarks PRIVATE "${CMAKE_SOURCE_DIR}/benchmarks")
    target_compile_definitions(glifistore_benchmarks PRIVATE GLIFISTORE_GIT_SHA="${GLIFISTORE_GIT_SHA}")
    glifistore_set_warnings(glifistore_benchmarks)
    glifistore_enable_hardening(glifistore_benchmarks)
    glifistore_enable_ipo(glifistore_benchmarks)
    set_target_properties(glifistore_benchmarks PROPERTIES FOLDER "Benchmarks")
    glifistore_xcode_scheme(glifistore_benchmarks RELEASE ARGUMENTS
        --filter store-put-get --ops 200000 --warmup 1 --repeats 7
    )
    add_executable(glifistore_memory_census_benchmark
        memory_census_benchmark.cpp
        harness.cpp
    )
    target_link_libraries(
        glifistore_memory_census_benchmark
        PRIVATE GlifiStore::core glifistore_project_options
    )
    target_include_directories(glifistore_memory_census_benchmark
        PRIVATE "${CMAKE_SOURCE_DIR}/benchmarks" "${CMAKE_SOURCE_DIR}/src"
    )
    target_compile_definitions(glifistore_memory_census_benchmark
        PRIVATE GLIFISTORE_GIT_SHA="${GLIFISTORE_GIT_SHA}"
    )
    glifistore_set_warnings(glifistore_memory_census_benchmark)
    glifistore_enable_hardening(glifistore_memory_census_benchmark)
    glifistore_enable_ipo(glifistore_memory_census_benchmark)
    set_target_properties(glifistore_memory_census_benchmark PROPERTIES FOLDER "Benchmarks")
    add_executable(glifistore_compaction_benchmark
        compaction_benchmark.cpp
    )
    target_link_libraries(
        glifistore_compaction_benchmark
        PRIVATE GlifiStore::core glifistore_project_options
    )
    target_compile_definitions(
        glifistore_compaction_benchmark
        PRIVATE GLIFISTORE_GIT_SHA="${GLIFISTORE_GIT_SHA}"
    )
    glifistore_set_warnings(glifistore_compaction_benchmark)
    glifistore_enable_hardening(glifistore_compaction_benchmark)
    glifistore_enable_ipo(glifistore_compaction_benchmark)
    set_target_properties(glifistore_compaction_benchmark PROPERTIES FOLDER "Benchmarks")
    glifistore_xcode_scheme(glifistore_compaction_benchmark RELEASE ARGUMENTS
        --repeats 3
    )
    add_executable(glifistore_maintenance_benchmark
        maintenance_benchmark.cpp
    )
    target_link_libraries(
        glifistore_maintenance_benchmark
        PRIVATE GlifiStore::core glifistore_project_options
    )
    target_compile_definitions(
        glifistore_maintenance_benchmark
        PRIVATE GLIFISTORE_GIT_SHA="${GLIFISTORE_GIT_SHA}"
    )
    glifistore_set_warnings(glifistore_maintenance_benchmark)
    glifistore_enable_hardening(glifistore_maintenance_benchmark)
    glifistore_enable_ipo(glifistore_maintenance_benchmark)
    set_target_properties(glifistore_maintenance_benchmark PROPERTIES FOLDER "Benchmarks")
    glifistore_xcode_scheme(glifistore_maintenance_benchmark RELEASE ARGUMENTS
        --repeats 3
    )
    add_executable(glifistore_server_benchmarks
        server_benchmark.cpp
        harness.cpp
    )
    target_link_libraries(glifistore_server_benchmarks
        PRIVATE GlifiStore::server GlifiStore::client glifistore_project_options)
    target_include_directories(glifistore_server_benchmarks
        PRIVATE "${CMAKE_SOURCE_DIR}/benchmarks")
    target_compile_definitions(glifistore_server_benchmarks
        PRIVATE GLIFISTORE_GIT_SHA="${GLIFISTORE_GIT_SHA}")
    glifistore_set_warnings(glifistore_server_benchmarks)
    glifistore_enable_hardening(glifistore_server_benchmarks)
    glifistore_enable_ipo(glifistore_server_benchmarks)
    set_target_properties(glifistore_server_benchmarks PROPERTIES FOLDER "Benchmarks")
    glifistore_xcode_scheme(glifistore_server_benchmarks RELEASE ARGUMENTS
        --ops 100000 --workers 2 --clients 2 --pipeline 32 --warmup 1 --repeats 7
    )
    add_executable(glifistore_client_benchmark client_benchmark.cpp)
    target_link_libraries(glifistore_client_benchmark
        PRIVATE GlifiStore::client glifistore_project_options)
    target_compile_definitions(glifistore_client_benchmark
        PRIVATE GLIFISTORE_SDK_VERSION="${PROJECT_VERSION}")
    glifistore_set_warnings(glifistore_client_benchmark)
    glifistore_enable_hardening(glifistore_client_benchmark)
    glifistore_enable_ipo(glifistore_client_benchmark)
    set_target_properties(glifistore_client_benchmark PROPERTIES FOLDER "Benchmarks")
    if(BUILD_TESTING)
        foreach(distribution IN ITEMS uniform worker-affine single-worker zipf)
            # single-worker concentrates all keys on one paired lane. 4×4 on a slow
            # hosted VM can trip generation-admission resource_exhausted; the harness
            # then undercounts hits (admission reject ≠ visibility bug). Hosted benches
            # already bound this case; keep a hotspot smoke with 1×2 so routing stress
            # remains without treating admission pressure as a hard miss.
            if(distribution STREQUAL "single-worker")
                set(_par_workers 1)
                set(_par_threads 2)
            else()
                set(_par_workers 4)
                set(_par_threads 4)
            endif()
            add_test(
                NAME glifistore_benchmark_parallel_${distribution}
                COMMAND glifistore_benchmarks
                    --filter store-parallel-all
                    --ops 512
                    --workers ${_par_workers}
                    --threads ${_par_threads}
                    --distribution ${distribution}
                    --warmup 0
                    --repeats 1
            )
        endforeach()
        add_test(
            NAME glifistore_benchmark_parallel_put_latency_smoke
            COMMAND glifistore_benchmarks
                --filter store-parallel-put
                --ops 256
                --workers 1
                --threads 1
                --distribution single-worker
                --value-size 64
                --latency
                --warmup 0
                --repeats 1
            )
        set_tests_properties(glifistore_benchmark_parallel_put_latency_smoke PROPERTIES
            PASS_REGULAR_EXPRESSION "latency_samples=256"
        )
        add_test(
            NAME glifistore_benchmark_durable_smoke
            COMMAND glifistore_benchmarks
                --filter store-durable-all
                --ops 128
                --workers 1
                --warmup 0
                --repeats 1
        )
        add_test(
            NAME glifistore_benchmark_memory_census_smoke
            COMMAND glifistore_memory_census_benchmark
                --entries 60000
                --key-bytes 16
                --value-bytes 64
                --workers 1
        )
        set_tests_properties(glifistore_benchmark_memory_census_smoke PROPERTIES
            PASS_REGULAR_EXPRESSION
                "read_generation_base_record_mapped_storage_bytes=(0|3670016).*read_generation_spare_mapping_bytes=(0|4194304).*compact_lookup_saved_bytes=.*allocator_stats_available="
        )
        add_test(
            NAME glifistore_benchmark_durable_parallel_smoke
            COMMAND glifistore_benchmarks
                --filter store-durable-parallel-all
                --ops 128
                --workers 4
                --threads 4
                --distribution worker-affine
                --warmup 0
                --repeats 1
        )
        add_test(
            NAME glifistore_benchmark_durable_group_smoke
            COMMAND glifistore_benchmarks
                --filter store-durable-group-parallel-put
                --ops 64
                --workers 1
                --threads 4
                --distribution single-worker
                --latency
                --warmup 0
                --repeats 1
        )
        set_tests_properties(glifistore_benchmark_durable_group_smoke PROPERTIES
            PASS_REGULAR_EXPRESSION "latency_samples=64"
        )
        add_test(
            NAME glifistore_benchmark_rejects_invalid_numeric_option
            COMMAND glifistore_benchmarks --ops not-a-number
        )
        set_tests_properties(glifistore_benchmark_rejects_invalid_numeric_option PROPERTIES
            WILL_FAIL TRUE
        )
        add_test(
            NAME glifistore_benchmark_rejects_numeric_suffix
            COMMAND glifistore_benchmarks --ops 1junk
        )
        set_tests_properties(glifistore_benchmark_rejects_numeric_suffix PROPERTIES
            WILL_FAIL TRUE
        )
        add_test(
            NAME glifistore_compaction_benchmark_rejects_numeric_suffix
            COMMAND glifistore_compaction_benchmark --repeats 1junk
        )
        set_tests_properties(glifistore_compaction_benchmark_rejects_numeric_suffix PROPERTIES
            WILL_FAIL TRUE
        )
        if(GLIFISTORE_BUILD_LAB)
            add_test(
                NAME glifistore_generation_shell_benchmark_rejects_numeric_suffix
                COMMAND glifistore_generation_shell_benchmark --ops 1junk
            )
            set_tests_properties(glifistore_generation_shell_benchmark_rejects_numeric_suffix PROPERTIES
                WILL_FAIL TRUE
            )
        endif()
        if(GLIFISTORE_BUILD_LAB)
            add_test(
                NAME glifistore_generation_publication_benchmark_rejects_numeric_suffix
                COMMAND glifistore_generation_publication_benchmark --ops 1junk
            )
            set_tests_properties(glifistore_generation_publication_benchmark_rejects_numeric_suffix PROPERTIES
                WILL_FAIL TRUE
            )
        endif()
        add_test(
            NAME glifistore_maintenance_benchmark_rejects_numeric_suffix
            COMMAND glifistore_maintenance_benchmark --operations 1junk
        )
        set_tests_properties(glifistore_maintenance_benchmark_rejects_numeric_suffix PROPERTIES
            WILL_FAIL TRUE
        )
        add_test(
            NAME glifistore_memory_census_benchmark_rejects_numeric_suffix
            COMMAND glifistore_memory_census_benchmark --entries 1junk
        )
        set_tests_properties(glifistore_memory_census_benchmark_rejects_numeric_suffix PROPERTIES
            WILL_FAIL TRUE
        )
        add_test(
            NAME glifistore_benchmark_maintenance_smoke
            COMMAND glifistore_maintenance_benchmark
                --operations 128
                --threads 2
                --keys 16
                --value-bytes 1024
                --reclaim-value-bytes 1024
                --put-percent 10
                --maintenance-interval-ms 1
                --cooldown-ms 0
                --warmup 0
                --repeats 1
                --mode disabled
        )
        set_tests_properties(glifistore_benchmark_maintenance_smoke PROPERTIES
            PASS_REGULAR_EXPRESSION "disabled,1,128"
        )
        add_test(
            NAME glifistore_benchmark_server_tcp
            COMMAND glifistore_server_benchmarks
                --ops 128
                --workers 2
                --clients 2
                --pipeline 8
                --warmup 0
                --repeats 1
        )
        add_test(
            NAME glifistore_benchmark_server_tcp_large_get
            COMMAND glifistore_server_benchmarks
                --ops 8
                --workers 1
                --clients 1
                --pipeline 8
                --value-size 262144
                --workload get-only
                --warmup 0
                --repeats 1
        )
        add_test(
            NAME glifistore_benchmark_server_tcp_durable_group
            COMMAND glifistore_server_benchmarks
                --ops 32
                --workers 1
                --clients 4
                --pipeline 8
                --storage-mode durable-group
                --group-max-records 4
                --group-max-wait-ms 20
                --warmup 0
                --repeats 1
        )
        set_tests_properties(glifistore_benchmark_server_tcp_durable_group PROPERTIES
            # Under sanitizers / qemu, adaptive batching may close before hitting
            # group-max-records=4; require that group commit ran with a sane batch.
            PASS_REGULAR_EXPRESSION "maximum_durable_batch_records=[1-4]"
        )
    endif()
endif()
