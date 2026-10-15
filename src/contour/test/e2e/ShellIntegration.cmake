# End-to-end check for the contour_e2e_shell_integration test: asserts that
# `contour generate integration` emits, for every supported shell, exactly the script that lives in
# src/contour/cli/shell-integration/.
#
# Run by ctest as
#   cmake -D CONTOUR_BINARY=... -D SHELLS=bash,fish,... -D SOURCE_DIR=... -D WORK_DIR=... -P ShellIntegration.cmake
# WORK_DIR is passed explicitly rather than derived from the working directory, so running this by
# hand cannot scatter output into whatever directory it was started from.
# A CMake script for the same reasons CliVerbs.cmake is one: it has to run on Windows, where `sh` is
# not available, and CMake is by definition present in every build.
#
# NB: `cmake -P` exits 0 even after message(FATAL_ERROR) (CMake < 3.31), so the exit code cannot be
# the verdict. The last line prints a success marker instead, which the test's
# PASS_REGULAR_EXPRESSION requires; anything that stops this script early withholds it.
#
# The point is the GUI-less build. These scripts used to reach the binary through Qt's resource
# system, which made a CLI verb depend on Qt; they are embedded as C++ data now. This test is
# registered for BOTH configurations, because the embedding is what both of them use -- and a
# generator that dropped a byte, mangled a line ending or silently emitted nothing would otherwise
# be found by a user whose shell broke rather than by CI.

if(NOT EXISTS "${CONTOUR_BINARY}")
    message(FATAL_ERROR "CONTOUR_BINARY does not name an existing file: '${CONTOUR_BINARY}'")
endif()
if(NOT SHELLS)
    message(FATAL_ERROR "SHELLS must name at least one shell")
endif()
# Comma-separated on the command line, where a ';' would have split it into arguments of its own.
string(REPLACE "," ";" SHELLS "${SHELLS}")
if(NOT IS_DIRECTORY "${SOURCE_DIR}/shell-integration")
    message(FATAL_ERROR "SOURCE_DIR does not hold a shell-integration directory: '${SOURCE_DIR}'")
endif()
if(NOT WORK_DIR)
    message(FATAL_ERROR "WORK_DIR must name a directory to write the generated scripts to")
endif()

set(outputDir "${WORK_DIR}/e2e-shell-integration")
file(REMOVE_RECURSE "${outputDir}")
file(MAKE_DIRECTORY "${outputDir}")

set(failures "")

foreach(shell IN LISTS SHELLS)
    set(generated "${outputDir}/integration.${shell}")
    execute_process(
        COMMAND "${CONTOUR_BINARY}" generate integration shell "${shell}" to "${generated}"
        OUTPUT_VARIABLE generateOutput
        ERROR_VARIABLE generateError
        RESULT_VARIABLE generateResult
        TIMEOUT 30)
    if(NOT generateResult EQUAL 0)
        list(APPEND failures "'generate integration shell ${shell}' failed (${generateResult}): ${generateError}")
        continue()
    endif()
    if(NOT EXISTS "${generated}")
        list(APPEND failures "'generate integration shell ${shell}' wrote no file")
        continue()
    endif()

    # Compared byte for byte, with no CR on the generated side: a script with CRLF in it breaks in the
    # shell that sources it, on Windows too (Git Bash). Only the source file's CRLF is folded to LF, as
    # the embedding folds it: its line endings depend on the checkout's git settings. Folded on the byte
    # list, where a ';' separates every byte, so that no "0d0a" across two bytes can match.
    file(READ "${generated}" generatedText HEX)
    file(READ "${SOURCE_DIR}/shell-integration/shell-integration.${shell}" expectedText HEX)
    string(REGEX MATCHALL "[0-9a-f][0-9a-f]" generatedBytes "${generatedText}")
    string(REGEX MATCHALL "[0-9a-f][0-9a-f]" expectedBytes "${expectedText}")
    string(REPLACE "0d;0a" "0a" expectedBytes "${expectedBytes}")

    if(NOT generatedBytes)
        list(APPEND failures "'generate integration shell ${shell}' produced an empty script")
    elseif(NOT generatedBytes STREQUAL expectedBytes)
        list(LENGTH generatedBytes generatedLength)
        list(LENGTH expectedBytes expectedLength)
        list(APPEND failures
             "'generate integration shell ${shell}' does not match shell-integration.${shell} \
(${generatedLength} bytes vs ${expectedLength})")
    endif()
endforeach()

# A file named beyond ASCII is written where it is named. Windows hands a program its command line in
# UTF-16, and the narrow argv main() is given spells it in the ANSI code page, which spells most of
# these characters not at all: ü, €, 李 and 😀 are two, three and four UTF-8 bytes long.
list(GET SHELLS 0 firstShell)
set(awkwardDir "${outputDir}/ü€李😀")
file(MAKE_DIRECTORY "${awkwardDir}")
set(awkwardFile "${awkwardDir}/integration.${firstShell}")
execute_process(
    COMMAND "${CONTOUR_BINARY}" generate integration shell "${firstShell}" to "${awkwardFile}"
    OUTPUT_QUIET
    ERROR_VARIABLE awkwardError
    RESULT_VARIABLE awkwardResult
    TIMEOUT 30)
if(NOT awkwardResult EQUAL 0)
    list(APPEND failures "'generate integration' into a directory beyond ASCII failed (${awkwardResult}): ${awkwardError}")
elseif(NOT EXISTS "${awkwardFile}")
    list(APPEND failures "'generate integration' into a directory beyond ASCII wrote no file there")
endif()

# An unknown shell must fail loudly rather than write an empty file that a user would then source.
execute_process(
    COMMAND "${CONTOUR_BINARY}" generate integration shell nosuchshell to -
    OUTPUT_QUIET ERROR_QUIET
    RESULT_VARIABLE unknownResult
    TIMEOUT 30)
if(unknownResult EQUAL 0)
    list(APPEND failures "'generate integration shell nosuchshell' unexpectedly succeeded")
endif()

# A shell that integrates natively gets no file at all, not an empty one.
set(nativeFile "${outputDir}/integration.nu")
execute_process(
    COMMAND "${CONTOUR_BINARY}" generate integration shell nu to "${nativeFile}"
    OUTPUT_QUIET ERROR_QUIET
    RESULT_VARIABLE nativeResult
    TIMEOUT 30)
if(nativeResult EQUAL 0)
    list(APPEND failures "'generate integration shell nu' unexpectedly succeeded")
endif()
if(EXISTS "${nativeFile}")
    list(APPEND failures "'generate integration shell nu' wrote a file")
endif()

# A file that cannot be written is an error, not a silent success.
execute_process(
    COMMAND "${CONTOUR_BINARY}" generate integration shell "${firstShell}" to "${outputDir}/no-such-directory/f"
    OUTPUT_QUIET ERROR_QUIET
    RESULT_VARIABLE unwritableResult
    TIMEOUT 30)
if(unwritableResult EQUAL 0)
    list(APPEND failures "'generate integration' into a directory that does not exist unexpectedly succeeded")
endif()

if(NOT failures STREQUAL "")
    string(REPLACE ";" "\n  - " report "${failures}")
    message(FATAL_ERROR "shell integration mismatches:\n  - ${report}")
endif()

list(LENGTH SHELLS shellCount)
message(STATUS "shell-integration: OK (${shellCount} shells)")
