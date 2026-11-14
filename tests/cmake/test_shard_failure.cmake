if(NOT DEFINED GLIFISTORE_TEST_EXECUTABLE OR NOT DEFINED EXPECTED_DIAGNOSTIC)
    message(FATAL_ERROR "GLIFISTORE_TEST_EXECUTABLE and EXPECTED_DIAGNOSTIC are required")
endif()

set(shard_environment "${CMAKE_COMMAND}" -E env)
if(DEFINED SHARD_COUNT)
    list(APPEND shard_environment "GLIFISTORE_TEST_SHARD_COUNT=${SHARD_COUNT}")
else()
    list(APPEND shard_environment --unset=GLIFISTORE_TEST_SHARD_COUNT)
endif()
if(DEFINED SHARD_INDEX)
    list(APPEND shard_environment "GLIFISTORE_TEST_SHARD_INDEX=${SHARD_INDEX}")
else()
    list(APPEND shard_environment --unset=GLIFISTORE_TEST_SHARD_INDEX)
endif()

execute_process(
    COMMAND ${shard_environment} "${GLIFISTORE_TEST_EXECUTABLE}"
    RESULT_VARIABLE shard_result
    OUTPUT_VARIABLE shard_stdout
    ERROR_VARIABLE shard_stderr
)
if(shard_result EQUAL 0)
    message(FATAL_ERROR "invalid shard configuration was accepted")
endif()

string(CONCAT shard_output "${shard_stdout}" "${shard_stderr}")
if(NOT shard_output MATCHES "${EXPECTED_DIAGNOSTIC}")
    message(FATAL_ERROR
        "invalid shard configuration did not emit '${EXPECTED_DIAGNOSTIC}':\n${shard_output}"
    )
endif()
