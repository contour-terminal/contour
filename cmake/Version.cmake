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
        execute_process(COMMAND git describe --all
            OUTPUT_VARIABLE git_branch
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        string(REGEX MATCH "^(.*)\/(.*)$$" _ ${git_branch})
        set(THE_GIT_BRANCH "${CMAKE_MATCH_2}")
        message(STATUS "[Version] Git branch: ${THE_GIT_BRANCH}")

        execute_process(COMMAND git rev-parse --short HEAD
            OUTPUT_VARIABLE git_sha_short
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        set(THE_GIT_SHA_SHORT "${git_sha_short}")
        message(STATUS "[Version] Git SHA: ${THE_GIT_SHA_SHORT}")

        file(READ "${CMAKE_SOURCE_DIR}/Changelog.md" changelog_contents)
        string(REGEX MATCH "^^### ([0-9]*\.[0-9]+\.[0-9]+).*$" _ "${changelog_contents}")
        # extract and construct version triple
        set(THE_VERSION ${CMAKE_MATCH_1})
        # extract suffix, construct full version string
        string(REGEX MATCH "^^### ([0-9]*\.[0-9]+\.[0-9]+) \\(([^\)]*)\\).*$" _ "${changelog_contents}")

        if(NOT ("${CMAKE_MATCH_2}" STREQUAL ""))
            set(THE_VERSION_STRING "${THE_VERSION}-${CMAKE_MATCH_2}-${THE_GIT_BRANCH}-${THE_GIT_SHA_SHORT}")
        else()
            set(THE_VERSION_STRING "${THE_VERSION}-${THE_GIT_BRANCH}-${THE_GIT_SHA_SHORT}")
        endif()
        set(THE_SOURCE "git & ${CMAKE_SOURCE_DIR}/Changelog.md")
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

