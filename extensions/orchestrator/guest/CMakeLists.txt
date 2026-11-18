# SPDX-License-Identifier: Apache-2.0
#
# The orchestrator's WebAssembly module: a wasm32-wasip1 reactor of the daemon world
# (`terminal-extensions:daemon`, wit/daemon/ in Contour's tree). A project of its own, like harness/, configured
# with a WASI C++ toolchain; Contour builds it with contour_add_wasm_cxx_guest() (cmake/ContourWasiCxx.cmake)
# when CONTOUR_EXTENSIONS_ORCHESTRATOR_GUEST is on. The core, the dashboard and the driver are the very sources the native
# build compiles and tests; only WitHost.cpp and Exports.cpp, the adapter to the generated bindings, are
# WebAssembly-only.
#
#   ORCHESTRATOR_MANIFEST_SECTION_SOURCE  the C++ source carrying the manifest section, which the host build
#                                         generates with the native orchestrator_manifest tool
#                                         (cmake/EmbedManifest.cmake).
cmake_minimum_required(VERSION 3.25 FATAL_ERROR)
project(orchestrator_guest LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
    message(FATAL_ERROR "orchestrator_guest: configure with a WASI C++ toolchain (see above).")
endif()
if(NOT ORCHESTRATOR_MANIFEST_SECTION_SOURCE OR NOT EXISTS "${ORCHESTRATOR_MANIFEST_SECTION_SOURCE}")
    message(FATAL_ERROR "orchestrator_guest: ORCHESTRATOR_MANIFEST_SECTION_SOURCE names no generated manifest section.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Size-optimized and link-time optimized, as the parse budget was measured (harness/).
add_compile_options(-Os -flto)
add_link_options(-flto)

# glaze, core-cpp and libunicode, as the core's own build takes them: Contour hands over the sources it carries.
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Dependencies.cmake)

# The core, the dashboard and the driver, from the project's own CMakeLists; only what the module links is built.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/.. orchestrator EXCLUDE_FROM_ALL)

add_executable(orchestrator
    Exports.cpp
    WitHost.cpp
    WitHost.hpp
    bindings/daemon.c
    bindings/daemon.h
    ${ORCHESTRATOR_MANIFEST_SECTION_SOURCE}
)
# The dashboard, built for WebAssembly from the same sources the native tests cover, for the driver to serve.
target_link_libraries(orchestrator PRIVATE orchestrator_guest_driver orchestrator_view)
# The generated bindings, as <bindings/daemon.h>.
target_include_directories(orchestrator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# The adapter is held to Contour's warnings; the generated bindings are not edited, and not warned about.
set_source_files_properties(Exports.cpp WitHost.cpp PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wpedantic;-Werror")
