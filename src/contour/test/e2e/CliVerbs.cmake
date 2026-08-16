# Verb-surface check for the contour_e2e_cli_verbs test: asserts that a build configuration exposes
# exactly the command-line verbs it is supposed to.
#
# Run by ctest as `cmake -D CONTOUR_BINARY=... -D FRONTEND=... -P CliVerbs.cmake`. A CMake script
# rather than a shell script because this test must run on Windows too, where the daemon's transport
# gets the least coverage and `sh` is not available; CMake is by definition present in every build.
#
# NB: `cmake -P` exits 0 even after message(FATAL_ERROR) (CMake < 3.31), so the exit code cannot be
# the verdict -- a failing assertion would silently pass. The last line below prints a success
# marker instead, which the test's PASS_REGULAR_EXPRESSION requires; anything that stops this script
# early, FATAL_ERROR included, withholds the marker and fails the test.
#
# The point is the headless build. CONTOUR_FRONTEND_GUI=OFF drops everything that opens a window,
# and what must survive is `contour daemon` -- the reason a GUI-less build is worth having. A GUI
# source landing outside its CMake guard, or the daemon verb drifting from ContourApp into
# ContourGuiApp, both show up here as a verb on the wrong side of that line.

if(NOT EXISTS "${CONTOUR_BINARY}")
    message(FATAL_ERROR "CONTOUR_BINARY does not name an existing file: '${CONTOUR_BINARY}'")
endif()
if(NOT FRONTEND STREQUAL "gui" AND NOT FRONTEND STREQUAL "headless")
    message(FATAL_ERROR "FRONTEND must be 'gui' or 'headless', got '${FRONTEND}'")
endif()

# Verbs that must be present in EVERY configuration: the crispy::App base plus everything
# ContourApp registers. `daemon` is the one that makes this test worth running; `documentation` is
# the one the Docs workflow runs, and it runs it against a CONTOUR_FRONTEND_GUI=OFF build -- so a
# guard slipping around that verb would not fail a build, it would silently stop the website from
# updating.
set(ALWAYS_PRESENT version license help capture cat daemon list-debug-tags documentation)
# Verbs ContourGuiApp registers, which exist only when the GUI frontend is built.
set(GUI_ONLY client font-locator)

execute_process(
    COMMAND "${CONTOUR_BINARY}" help
    OUTPUT_VARIABLE helpText
    ERROR_VARIABLE helpError
    RESULT_VARIABLE helpResult
    TIMEOUT 30)
if(NOT helpResult EQUAL 0)
    message(FATAL_ERROR "'contour help' failed (${helpResult}): ${helpError}")
endif()
if(helpText STREQUAL "")
    message(FATAL_ERROR "'contour help' produced no output")
endif()

# The verb table is what `contour help` renders, one indented "contour <verb>" line per command
# (crispy::CLI::printCommand). Anchored to the start of such a line rather than grepped loosely, so
# a verb merely NAMED in some other verb's help text cannot pass for a verb that exists.
function(contour_has_verb verb outVar)
    string(REGEX MATCH "(^|\n)[ \t]+contour ${verb}([ \t]|\r|\n|$)" matched "${helpText}")
    if(matched STREQUAL "")
        set(${outVar} FALSE PARENT_SCOPE)
    else()
        set(${outVar} TRUE PARENT_SCOPE)
    endif()
endfunction()

set(failures "")
foreach(verb IN LISTS ALWAYS_PRESENT)
    contour_has_verb("${verb}" present)
    if(NOT present)
        list(APPEND failures "verb '${verb}' is missing from a ${FRONTEND} build")
    endif()
endforeach()
foreach(verb IN LISTS GUI_ONLY)
    contour_has_verb("${verb}" present)
    if(FRONTEND STREQUAL "gui" AND NOT present)
        list(APPEND failures "verb '${verb}' is missing from a gui build")
    elseif(FRONTEND STREQUAL "headless" AND present)
        list(APPEND failures "verb '${verb}' is present in a headless build")
    endif()
endforeach()
if(NOT failures STREQUAL "")
    string(REPLACE ";" "\n  - " report "${failures}")
    message(FATAL_ERROR "unexpected CLI verb surface:\n  - ${report}\n--- contour help ---\n${helpText}")
endif()

# Absent from the help table is not the same as unreachable. An unknown verb fails to parse
# (crispy::app::run returns EXIT_FAILURE), so the exit status answers the stronger question: is
# `client` gone from the command tree, or merely undocumented in this build?
if(FRONTEND STREQUAL "headless")
    execute_process(
        COMMAND "${CONTOUR_BINARY}" client
        OUTPUT_QUIET ERROR_QUIET
        RESULT_VARIABLE clientResult
        TIMEOUT 30)
    if(clientResult EQUAL 0)
        message(FATAL_ERROR "'contour client' is still reachable in a headless build")
    endif()
endif()

message(STATUS "cli-verbs: OK (${FRONTEND})")
