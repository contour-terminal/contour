# Scope check for the contour_e2e_wix_scope test: asserts that CPack's WiX scope table, once
# cmake/CPackWixScope.cmake has applied the selected row, names the right upgrade code, package file, install
# folder and extra source, and that a scope the table does not know, or a CMake too old for the installers,
# stops the packaging with a message that says why.
#
# Run by ctest as
#   cmake -D CPACK_CONFIG=<CPackConfig.cmake> -D SCOPE_SCRIPT=<cmake/CPackWixScope.cmake> -P WixScope.cmake
#
# It reads the generated CPack configuration rather than running `cpack`, so it needs no WiX and no package.
# What the script refuses is checked in a nested run, by the message it prints; the verdict of this one is the
# marker printed last, which the test's PASS_REGULAR_EXPRESSION requires.

foreach(_file IN ITEMS CPACK_CONFIG SCOPE_SCRIPT)
    if(NOT EXISTS "${${_file}}")
        message(FATAL_ERROR "${_file} does not name an existing file: '${${_file}}'")
    endif()
endforeach()

# Applies the project config the way CPack does: the generated configuration first, then the script, with the
# generator and the version cpack would have set. APPLY_GENERATOR is the generator, WIX where not given.
macro(apply_scope)
    include("${CPACK_CONFIG}")
    set(CPACK_GENERATOR WIX)
    if(APPLY_GENERATOR)
        set(CPACK_GENERATOR "${APPLY_GENERATOR}")
    endif()
    set(CPACK_PACKAGE_VERSION 1.2.3.4)
    include("${SCOPE_SCRIPT}")
endmacro()

function(expect_equal what actual expected)
    if(NOT "${actual}" STREQUAL "${expected}")
        message(FATAL_ERROR "${what}: expected '${expected}', got '${actual}'")
    endif()
endfunction()

# The nested run: the same script, asked to apply one scope (or none) for one generator, and nothing else.
if(DEFINED APPLY_ONLY)
    apply_scope()
    return()
endif()

# Runs the script for @scope and @generator, and expects it to refuse with a message holding every word after.
function(expect_refusal what scope generator)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -D "CPACK_CONFIG=${CPACK_CONFIG}" -D "SCOPE_SCRIPT=${SCOPE_SCRIPT}"
                -D APPLY_ONLY=ON -D "CPACK_CONTOUR_WIX_SCOPE=${scope}" -D "APPLY_GENERATOR=${generator}"
                -P "${CMAKE_CURRENT_LIST_FILE}"
        RESULT_VARIABLE _result
        ERROR_VARIABLE _error
        OUTPUT_VARIABLE _output)
    if(NOT "${_error}${_output}" MATCHES "CMake Error")
        message(FATAL_ERROR "${what}: accepted, but should have been refused")
    endif()
    foreach(_word IN LISTS ARGN)
        if(NOT "${_error}${_output}" MATCHES "${_word}")
            message(FATAL_ERROR "${what}: the message does not say '${_word}': ${_error}${_output}")
        endif()
    endforeach()
endfunction()

# CPack must be told to run the script, or none of the below is reached in a real package.
include("${CPACK_CONFIG}")
expect_equal("CPACK_PROJECT_CONFIG_FILE" "${CPACK_PROJECT_CONFIG_FILE}" "${SCOPE_SCRIPT}")

# Every Windows package is named by the script, the zip too, and the zip has no scope.
set(APPLY_GENERATOR ZIP)
apply_scope()
expect_equal("zip package name" "${CPACK_PACKAGE_FILE_NAME}" "contour-1.2.3.4-win64")
unset(APPLY_GENERATOR)

if(CMAKE_VERSION VERSION_LESS 3.30)
    # The installers' WiX 4 sources need CMake 3.30; the zip does not.
    expect_refusal("an installer on CMake ${CMAKE_VERSION}" "" WIX "3\\.30" "${CMAKE_VERSION}")
    message(STATUS "wix-scope: OK")
    return()
endif()

# No scope given: the per-machine installer.
unset(CPACK_CONTOUR_WIX_SCOPE)
apply_scope()
expect_equal("default scope" "${CPACK_WIX_INSTALL_SCOPE}" "perMachine")
expect_equal("per-machine upgrade code" "${CPACK_WIX_UPGRADE_GUID}" "0E736497-2B72-4117-95E9-54EC6D000000")
expect_equal("per-machine package name" "${CPACK_PACKAGE_FILE_NAME}" "contour-1.2.3.4-win64")
expect_equal("per-machine install folder" "${CPACK_PACKAGE_INSTALL_DIRECTORY}" "Contour")
if(NOT CPACK_WIX_EXTRA_SOURCES MATCHES "wix_machine\\.wxs$")
    message(FATAL_ERROR "per-machine extra source: expected wix_machine.wxs, got '${CPACK_WIX_EXTRA_SOURCES}'")
endif()

if(CMAKE_VERSION VERSION_LESS 4.3)
    # CPack takes perUser only from 4.3; below, it has to say so rather than build a per-machine package.
    expect_refusal("the per-user installer on CMake ${CMAKE_VERSION}" perUser WIX "4\\.3" "${CMAKE_VERSION}")
else()
    set(CPACK_CONTOUR_WIX_SCOPE perUser)
    apply_scope()
    expect_equal("per-user scope" "${CPACK_WIX_INSTALL_SCOPE}" "perUser")
    expect_equal("per-user upgrade code" "${CPACK_WIX_UPGRADE_GUID}" "AFF353C7-C33A-474C-B9C7-EDD160677BC9")
    expect_equal("per-user package name" "${CPACK_PACKAGE_FILE_NAME}" "contour-1.2.3.4-win64-user")
    expect_equal("per-user install folder" "${CPACK_PACKAGE_INSTALL_DIRECTORY}" "Programs/Contour")
    if(NOT CPACK_WIX_EXTRA_SOURCES MATCHES "wix_user\\.wxs$")
        message(FATAL_ERROR "per-user extra source: expected wix_user.wxs, got '${CPACK_WIX_EXTRA_SOURCES}'")
    endif()
endif()

# A misspelt scope must not fall back to one of the known ones.
expect_refusal("scope 'peruser'" peruser WIX perMachine perUser)

message(STATUS "wix-scope: OK")
