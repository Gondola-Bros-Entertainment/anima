# Runs COMMAND, a list of the program and its arguments, and passes only when the program exits with
# status 1 and prints MESSAGE. A crash or a successful run fails even when its output holds MESSAGE.
execute_process(COMMAND ${COMMAND} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
if(NOT result STREQUAL "1")
    message(FATAL_ERROR "Expected exit status 1, got ${result}:\n${output}")
endif()
string(FIND "${output}" "${MESSAGE}" at)
if(at EQUAL -1)
    message(FATAL_ERROR "Expected \"${MESSAGE}\" in:\n${output}")
endif()
