# SPDX-License-Identifier: Apache-2.0
#
# The project takes its dependencies from the tree it is built in. Inside Contour every target named here
# already exists -- Catch2 from Contour's third-party setup, core::* from its vendored core-cpp -- and nothing
# is fetched; a distribution build must never reach the network. Only a standalone checkout (this directory
# as the top-level project) fetches what is missing, pinned to the versions Contour carries:
# Catch2 as in cmake/ContourThirdParties.cmake, core-cpp as in vendor/core-cpp/MANIFEST.
#
# Only the test target has dependencies so far; each further one lands with the first code that uses it.

include(FetchContent)

set(ORCHESTRATOR_CATCH2_VERSION "3.4.0")
set(ORCHESTRATOR_CORE_CPP_COMMIT "c349e9436e5563b929ce58c6e978824a02eacd39") # core-cpp v0.20.0

# Fetches a missing dependency in a standalone build, and refuses to inside a host tree that lacks it.
macro(orchestrator_require target name)
    if(NOT TARGET ${target})
        if(NOT PROJECT_IS_TOP_LEVEL)
            message(FATAL_ERROR "orchestrator: the host tree defines no ${target}; it is not fetched here.")
        endif()
        message(STATUS "orchestrator: fetching ${name}")
        FetchContent_Declare(${name} ${ARGN})
        FetchContent_MakeAvailable(${name})
    endif()
endmacro()

if(ORCHESTRATOR_TESTING)
    orchestrator_require(Catch2::Catch2 Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v${ORCHESTRATOR_CATCH2_VERSION}
    )
    if(NOT TARGET core::testing)
        # Just the modules this project links: no TLS, no TUI, no systemd, none of core-cpp's own tests.
        set(CORE_CPP_TESTING OFF)
        set(CORE_CPP_BUILD_EXAMPLES OFF)
        set(CORE_CPP_INSTALL OFF)
        set(CORE_CPP_WITH_TUI OFF)
        set(CORE_CPP_WITH_TLS OFF)
        set(CORE_CPP_WITH_SYSTEMD OFF)
    endif()
    orchestrator_require(core::testing core-cpp
        GIT_REPOSITORY https://github.com/contour-terminal/core-cpp.git
        GIT_TAG ${ORCHESTRATOR_CORE_CPP_COMMIT}
    )
endif()
