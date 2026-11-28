# SPDX-License-Identifier: Apache-2.0
#
# Everything core-cpp does to global state, and the only file that may.
#
# The top-level CMakeLists.txt includes this only when core-cpp is the top-level
# project, after project() and before core_cpp_resolve_dependencies(). As a
# subproject core-cpp changes nothing of its parent's: no launcher, no standard,
# no flags. tests/cmake/check-cmake-hygiene.cmake holds every other file to that.

include_guard(GLOBAL)

# First, so that everything compiled from here on goes through the compiler cache,
# CPM-fetched dependencies included. The module is a verbatim copy; see
# portable/README.md.
include("${CMAKE_CURRENT_LIST_DIR}/portable/CompileCache.cmake")

# The module sets CMAKE_CXX_COMPILER_LAUNCHER as a normal variable, so CMakeCache.txt
# would not otherwise show which launcher this build tree uses. This records it for
# CI and for people; nothing reads it back.
set(CORE_CPP_CXX_COMPILER_LAUNCHER "${CMAKE_CXX_COMPILER_LAUNCHER}" CACHE INTERNAL
    "The compiler launcher cmake/portable/CompileCache.cmake selected for this build tree")

# Bound every dependency transfer of this configure, so a stalled one ends instead of hanging:
# the CPM bootstrap's download reads PACE_FETCH_SILENCE_SECONDS, and every git clone
# inherits the GIT_HTTP_LOW_SPEED_* environment it exports. Here, because that environment is
# process-wide, and before core_cpp_resolve_dependencies(), because it must precede every fetch.
# The module is a verbatim copy from Pace (formerly fastcached); its reasoning is in the file.
include("${CMAKE_CURRENT_LIST_DIR}/FetchTransferBound.cmake")

# The dependencies core-cpp fetches are compiled with the same standard as the code
# that includes their headers. Catch2 in particular compiles parts of itself only from
# C++17 on, and a test that uses them would fail to link otherwise. As a subproject,
# the Catch2 row's WRAP (core_cpp_catch2_standard) sets it on the fetched targets instead.
#
# 23 is the default, not an override: a -DCMAKE_CXX_STANDARD=26 on the command line (CI's
# C++26 leg) must reach the compile. A normal variable would shadow that cache entry, and the
# leg would build C++23 and report green.
if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 23)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# core-cpp has no module units. Its own targets and probes scan for none in either role: the root
# CMakeLists.txt sets CMP0155 OLD, and core_cpp_apply_toolchain() turns scanning off per target. A
# fetched dependency sets CMP0155 NEW again with its own cmake_minimum_required(), and from C++20 on
# its try_compile() probes then scan; where the compiler has no clang-scan-deps (FreeBSD's base
# clang) every one of them fails. This default reaches those too, so it is global, hence top-level.
if(NOT DEFINED CMAKE_CXX_SCAN_FOR_MODULES)
    set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_COLOR_DIAGNOSTICS ON)
