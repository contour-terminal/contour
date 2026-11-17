# Builds C++ WebAssembly guests (wasm32-wasip1 reactors) in sub-builds of their own, with a WASI C++ toolchain.
#
# Included only with CONTOUR_EXTENSIONS_ORCHESTRATOR_GUEST, which then needs such a toolchain: clang with the
# wasm32-wasip1 target, wasm-ld, llvm-ar and a sysroot holding wasi-libc and libc++. They are looked for in
#
#   1. WASI_SDK_PREFIX, the cache variable, else the environment variable: a wasi-sdk (>= 34) install;
#   2. else the system: clang, wasm-ld and llvm-ar on PATH, the sysroot at /usr/share/wasi-sysroot.
#
# What is found is proven by building a C++23 reactor with it, and a missing piece fails the configuration
# here, naming it, rather than mid-build. The guests' toolchain file is CONTOUR_WASI_CXX_TOOLCHAIN_FILE;
# contour_add_wasm_cxx_guest() hands it what was found.

include(ExternalProject)

set(WASI_SDK_PREFIX "" CACHE PATH
    "A wasi-sdk (>= 34) for the C++ WebAssembly guests; empty: the environment's WASI_SDK_PREFIX, else the system")
set(CONTOUR_WASI_CXX_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/toolchains/wasm32-wasip1.cmake")

# The toolchain's programs, as `<variable the toolchain file reads>:<program name>`.
set(_contour_wasi_tools
    CONTOUR_WASI_C_COMPILER:clang
    CONTOUR_WASI_CXX_COMPILER:clang++
    CONTOUR_WASI_LINKER:wasm-ld
    CONTOUR_WASI_AR:llvm-ar
    CONTOUR_WASI_RANLIB:llvm-ranlib
)
# Every variable the toolchain file reads.
set(_contour_wasi_variables CONTOUR_WASI_SYSROOT CONTOUR_WASI_CMAKE_DIR)
foreach(_row IN LISTS _contour_wasi_tools)
    string(REGEX REPLACE ":.*" "" _variable "${_row}")
    list(APPEND _contour_wasi_variables ${_variable})
endforeach()

# Fails the configuration with the one message a missing toolchain gets.
# Its arguments are concatenated into what is missing.
function(_contour_wasi_cxx_missing)
    string(CONCAT _what ${ARGN})
    message(FATAL_ERROR "CONTOUR_EXTENSIONS_ORCHESTRATOR_GUEST needs a WASI C++ toolchain: ${_what}. "
                        "Set WASI_SDK_PREFIX to a wasi-sdk >= 34.")
endfunction()

# Sets every variable of _contour_wasi_variables in the caller's scope, from a wasi-sdk or the system.
function(_contour_wasi_cxx_locate)
    set(_prefix "${WASI_SDK_PREFIX}")
    if(NOT _prefix AND NOT "$ENV{WASI_SDK_PREFIX}" STREQUAL "")
        set(_prefix "$ENV{WASI_SDK_PREFIX}")
    endif()

    if(_prefix)
        # Absolute, a relative one taken against the source tree: the tools' paths are handed to sub-builds
        # running elsewhere, and -fuse-ld takes the linker's path as it is.
        cmake_path(ABSOLUTE_PATH _prefix BASE_DIRECTORY "${CMAKE_SOURCE_DIR}" NORMALIZE)
        foreach(_row IN LISTS _contour_wasi_tools)
            string(REGEX REPLACE ":.*" "" _variable "${_row}")
            string(REGEX REPLACE "^[^:]*:" "" _tool "${_row}")
            set(_path "${_prefix}/bin/${_tool}${CMAKE_EXECUTABLE_SUFFIX}")
            if(NOT EXISTS "${_path}")
                _contour_wasi_cxx_missing("no ${_tool} at ${_path}")
            endif()
            set(${_variable} "${_path}" PARENT_SCOPE)
        endforeach()
        set(_sysroot "${_prefix}/share/wasi-sysroot")
        if(EXISTS "${_prefix}/share/cmake/Platform/WASI.cmake")
            set(CONTOUR_WASI_CMAKE_DIR "${_prefix}/share/cmake" PARENT_SCOPE)
        else()
            set(CONTOUR_WASI_CMAKE_DIR "" PARENT_SCOPE)
        endif()
    else()
        foreach(_row IN LISTS _contour_wasi_tools)
            string(REGEX REPLACE ":.*" "" _variable "${_row}")
            string(REGEX REPLACE "^[^:]*:" "" _tool "${_row}")
            find_program(_path NAMES ${_tool} NO_CACHE)
            if(NOT _path)
                _contour_wasi_cxx_missing("no ${_tool} on PATH, and WASI_SDK_PREFIX is not set")
            endif()
            set(${_variable} "${_path}" PARENT_SCOPE)
            unset(_path)
        endforeach()
        set(_sysroot "/usr/share/wasi-sysroot")
        set(CONTOUR_WASI_CMAKE_DIR "" PARENT_SCOPE)
    endif()

    if(NOT IS_DIRECTORY "${_sysroot}")
        _contour_wasi_cxx_missing("no WASI sysroot at ${_sysroot}")
    endif()
    if(NOT IS_DIRECTORY "${_sysroot}/include/c++/v1" AND NOT IS_DIRECTORY "${_sysroot}/include/wasm32-wasip1/c++/v1")
        _contour_wasi_cxx_missing("the WASI sysroot ${_sysroot} holds no libc++")
    endif()
    set(CONTOUR_WASI_SYSROOT "${_sysroot}" PARENT_SCOPE)
endfunction()

# Proves the toolchain by configuring and building a C++23 reactor with it, once per toolchain: the result is
# cached under what was found and the compiler's own version, so that an SDK replaced at the same prefix, or
# one that no longer runs, is proven again.
function(_contour_wasi_cxx_prove)
    set(_flags ${_contour_wasi_toolchain_args})
    execute_process(
        COMMAND "${CONTOUR_WASI_CXX_COMPILER}" --version
        RESULT_VARIABLE _version_result
        OUTPUT_VARIABLE _version
        ERROR_VARIABLE _version
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _version_result EQUAL 0)
        string(REGEX MATCH "[^\n]+" _cause "${_version}")
        _contour_wasi_cxx_missing("${CONTOUR_WASI_CXX_COMPILER} does not run (${_cause})")
    endif()
    set(_key ${_flags} "${_version}")
    if("${_key}" STREQUAL "${_CONTOUR_WASI_CXX_PROVEN}")
        return()
    endif()

    set(_dir "${CMAKE_BINARY_DIR}/CMakeFiles/ContourWasiCxx")
    file(WRITE "${_dir}/src/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.25)
project(ContourWasiCxxProbe CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(reactor Reactor.cpp)
]=])
    file(WRITE "${_dir}/src/Reactor.cpp" [=[
#include <expected>
#include <string>

[[clang::export_name("probe")]] int probe(int count)
{
    auto const text = std::string(static_cast<std::size_t>(count), 'x');
    auto const length = std::expected<std::size_t, int> { text.size() };
    return static_cast<int>(length.value_or(0));
}
]=])
    # A configuration of its own, not try_compile(): a try_compile project inherits this build's compilers,
    # whatever toolchain file it is handed.
    file(REMOVE_RECURSE "${_dir}/build")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${_dir}/src" -B "${_dir}/build" -G "${CMAKE_GENERATOR}"
                "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}" ${_flags}
        RESULT_VARIABLE _configured
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _output)
    if(_configured EQUAL 0)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" --build "${_dir}/build"
            RESULT_VARIABLE _built
            OUTPUT_VARIABLE _output
            ERROR_VARIABLE _output)
    endif()
    if(NOT _configured EQUAL 0 OR NOT _built EQUAL 0 OR NOT EXISTS "${_dir}/build/reactor.wasm")
        # The cause: the compiler's or the linker's own `error:` line, else the line CMake explains its
        # error with (the one after "CMake Error at <file>:<line>"), else the output's last line.
        string(REGEX MATCH "[^\n]*error: [^\n]*" _cause "${_output}")
        if(NOT _cause AND "${_output}" MATCHES "CMake Error[^\n]*\n[ \t\n]*([^\n]+)")
            set(_cause "${CMAKE_MATCH_1}")
        endif()
        if(NOT _cause)
            string(STRIP "${_output}" _cause)
            string(REGEX REPLACE ".*\n" "" _cause "${_cause}")
        endif()
        string(STRIP "${_cause}" _cause)
        _contour_wasi_cxx_missing(
            "${CONTOUR_WASI_CXX_COMPILER} with the sysroot ${CONTOUR_WASI_SYSROOT} cannot build a C++23 reactor "
            "for wasm32-wasip1 (${_cause})")
    endif()
    set(_CONTOUR_WASI_CXX_PROVEN "${_key}" CACHE INTERNAL "The WASI C++ toolchain a C++23 reactor was built with")
