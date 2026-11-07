cmake_minimum_required(VERSION 3.25)

# This directory can also be configured independently, without a C++ toolchain.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(GlifiStoreDocumentation LANGUAGES NONE)
endif()

find_package(Doxygen 1.9.8 REQUIRED COMPONENTS doxygen)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
get_filename_component(GLIFISTORE_DOCS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(GLIFISTORE_DOCS_SOURCE_URL "https://github.com/gipidevdor/GlifiStore"
    CACHE STRING "Repository URL used for links to source files and evidence")
set(GLIFISTORE_DOCS_REVISION "main"
    CACHE STRING "Repository revision used for links to source files and evidence")

add_custom_target(glifistore-docs
    COMMAND "${Python3_EXECUTABLE}" "${GLIFISTORE_DOCS_ROOT}/scripts/build_docs.py"
        --doxygen "${DOXYGEN_EXECUTABLE}"
        --output "${CMAKE_CURRENT_BINARY_DIR}"
        --source-url "${GLIFISTORE_DOCS_SOURCE_URL}"
        --revision "${GLIFISTORE_DOCS_REVISION}"
    WORKING_DIRECTORY "${GLIFISTORE_DOCS_ROOT}"
    COMMENT "Generating GlifiStore C++ reference and technical guides"
    VERBATIM
    USES_TERMINAL
)
