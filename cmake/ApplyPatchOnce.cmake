# Applies PATCH_FILE to the source tree in the working directory, once: a tree it is already applied to (a
# reconfigure, or a populated source cache) is left as it is. Run as the PATCH_COMMAND of a fetched dependency:
#
#   ${CMAKE_COMMAND} -D GIT_EXECUTABLE=<git> -D PATCH_FILE=<file> -P ApplyPatchOnce.cmake
#
# Through `git apply`, which asks nothing and behaves alike everywhere (BSD patch answers its own "reversed
# patch?" prompt when it has no terminal): applied in reverse cleanly, the patch is in place; applied forward
# cleanly, it is not, and is applied; neither, the tree holds part of it, or something else, and that fails.
# The working directory need not be a repository; GIT_CEILING_DIRECTORIES keeps git from taking an enclosing
# one's paths for the patch's.
cmake_minimum_required(VERSION 3.25)
foreach(_required IN ITEMS GIT_EXECUTABLE PATCH_FILE)
    if(NOT ${_required})
        message(FATAL_ERROR "ApplyPatchOnce.cmake needs -D ${_required}=...")
    endif()
endforeach()

get_filename_component(_tree "." ABSOLUTE)
get_filename_component(_ceiling "${_tree}" DIRECTORY)

function(_git_apply result)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${_ceiling}"
                "${GIT_EXECUTABLE}" apply ${ARGN} "${PATCH_FILE}"
        RESULT_VARIABLE _status
        OUTPUT_QUIET ERROR_QUIET
    )
    set(${result} ${_status} PARENT_SCOPE)
endfunction()

_git_apply(_applied --check --reverse)
if(_applied EQUAL 0)
    message(STATUS "Already applied: ${PATCH_FILE}")
    return()
endif()
_git_apply(_applicable --check)
if(NOT _applicable EQUAL 0)
    message(FATAL_ERROR "${PATCH_FILE} is neither applied to ${_tree} nor applicable to it: the tree holds part of "
                        "it, or something else.")
endif()
_git_apply(_patched)
if(NOT _patched EQUAL 0)
    message(FATAL_ERROR "Could not apply ${PATCH_FILE} to ${_tree}.")
endif()
message(STATUS "Applied: ${PATCH_FILE}")