endfunction()

_contour_wasi_cxx_locate()
# What every configuration built with the toolchain is handed: the toolchain file, and what it reads.
set(_contour_wasi_toolchain_args "-DCMAKE_TOOLCHAIN_FILE=${CONTOUR_WASI_CXX_TOOLCHAIN_FILE}")
foreach(_variable IN LISTS _contour_wasi_variables)
    list(APPEND _contour_wasi_toolchain_args "-D${_variable}=${${_variable}}")
endforeach()
_contour_wasi_cxx_prove()
message(STATUS "WASI C++ toolchain: ${CONTOUR_WASI_CXX_COMPILER}, sysroot ${CONTOUR_WASI_SYSROOT}")

# The separator a list value is written with in a guest's CMAKE_ARGS: ExternalProject would split a `;`.
set(CONTOUR_WASM_CXX_LIST_SEPARATOR "|")

# glaze, as Contour resolved it (cmake/ContourThirdParties.cmake): a source tree is handed to the guest's
# FetchContent, a system package (header-only) by its include directories. Those are the build-tree half of
# their `$<BUILD_INTERFACE:…>`, without the install half; any other generator expression cannot be resolved
# at configure time and fails here.
if(glaze_SOURCE_DIR)
    set(_contour_wasi_glaze_args "-DFETCHCONTENT_SOURCE_DIR_GLAZE=${glaze_SOURCE_DIR}")
