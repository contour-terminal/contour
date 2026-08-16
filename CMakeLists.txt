cmake_minimum_required(VERSION 3.25 FATAL_ERROR)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(Version)
GetVersionInformation(CONTOUR_VERSION CONTOUR_VERSION_STRING)
file(WRITE "${CMAKE_BINARY_DIR}/version.txt" "${CONTOUR_VERSION_STRING}")

# Before project(), which is where CMake consumes it. This is a property of the code, not
# of any one build configuration, so it lives here rather than being restated in every
# preset and CI job: 13.3 is the first macOS whose libc++ exports the floating-point
# std::to_chars that <format> needs, and we use std::format throughout. Building against
# an older target fails to compile with "'to_chars' has been explicitly marked unavailable".
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS version to build for")
endif()

project(contour VERSION "${CONTOUR_VERSION}" LANGUAGES CXX C)

# setting defaults
if (NOT(CMAKE_CXX_STANDARD))
    set(CMAKE_CXX_STANDARD 23)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_COLOR_DIAGNOSTICS ON)

# Ninja starts as many link steps as there are cores, and the tail of this build is a dozen test
# executables that each pull the whole tree in with debug info. Four of those linking at once
# exhausted the 16 GB x86_64 GitHub runner: the kernel took the runner agent with it, and the job
# reported "The runner has received a shutdown signal" and exit 143 -- a message that reads like
# infrastructure and is not. It struck four times out of five, always between ninja steps 608 and
# 610 of 623, i.e. always in the concurrent-link phase, while the arm64 GCC and x86_64 Clang jobs
# of the very same runs passed (Clang's debug info is smaller, and it never tipped over).
#
# The cap is on LINK steps only, which is where the memory goes; compiling still uses every core,
# and that is where the build actually spends its time. CONTOUR_LINK_JOBS raises or lowers it for
# a machine that wants something else.
set(CONTOUR_LINK_JOBS "2" CACHE STRING "Maximum number of concurrent link steps (Ninja generators)")
if(CMAKE_GENERATOR MATCHES "Ninja")
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS contour_link_pool=${CONTOUR_LINK_JOBS})
    # Read when each target is created, so it must be set before any add_subdirectory() below.
    set(CMAKE_JOB_POOL_LINK contour_link_pool)
endif()

if((CMAKE_CXX_COMPILER_ID STREQUAL "Clang") OR (CMAKE_CXX_COMPILER_ID STREQUAL "GNU"))
    set(CMAKE_CXX_FLAGS_DEBUG "-Og -g")
endif()

# Enable parallel building if CL.exe is used
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP")
endif()

# Compiler caching. Loaded before any compiler invocation so that CPM-fetched dependencies
# get cached too. CompileCache.cmake prefers the fastcache-cc launcher (when installed and
# FASTCACHE_ADDR names a daemon), falls back to sccache and then ccache, and no-ops when none
# is usable. It also declares the USE_COMPILER_CACHE option that gates all of it.
include(CompileCache)
include(ClangTidy)

if(NOT WIN32 AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Choose the build mode." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release MinSizeRel RelWithDebInfo)
endif()

# Setting intermediate output directories for Windows
if(WIN32)
    get_property(_isMultiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(_isMultiConfig)
        set(_base_suffix "/$<CONFIG>")
    else()
        set(_base_suffix "")
    endif()
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}${_base_suffix}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}${_base_suffix}/lib")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}${_base_suffix}/lib")
endif()

find_package(Threads)

if(DEFINED MSVC)
    add_definitions(-D_USE_MATH_DEFINES)
    add_definitions(-DNOMINMAX)
    add_compile_options(/utf-8)
    add_compile_options(/nologo)
    add_compile_options(/bigobj)
endif()

if(APPLE)
    # add -fexperimental-library option for libc++
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexperimental-library")
endif()

