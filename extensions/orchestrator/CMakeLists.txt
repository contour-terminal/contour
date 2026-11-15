# SPDX-License-Identifier: Apache-2.0
#
# The agent orchestrator: a project of its own, built inside Contour today and extracted into its own
# repository later. It therefore includes nothing from Contour's src/; its contract with Contour is the
# daemon extension world, the manifest and the `contour agent` verbs.
cmake_minimum_required(VERSION 3.25 FATAL_ERROR)
project(orchestrator LANGUAGES CXX)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

# Contour's gates, mirroring src/CMakeLists.txt: both are directory-scoped there, so a project beside src/
# has to ask for them itself. Standalone, PedanticCompiler is absent and the include does nothing.
include(PedanticCompiler OPTIONAL)
if(CONTOUR_CODE_COVERAGE_ENABLED)
    add_compile_options(-g --coverage)
endif()

option(ORCHESTRATOR_TESTING "Enables building of unittests for the agent orchestrator [default: CONTOUR_TESTING, OFF standalone]" ${CONTOUR_TESTING})

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dependencies.cmake)

# The sans-IO core. Header-only for now, so INTERFACE; it becomes STATIC with its first translation unit.
add_library(orchestrator_core INTERFACE
    core/Utf8.hpp
)
target_compile_features(orchestrator_core INTERFACE cxx_std_23)
# The include root is the directory above this project, so that sources spell <orchestrator/core/X.hpp>
# (<core/...> is core-cpp's). An extracted checkout must therefore live in a directory named `orchestrator`.
target_include_directories(orchestrator_core INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
)

if(ORCHESTRATOR_TESTING)
    enable_testing()
    add_executable(orchestrator_test
        core/test_main.cpp
        core/Utf8_test.cpp
    )
    target_link_libraries(orchestrator_test PRIVATE orchestrator_core core::testing Catch2::Catch2)
    add_test(NAME orchestrator_test COMMAND orchestrator_test)
    set_tests_properties(orchestrator_test PROPERTIES LABELS daemon)

    if(NOT WIN32)
        # Catch2's TEST_CASE expands __COUNTER__, which Clang flags as a C2y extension under -Werror.
        target_compile_options(orchestrator_test PRIVATE -Wno-c2y-extensions)
    endif()
endif()
