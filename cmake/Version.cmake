# This file is part of the "libterminal" project
#   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# CMake function to extract version triple and full version string from the source code repository.
#
# The following locations are checked in order:
# 1.) /version.txt file
# 2.) /.git directory with the output of `git describe ...`)
# 3.) /Changelog.md with the first line's version number and optional (suffix) string
#
function(GetVersionInformation VersionTripleVar VersionStringVar)
    if(EXISTS "${CMAKE_SOURCE_DIR}/version.txt")
        file(READ "${CMAKE_SOURCE_DIR}/version.txt" version_text)
        string(STRIP "${version_text}" version_text)
        string(REGEX MATCH "^v?([0-9]*\\.[0-9]+\\.[0-9]+).*$" _ ${version_text})
        set(THE_VERSION ${CMAKE_MATCH_1})
        set(THE_VERSION_STRING "${version_text}")
        set(THE_SOURCE "${CMAKE_SOURCE_DIR}/version.txt")
    endif()

    if(("${THE_VERSION}" STREQUAL "" OR "${THE_VERSION_STRING}" STREQUAL "") AND (EXISTS "${CMAKE_SOURCE_DIR}/.git"))
        execute_process(COMMAND git describe --always --dirty=-modified --tags
            OUTPUT_VARIABLE version_text
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        string(REGEX MATCH "^v?([0-9]*\\.[0-9]+\\.[0-9]+).*$" _ ${version_text})
        if(NOT("${CMAKE_MATCH_1}" STREQUAL ""))
            set(THE_VERSION ${CMAKE_MATCH_1})
            set(THE_VERSION_STRING "${version_text}")
            set(THE_SOURCE "git")
        endif()
    endif()

    if(("${THE_VERSION}" STREQUAL "" OR "${THE_VERSION_STRING}" STREQUAL "") AND (EXISTS "${CMAKE_SOURCE_DIR}/Changelog.md"))
        file(READ "${CMAKE_SOURCE_DIR}/Changelog.md" changelog_contents)
        # extract and construct version triple
        string(REGEX MATCH "^^### ([0-9]*\.[0-9]+\.[0-9]+).*$" _ "${changelog_contents}")
        set(THE_VERSION ${CMAKE_MATCH_1})
        # extract suffix, construct full version string
        string(REGEX MATCH "^^### ([0-9]*\.[0-9]+\.[0-9]+) \\(([^\)]*)\\).*$" _ "${changelog_contents}")
        if(NOT ("${CMAKE_MATCH_2}" STREQUAL ""))
            set(THE_VERSION_STRING "${THE_VERSION}-${CMAKE_MATCH_2}")
        else()
            set(THE_VERSION_STRING "${THE_VERSION}")
        endif()
        set(THE_SOURCE "${CMAKE_SOURCE_DIR}/Changelog.md")
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

