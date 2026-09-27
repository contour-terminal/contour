# SPDX-License-Identifier: Apache-2.0
#
# core-cpp's options, as Part I §3 of the design spec lists them. Every one is
# CORE_CPP_-prefixed, and a parent project may preset any of them as a normal
# variable before adding core-cpp (CMP0077).

include(CMakeDependentOption)

option(CORE_CPP_TESTING "Build core-cpp's own tests" ${PROJECT_IS_TOP_LEVEL})
option(CORE_CPP_CATCH2_MAIN
       "Build core::testing_main, the Catch2 main() with core-cpp's exit-code contract (needs Catch2)"
       ${CORE_CPP_TESTING})
# A parent that exports targets of its own linking core-cpp's turns this on: CMake refuses an export
# whose targets link one in no export set (cmake/CoreCppInstall.cmake).
option(CORE_CPP_INSTALL "Install core-cpp's targets and export them as the CMake package core-cpp"
       ${PROJECT_IS_TOP_LEVEL})
option(CORE_CPP_BUILD_EXAMPLES "Build core-cpp's examples" ${PROJECT_IS_TOP_LEVEL})
option(CORE_CPP_FETCH_DEPS
       "Fetch a dependency with CPM when neither the parent project nor find_package() provides it"
       ON)
option(CORE_CPP_WITH_TUI "Build core::tui, and core::tui_output with it" ON)
# Its own option because it links core::base alone: a program that only writes styled output
# (Lightweight's dbtool) turns CORE_CPP_WITH_TUI off and this on, and gets neither libunicode nor the
# event loop. It defaults to CORE_CPP_WITH_TUI rather than to ON so that a consumer which already
# turns the TUI off -- contour, whose vendored copy leaves src/core/tui out -- keeps building
# nothing there. The default is read once, when the cache entry is made: turning CORE_CPP_WITH_TUI
# off in an existing tree leaves this on, which costs a leaf with no dependency and nothing else.
option(CORE_CPP_WITH_TUI_OUTPUT "Build core::tui_output, the styled-output leaf that links core::base alone"
       ${CORE_CPP_WITH_TUI})
cmake_dependent_option(CORE_CPP_WITH_IMAGES "Decode images in core::tui (stb_image)" ON
                       "CORE_CPP_WITH_TUI" OFF)
option(CORE_CPP_WITH_TLS "Build core::net_tls (OpenSSL)" OFF)
option(CORE_CPP_WITH_TRACY "Instrument core-cpp for the Tracy profiler" OFF)
option(CORE_CPP_PEDANTIC "Compile core-cpp's targets with the pedantic warning set" ${PROJECT_IS_TOP_LEVEL})
option(CORE_CPP_WERROR "Treat warnings in core-cpp's targets as errors" OFF)
option(CORE_CPP_CLANG_TIDY
       "Run clang-tidy on core-cpp's targets; OFF also clears a CXX_CLANG_TIDY they would inherit"
       OFF)
set(CORE_CPP_SANITIZERS "" CACHE STRING
    "Sanitizers for core-cpp's targets, a list of address, undefined, thread and leak (top-level builds only)")
option(CORE_CPP_COVERAGE "Instrument core-cpp's targets for coverage" OFF)
option(CORE_CPP_MSVC_STATIC_RUNTIME_VARIANTS
       "With an MSVC-ABI compiler, also declare a static-CRT twin core::<name>_mt of every compiled module" OFF)

# core-cpp's own tests link core::testing_main, so testing forces it on. A normal variable
# shadows the cache entry, as below for Emscripten, and leaves the parent's cache alone.
if(CORE_CPP_TESTING AND NOT CORE_CPP_CATCH2_MAIN)
    message(STATUS "[core-cpp] CORE_CPP_CATCH2_MAIN is ON because CORE_CPP_TESTING is ON")
    set(CORE_CPP_CATCH2_MAIN ON)
endif()

# core::tui links core::tui_output, so the full TUI forces the leaf on, the same way testing forces
# the Catch2 main.
if(CORE_CPP_WITH_TUI AND NOT CORE_CPP_WITH_TUI_OUTPUT)
    message(STATUS "[core-cpp] CORE_CPP_WITH_TUI_OUTPUT is ON because CORE_CPP_WITH_TUI is ON")
    set(CORE_CPP_WITH_TUI_OUTPUT ON)
endif()

# A sanitizer instruments core-cpp's targets only. As a subproject that would mix
# instrumented and uninstrumented code under one parent, which is what makes TSan
# report races that are not there. A parent instruments core-cpp itself, through
# the CORE_CPP_TARGETS global property.
if(CORE_CPP_SANITIZERS AND NOT PROJECT_IS_TOP_LEVEL)
    message(FATAL_ERROR
        "CORE_CPP_SANITIZERS='${CORE_CPP_SANITIZERS}' is only for a top-level core-cpp build. "
        "A parent project applies its sanitizers to the targets in the CORE_CPP_TARGETS global property.")
endif()

# Under Emscripten only the WebAssembly subset builds (Part I §1), which has neither
# a terminal nor TLS. A normal variable shadows the cache entry for core-cpp's
# directories and leaves the parent's cache alone.
if(EMSCRIPTEN)
    foreach(_coreCppWasmOff IN ITEMS CORE_CPP_WITH_TUI CORE_CPP_WITH_TUI_OUTPUT CORE_CPP_WITH_IMAGES
                                         CORE_CPP_WITH_TLS)
        if(${_coreCppWasmOff})
            message(STATUS "[core-cpp] ${_coreCppWasmOff} is OFF under Emscripten (WebAssembly subset only)")
        endif()
        set(${_coreCppWasmOff} OFF)
    endforeach()
endif()
