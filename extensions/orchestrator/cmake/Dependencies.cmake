# SPDX-License-Identifier: Apache-2.0
#
# The project takes its dependencies from the tree it is built in. Inside Contour every target named here
# already exists -- Catch2 and glaze from Contour's third-party setup, core::* from its vendored core-cpp --
# and nothing is fetched; a distribution build must never reach the network. Only a standalone checkout (this
# directory as the top-level project) fetches what is missing, pinned to the versions Contour carries:
# Catch2, glaze and libunicode as in cmake/ContourThirdParties.cmake, core-cpp as in vendor/core-cpp/MANIFEST.
# A WebAssembly guest's sub-build is such a top-level project too, and is handed the sources Contour carries
# (cmake/ContourWasiCxx.cmake) through FETCHCONTENT_SOURCE_DIR_<NAME>, so it fetches nothing either.
#
# Each dependency lands with the first code that uses it.

include(FetchContent)

set(ORCHESTRATOR_CATCH2_VERSION "3.4.0")
set(ORCHESTRATOR_GLAZE_VERSION "9.0.0")
set(ORCHESTRATOR_GLAZE_SHA256 "dd7b033c5bf6e4308615bb7834c541291d5b20f64121d9a68222a39414635517")
set(ORCHESTRATOR_CORE_CPP_COMMIT "190a83f295bb12ed24ae6a3874276ff291f2b065") # core-cpp v0.21.0
set(ORCHESTRATOR_LIBUNICODE_VERSION "0.9.5")

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

# Defines glaze::glaze, header-only, over the include directories given -- as Contour defines it, which this
# mirrors.
function(orchestrator_define_glaze)
    add_library(glaze_glaze INTERFACE)
    add_library(glaze::glaze ALIAS glaze_glaze)
    target_compile_features(glaze_glaze INTERFACE cxx_std_23)
    target_include_directories(glaze_glaze SYSTEM INTERFACE ${ARGN})
    if(MSVC)
        target_compile_options(glaze_glaze INTERFACE /Zc:preprocessor /permissive- /Zc:lambda)
    else()
        target_compile_options(glaze_glaze INTERFACE -Wno-missing-braces)
    endif()
endfunction()

# glaze, the core's JSON reader. A system package's include directories, which Contour hands a WebAssembly
# guest's sub-build as ORCHESTRATOR_GLAZE_INCLUDE_DIR (cmake/ContourWasiCxx.cmake), are taken as they are.
# Otherwise its sources are fetched, or taken from FETCHCONTENT_SOURCE_DIR_GLAZE; its own CMakeLists is never run
# (glaze 9 requires CMake 3.31, newer than this project does): SOURCE_SUBDIR names a directory without one, so
# MakeAvailable only populates the sources.
if(NOT TARGET glaze::glaze AND ORCHESTRATOR_GLAZE_INCLUDE_DIR)
    orchestrator_define_glaze(${ORCHESTRATOR_GLAZE_INCLUDE_DIR})
endif()
orchestrator_require(glaze::glaze glaze
    URL "https://github.com/stephenberry/glaze/archive/refs/tags/v${ORCHESTRATOR_GLAZE_VERSION}.tar.gz"
    URL_HASH SHA256=${ORCHESTRATOR_GLAZE_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SOURCE_SUBDIR "orchestrator-header-only"
)
if(NOT TARGET glaze::glaze)
    orchestrator_define_glaze("$<BUILD_INTERFACE:${glaze_SOURCE_DIR}/include>")
endif()

if(ORCHESTRATOR_TESTING)
    orchestrator_require(Catch2::Catch2 Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v${ORCHESTRATOR_CATCH2_VERSION}
    )
endif()

# core-cpp: the headless terminal UI the dashboard is drawn with (core::tui_core), and core::testing for the
# tests. Both come from one core-cpp; it is fetched once when either is missing.
set(_orchestrator_core_cpp_targets core::tui_core)
if(ORCHESTRATOR_TESTING)
    list(APPEND _orchestrator_core_cpp_targets core::testing)
endif()
set(_orchestrator_core_cpp_missing "")
foreach(_target IN LISTS _orchestrator_core_cpp_targets)
    if(NOT TARGET ${_target} AND NOT _orchestrator_core_cpp_missing)
        set(_orchestrator_core_cpp_missing ${_target})
    endif()
endforeach()

if(_orchestrator_core_cpp_missing AND PROJECT_IS_TOP_LEVEL)
    # libunicode, which core::tui_core links, comes first, so that core-cpp takes it from here and fetches
    # nothing itself. Just the library: no tests, tools or examples. A WebAssembly guest's sub-build is handed
    # its source tree (FETCHCONTENT_SOURCE_DIR_LIBUNICODE) with the tables a native build generated, since the
    # generator cannot run there, and builds it without SIMD, which wasi-sdk's libc++ lacks.
    set(LIBUNICODE_TESTING OFF)
    set(LIBUNICODE_BENCHMARK OFF)
    set(LIBUNICODE_TOOLS OFF)
    set(LIBUNICODE_EXAMPLES OFF)
    set(LIBUNICODE_BUILD_STATIC ON)
    if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
        set(LIBUNICODE_SIMD_IMPLEMENTATION "none" CACHE STRING "libunicode: SIMD implementation to use" FORCE)
    endif()
    orchestrator_require(unicode::unicode libunicode
        GIT_REPOSITORY https://github.com/contour-terminal/libunicode.git
        GIT_TAG v${ORCHESTRATOR_LIBUNICODE_VERSION}
        SYSTEM
    )
    # Without SIMD, libunicode's scan.cpp leaves a template unused; the fix belongs in libunicode, and core-cpp
    # silences it on a libunicode it builds itself the same way, on that target alone.
    if(CMAKE_SYSTEM_NAME STREQUAL "WASI" AND TARGET unicode)
        target_compile_options(unicode PRIVATE -Wno-unused-template)
    endif()

    # Just the modules this project links: the headless TUI and not the terminal half, no TLS, no systemd,
    # none of core-cpp's own tests, and nothing fetched behind this project's back.
    set(CORE_CPP_TESTING OFF)
    set(CORE_CPP_CATCH2_MAIN OFF)
    set(CORE_CPP_BUILD_EXAMPLES OFF)
    set(CORE_CPP_INSTALL OFF)
    set(CORE_CPP_FETCH_DEPS OFF)
    set(CORE_CPP_WITH_TUI OFF)
    set(CORE_CPP_WITH_TUI_CORE ON)
    set(CORE_CPP_WITH_TUI_OUTPUT ON)
    set(CORE_CPP_WITH_TLS OFF)
    set(CORE_CPP_WITH_SYSTEMD OFF)
endif()
if(_orchestrator_core_cpp_missing)
    orchestrator_require(${_orchestrator_core_cpp_missing} core-cpp
        GIT_REPOSITORY https://github.com/contour-terminal/core-cpp.git
        GIT_TAG ${ORCHESTRATOR_CORE_CPP_COMMIT}
        SYSTEM
    )
endif()