else()
    get_target_property(_contour_wasi_glaze_entries glaze::glaze INTERFACE_INCLUDE_DIRECTORIES)
    set(_contour_wasi_glaze_include "")
    foreach(_entry IN LISTS _contour_wasi_glaze_entries)
        if(_entry MATCHES "^\\$<BUILD_INTERFACE:(.*)>$")
            list(APPEND _contour_wasi_glaze_include "${CMAKE_MATCH_1}")
        elseif(_entry MATCHES "^\\$<INSTALL_INTERFACE:")
            continue()
        elseif(_entry MATCHES "\\$<")
            message(FATAL_ERROR "glaze's include directory ${_entry} cannot be handed to a WebAssembly guest.")
        else()
            list(APPEND _contour_wasi_glaze_include "${_entry}")
        endif()
    endforeach()
    list(JOIN _contour_wasi_glaze_include "${CONTOUR_WASM_CXX_LIST_SEPARATOR}" _contour_wasi_glaze_include)
    set(_contour_wasi_glaze_args "-DORCHESTRATOR_GLAZE_INCLUDE_DIR=${_contour_wasi_glaze_include}")
endif()

# contour_add_wasm_cxx_guest(NAME <name> SOURCE_DIR <dir> OUTPUT_VAR <var> [CMAKE_ARGS <args>...])
#
# Builds the CMake project at <dir> with the WASI C++ toolchain, offline (FETCHCONTENT_FULLY_DISCONNECTED),
# in a sub-build of its own: ExternalProject <name>_wasm_cxx, run on every build so that it sees its own
# sources change, behind the target <name>_wasm. The project builds an executable target named <name>, which
# lands at ${CMAKE_BINARY_DIR}/wasm/<name>.wasm; that path is set in <var> in the caller's scope. Extra
# CMAKE_ARGS go to the project's configuration last; a list value in them is joined with
# CONTOUR_WASM_CXX_LIST_SEPARATOR, and arrives as a list.
function(contour_add_wasm_cxx_guest)
    cmake_parse_arguments(_ARG "" "NAME;SOURCE_DIR;OUTPUT_VAR" "CMAKE_ARGS" ${ARGN})
    foreach(_required IN ITEMS NAME SOURCE_DIR OUTPUT_VAR)
        if(NOT _ARG_${_required})
            message(FATAL_ERROR "contour_add_wasm_cxx_guest: ${_required} is required")
        endif()
    endforeach()

    set(_output_dir "${CMAKE_BINARY_DIR}/wasm")
    set(_output "${_output_dir}/${_ARG_NAME}.wasm")

    ExternalProject_Add(${_ARG_NAME}_wasm_cxx
        SOURCE_DIR "${_ARG_SOURCE_DIR}"
        BINARY_DIR "${_output_dir}/${_ARG_NAME}-build"
        # Not every guest uses glaze or FetchContent; the arguments that hand them over are no warning.
        CMAKE_ARGS --no-warn-unused-cli
                   ${_contour_wasi_toolchain_args}
                   -DCMAKE_BUILD_TYPE=Release
                   -DFETCHCONTENT_FULLY_DISCONNECTED=ON
                   ${_contour_wasi_glaze_args}
                   "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${_output_dir}"
                   ${_ARG_CMAKE_ARGS}
        LIST_SEPARATOR "${CONTOUR_WASM_CXX_LIST_SEPARATOR}"
        BUILD_ALWAYS ON
        BUILD_BYPRODUCTS "${_output}"
        INSTALL_COMMAND ""
    )
    add_custom_target(${_ARG_NAME}_wasm DEPENDS ${_ARG_NAME}_wasm_cxx)
    set(${_ARG_OUTPUT_VAR} "${_output}" PARENT_SCOPE)
endfunction()
