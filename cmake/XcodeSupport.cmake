include_guard(GLOBAL)

include(CMakeParseArguments)

function(glifistore_xcode_scheme target)
    set(options RELEASE)
    set(multi_value_arguments ARGUMENTS)
    cmake_parse_arguments(GLIFISTORE_XCODE "${options}" "" "${multi_value_arguments}" ${ARGN})

    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Cannot configure an Xcode scheme for unknown target: ${target}")
    endif()
    if(NOT CMAKE_GENERATOR STREQUAL "Xcode")
        return()
    endif()

    set_target_properties("${target}" PROPERTIES
        XCODE_GENERATE_SCHEME TRUE
        XCODE_SCHEME_WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    )
    if(GLIFISTORE_XCODE_RELEASE)
        set_target_properties("${target}" PROPERTIES
            XCODE_SCHEME_LAUNCH_CONFIGURATION Release
        )
    endif()
    if(GLIFISTORE_XCODE_ARGUMENTS)
        set_target_properties("${target}" PROPERTIES
            XCODE_SCHEME_ARGUMENTS "${GLIFISTORE_XCODE_ARGUMENTS}"
        )
    endif()
endfunction()

function(glifistore_add_xcode_project_files)
    if(NOT CMAKE_GENERATOR STREQUAL "Xcode")
        return()
    endif()

    file(GLOB_RECURSE glifistore_xcode_headers CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/include/*.hpp"
        "${PROJECT_SOURCE_DIR}/src/*.hpp"
        "${PROJECT_SOURCE_DIR}/tests/*.hpp"
        "${PROJECT_SOURCE_DIR}/benchmarks/*.hpp"
    )
    file(GLOB_RECURSE glifistore_xcode_docs CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/docs/*.md"
    )
    file(GLOB glifistore_xcode_scripts CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/scripts/*.sh"
        "${PROJECT_SOURCE_DIR}/scripts/*.py"
    )
    set(glifistore_xcode_root_files
        "${PROJECT_SOURCE_DIR}/README.md"
        "${PROJECT_SOURCE_DIR}/CHANGELOG.md"
        "${PROJECT_SOURCE_DIR}/CONTRIBUTING.md"
        "${PROJECT_SOURCE_DIR}/SECURITY.md"
        "${PROJECT_SOURCE_DIR}/CMakeLists.txt"
        "${PROJECT_SOURCE_DIR}/CMakePresets.json"
        "${PROJECT_SOURCE_DIR}/Makefile"
        "${PROJECT_SOURCE_DIR}/VERSION"
    )
    set(glifistore_xcode_files
        ${glifistore_xcode_headers}
        ${glifistore_xcode_docs}
        ${glifistore_xcode_scripts}
        ${glifistore_xcode_root_files}
    )
    source_group(TREE "${PROJECT_SOURCE_DIR}" PREFIX "Project" FILES ${glifistore_xcode_files})
    add_custom_target(glifistore_project_files SOURCES ${glifistore_xcode_files})
    set_target_properties(glifistore_project_files PROPERTIES
        FOLDER "Project"
        XCODE_GENERATE_SCHEME FALSE
    )
endfunction()
