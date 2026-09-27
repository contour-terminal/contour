# SPDX-License-Identifier: Apache-2.0
#
# How a module directory declares its targets and its tests.
#
#   core_cpp_add_module(<name> KIND STATIC|INTERFACE|OBJECT
#                       [HEADERS <public header>...]
#                       [SOURCES <source or private header>...]
#                       [SOURCES_POSIX ...] [SOURCES_LINUX ...] [SOURCES_BSD ...] [SOURCES_WINDOWS ...]
#                       [SOURCES_EMSCRIPTEN ...]
#                       [PUBLIC_LIBS <lib>...] [PRIVATE_LIBS <lib>...]
#                       [NO_STATIC_RUNTIME_TWIN])
#
#   core_cpp_add_test(<module> [NAME <name>]
#                     [SOURCES ...] [SOURCES_POSIX ...] [SOURCES_LINUX ...]
#                     [SOURCES_BSD ...] [SOURCES_WINDOWS ...] [SOURCES_EMSCRIPTEN ...]
#                     [LIBS <lib>...] [LABELS <label>...] [DEFINITIONS <definition>...]
#                     [TIMEOUT <seconds>])
#
# Every test core_cpp_add_test() registers is bounded: 300 seconds unless TIMEOUT says otherwise.
# See the comment at the property for what the number protects against and how it was measured.
#
# core_cpp_add_module() creates the real target core-cpp-<name> and its alias
# core::<name>, and appends a compiled one to the global property CORE_CPP_TARGETS,
# which is how a parent project instruments core-cpp's code together with its own.
# HEADERS are the public headers; they form the target's HEADERS file
# set, based at src/ or, for a header CMake generates, at the generated include
# root, so the target is install-ready. Private headers (detail/, posix/, linux/,
# bsd/, darwin/, windows/, emscripten/) go in a SOURCES list and are in no file
# set.
#
# SOURCES is compiled everywhere the module builds, and each platform list where the
# platform source table below says. Under Emscripten the module row's PLATFORMS
# decides: an `any` module compiles SOURCES and SOURCES_EMSCRIPTEN, a `wasm-subset`
# module only SOURCES_EMSCRIPTEN, which lists the subset (Part I §1).
#
# It must be called from a directory that core_cpp_add_modules() entered for a row
# of the module table, and a target it links as core::<x> must be one its row allows:
# under the module's row, one of the same module or of a module the row lists in
# DEPS. That is how the table's layering is enforced rather than merely
# documented. A target with a core_cpp_module_target() row follows that row rather
# than its module's: it links only what the row's DEPS name, its KIND is checked
# against it, and where its PLATFORMS or WHEN say it does not build, it is not
# created. tests/cmake/check-layering.cmake proves each refusal.
#
# core_cpp_add_test() builds core-cpp-<module>-test from the module's *_test.cpp
# files, links it with core::<module> (when that target exists) and
# core::testing_main, and registers it with ctest as core-cpp.<module>. A module
# with a second test binary names it: NAME <name> builds core-cpp-<name>-test and
# registers core-cpp.<name>, with the module's labels all the same. When <name> is a
# target of the module, the binary links that target instead of core::<module>, and
# builds where that target's row says it does. DEFINITIONS
# are compile definitions of that binary alone, PRIVATE like every flag here: a
# definition that changes what a header declares must hold for every translation
# unit of a program, so it gets a binary of its own rather than a few of its files.

include_guard(GLOBAL)

# The exit status of a test binary whose every test case was skipped. ctest reports
# such a run as skipped (SKIP_RETURN_CODE), core::testing_main returns it
# (core/Config.hpp), and core::testing_main carries it as the target property
# CORE_CPP_SKIP_EXIT_CODE for consumers that register their own tests.
set(CORE_CPP_SKIP_EXIT_CODE 77)