# src/coro/Cancellation.hpp is built on C++20 <stop_token> and refuses to compile without it. One
# standard library we target ships it switched off: libc++ guards stop_token and jthread with
# _LIBCPP_HAS_NO_EXPERIMENTAL_STOP_TOKEN, which __config defines unless _LIBCPP_ENABLE_EXPERIMENTAL
# is -- and that is what -fexperimental-library sets. libc++ 20 dropped the guard, so this reaches
# libc++ 18 and 19 only; FreeBSD 15's base Clang 19 is where CI meets it, and so does anyone
# building there with the base toolchain.
#
# Global rather than a property of the `coro` target, which is what this was first written as: coro
# is header-only, and its headers are pulled in through several layers of other headers -- contour's
# ReactorThread.hpp reaches Cancellation.hpp, so contour_gui_test compiled it while linking no coro
# target and got none of its INTERFACE flags. Whether the standard library needs this switch is a
# property of the toolchain, not of any one target, so every translation unit gets the same answer.
#
# Probed, not version-tested: what matters is the standard library the compiler actually uses, which
# its version number does not name -- and a probe stops adding the flag by itself once a platform
# moves on to libc++ 20, instead of pinning a workaround to a version range someone must curate.
include(CheckCXXSourceCompiles)
set(_stop_token_probe "
#include <version>
#if !defined(__cpp_lib_jthread) || __cpp_lib_jthread < 201911L
#error stop_token unavailable
#endif
int main() { return 0; }
")
check_cxx_source_compiles("${_stop_token_probe}" CONTOUR_HAS_STOP_TOKEN)
if(NOT CONTOUR_HAS_STOP_TOKEN)
    set(CMAKE_REQUIRED_FLAGS "-fexperimental-library")
    check_cxx_source_compiles("${_stop_token_probe}" CONTOUR_HAS_STOP_TOKEN_WHEN_EXPERIMENTAL)
    unset(CMAKE_REQUIRED_FLAGS)
    if(NOT CONTOUR_HAS_STOP_TOKEN_WHEN_EXPERIMENTAL)
        message(FATAL_ERROR
            "coro requires C++20 <stop_token> (__cpp_lib_jthread), which this standard library does "
            "not provide even with -fexperimental-library. Use libstdc++ 11+, libc++ 18+, or MSVC "
            "STL -- or add the minimal fallback that src/coro/Cancellation.hpp describes.")
    endif()
    # Compile AND link: -fexperimental-library also selects the matching libc++experimental.
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexperimental-library")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fexperimental-library")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fexperimental-library")
    message(STATUS "<stop_token>: enabling -fexperimental-library (libc++ keeps it gated)")
endif()

if (CONTOUR_BUILD_WITH_MIMALLOC)
    add_definitions(-DCONTOUR_BUILD_WITH_MIMALLOC)
endif()

# Enables STL container checker if not building a release.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_definitions(-D_GLIBCXX_ASSERTIONS)
endif()

find_program(ADDR2LINE NAMES addr2line llvm-addr2line)
set(ADDR2LINE_DEFAULT OFF)
if(NOT("${ADDR2LINE}" STREQUAL ""))
    set(ADDR2LINE_DEFAULT ON)
endif()
message(STATUS "addr2line: ${ADDR2LINE}")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(MAINTAINER_MODE ON)
else()
    set(MAINTAINER_MODE OFF)
endif()

option(CONTOUR_TESTING "Enables building of unittests for libterminal [default: ${MAINTAINER_MODE}]" ${MAINTAINER_MODE})
option(CONTOUR_VERIFY_STATE "Enables expensive internal terminal-state invariant checks (Grid/Terminal verifyState) [default: ${MAINTAINER_MODE}]" ${MAINTAINER_MODE})
option(CONTOUR_FRONTEND_GUI "Enables GUI frontend." ON)
option(CONTOUR_COVERAGE "Builds with codecov [default: OFF]" OFF)
option(CONTOUR_SANITIZE "Builds with Address sanitizer enabled [default: OFF]" "OFF")
option(CONTOUR_STACKTRACE_ADDR2LINE "Uses addr2line to pretty-print SEGV stacktrace." ${ADDR2LINE_DEFAULT})
option(CONTOUR_BUILD_WITH_MIMALLOC "Builds with mimalloc [default: OFF]" OFF)
option(CONTOUR_INSTALL_TOOLS "Installs tools, if built [default: OFF]" OFF)
option(CONTOUR_PACKAGE_TERMINFO "Package terminfo files" ON)
option(CONTOUR_WITH_UTEMPTER "Build with utempter support [default: ON]" ON)
option(CONTOUR_USE_CPM "Use CPM to fetch dependencies [default: ON]" ON)
option(CONTOUR_BUILD_STATIC "Link to static libraries [default: OFF]" OFF)
option(CONTOUR_BUILD_NATIVE "Build for native architecture [default: OFF]" OFF)
if(CONTOUR_BUILD_STATIC)
    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(CMAKE_LINK_SEARCH_END_STATIC ON)
    set(CMAKE_EXE_LINKER_FLAGS "-static-libgcc -static-libstdc++")
endif()

if(NOT WIN32 AND NOT CONTOUR_SANITIZE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CONTOUR_SANITIZE "OFF" CACHE STRING "Choose the sanitizer mode." FORCE)
    set_property(CACHE CONTOUR_SANITIZE PROPERTY STRINGS OFF address thread)
