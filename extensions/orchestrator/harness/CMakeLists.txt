# SPDX-License-Identifier: Apache-2.0
#
# The parse-budget harness: the core built for wasm32-wasip1 as a reactor exporting `parse`, so that Contour's
# vtextension_test can time the core's `gh-prs` mapping under the bundled WAMR (src/vtextension/
# ParseBudget_test.cpp). A project of its own, configured with wasi-sdk's toolchain file; the core's
# sources are the very ones the native build compiles, unchanged.
#
#   cmake -S extensions/orchestrator/harness -B out/orchestrator-harness -G Ninja \
#       -DCMAKE_BUILD_TYPE=MinSizeRel \
#       -DWASI_SDK_PREFIX=$WASI_SDK_PREFIX \
#       -DCMAKE_TOOLCHAIN_FILE=$WASI_SDK_PREFIX/share/cmake/wasi-sdk-p1.cmake \
#       -DFETCHCONTENT_SOURCE_DIR_GLAZE=<a glaze 9.0.0 checkout, e.g. out/clang-asan/_deps/glaze-src>
#   cmake --build out/orchestrator-harness --target orchestrator_parse_harness
#
# The module is out/orchestrator-harness/orchestrator_parse_harness.wasm. With -DORCH_PARSE_MAPPING=glab-mrs it
# times the `glab-mrs` mapping instead (core/testdata/gitlab/mrs-100.json).
cmake_minimum_required(VERSION 3.25 FATAL_ERROR)
project(orchestrator_parse_harness LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
    message(FATAL_ERROR "orchestrator_parse_harness: configure with wasi-sdk's toolchain file (see above).")
endif()

# The guest's flags (§13 of the design): no exceptions and no RTTI, which also selects the sysroot's
# exception-free libc++; size-optimized and link-time optimized, as a shipped guest would be.
add_compile_options(-Os -fno-exceptions -fno-rtti -flto)
add_link_options(-flto -mexec-model=reactor)

# glaze, as the core's own build takes it: fetched at the pinned version unless FETCHCONTENT_SOURCE_DIR_GLAZE
# names a checkout.
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Dependencies.cmake)

# The core, from its own CMakeLists; only what the harness links is built.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/.. orchestrator EXCLUDE_FROM_ALL)

set(ORCH_PARSE_MAPPING "gh-prs" CACHE STRING "The mapping `parse` times: gh-prs or glab-mrs")
set_property(CACHE ORCH_PARSE_MAPPING PROPERTY STRINGS gh-prs glab-mrs)

add_executable(orchestrator_parse_harness ParseHarness.cpp)
target_link_libraries(orchestrator_parse_harness PRIVATE orchestrator_core)
if(ORCH_PARSE_MAPPING STREQUAL "glab-mrs")
    target_compile_definitions(orchestrator_parse_harness PRIVATE ORCH_PARSE_GLAB_MRS=1)
elseif(NOT ORCH_PARSE_MAPPING STREQUAL "gh-prs")
    message(FATAL_ERROR "ORCH_PARSE_MAPPING: gh-prs or glab-mrs, not ${ORCH_PARSE_MAPPING}")
endif()
# The toolchain file asks for the suffix, but the platform module it loads resets it.
set_target_properties(orchestrator_parse_harness PROPERTIES SUFFIX ".wasm")