# "<keyword>|<variable>": the platform-specific source list <keyword> is compiled
# when <variable> is true. BSD includes macOS, which shares its kqueue. POSIX means a
# native POSIX system: Emscripten sets UNIX and its libc emulates much of POSIX, but
# it has no pipes, signals, sockets or processes to speak of, so a SOURCES_POSIX file
# is not compiled there; SOURCES_EMSCRIPTEN is.
set(CORE_CPP_PLATFORM_POSIX OFF)
if(UNIX AND NOT EMSCRIPTEN)
    set(CORE_CPP_PLATFORM_POSIX ON)
endif()
set(CORE_CPP_PLATFORM_BSD OFF)
if(APPLE OR BSD)
    set(CORE_CPP_PLATFORM_BSD ON)
endif()
set(CORE_CPP_PLATFORM_SOURCE_TABLE
    "SOURCES_POSIX|CORE_CPP_PLATFORM_POSIX"
    "SOURCES_LINUX|LINUX"
    "SOURCES_BSD|CORE_CPP_PLATFORM_BSD"
    "SOURCES_WINDOWS|WIN32"
    "SOURCES_EMSCRIPTEN|EMSCRIPTEN"
)
set(CORE_CPP_SOURCE_KEYWORDS SOURCES)

# Static-CRT twins (CORE_CPP_MSVC_STATIC_RUNTIME_VARIANTS). The MSVC ABI records the C runtime a
# translation unit was compiled against, and the linker refuses to mix them (/failifmismatch
# RuntimeLibrary). A parent that builds a /MD daemon and a /MT tool in one build -- fastcached's
# fastcache-cc is installed as a bare exe, so it links the CRT statically -- cannot link both
# against one core-cpp library, so the option declares a second, static-CRT copy of each compiled
# module beside the first. Every other compiler has one C runtime, so there the option is ignored,
# and says so once.
set(CORE_CPP_BUILD_STATIC_RUNTIME_VARIANTS OFF)
if(CORE_CPP_MSVC_STATIC_RUNTIME_VARIANTS)
    if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        set(CORE_CPP_BUILD_STATIC_RUNTIME_VARIANTS ON)
        message(STATUS "[core-cpp] static-CRT twins: every compiled module also builds as core::<name>_mt")
    else()
        message(STATUS
            "[core-cpp] CORE_CPP_MSVC_STATIC_RUNTIME_VARIANTS is ignored: ${CMAKE_CXX_COMPILER_ID} does not "
            "target the MSVC ABI, which is the only one with a choice of C runtime")
    endif()
endif()
foreach(_coreCppRow IN LISTS CORE_CPP_PLATFORM_SOURCE_TABLE)
    string(REGEX REPLACE "\\|.*$" "" _coreCppKeyword "${_coreCppRow}")
    list(APPEND CORE_CPP_SOURCE_KEYWORDS ${_coreCppKeyword})
endforeach()

## @brief Sets @p outVar to the sources the parsed arguments with prefix @p prefix
## select for this platform: SOURCES plus every platform list that applies. A module
## whose row says PLATFORMS @p platforms wasm-subset has only its SOURCES_EMSCRIPTEN
## under Emscripten.
function(core_cpp_selected_sources prefix platforms outVar)
    set(sources ${${prefix}_SOURCES})
    if(EMSCRIPTEN AND platforms STREQUAL "wasm-subset")
        set(sources "")
    endif()
    foreach(row IN LISTS CORE_CPP_PLATFORM_SOURCE_TABLE)
        string(REPLACE "|" ";" fields "${row}")
        list(GET fields 0 keyword)
        list(GET fields 1 platform)
        if(${platform})
            list(APPEND sources ${${prefix}_${keyword}})
        endif()
    endforeach()
    set(${outVar} "${sources}" PARENT_SCOPE)
endfunction()

## @brief Sets @p outVar to ON when a table row with PLATFORMS @p platforms and WHEN
## @p when builds in this configuration: its option, if it names one, is ON, and it is
## not native-only in an Emscripten build.
function(core_cpp_row_builds platforms when outVar)
    set(builds ON)
    if(when AND NOT ${when})
        set(builds OFF)
    endif()
    if(EMSCRIPTEN AND platforms STREQUAL "native")
        set(builds OFF)
    endif()
    set(${outVar} ${builds} PARENT_SCOPE)
