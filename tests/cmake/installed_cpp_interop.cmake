cmake_minimum_required(VERSION 3.25)

foreach(required IN ITEMS GLIFISTORE_BINARY_DIR GLIFISTORE_SOURCE_DIR
                          GLIFISTORE_TEST_ROOT GLIFISTORE_BASH)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${GLIFISTORE_TEST_ROOT}")
set(prefix "${GLIFISTORE_TEST_ROOT}/prefix")
set(output "${GLIFISTORE_TEST_ROOT}/glifistore-interop-cpp")

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${GLIFISTORE_BINARY_DIR}" --prefix "${prefix}"
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error
)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "Installing test prefix failed\n${install_output}\n${install_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "CMAKE=${CMAKE_COMMAND}"
            "CXX=${GLIFISTORE_CXX_COMPILER}"
            "GLIFISTORE_SANITIZERS=${GLIFISTORE_SANITIZERS}"
            "${GLIFISTORE_BASH}"
            "${GLIFISTORE_SOURCE_DIR}/scripts/build-installed-cpp-interop.sh"
            "${prefix}" "${output}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR
        "Installed-prefix C++ interop build failed\n${build_output}\n${build_error}")
endif()
if(NOT EXISTS "${output}")
    message(FATAL_ERROR "Installed-prefix C++ interop executable is missing: ${output}")
endif()

execute_process(
    COMMAND "${output}" --help
    RESULT_VARIABLE run_result
    OUTPUT_QUIET
    ERROR_QUIET
)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "Installed-prefix C++ interop executable did not run")
endif()