endif()

if(CONTOUR_BUILD_NATIVE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")
endif()

# ----------------------------------------------------------------------------
# code coverage

if(CONTOUR_COVERAGE AND NOT MSVC)
    add_compile_options(-g --coverage)
    set(CMAKE_EXE_LINKER_FLAGS "--coverage ${CMAKE_EXE_LINKER_FLAGS}")
    set(CONTOUR_CODE_COVERAGE_ENABLED ON)
else()
    set(CONTOUR_CODE_COVERAGE_ENABLED OFF)
endif()

if(NOT(CONTOUR_SANITIZE STREQUAL "OFF"))
    # Map the selected sanitizer mode to its -fsanitize set. "address" also enables UndefinedBehaviorSanitizer
    # (ASan+UBSan compose; ASan+TSan do not). Keep the flags in one variable so both the compile and the link
    # step receive them — a sanitizer runtime must be linked in, not only compiled against.
    if(CONTOUR_SANITIZE STREQUAL "address")
        set(CONTOUR_SANITIZE_FLAGS "-fsanitize=address,undefined -fsanitize-address-use-after-scope")
    elseif(CONTOUR_SANITIZE STREQUAL "thread")
        set(CONTOUR_SANITIZE_FLAGS "-fsanitize=thread")
    else()
        set(CONTOUR_SANITIZE_FLAGS "-fsanitize=${CONTOUR_SANITIZE}")
    endif()
    message(STATUS "Enabling ${CONTOUR_SANITIZE} sanitizer (flags: ${CONTOUR_SANITIZE_FLAGS}).")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-omit-frame-pointer ${CONTOUR_SANITIZE_FLAGS}")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-omit-frame-pointer ${CONTOUR_SANITIZE_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${CONTOUR_SANITIZE_FLAGS}")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${CONTOUR_SANITIZE_FLAGS}")
else()
    message(STATUS "No sanitizer enabled.")
endif()

if(CONTOUR_TESTING)
    enable_testing()

    # A lint gate registered here is Python, not shell, so that it runs on every platform this
    # project supports. These two used to be bash, guarded by `if(NOT WIN32)`, which left Windows
    # developers — and the Windows CI job, the one job that invokes `ctest` directly — with no spell
    # check and no workflow check at all. The other scripts/check-*.sh remain bash because CI is
    # their only caller; the rule is that anything ctest runs has to be portable. Both gates fetch
    # their tool on demand and skip gracefully when it cannot be obtained, so neither is gated on a
    # build target and both run in a fresh tree.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        # One line per gate. The label and timeout are applied to whatever this registers, so a new
        # gate cannot land outside `ctest -L lint` by someone forgetting a second edit — which is
        # the same silently-skipped-check problem these gates exist to catch.
        set(CONTOUR_LINT_GATES "")
        macro(contour_add_lint_gate name script)
            add_test(NAME ${name}
                     COMMAND ${Python3_EXECUTABLE}
                             "${CMAKE_CURRENT_SOURCE_DIR}/scripts/${script}" ${ARGN})
            list(APPEND CONTOUR_LINT_GATES ${name})
        endmacro()

        # Lint the GitHub Actions workflows locally so a workflow-file error (which silently breaks
        # CI dispatch on GitHub — a check-suite with zero jobs failing at 0s) is caught before it is
        # pushed.
        contour_add_lint_gate(check_workflows check-workflows.py)

        # Spell-check the tree. CI runs this very script, so a local `ctest` run and the CI gate
        # cannot disagree — that is what stops a spelling failure from costing an extra push. CI
        # sets CHECK_SPELLING_REQUIRE_TOOL so the skip cannot turn into a false green there.
        contour_add_lint_gate(check_spelling check-spelling.py)

        # The justification rule in `_typos.toml` is the only thing keeping the exception list from
        # growing back into the 2,237-entry allow-list it replaced, and its parser has been wrong
        # before. This case needs no network, so it stays meaningful on an offline clone where the
        # gate above skips.
        contour_add_lint_gate(check_spelling_selftest check-spelling.py --self-test)

        # 300s, not the 120s these had as shell scripts: a cold cache now downloads the tool on
        # Windows too, and a CI runner's first run pays for both.
        set_tests_properties(${CONTOUR_LINT_GATES} PROPERTIES LABELS "lint" TIMEOUT 300)
    else()
        # A warning, not a status line: losing both lint gates is exactly the silent gap this
        # change set out to close, and it must not scroll past in a wall of configure output.
        message(WARNING "Python 3 not found: the lint gates (check_spelling, check_workflows) "
                        "are not registered.")
    endif()