endfunction()

## @brief Sets @p outPrefix_PLATFORMS, _WHEN, _KIND and _DEPS to the table row that holds
## target @p name of module @p module: its own core_cpp_module_target() row when it has one,
## else the module's row, and @p outPrefix_OWN to ON for the former. _KIND is empty for a
## target other than the module's own that has no row, whose kind the table does not state.
function(core_cpp_target_row name module outPrefix)
    set(own OFF)
    if(DEFINED CORE_CPP_TARGET_${name}_MODULE)
        if(NOT CORE_CPP_TARGET_${name}_MODULE STREQUAL module)
            message(FATAL_ERROR
                "core-cpp target ${name} is declared for module '${CORE_CPP_TARGET_${name}_MODULE}' in "
                "cmake/CoreCppModules.cmake, but module '${module}' builds it.")
        endif()
        set(prefix CORE_CPP_TARGET_${name})
        set(own ON)
    else()
        set(prefix CORE_CPP_MODULE_${module})
    endif()
    set(kind "${${prefix}_KIND}")
    if(NOT name STREQUAL module AND NOT own)
        set(kind "")
    endif()
    foreach(field IN ITEMS PLATFORMS WHEN DEPS)
        set(${outPrefix}_${field} "${${prefix}_${field}}" PARENT_SCOPE)
    endforeach()
    set(${outPrefix}_KIND "${kind}" PARENT_SCOPE)
    set(${outPrefix}_OWN ${own} PARENT_SCOPE)
endfunction()

## @brief Refuses a core::<x> in @p libs that target @p name of module @p module may not link,
## by the row that holds it (core_cpp_target_row). A row of the target's own is the whole
## list: its DEPS name a target of the same module by that target's name, and another module
## by the module's. The module's row lets a target link any target of the module besides
## those of the modules it lists.
function(core_cpp_check_layering name module libs)
    core_cpp_target_row(${name} ${module} row)
    set(target core-cpp-${name})
    foreach(lib IN LISTS libs)
        if(NOT lib MATCHES "^core::(.+)$")
            continue()
        endif()
        set(linked "${CMAKE_MATCH_1}")
        if(NOT TARGET core-cpp-${linked})
            message(FATAL_ERROR
                "${target} links ${lib}, which does not exist (yet). A module may only link modules "
                "declared above it in cmake/CoreCppModules.cmake.")
        endif()
        get_target_property(owner core-cpp-${linked} CORE_CPP_MODULE)
        if(row_OWN)
            set(entry "${owner}")
            if(owner STREQUAL module)
                set(entry "${linked}")
            endif()
            if(NOT entry IN_LIST row_DEPS)
                set(named "they name: '${row_DEPS}'")
                if(NOT row_DEPS)
                    set(named "it has none, so the target links no core-cpp target")
                endif()
                message(FATAL_ERROR
                    "${target} links ${lib}, which the '${name}' row of cmake/CoreCppModules.cmake does not "
                    "allow: its DEPS do not name '${entry}' (${named}).")
            endif()
        elseif(NOT owner STREQUAL module AND NOT owner IN_LIST row_DEPS)
            message(FATAL_ERROR
                "${target} links ${lib} from module '${owner}', which the '${module}' row of "
                "cmake/CoreCppModules.cmake does not list in DEPS (it lists: '${row_DEPS}').")
        endif()
    endforeach()
endfunction()

