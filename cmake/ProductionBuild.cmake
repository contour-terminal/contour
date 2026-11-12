# Production build: link-time optimization and the platform's exploit mitigations, for Contour's own
# code only.
#
# Included from the top-level CMakeLists.txt after the dependencies are fetched and before `src/` and
# `examples/` are added, so every target created after the include is first-party and every dependency
# (vcpkg ports, Qt, CPM's `_deps`, vendor/core-cpp) is untouched. The flags are directory-scoped
# (`add_compile_options`, `CMAKE_*` variables): they reach the subdirectories added afterwards and
# nothing created before. The top-level directory itself only holds INTERFACE and custom targets.
#
# The configurations that cannot be a production build are refused earlier, by ProductionBuildRefusals.cmake,
# before CPM and FetchContent fetch their dependencies (vcpkg installs its ports even earlier, in project()); the
# toolchain without a flag table is refused here. scripts/check-production-build.py verifies the result against
# the manifest this file writes.

include_guard(GLOBAL)

include(CheckCXXCompilerFlag)
include(CheckIPOSupported)
include(CheckLinkerFlag)

## Registers the check of a production build's main binary; a no-op unless CONTOUR_PRODUCTION_BUILD is ON.
## Call it once: the check is registered as the test `check_production_build`, the name CI selects.
## @param target The executable target whose linked binary the check reads.
function(contour_production_executable target)
    if(NOT CONTOUR_PRODUCTION_BUILD)
        return()
    endif()
    if(NOT CONTOUR_TESTING)
        return()
    endif()
    if(NOT Python3_Interpreter_FOUND)
        message(WARNING "CONTOUR_PRODUCTION_BUILD=ON found no Python 3 interpreter, so the test "
            "check_production_build is not registered and the production flags go unchecked.")
        return()
    endif()
    set(_command
        "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/check-production-build.py"
        --manifest "${PROJECT_BINARY_DIR}/production-build.json"
        --binary "$<TARGET_FILE:${target}>")
    add_test(NAME check_production_build COMMAND ${_command})
    set_tests_properties(check_production_build PROPERTIES LABELS production)
endfunction()

if(NOT CONTOUR_PRODUCTION_BUILD)
    return()
endif()

# --- the platform table --------------------------------------------------------------------------

# <platform key>|<binary format>|<probe tool>; "-" for no probe. The probe tool is what
# scripts/check-production-build.py runs on the binary. A Mach-O binary has no probe: its only checked
# properties were its debug symbols, which no build produces any more, so macOS is checked through the
# compile database alone.
set(_contour_platforms
    "msvc|pe|dumpbin"
    "linux|elf|readelf"
    "macos|macho|-")

set(_contour_keys "")
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(_contour_keys msvc)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
    set(_contour_keys linux)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        list(APPEND _contour_keys linux-x86_64)
    endif()
elseif(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang)$")
    set(_contour_keys macos)
else()
    message(FATAL_ERROR "CONTOUR_PRODUCTION_BUILD=ON supports MSVC on Windows, GCC or Clang on Linux and "
        "AppleClang or Clang on macOS, but this is ${CMAKE_CXX_COMPILER_ID} on ${CMAKE_SYSTEM_NAME}.")
endif()

foreach(_row IN LISTS _contour_platforms)
    string(REPLACE "|" ";" _fields "${_row}")
    list(GET _fields 0 _key)
    if(_key IN_LIST _contour_keys)
        list(GET _fields 1 _contour_format)
        list(GET _fields 2 _contour_probe)
    endif()
endforeach()

# --- whole-program optimization ------------------------------------------------------------------

check_ipo_supported(RESULT _contour_ipo_ok OUTPUT _contour_ipo_output LANGUAGES C CXX)
if(NOT _contour_ipo_ok)
    message(FATAL_ERROR "CONTOUR_PRODUCTION_BUILD=ON needs link-time optimization, "
        "which ${CMAKE_CXX_COMPILER_ID} does not support here: ${_contour_ipo_output}")
endif()
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
if("macos" IN_LIST _contour_keys)
    # CMake applies the IPO option to the C and C++ sources only; the Objective-C and Objective-C++ ones
    # (src/text_shaper/CoreTextLocator.mm, ...) would otherwise stay out of the link-time optimization.
    add_compile_options("$<$<COMPILE_LANGUAGE:OBJC,OBJCXX>:${CMAKE_CXX_COMPILE_OPTIONS_IPO}>")
endif()

# --- the flag table ------------------------------------------------------------------------------