endif()
# ----------------------------------------------------------------------------

if(CONTOUR_USE_CPM)
    # download CPM.cmake
    file(
        DOWNLOAD
        https://github.com/cpm-cmake/CPM.cmake/releases/download/v0.38.3/CPM.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake
        EXPECTED_HASH SHA256=cc155ce02e7945e7b8967ddfaff0b050e958a723ef7aad3766d368940cb15494
    )
    include(${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake)
endif()


# A search-path hint, not a requirement: cmake/FindQt.cmake returns immediately on Linux and the
# BSDs, and on Windows and macOS only appends whatever Qt it discovers to CMAKE_PREFIX_PATH, saying
# so in a status line when it finds none.
#
# Guarded all the same, because a hint that finds nothing still prints as though something were
# missing. Without the GUI frontend nothing in the build asks for Qt at all -- src/contour looks it
# up only inside the same guard -- so a machine with no Qt installed should hear nothing about it.
if(CONTOUR_FRONTEND_GUI)
    include(FindQt)
endif()

# The vendored third-party code (fetched via CPM/FetchContent) must NOT be run through clang-tidy: it
# has no .clang-tidy config (clang-tidy then aborts with "no checks enabled") and is not ours to lint.
# CMAKE_CXX_CLANG_TIDY is inherited by every target created after it is set, so stash it across the
# dependency fetch and restore it only for our own first-party src/ tree.
set(_CONTOUR_SAVED_CLANG_TIDY "${CMAKE_CXX_CLANG_TIDY}")
unset(CMAKE_CXX_CLANG_TIDY)
include(ContourThirdParties)
set(CMAKE_CXX_CLANG_TIDY "${_CONTOUR_SAVED_CLANG_TIDY}")
add_subdirectory(src)
add_subdirectory(examples)

# ----------------------------------------------------------------------------
# Summary

if("${CMAKE_CXX_COMPILER_LAUNCHER}" STREQUAL "")
    set(USING_COMPILER_CACHE_STRING "OFF")
else()
    # The launcher may be a `cmake -E env NAME=VALUE ... <program>` wrapper; the program that
    # identifies the cache is always the last element. Report that rather than ${CCACHE}, which
    # CompileCache.cmake fills in from PATH even when another launcher was chosen.
    list(GET CMAKE_CXX_COMPILER_LAUNCHER -1 USING_COMPILER_CACHE_STRING)
endif()

if("${CMAKE_CXX_CLANG_TIDY}" STREQUAL "")
    set(USING_TIDY_STRING "OFF")
else()
    set(USING_TIDY_STRING "ON (${CMAKE_CXX_CLANG_TIDY})")
endif()

add_custom_target(package-verbose
    COMMAND ${CMAKE_CPACK_COMMAND} --config CPackConfig.cmake --verbose
    COMMENT "Build package with verbose output"
    VERBATIM
    USES_TERMINAL
)

macro(ContourConfigurationSummary)
    message(STATUS "==============================================================================")
    message(STATUS "    Contour (${CONTOUR_VERSION_STRING})")
    message(STATUS "------------------------------------------------------------------------------")
    message(STATUS "System:                                             ${CMAKE_SYSTEM_PROCESSOR} (${CMAKE_SYSTEM_NAME})")
    message(STATUS "Build type:                                         ${CMAKE_BUILD_TYPE}")
    message(STATUS "C++ standard:                                       C++${CMAKE_CXX_STANDARD} (${CMAKE_CXX_COMPILER_ID})")
    message(STATUS "Compiler cache:                                     ${USING_COMPILER_CACHE_STRING}")
    message(STATUS "Build with sanitizer:                               ${CONTOUR_SANITIZE}")
    message(STATUS "Build unit tests:                                   ${CONTOUR_TESTING}")
    message(STATUS "Verify terminal state invariants:                   ${CONTOUR_VERIFY_STATE}")
    message(STATUS "Enable with code coverage:                          ${CONTOUR_CODE_COVERAGE_ENABLED}")
    message(STATUS "Build contour frontend GUI:                         ${CONTOUR_FRONTEND_GUI}")
    message(STATUS "Build contour using mimalloc:                       ${CONTOUR_BUILD_WITH_MIMALLOC}")
    message(STATUS "Clang Tidy:                                         ${USING_TIDY_STRING}")
    message(STATUS "|> Enable performance metrics:                      ${CONTOUR_PERF_STATS}")
    message(STATUS "------------------------------------------------------------------------------")
endmacro()

ContourThirdPartiesSummary2()
ContourConfigurationSummary()
