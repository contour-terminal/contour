# Production build: link-time optimization and the platform's exploit mitigations, for Contour's own
# code only.
#
# Included from the top-level CMakeLists.txt after the dependencies are fetched and before `src/` and
# `examples/` are added, so every target created after the include is first-party and every dependency
# (vcpkg ports, Qt, CPM's `_deps`, vendor/core-cpp) is untouched. The flags are directory-scoped
# (`add_compile_options`, `CMAKE_*` variables): they reach the subdirectories added afterwards and
# nothing created before. The top-level directory itself only holds INTERFACE and custom targets.
#
# scripts/check-production-build.py verifies the result against the manifest this file writes.

include_guard(GLOBAL)

include(CheckCXXCompilerFlag)
include(CheckIPOSupported)
include(CheckLinkerFlag)
include(CheckPIESupported)

## Registers the check of a production build's main binary; a no-op unless CONTOUR_PRODUCTION_BUILD is ON.
## @param target The executable target whose linked binary (and PDB, on MSVC) the check reads.
function(contour_production_executable target)
    if(NOT CONTOUR_PRODUCTION_BUILD)
        return()
    endif()
    if(CONTOUR_TESTING AND Python3_Interpreter_FOUND)
        set(_command
            "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/check-production-build.py"
            --manifest "${PROJECT_BINARY_DIR}/production-build.json"
            --binary "$<TARGET_FILE:${target}>")
        if(CONTOUR_PRODUCTION_PLATFORM STREQUAL "msvc")
            list(APPEND _command --symbols "$<TARGET_PDB_FILE:${target}>")
        endif()
        add_test(NAME check_production_build COMMAND ${_command})
        set_tests_properties(check_production_build PROPERTIES LABELS production)
    endif()
endfunction()

if(NOT CONTOUR_PRODUCTION_BUILD)
    return()
endif()

# --- refusals: a configuration whose result would not be what the packages ship ------------------

