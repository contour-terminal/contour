# LeakSanitizer's suppressions, compiled into every executable of an AddressSanitizer build.
#
# cmake/lsan-suppressions.txt is the one list. At build time it becomes the string a C function
# __lsan_default_suppressions() returns -- the hook the sanitizer runtime asks for its built-in suppressions
# -- in an object library whose object every executable is built with. So a plain run of an ASan `contour`
# or test applies it, with no LSAN_OPTIONS; and the same file still works as LSAN_OPTIONS=suppressions=<file>.
#
# C, not C++: the hook's name is the runtime's, and is no name of ours to lint.

# Script mode: writes OUTPUT, the C source of the hook, from INPUT, the suppressions file.
if(CMAKE_SCRIPT_MODE_FILE)
    file(STRINGS "${INPUT}" _lines)
    set(_body "")
    foreach(_line IN LISTS _lines)
        string(STRIP "${_line}" _line)
        if(_line STREQUAL "" OR _line MATCHES "^#")
            continue()
        endif()
        string(REPLACE "\\" "\\\\" _line "${_line}")
        string(REPLACE "\"" "\\\"" _line "${_line}")
        string(APPEND _body "           \"${_line}\\n\"\n")
    endforeach()
    file(WRITE "${OUTPUT}.tmp"
        "/* Generated from cmake/lsan-suppressions.txt by cmake/LeakSanitizerSuppressions.cmake. */\n"
        "\n"
        "const char* __lsan_default_suppressions(void);\n"
        "\n"
        "/* The suppressions LeakSanitizer applies before any it is given in LSAN_OPTIONS. */\n"
        "const char* __lsan_default_suppressions(void)\n"
        "{\n"
        "    return \"\"\n"
        "${_body}"
        "        ;\n"
        "}\n")
    # Rewritten only when it changes, so an edit of a comment rebuilds nothing.
    file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
    file(REMOVE "${OUTPUT}.tmp")
    return()
endif()

set(CONTOUR_LSAN_SUPPRESSIONS_FILE "${CMAKE_CURRENT_LIST_DIR}/lsan-suppressions.txt")

# Builds the suppressions into every executable target defined in @p directory and below.
function(_contour_link_lsan_suppressions directory)
    get_property(_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_target IN LISTS _targets)
        get_target_property(_type ${_target} TYPE)
        if(_type STREQUAL "EXECUTABLE")
            # Its object as a source, not a link: the targets mix target_link_libraries() signatures.
            target_sources(${_target} PRIVATE $<TARGET_OBJECTS:contour_lsan_suppressions>)
        endif()
    endforeach()
    get_property(_subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(_subdirectory IN LISTS _subdirectories)
        _contour_link_lsan_suppressions("${_subdirectory}")
    endforeach()
endfunction()

# Builds the hook and links it into every executable of this project. A no-op unless CONTOUR_SANITIZE is
# "address": LeakSanitizer comes with AddressSanitizer here, and a build without it has no runtime to ask.
# Call it once, after every add_subdirectory() that defines an executable.
function(contour_add_lsan_suppressions)
    if(NOT CONTOUR_SANITIZE STREQUAL "address")
        return()
    endif()
    set(_source "${CMAKE_BINARY_DIR}/generated/LeakSanitizerSuppressions.c")
    add_custom_command(
        OUTPUT "${_source}"
        COMMAND ${CMAKE_COMMAND} -DINPUT=${CONTOUR_LSAN_SUPPRESSIONS_FILE} -DOUTPUT=${_source}
                -P "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
        DEPENDS "${CONTOUR_LSAN_SUPPRESSIONS_FILE}" "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
        COMMENT "Generating LeakSanitizer's default suppressions"
        VERBATIM)
    add_library(contour_lsan_suppressions OBJECT "${_source}")
    _contour_link_lsan_suppressions("${CMAKE_SOURCE_DIR}")
endfunction()
