# Self-test of ApplyPatchOnce.cmake, in a scratch tree under WORK_DIR: a tree without the patch is patched, a
# tree with it is left alone, and a tree holding part of it fails -- each through `git apply`, which asks no
# question, run without a terminal (stdin from the null device) as a build runs it. Then the tree as a fetched
# dependency is: a git clone of its own, which is patched alike.
#
#   ${CMAKE_COMMAND} -D GIT_EXECUTABLE=<git> -D WORK_DIR=<dir> -P ApplyPatchOnceTest.cmake
cmake_minimum_required(VERSION 3.25)
foreach(_required IN ITEMS GIT_EXECUTABLE WORK_DIR)
    if(NOT ${_required})
        message(FATAL_ERROR "ApplyPatchOnceTest.cmake needs -D ${_required}=...")
    endif()
endforeach()

set(_script "${CMAKE_CURRENT_LIST_DIR}/ApplyPatchOnce.cmake")
set(_tree "${WORK_DIR}/tree")
set(_patch "${WORK_DIR}/two-hunks.patch")
if(CMAKE_HOST_WIN32)
    set(_null NUL)
else()
    set(_null /dev/null)
endif()

# Two hunks, far enough apart to apply one without the other.
set(_original "")
foreach(_line RANGE 1 20)
    string(APPEND _original "line ${_line}\n")
endforeach()
string(REPLACE "line 2\n" "line 2 patched\n" _first "${_original}")
string(REPLACE "line 19\n" "line 19 patched\n" _both "${_first}")
file(WRITE "${_patch}"
     "--- a/file.txt\n+++ b/file.txt\n"
     "@@ -1,5 +1,5 @@\n line 1\n-line 2\n+line 2 patched\n line 3\n line 4\n line 5\n"
     "@@ -16,5 +16,5 @@\n line 16\n line 17\n line 18\n-line 19\n+line 19 patched\n line 20\n")

# Runs the script on the scratch tree as it is; @p status and @p output what it came to.
function(_apply status output)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -D "GIT_EXECUTABLE=${GIT_EXECUTABLE}" -D "PATCH_FILE=${_patch}" -P "${_script}"
        WORKING_DIRECTORY "${_tree}"
        INPUT_FILE "${_null}"
        RESULT_VARIABLE _status
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _output
        TIMEOUT 60
    )
    set(${status} "${_status}" PARENT_SCOPE)
    set(${output} "${_output}" PARENT_SCOPE)
endfunction()

# Runs git with the arguments given in the scratch tree, which must succeed.
function(_git)
    execute_process(COMMAND "${GIT_EXECUTABLE}" ${ARGN} WORKING_DIRECTORY "${_tree}"
                    INPUT_FILE "${_null}" RESULT_VARIABLE _git_status OUTPUT_QUIET ERROR_QUIET)
    if(NOT _git_status EQUAL 0)
        message(FATAL_ERROR "apply-patch-once: could not make the clone (git ${ARGN}: ${_git_status})")
    endif()
endfunction()

# Runs the script on a fresh scratch tree holding @p content; @p status and @p output what it came to. With a
# fourth argument CLONE, the tree is a repository of its own, its content committed, and checked out again as a
# Windows runner's git does (core.autocrlf=true): with CRLF line ends, which the LF patch must still match.
function(_run_on content status output)
    file(REMOVE_RECURSE "${_tree}")
    file(WRITE "${_tree}/file.txt" "${content}")
    if(ARGC GREATER 3 AND ARGV3 STREQUAL "CLONE")
        _git(init --quiet)
        _git(config core.autocrlf true)
        _git(add file.txt)
        _git(-c user.name=test -c user.email=test@invalid -c commit.gpgsign=false commit --quiet -m tree)
        file(REMOVE "${_tree}/file.txt")
        _git(checkout -- file.txt)
    endif()
    _apply(_status _output)
    set(${status} "${_status}" PARENT_SCOPE)
    set(${output} "${_output}" PARENT_SCOPE)
endfunction()

# Fails the test, naming what was expected, unless the condition (given as if() takes it) holds.
macro(_expect description)
    if(NOT (${ARGN}))
        message(SEND_ERROR "apply-patch-once: FAILED: ${description}")
    endif()
endmacro()

# Not applied: applied.
_run_on("${_original}" _status _output)
file(READ "${_tree}/file.txt" _after)
_expect("an unpatched tree is patched (exit ${_status}: ${_output})" _status EQUAL 0)
_expect("an unpatched tree ends up with both hunks" _after STREQUAL _both)

# Applied: left alone, and said so.
_run_on("${_both}" _status _output)
file(READ "${_tree}/file.txt" _after)
_expect("a patched tree passes (exit ${_status}: ${_output})" _status EQUAL 0)
_expect("a patched tree is left as it was" _after STREQUAL _both)
_expect("a patched tree is said to be patched" _output MATCHES "Already applied")

# Half applied: fails, and changes nothing.
_run_on("${_first}" _status _output)
file(READ "${_tree}/file.txt" _after)
_expect("a half-patched tree fails" NOT _status EQUAL 0)
_expect("a half-patched tree is left as it was" _after STREQUAL _first)

# A clone of its own, as CPM fetches a dependency: patched, and left alone once patched.
_run_on("${_original}" _status _output CLONE)
# Its line ends read as hex: file(READ) as text drops the carriage returns.
file(READ "${_tree}/file.txt" _after_hex HEX)
file(READ "${_tree}/file.txt" _after)
string(REPLACE "\r\n" "\n" _after "${_after}")
_expect("an unpatched clone is patched (exit ${_status}: ${_output})" _status EQUAL 0)
_expect("an unpatched clone ends up with both hunks" _after STREQUAL _both)
_expect("an unpatched clone keeps its CRLF line ends" _after_hex MATCHES "0d0a$")
_apply(_status _output)
_expect("a patched clone passes (exit ${_status}: ${_output})" _status EQUAL 0)
_expect("a patched clone is said to be patched" _output MATCHES "Already applied")

file(REMOVE_RECURSE "${WORK_DIR}")
message(STATUS "apply-patch-once: 4 states checked")