## @brief Declares core-cpp-<@p name>-mt, alias core::<@p name>_mt: the static-CRT twin of the
## compiled module target @p name of module @p module, built from @p sources and linking what
## @p publicLibs and @p privateLibs link.
##
## Same sources, same flags (core_cpp_apply_toolchain), same include directories and usage
## requirements, and MSVC_RUNTIME_LIBRARY MultiThreaded[Debug]. A core::<x> it links becomes
## core::<x>_mt where x has a twin -- a compiled module -- and stays as it is where it has none,
## which is an INTERFACE module (core::async, core::net_types): header-only, so it has no C runtime
## to disagree about. Anything else (Threads, OpenSSL, libunicode) is linked as given, and a
## consumer that links a twin into a /MT program must supply those built /MT too.
##
## EXCLUDE_FROM_ALL, so a twin is built only when something links it: a parent that links
## core::net_mt builds base, log, platform and net twice and nothing else twice. It joins
## CORE_CPP_TARGETS like any compiled library, so a parent instruments it too, and it is not in
## CORE_CPP_HEADER_TARGETS: its headers are its original's, and are checked there.
function(core_cpp_add_static_runtime_twin name module sources publicLibs privateLibs)
    set(twin core-cpp-${name}-mt)
    add_library(${twin} STATIC EXCLUDE_FROM_ALL)
    add_library(core::${name}_mt ALIAS ${twin})
    set_target_properties(${twin} PROPERTIES
        CORE_CPP_MODULE "${module}"
        CORE_CPP_STATIC_RUNTIME_TWIN_OF "core-cpp-${name}"
        EXPORT_NAME "${name}_mt"
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_sources(${twin} PRIVATE ${sources})
    target_include_directories(${twin} PUBLIC
        "$<BUILD_INTERFACE:${CORE_CPP_SOURCE_DIR}/src>"
        "$<BUILD_INTERFACE:${CORE_CPP_GENERATED_INCLUDE_DIR}>")
    foreach(scope IN ITEMS PUBLIC PRIVATE)
        if(scope STREQUAL "PUBLIC")
            set(libs ${publicLibs})
        else()
            set(libs ${privateLibs})
        endif()
        set(mapped "")
        foreach(lib IN LISTS libs)
            # Two steps, not one `if(... MATCHES ... AND TARGET ...${CMAKE_MATCH_1}...)`: the variable
            # is expanded before the condition runs, so it would be the PREVIOUS match's.
            set(twinned "")
            if(lib MATCHES "^core::(.+)$")
                set(twinned "${CMAKE_MATCH_1}")
            endif()
            if(twinned AND TARGET core-cpp-${twinned}-mt)
                list(APPEND mapped core::${twinned}_mt)
            else()
                list(APPEND mapped ${lib})
            endif()
        endforeach()
        if(mapped)
            target_link_libraries(${twin} ${scope} ${mapped})
        endif()
    endforeach()
    core_cpp_apply_toolchain(${twin})
    set_property(GLOBAL APPEND PROPERTY CORE_CPP_TARGETS ${twin})
endfunction()

function(core_cpp_add_module name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "NO_STATIC_RUNTIME_TWIN" "KIND"
                          "HEADERS;${CORE_CPP_SOURCE_KEYWORDS};PUBLIC_LIBS;PRIVATE_LIBS")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "core_cpp_add_module(${name}): unexpected arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT DEFINED CORE_CPP_CURRENT_MODULE)
        message(FATAL_ERROR
            "core_cpp_add_module(${name}) is called outside a module directory. Add a core_cpp_module() "
            "row to cmake/CoreCppModules.cmake; core_cpp_add_modules() enters its directory.")
    endif()
    set(module "${CORE_CPP_CURRENT_MODULE}")
    core_cpp_target_row(${name} ${module} row)
    if(row_KIND AND NOT arg_KIND STREQUAL row_KIND)
        message(FATAL_ERROR
            "core_cpp_add_module(${name}) says KIND ${arg_KIND}; its row in cmake/CoreCppModules.cmake "
            "says ${row_KIND}.")
    endif()
    # A target whose row does not build here is not created; its directory asks
    # `if(TARGET core::<name>)` before it touches the target again.
    core_cpp_row_builds("${row_PLATFORMS}" "${row_WHEN}" builds)
    if(NOT builds)
        return()
    endif()
    core_cpp_selected_sources(arg "${row_PLATFORMS}" sources)

    set(target core-cpp-${name})
    if(arg_KIND STREQUAL "STATIC")
        if(NOT sources)
            message(FATAL_ERROR "core_cpp_add_module(${name}): a STATIC library needs sources; a header-only one is KIND INTERFACE.")
        endif()
        add_library(${target} STATIC)
        set(usage PUBLIC)
    elseif(arg_KIND STREQUAL "OBJECT")
        add_library(${target} OBJECT)
        set(usage PUBLIC)
    elseif(arg_KIND STREQUAL "INTERFACE")
        if(sources OR arg_PRIVATE_LIBS)
            message(FATAL_ERROR "core_cpp_add_module(${name}): an INTERFACE library has no sources and no private libraries.")
        endif()
        add_library(${target} INTERFACE)
        set(usage INTERFACE)
    else()
        message(FATAL_ERROR "core_cpp_add_module(${name}): KIND must be STATIC, INTERFACE or OBJECT, not '${arg_KIND}'.")
    endif()
    add_library(core::${name} ALIAS ${target})
    set_target_properties(${target} PROPERTIES CORE_CPP_MODULE "${module}" EXPORT_NAME "${name}")
    # Every target of the table, INTERFACE and OBJECT ones included and static-CRT twins not, in the
    # table's order: what core_cpp_install() installs (cmake/CoreCppInstall.cmake).
    set_property(GLOBAL APPEND PROPERTY CORE_CPP_MODULE_TARGETS ${target})

    if(sources)
        target_sources(${target} PRIVATE ${sources})
    endif()
    if(arg_HEADERS)
        # The generated headers (core/Config.hpp) are public too, based at the generated root.
        target_sources(${target} ${usage}
            FILE_SET HEADERS
            BASE_DIRS "${CORE_CPP_SOURCE_DIR}/src" "${CORE_CPP_GENERATED_INCLUDE_DIR}"
            FILES ${arg_HEADERS})
    endif()
    target_include_directories(${target} ${usage}
        "$<BUILD_INTERFACE:${CORE_CPP_SOURCE_DIR}/src>"
        "$<BUILD_INTERFACE:${CORE_CPP_GENERATED_INCLUDE_DIR}>")

    core_cpp_check_layering(${name} ${module} "${arg_PUBLIC_LIBS};${arg_PRIVATE_LIBS}")
    if(arg_PUBLIC_LIBS)
        target_link_libraries(${target} ${usage} ${arg_PUBLIC_LIBS})
    endif()
    if(arg_PRIVATE_LIBS)
        target_link_libraries(${target} PRIVATE ${arg_PRIVATE_LIBS})
    endif()

    if(arg_KIND STREQUAL "INTERFACE")
        target_compile_features(${target} INTERFACE cxx_std_23)
    else()
        core_cpp_apply_toolchain(${target})
        # Every compiled library of core-cpp, by its real name (a property cannot be set on an
        # alias), in the order the table declares them. A parent that instruments its build --
        # sanitizers, coverage -- applies the same to these, so its own code and core-cpp's are
        # instrumented alike; mixing the two is what makes ThreadSanitizer report races that are
        # not there, and is why CORE_CPP_SANITIZERS refuses to run in a subproject build
        # (cmake/CoreCppOptions.cmake). Test binaries are not here: a parent does not build them.
        set_property(GLOBAL APPEND PROPERTY CORE_CPP_TARGETS ${target})
    endif()
    # NO_STATIC_RUNTIME_TWIN: a module whose directory edits its target after this call, which the
    # twin would not mirror, or that links something only ever built /MD.
    if(arg_KIND STREQUAL "STATIC" AND CORE_CPP_BUILD_STATIC_RUNTIME_VARIANTS AND NOT arg_NO_STATIC_RUNTIME_TWIN)
        core_cpp_add_static_runtime_twin(${name} ${module} "${sources}" "${arg_PUBLIC_LIBS}" "${arg_PRIVATE_LIBS}")
    endif()

    # Every target that publishes headers, INTERFACE ones included -- which is why this is a list
    # of its own rather than CORE_CPP_TARGETS, whose members are the COMPILED libraries a parent
    # instruments. core::async is INTERFACE and publishes twenty of them, so a header self-check
    # reading CORE_CPP_TARGETS would silently cover no part of that module
    # (cmake/CoreCppHeaderSelfCheck.cmake, core-cpp#31).
    if(arg_HEADERS)
        set_property(GLOBAL APPEND PROPERTY CORE_CPP_HEADER_TARGETS ${target})
    endif()
endfunction()

function(core_cpp_add_test module)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "NAME;TIMEOUT" "${CORE_CPP_SOURCE_KEYWORDS};LIBS;LABELS;DEFINITIONS")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "core_cpp_add_test(${module}): unexpected arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT CORE_CPP_TESTING)
        return()
    endif()
    set(name "${module}")
    if(DEFINED arg_NAME)
        set(name "${arg_NAME}")
    endif()
    # A binary named after a target with a row of its own tests that target, and builds where it does.
    core_cpp_target_row(${name} ${module} row)
    core_cpp_row_builds("${row_PLATFORMS}" "${row_WHEN}" builds)
    if(NOT builds)
        return()
    endif()
    core_cpp_selected_sources(arg "${row_PLATFORMS}" sources)
    if(NOT sources)
        message(FATAL_ERROR "core_cpp_add_test(${module}): no test sources for this platform.")
    endif()

    set(target core-cpp-${name}-test)
    add_executable(${target} ${sources})
    set(libs ${arg_LIBS} core::testing_main)
    set(tested core::${module})
    if(TARGET core-cpp-${name})
        get_target_property(owner core-cpp-${name} CORE_CPP_MODULE)
        if(owner STREQUAL module)
            set(tested core::${name})
        endif()
    endif()
    if(TARGET ${tested})
        list(PREPEND libs ${tested})
    endif()
    target_link_libraries(${target} PRIVATE ${libs})
    if(arg_DEFINITIONS)
        target_compile_definitions(${target} PRIVATE ${arg_DEFINITIONS})
    endif()
    core_cpp_apply_toolchain(${target})

    # The variable rather than core::testing_main's property: a module declared before testing in
    # the table registers its test before that target exists.
    set(labels core-cpp ${module} ${arg_LABELS})
    add_test(NAME core-cpp.${name} COMMAND ${target})
    set_tests_properties(core-cpp.${name} PROPERTIES
        SKIP_RETURN_CODE ${CORE_CPP_SKIP_EXIT_CODE}
        LABELS "${labels}")
    # EVERY test this function registers is bounded, whether or not it asked to be. A case can HANG
    # rather than fail -- a lost readiness wake-up parks a flow with nothing left to resume it, a
    # wrapping loop stops making progress -- and both have happened in this tree. ctest's own
    # default is 1500 seconds, so an unbounded binary reports "Timeout" after 25 minutes of silence,
    # naming neither the case nor what it waited for. An opt-in bound would leave exactly the
    # binaries nobody thought about unbounded, which are the ones that need it.
    #
    # 300 seconds, measured rather than guessed: the slowest binary here is core-cpp.tui at 9.4s on
    # clang-debug, 11.1s under asan/ubsan and 9.7s on cl-debug, and every other one is under a
    # second. That is ~27x the slowest sanitizer run, so a 5x slowdown on a loaded runner still
    # leaves 5x of headroom, while a hang is named in five minutes.
    #
    # It reaches only what THIS function registers. A bare add_test() elsewhere keeps ctest's
    # 1500-second default unless it sets a TIMEOUT of its own, and two places register that way:
    # the checks in tests/, which set theirs per check because their runtimes differ by three orders
    # of magnitude (0.09s to 93s), and core-cpp.async-link-smoke in src/core/async/CMakeLists.txt.
    # Adding a bare add_test() means deciding its bound with it.
    #
    # The bound on a WAIT still belongs in the case, which can say what it waited for
    # (.agent/rules/testing.md). This only stops a missed one from costing 25 minutes.
    set(timeout 300)
    if(DEFINED arg_TIMEOUT)
        set(timeout ${arg_TIMEOUT}) # a binary that knows its own shape may bound itself tighter
    endif()
    set_tests_properties(core-cpp.${name} PROPERTIES TIMEOUT ${timeout})
endfunction()
