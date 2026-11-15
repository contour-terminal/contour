# SPDX-License-Identifier: Apache-2.0
#
# The project takes its dependencies from the tree it is built in. Inside Contour every target named here
# already exists -- Catch2 and glaze from Contour's third-party setup, core::* from its vendored core-cpp --
# and nothing is fetched; a distribution build must never reach the network. Only a standalone checkout (this
# directory as the top-level project) fetches what is missing, pinned to the versions Contour carries:
# Catch2 and glaze as in cmake/ContourThirdParties.cmake, core-cpp as in vendor/core-cpp/MANIFEST.
#
# Each dependency lands with the first code that uses it.

include(FetchContent)

set(ORCHESTRATOR_CATCH2_VERSION "3.4.0")
set(ORCHESTRATOR_GLAZE_VERSION "9.0.0")
set(ORCHESTRATOR_GLAZE_SHA256 "dd7b033c5bf6e4308615bb7834c541291d5b20f64121d9a68222a39414635517")
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

# glaze, the core's JSON reader. It is header-only, and its own CMakeLists is never run (glaze 9 requires
# CMake 3.31, newer than this project does): SOURCE_SUBDIR names a directory without one, so MakeAvailable only
# populates the sources, and the target is defined here -- as Contour defines it, which this mirrors.
orchestrator_require(glaze::glaze glaze
    URL "https://github.com/stephenberry/glaze/archive/refs/tags/v${ORCHESTRATOR_GLAZE_VERSION}.tar.gz"
    URL_HASH SHA256=${ORCHESTRATOR_GLAZE_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SOURCE_SUBDIR "orchestrator-header-only"
)
if(NOT TARGET glaze::glaze)
    add_library(glaze_glaze INTERFACE)
    add_library(glaze::glaze ALIAS glaze_glaze)
    target_compile_features(glaze_glaze INTERFACE cxx_std_23)
    target_include_directories(glaze_glaze SYSTEM INTERFACE "$<BUILD_INTERFACE:${glaze_SOURCE_DIR}/include>")
    if(MSVC)
        target_compile_options(glaze_glaze INTERFACE /Zc:preprocessor /permissive- /Zc:lambda)
    else()
        target_compile_options(glaze_glaze INTERFACE -Wno-missing-braces)
    endif()
endif()

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
