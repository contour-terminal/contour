# A host-side test guest: the smallest C++ module of the daemon world, built by contour_add_wasm_cxx_guest()
# (cmake/ContourWasiCxx.cmake) with the WASI C++ toolchain, and loaded by vtextension_test. It uses no
# bindings: its few imports and exports are declared by hand in Probe.cpp.
cmake_minimum_required(VERSION 3.25)
project(cxx-daemon-probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(cxx-daemon-probe Probe.cpp)
target_compile_options(cxx-daemon-probe PRIVATE -Wall -Wextra -Wpedantic -Werror)

# The list values contour_add_wasm_cxx_guest() hands over must arrive as lists, entry by entry: glaze's
# include directories (a system package's), and CXX_PROBE_EXPECTED_DIRECTORIES, two directories vtextension
# passes for this check alone.
if(DEFINED CXX_PROBE_EXPECTED_DIRECTORIES)
    list(LENGTH CXX_PROBE_EXPECTED_DIRECTORIES _count)
    if(NOT _count EQUAL 2)
        message(FATAL_ERROR "CXX_PROBE_EXPECTED_DIRECTORIES arrived as ${_count} entries, not 2: ${CXX_PROBE_EXPECTED_DIRECTORIES}")
    endif()
endif()
foreach(_directory IN LISTS CXX_PROBE_EXPECTED_DIRECTORIES ORCHESTRATOR_GLAZE_INCLUDE_DIR)
    if(NOT IS_DIRECTORY "${_directory}")
        message(FATAL_ERROR "A list handed to the guest arrived mangled: ${_directory} is not a directory")
    endif()
endforeach()