# <platform key>|<where>|<flag>. where: compile (C/C++ compile lines), link (executable and
# shared-library link lines), both (code-generation flags, which LTO wants at link time too).
#
# Linux builds a position-independent executable with the rows -fPIE and -pie, not with
# CMAKE_POSITION_INDEPENDENT_CODE: that one compiles every static library with -fPIC, whose calls and data go
# through the PLT and GOT, and cost the tui workload 2 to 3 % of throughput. -fPIE objects cannot go into a
# shared or module library, so none may be first-party: that is checked, after all targets exist, at the end
# of this file. Code that links Qt keeps the -fPIC Qt requires (Qt6::Core's INTERFACE_POSITION_INDEPENDENT_CODE),
# which is why the flag itself is not forbidden.
set(_contour_production_flags
    "msvc|compile|/Gw"
    "msvc|compile|/Gy"
    "msvc|both|/guard:cf"
    "msvc|link|/OPT:REF"
    "msvc|link|/OPT:ICF"
    "msvc|link|/CETCOMPAT"
    "linux|compile|-fPIE"
    "linux|compile|-U_FORTIFY_SOURCE"
    "linux|compile|-D_FORTIFY_SOURCE=3"
    "linux|both|-fstack-protector-strong"
    "linux|both|-fstack-clash-protection"
    "linux-x86_64|both|-fcf-protection"
    "linux|link|-pie"
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

list(APPEND _contour_compile_flags ${CMAKE_CXX_COMPILE_OPTIONS_IPO})
set(_contour_tool "")
if(NOT _contour_probe STREQUAL "-")
    get_filename_component(_contour_linker_dir "${CMAKE_LINKER}" DIRECTORY)
    get_filename_component(_contour_readelf_dir "${CMAKE_READELF}" DIRECTORY)
    find_program(CONTOUR_PRODUCTION_PROBE ${_contour_probe}
        HINTS "${_contour_linker_dir}" "${_contour_readelf_dir}" REQUIRED)
    file(TO_CMAKE_PATH "${CONTOUR_PRODUCTION_PROBE}" _contour_tool)
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

# The directories whose targets are Contour's own (relative to the source directory): the flags above reach
# them, the manifest names them, and no target in them may be a shared or module library.
set(_contour_first_party_dirs src examples)

file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}" _contour_source_dir)
file(TO_CMAKE_PATH "${PROJECT_BINARY_DIR}" _contour_binary_dir)
_contour_json_array(_contour_compile_json ${_contour_compile_flags})

set(_contour_manifest "{}")
string(JSON _contour_manifest SET "${_contour_manifest}" format "\"${_contour_format}\"")
string(JSON _contour_manifest SET "${_contour_manifest}" source_dir "\"${_contour_source_dir}\"")
string(JSON _contour_manifest SET "${_contour_manifest}" binary_dir "\"${_contour_binary_dir}\"")
_contour_json_array(_contour_first_party_json ${_contour_first_party_dirs})
string(JSON _contour_manifest SET "${_contour_manifest}" first_party_dirs "${_contour_first_party_json}")
string(JSON _contour_manifest SET "${_contour_manifest}" third_party_dirs "[\"_deps\",\"vendor\"]")
string(JSON _contour_manifest SET "${_contour_manifest}" compile_flags "${_contour_compile_json}")
if(_contour_tool)
    string(JSON _contour_manifest SET "${_contour_manifest}" tool "\"${_contour_tool}\"")
endif()
file(WRITE "${PROJECT_BINARY_DIR}/production-build.json" "${_contour_manifest}\n")

# --- no first-party shared or module library -----------------------------------------------------

## Fails when a target of @p directory or of a directory below it is a shared or module library: -fPIE
## objects cannot be linked into one, and the link would die with a misleading "undefined reference to main".
## @param directory A source directory to walk, with its subdirectories.
function(_contour_forbid_shared_libraries directory)
    get_property(_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_target IN LISTS _targets)
        get_target_property(_type ${_target} TYPE)
        if(_type STREQUAL "SHARED_LIBRARY" OR _type STREQUAL "MODULE_LIBRARY")
            message(FATAL_ERROR "CONTOUR_PRODUCTION_BUILD=ON builds first-party code with -fPIE, which cannot go "
                "into the ${_type} '${_target}' (${directory}): make it static, or give it -fPIC in the flag table.")
        endif()
    endforeach()
    get_property(_subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(_subdirectory IN LISTS _subdirectories)
        _contour_forbid_shared_libraries("${_subdirectory}")
    endforeach()
endfunction()

## Walks the first-party directories once every target exists, at the end of the top-level directory.
function(_contour_check_first_party_libraries)
    foreach(_directory IN LISTS _contour_first_party_dirs)
        _contour_forbid_shared_libraries("${PROJECT_SOURCE_DIR}/${_directory}")
    endforeach()
endfunction()

cmake_language(DEFER CALL _contour_check_first_party_libraries)