if(NOT CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    message(FATAL_ERROR "CONTOUR_PRODUCTION_BUILD=ON needs CMAKE_BUILD_TYPE=RelWithDebInfo, "
        "but it is '${CMAKE_BUILD_TYPE}': the packages are RelWithDebInfo builds, and so are the binaries CI tests.")
endif()
if(NOT CONTOUR_SANITIZE STREQUAL "OFF")
    message(FATAL_ERROR "CONTOUR_PRODUCTION_BUILD=ON cannot be combined with CONTOUR_SANITIZE=${CONTOUR_SANITIZE}: "
        "a sanitized binary is not the one that is shipped.")
endif()
if(CONTOUR_CODE_COVERAGE_ENABLED)
    message(FATAL_ERROR "CONTOUR_PRODUCTION_BUILD=ON cannot be combined with CONTOUR_CODE_COVERAGE_ENABLED=${CONTOUR_CODE_COVERAGE_ENABLED} "
        "(CONTOUR_COVERAGE=${CONTOUR_COVERAGE}): an instrumented binary is not the one that is shipped.")
endif()

# --- platform keys -------------------------------------------------------------------------------

set(_contour_keys "")
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    list(APPEND _contour_keys msvc)
    set(CONTOUR_PRODUCTION_PLATFORM msvc)
    set(_contour_format pe)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
    list(APPEND _contour_keys linux)
    set(CONTOUR_PRODUCTION_PLATFORM linux)
    set(_contour_format elf)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        list(APPEND _contour_keys linux-x86_64)
    endif()
elseif(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang)$")
    list(APPEND _contour_keys macos)
    set(CONTOUR_PRODUCTION_PLATFORM macos)
    set(_contour_format macho)
else()
    message(FATAL_ERROR "CONTOUR_PRODUCTION_BUILD=ON supports MSVC on Windows, GCC or Clang on Linux and "
        "AppleClang on macOS, but this is ${CMAKE_CXX_COMPILER_ID} on ${CMAKE_SYSTEM_NAME}.")
endif()
set(CONTOUR_PRODUCTION_PLATFORM "${CONTOUR_PRODUCTION_PLATFORM}" CACHE INTERNAL "Platform key of the production build")

# --- whole-program optimization ------------------------------------------------------------------

check_ipo_supported(RESULT _contour_ipo_ok OUTPUT _contour_ipo_output LANGUAGES C CXX)
if(NOT _contour_ipo_ok)
    message(FATAL_ERROR "CONTOUR_PRODUCTION_BUILD=ON needs link-time optimization, "
        "which ${CMAKE_CXX_COMPILER_ID} does not support here: ${_contour_ipo_output}")
endif()
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

# --- position-independent executables (Linux) ----------------------------------------------------

if("linux" IN_LIST _contour_keys)
    check_pie_supported(OUTPUT_VARIABLE _contour_pie_output LANGUAGES C CXX)
    if(NOT CMAKE_CXX_LINK_PIE_SUPPORTED)
        message(FATAL_ERROR "CONTOUR_PRODUCTION_BUILD=ON needs position-independent executables: ${_contour_pie_output}")
    endif()
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# --- MSVC: full inlining and no incremental linking ----------------------------------------------

if("msvc" IN_LIST _contour_keys)
    # RelWithDebInfo inlines only __inline functions (/Ob1); Release's /Ob2 inlines anything suitable.
    foreach(_var CMAKE_C_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_RELWITHDEBINFO)
        string(REGEX REPLACE "([/-])Ob1" "\\1Ob2" ${_var} "${${_var}}")
    endforeach()
    # CMake defaults RelWithDebInfo to `/debug /INCREMENTAL`; /LTCG ignores it with warning LNK4075.
    foreach(_var CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO
            CMAKE_MODULE_LINKER_FLAGS_RELWITHDEBINFO)
        string(REGEX REPLACE "/INCREMENTAL( |$)" "/INCREMENTAL:NO\\1" ${_var} "${${_var}}")
    endforeach()
endif()

# --- the flag table ------------------------------------------------------------------------------

# <platform key>|<where>|<flag>. where: compile (C/C++ compile lines), link (executable and
# shared-library link lines), both (code-generation flags, which LTO wants at link time too).
set(_contour_production_flags
    "msvc|compile|/Gw"
    "msvc|compile|/Gy"
    "msvc|both|/guard:cf"
    "msvc|link|/OPT:REF"
    "msvc|link|/OPT:ICF"
    "msvc|link|/CETCOMPAT"
    "linux|compile|-U_FORTIFY_SOURCE"
    "linux|compile|-D_FORTIFY_SOURCE=3"
    "linux|both|-fstack-protector-strong"
    "linux|both|-fstack-clash-protection"
    "linux-x86_64|both|-fcf-protection"
    "linux|link|-Wl,-z,relro"
    "linux|link|-Wl,-z,now"
    "linux|link|-Wl,--as-needed"
    "macos|link|-Wl,-dead_strip")

set(_contour_compile_flags "")
foreach(_row IN LISTS _contour_production_flags)
    string(REPLACE "|" ";" _fields "${_row}")
    list(GET _fields 0 _key)
    list(GET _fields 1 _where)
    list(GET _fields 2 _flag)
    if(NOT _key IN_LIST _contour_keys)
        continue()
    endif()
    string(MAKE_C_IDENTIFIER "CONTOUR_PRODUCTION_FLAG_${_where}_${_flag}" _result)
    if(_where STREQUAL "link")
        check_linker_flag(CXX "${_flag}" ${_result})
    else()
        check_cxx_compiler_flag("${_flag}" ${_result})
    endif()
    if(NOT ${_result})
        message(FATAL_ERROR "CONTOUR_PRODUCTION_BUILD=ON needs ${_flag}, which ${CMAKE_CXX_COMPILER_ID} rejects.")
    endif()
    if(NOT _where STREQUAL "link")
        add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:${_flag}>")
        list(APPEND _contour_compile_flags "${_flag}")
    endif()
    if(NOT _where STREQUAL "compile")
        add_link_options("${_flag}")
    endif()
endforeach()

# --- the manifest scripts/check-production-build.py reads ----------------------------------------

set(_contour_replaced_flags "")
list(APPEND _contour_compile_flags ${CMAKE_CXX_COMPILE_OPTIONS_IPO})
if("msvc" IN_LIST _contour_keys)
    list(APPEND _contour_compile_flags /Ob2)
    set(_contour_replaced_flags /Ob1)
    get_filename_component(_contour_linker_dir "${CMAKE_LINKER}" DIRECTORY)
    find_program(CONTOUR_DUMPBIN dumpbin HINTS "${_contour_linker_dir}" REQUIRED)
    set(_contour_tool "${CONTOUR_DUMPBIN}")
elseif("linux" IN_LIST _contour_keys)
    if(CMAKE_READELF)
        set(_contour_tool "${CMAKE_READELF}")
    else()
        find_program(CONTOUR_READELF readelf REQUIRED)
        set(_contour_tool "${CONTOUR_READELF}")
    endif()
else()
    find_program(CONTOUR_DWARFDUMP dwarfdump REQUIRED)
    set(_contour_tool "${CONTOUR_DWARFDUMP}")
endif()

## Renders a list as a JSON array of strings.
function(_contour_json_array out)
    set(_items "")
    foreach(_item IN LISTS ARGN)
        list(APPEND _items "\"${_item}\"")
    endforeach()
    list(JOIN _items "," _joined)
    set(${out} "[${_joined}]" PARENT_SCOPE)
endfunction()

file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}" _contour_source_dir)
file(TO_CMAKE_PATH "${PROJECT_BINARY_DIR}" _contour_binary_dir)
file(TO_CMAKE_PATH "${_contour_tool}" _contour_tool)
_contour_json_array(_contour_compile_json ${_contour_compile_flags})
_contour_json_array(_contour_replaced_json ${_contour_replaced_flags})

set(_contour_manifest "{}")
string(JSON _contour_manifest SET "${_contour_manifest}" format "\"${_contour_format}\"")
string(JSON _contour_manifest SET "${_contour_manifest}" source_dir "\"${_contour_source_dir}\"")
string(JSON _contour_manifest SET "${_contour_manifest}" binary_dir "\"${_contour_binary_dir}\"")
string(JSON _contour_manifest SET "${_contour_manifest}" first_party_dirs "[\"src\",\"examples\"]")
string(JSON _contour_manifest SET "${_contour_manifest}" third_party_dirs "[\"_deps\",\"vendor\"]")
string(JSON _contour_manifest SET "${_contour_manifest}" compile_flags "${_contour_compile_json}")
string(JSON _contour_manifest SET "${_contour_manifest}" replaced_flags "${_contour_replaced_json}")
string(JSON _contour_manifest SET "${_contour_manifest}" tool "\"${_contour_tool}\"")
file(WRITE "${PROJECT_BINARY_DIR}/production-build.json" "${_contour_manifest}\n")
