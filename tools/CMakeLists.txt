# Offline/operator tools and PGO helper.
add_executable(glifistore_inspect_segment inspect_segment.cpp)
target_link_libraries(glifistore_inspect_segment
    PRIVATE GlifiStore::core glifistore_cli_support glifistore_project_options
)
target_compile_definitions(glifistore_inspect_segment PRIVATE GLIFISTORE_VERSION="${PROJECT_VERSION}")
glifistore_set_warnings(glifistore_inspect_segment)
glifistore_enable_hardening(glifistore_inspect_segment)
glifistore_enable_ipo(glifistore_inspect_segment)
set_target_properties(glifistore_inspect_segment PROPERTIES FOLDER "Tools")
glifistore_xcode_scheme(glifistore_inspect_segment ARGUMENTS --help)

add_executable(glifistore_verify_store verify_store.cpp)
target_link_libraries(glifistore_verify_store
    PRIVATE GlifiStore::core glifistore_cli_support glifistore_project_options
)
target_compile_definitions(glifistore_verify_store PRIVATE GLIFISTORE_VERSION="${PROJECT_VERSION}")
glifistore_set_warnings(glifistore_verify_store)
glifistore_enable_hardening(glifistore_verify_store)
glifistore_enable_ipo(glifistore_verify_store)
set_target_properties(glifistore_verify_store PROPERTIES FOLDER "Tools")
glifistore_xcode_scheme(glifistore_verify_store ARGUMENTS --help)

add_executable(glifistore_backup_store backup_store.cpp)
target_link_libraries(glifistore_backup_store
    PRIVATE GlifiStore::core glifistore_cli_support glifistore_project_options
)
target_compile_definitions(glifistore_backup_store PRIVATE GLIFISTORE_VERSION="${PROJECT_VERSION}")
glifistore_set_warnings(glifistore_backup_store)
glifistore_enable_hardening(glifistore_backup_store)
glifistore_enable_ipo(glifistore_backup_store)
set_target_properties(glifistore_backup_store PROPERTIES FOLDER "Tools")
glifistore_xcode_scheme(glifistore_backup_store ARGUMENTS --help)

add_executable(glifistore_migrate_store migrate_store.cpp)
target_link_libraries(glifistore_migrate_store
    PRIVATE GlifiStore::core glifistore_cli_support glifistore_project_options
)
target_compile_definitions(glifistore_migrate_store PRIVATE GLIFISTORE_VERSION="${PROJECT_VERSION}")
glifistore_set_warnings(glifistore_migrate_store)
glifistore_enable_hardening(glifistore_migrate_store)
glifistore_enable_ipo(glifistore_migrate_store)
set_target_properties(glifistore_migrate_store PROPERTIES FOLDER "Tools")
glifistore_xcode_scheme(glifistore_migrate_store ARGUMENTS --help)

add_executable(glifistore_repair_store repair_store.cpp)
target_link_libraries(glifistore_repair_store
    PRIVATE GlifiStore::core glifistore_cli_support glifistore_project_options
)
target_compile_definitions(glifistore_repair_store PRIVATE GLIFISTORE_VERSION="${PROJECT_VERSION}")
glifistore_set_warnings(glifistore_repair_store)
glifistore_enable_hardening(glifistore_repair_store)
glifistore_enable_ipo(glifistore_repair_store)
set_target_properties(glifistore_repair_store PROPERTIES FOLDER "Tools")
glifistore_xcode_scheme(glifistore_repair_store ARGUMENTS --help)

add_executable(glifistore_interop_client interop_client.cpp)
target_link_libraries(glifistore_interop_client
    PRIVATE GlifiStore::client glifistore_project_options
)
glifistore_set_warnings(glifistore_interop_client)
glifistore_enable_hardening(glifistore_interop_client)
glifistore_enable_ipo(glifistore_interop_client)
set_target_properties(glifistore_interop_client PROPERTIES FOLDER "Tools")
glifistore_xcode_scheme(glifistore_interop_client ARGUMENTS --help)

add_executable(glifistore_rebuild_index rebuild_index.cpp)
target_link_libraries(glifistore_rebuild_index
    PRIVATE GlifiStore::core glifistore_cli_support glifistore_project_options
)
target_compile_definitions(glifistore_rebuild_index PRIVATE GLIFISTORE_VERSION="${PROJECT_VERSION}")
glifistore_set_warnings(glifistore_rebuild_index)
glifistore_enable_hardening(glifistore_rebuild_index)
glifistore_enable_ipo(glifistore_rebuild_index)
set_target_properties(glifistore_rebuild_index PROPERTIES FOLDER "Tools")
glifistore_xcode_scheme(glifistore_rebuild_index ARGUMENTS --help)

if(GLIFISTORE_BUILD_BENCHMARKS OR BUILD_TESTING)
    add_executable(glifistore_pgo_durable pgo_durable.cpp)
    target_link_libraries(glifistore_pgo_durable PRIVATE GlifiStore::core glifistore_project_options)
    glifistore_set_warnings(glifistore_pgo_durable)
    glifistore_enable_hardening(glifistore_pgo_durable)
    # Must match core's IPO: Clang LTO archives are LLVM bitcode; linking
    # without INTERPROCEDURAL_OPTIMIZATION falls through to ld.bfd and fails
    # with "file format not recognized".
    glifistore_enable_ipo(glifistore_pgo_durable)
    set_target_properties(glifistore_pgo_durable PROPERTIES FOLDER "Tools")
endif()

