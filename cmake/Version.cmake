#SPDX-License-Identifier: Apache-2.0
# CMake function to extract version triple and full version string from the source code repository.
#
# The following locations are checked in order:
# 1.) /version.txt file
# 2.) /.git directory with the output of `git describe ...`)
# 3.) /metainfo.xml with the first line's version number and optional (suffix) string
#
function(GetVersionInformation VersionTripleVar VersionStringVar)
    # 1.) /version.txt file
    if(EXISTS "${CMAKE_SOURCE_DIR}/version.txt")
        file(READ "${CMAKE_SOURCE_DIR}/version.txt" version_text)
        string(STRIP "${version_text}" version_text)
        string(REGEX MATCH "^v?([0-9]*\\.[0-9]+\\.[0-9]+\\.[0-9]+).*$" _ ${version_text})
        set(THE_VERSION ${CMAKE_MATCH_1})
        set(THE_VERSION_STRING "${version_text}")
        set(THE_SOURCE "${CMAKE_SOURCE_DIR}/version.txt")
    endif()

    # 2.) .git directory with the output of `git describe ...`)
    if(("${THE_VERSION}" STREQUAL "" OR "${THE_VERSION_STRING}" STREQUAL "") AND (EXISTS "${CMAKE_SOURCE_DIR}/.git"))
        execute_process(COMMAND git describe --all
            OUTPUT_VARIABLE git_branch
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        string(REGEX MATCH "^(.*)\\/(.*)$$" _ "${git_branch}")
        set(THE_GIT_BRANCH "${CMAKE_MATCH_2}")
        message(STATUS "[Version] Git branch: ${THE_GIT_BRANCH}")

        execute_process(COMMAND git rev-parse --short HEAD
            OUTPUT_VARIABLE git_sha_short
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        set(THE_GIT_SHA_SHORT "${git_sha_short}")
        message(STATUS "[Version] Git SHA: ${THE_GIT_SHA_SHORT}")

        file(READ "${CMAKE_SOURCE_DIR}/metainfo.xml" changelog_contents)
        string(REGEX MATCH "<release version=\"([0-9]*\\.[0-9]+\\.[0-9]+)\".*$" _ "${changelog_contents}")
        # extract and construct version triple
        set(THE_VERSION ${CMAKE_MATCH_1})

        # maybe append CI run-ID.
        if (NOT("$ENV{RUN_ID}" STREQUAL ""))
            string(CONCAT THE_VERSION "${THE_VERSION}." $ENV{RUN_ID})
        endif()

        # extract suffix, construct full version string
        string(REGEX MATCH "<release version=\"([0-9]*\\.[0-9]+\\.[0-9]+)\".*$" _ "${changelog_contents}")

        set(THE_VERSION_STRING "${THE_VERSION}-${THE_GIT_BRANCH}-${THE_GIT_SHA_SHORT}")
        set(THE_SOURCE "git & ${CMAKE_SOURCE_DIR}/metainfo.xml")
    endif()

    # 3.) /metainfo.xml with the first line's version number and optional (suffix) string
    if(("${THE_VERSION}" STREQUAL "" OR "${THE_VERSION_STRING}" STREQUAL "") AND (EXISTS "${CMAKE_SOURCE_DIR}/metainfo.xml"))
        file(READ "${CMAKE_SOURCE_DIR}/metainfo.xml" changelog_contents)
        # extract and construct version triple
        string(REGEX MATCH "<release version=\"([0-9]*\\.[0-9]+\\.[0-9]+)\".*$" _ "${changelog_contents}")
        set(THE_VERSION ${CMAKE_MATCH_1})

        # maybe append CI run-ID.
        if (NOT("$ENV{RUN_ID}" STREQUAL ""))
            string(CONCAT THE_VERSION "${THE_VERSION}." $ENV{RUN_ID})
        endif()

        set(THE_VERSION_STRING "${THE_VERSION}")
        set(THE_SOURCE "${CMAKE_SOURCE_DIR}/metainfo.xml")
    endif()

    if("${THE_VERSION}" STREQUAL "" OR "${THE_VERSION_STRING}" STREQUAL "")
        message(FATAL_ERROR "Cannot extract version information.")
    endif()

    message(STATUS "[Version] version source: ${THE_SOURCE}")
    message(STATUS "[Version] version triple: ${THE_VERSION}")
    message(STATUS "[Version] version string: ${THE_VERSION_STRING}")

    # Write resulting version triple and version string to parent scope's variables.
    set(${VersionTripleVar} "${THE_VERSION}" PARENT_SCOPE)
    set(${VersionStringVar} "${THE_VERSION_STRING}" PARENT_SCOPE)
endfunction()

# Converts a version such as 1.2.255 to 0x0102ff
# Gratefully taken from https://github.com/Cisco-Talos/clamav/blob/17c9f5b64f4a9a3fd624b1c9668d034d898a2534/cmake/Version.cmake
function(HexVersion version_hex_var major minor patch)
    math(EXPR version_dec "${major} * 256 * 256 + ${minor} * 256 + ${patch}")
    set(version_hex "0x")
    foreach(i RANGE 5 0 -1)
        math(EXPR num "(${version_dec} >> (4 * ${i})) & 15")
        string(SUBSTRING "0123456789abcdef" ${num} 1 num_hex)
        set(version_hex "${version_hex}${num_hex}")
    endforeach()
    set(${version_hex_var} "${version_hex}" PARENT_SCOPE)
endfunction()

# Converts a number such as 104 to 68
# Gratefully taken from https://github.com/Cisco-Talos/clamav/blob/17c9f5b64f4a9a3fd624b1c9668d034d898a2534/cmake/Version.cmake
function(NumberToHex number output)
    set(hex "")
    foreach(i RANGE 1)
        math(EXPR nibble "${number} & 15")
        string(SUBSTRING "0123456789abcdef" "${nibble}" 1 nibble_hex)
        string(APPEND hex "${nibble_hex}")
        math(EXPR number "${number} >> 4")
    endforeach()
    string(REGEX REPLACE "(.)(.)" "\\2\\1" hex "${hex}")
    set("${output}" "${hex}" PARENT_SCOPE)
endfunction()
