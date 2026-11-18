# git apply PATCH in the current directory, unless it is applied already:
# FetchContent runs the patch step again when it reconfigures.
#
#   cmake -DGIT_EXECUTABLE=<git> -DPATCH=<file> -P apply_patch.cmake
execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --reverse --check --ignore-whitespace "${PATCH}"
    RESULT_VARIABLE already_applied
    OUTPUT_QUIET ERROR_QUIET
)
if (already_applied EQUAL 0)
    return()
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace "${PATCH}"
    RESULT_VARIABLE result
)
if (NOT result EQUAL 0)
    message(FATAL_ERROR "could not apply ${PATCH}")
endif()
