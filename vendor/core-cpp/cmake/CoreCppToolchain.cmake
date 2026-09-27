# SPDX-License-Identifier: Apache-2.0
#
# The toolchain policy of Part I §3, applied per target by core_cpp_apply_toolchain().
#
# Every flag here is PRIVATE to the target it is applied to. core-cpp never adds a
# PUBLIC or INTERFACE compile or link flag, so a consumer's own flags are never
# changed by linking a core-cpp target.

include_guard(GLOBAL)

include(CheckCXXCompilerFlag)
include(CheckCXXSourceCompiles)

# --- the compiler families the tables below are keyed on -----------------------
#
# MSVC is true for cl and for clang-cl: both take MSVC-style options, and both mean
# /Wall when given -Wall, which on clang-cl is -Weverything. CORE_CPP_GNU_DRIVER is
# GCC, Clang or AppleClang with GCC-style options.
set(CORE_CPP_CLANG OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CORE_CPP_CLANG ON)
endif()
set(CORE_CPP_GCC_OR_CLANG OFF)
if(CORE_CPP_CLANG OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(CORE_CPP_GCC_OR_CLANG ON)
endif()
set(CORE_CPP_GNU_DRIVER OFF)
if(CORE_CPP_GCC_OR_CLANG AND NOT MSVC)
    set(CORE_CPP_GNU_DRIVER ON)
endif()
set(CORE_CPP_MSVC_DRIVER OFF)
if(MSVC)
    set(CORE_CPP_MSVC_DRIVER ON)
endif()

# --- single-threaded WebAssembly ----------------------------------------------
#
# Without pthreads an Emscripten build must not link Threads::Threads: that would
# force -pthread, and with it SharedArrayBuffer, onto every consumer (Part I §1).
set(CORE_CPP_SINGLE_THREADED_WASM OFF)
if(EMSCRIPTEN)
    check_cxx_source_compiles(
        "#if !defined(__EMSCRIPTEN_PTHREADS__)\n#error single-threaded\n#endif\nint main() { return 0; }"
        CORE_CPP_EMSCRIPTEN_PTHREADS)
    if(NOT CORE_CPP_EMSCRIPTEN_PTHREADS)
        set(CORE_CPP_SINGLE_THREADED_WASM ON)
    endif()
endif()

# --- always --------------------------------------------------------------------

set(CORE_CPP_MSVC_DRIVER_FLAGS /utf-8 /permissive- /Zc:__cplusplus)
set(CORE_CPP_WINDOWS_DEFINITIONS NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)

# --- the pedantic set ---------------------------------------------------------
#
# The union of endo's cmake/PedanticCompiler.cmake (f774a210) and fastcached's
# cmake/portable/PedanticCompiler.cmake (eb9c9c68). Rows are "<family>|<flag>",
# where <family> names one of the variables above. Every flag of a row whose family
# applies is probed once, cached as CORE_CPP_HAS_<flag>, and dropped when the
# compiler does not know it, so one table serves GCC, Clang, AppleClang, clang-cl
# and cl.
#
# A row that turns a warning OFF (-Wno-<x>) says here why the finding cannot be
# fixed in core-cpp's code, and cites the core-cpp issue that tracks it if one can:
#
#   -Wno-c2y-extensions: Catch2's TEST_CASE and SECTION expand __COUNTER__, which
#       clang 22 classifies as a C2y extension under -Wpedantic. The expansion lands
#       in the test's own source, so Catch2 being a SYSTEM include does not hide it.
#       fastcached measured the same finding (PedanticCompiler.cmake at eb9c9c68).
set(CORE_CPP_PEDANTIC_TABLE
    "CORE_CPP_GNU_DRIVER|-Wall"
    "CORE_CPP_MSVC_DRIVER|/W4"
    "CORE_CPP_MSVC_DRIVER|/Zc:inline"
    "CORE_CPP_GCC_OR_CLANG|-Wextra"
    "CORE_CPP_GCC_OR_CLANG|-Wpedantic"
    "CORE_CPP_GCC_OR_CLANG|-Wconversion"
    "CORE_CPP_GCC_OR_CLANG|-Wsign-conversion"
    "CORE_CPP_GCC_OR_CLANG|-Wshadow"
    "CORE_CPP_GCC_OR_CLANG|-Wnon-virtual-dtor"
    "CORE_CPP_GCC_OR_CLANG|-Wold-style-cast"
    "CORE_CPP_GCC_OR_CLANG|-Wcast-align"
    "CORE_CPP_GCC_OR_CLANG|-Wunused"
    "CORE_CPP_GCC_OR_CLANG|-Woverloaded-virtual"
    "CORE_CPP_GCC_OR_CLANG|-Wnull-dereference"
    "CORE_CPP_GCC_OR_CLANG|-Wdouble-promotion"
    "CORE_CPP_GCC_OR_CLANG|-Wformat=2"
    "CORE_CPP_GCC_OR_CLANG|-Wimplicit-fallthrough"
    "CORE_CPP_GCC_OR_CLANG|-Wduplicate-enum"
    "CORE_CPP_GCC_OR_CLANG|-Wduplicated-cond"
    "CORE_CPP_GCC_OR_CLANG|-Wextra-semi"
    "CORE_CPP_GCC_OR_CLANG|-Wunused-template"
    "CORE_CPP_GCC_OR_CLANG|-Wfinal-dtor-non-final-class"
    "CORE_CPP_GCC_OR_CLANG|-Wlogical-op"
    "CORE_CPP_GCC_OR_CLANG|-Wmissing-declarations"
    "CORE_CPP_GCC_OR_CLANG|-Wnewline-eof"
    "CORE_CPP_GCC_OR_CLANG|-Wpessimizing-move"
    "CORE_CPP_GCC_OR_CLANG|-Wredundant-move"
    "CORE_CPP_GCC_OR_CLANG|-Wsuggest-destructor-override"
    "CORE_CPP_CLANG|-Wdangling-reference"
    "CORE_CPP_CLANG|-Wno-c2y-extensions"
)

# "<family>|<flag>": warnings become errors, with CORE_CPP_WERROR.
set(CORE_CPP_WERROR_TABLE
    "CORE_CPP_GNU_DRIVER|-Werror"
    "CORE_CPP_MSVC_DRIVER|/WX"
)

# "<name>|<GCC/Clang flags>|<MSVC-driver flags>": compile and link flags per
# sanitizer. An empty column means the family has no such sanitizer.
set(CORE_CPP_SANITIZER_TABLE
    "address|-fsanitize=address -fno-omit-frame-pointer|/fsanitize=address"
    "undefined|-fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer|"
    "thread|-fsanitize=thread -fno-omit-frame-pointer|"
    "leak|-fsanitize=leak -fno-omit-frame-pointer|"
)

# "<family>|<compile and link flags>": coverage. Clang uses source-based coverage
# (llvm-cov); GCC uses gcov.
set(CORE_CPP_COVERAGE_TABLE
    "CORE_CPP_CLANG|-fprofile-instr-generate -fcoverage-mapping"
    "CORE_CPP_GCC_OR_CLANG|--coverage"
)

## @brief Sets @p outVar to the flags of the "<family>|<flag>" rows in @p table whose
## family applies to this compiler, keeping only flags the compiler accepts.
function(core_cpp_probed_flags table outVar)
    set(flags "")
    foreach(row IN LISTS ${table})
        string(FIND "${row}" "|" bar)
        string(SUBSTRING "${row}" 0 ${bar} family)
        math(EXPR flagAt "${bar} + 1")
        string(SUBSTRING "${row}" ${flagAt} -1 flag)
        if(NOT ${family})
            continue()
        endif()
        string(REGEX REPLACE "^[-/]" "" probe "${flag}")
        string(MAKE_C_IDENTIFIER "CORE_CPP_HAS_${probe}" probe)
        check_cxx_compiler_flag("${flag}" ${probe})
        if(${probe})
            list(APPEND flags "${flag}")
        endif()
    endforeach()
    set(${outVar} "${flags}" PARENT_SCOPE)
endfunction()

## @brief Sets @p outVar to the flags of the first row of @p table (rows are
## "<family>|<flags>") whose family applies to this compiler, split into a list.
function(core_cpp_first_applicable_flags table outVar)
    set(flags "")
    foreach(row IN LISTS ${table})
        string(FIND "${row}" "|" bar)
        string(SUBSTRING "${row}" 0 ${bar} family)
        math(EXPR flagsAt "${bar} + 1")
        string(SUBSTRING "${row}" ${flagsAt} -1 rowFlags)
        if(${family})
            separate_arguments(flags NATIVE_COMMAND "${rowFlags}")
            break()
        endif()
    endforeach()
    set(${outVar} "${flags}" PARENT_SCOPE)
endfunction()

set(CORE_CPP_PEDANTIC_FLAGS "")
if(CORE_CPP_PEDANTIC)
    core_cpp_probed_flags(CORE_CPP_PEDANTIC_TABLE CORE_CPP_PEDANTIC_FLAGS)
endif()

set(CORE_CPP_WERROR_FLAGS "")
if(CORE_CPP_WERROR)
    core_cpp_first_applicable_flags(CORE_CPP_WERROR_TABLE CORE_CPP_WERROR_FLAGS)
endif()

set(CORE_CPP_SANITIZER_FLAGS "")
foreach(_coreCppSanitizer IN LISTS CORE_CPP_SANITIZERS)
    set(_coreCppSanitizerKnown OFF)
    foreach(_coreCppRow IN LISTS CORE_CPP_SANITIZER_TABLE)
        string(REPLACE "|" ";" _coreCppFields "${_coreCppRow}")
        list(GET _coreCppFields 0 _coreCppName)
        if(NOT _coreCppName STREQUAL _coreCppSanitizer)
            continue()
        endif()
        set(_coreCppSanitizerKnown ON)
        if(CORE_CPP_MSVC_DRIVER)
            list(GET _coreCppFields 2 _coreCppFlags)
        else()
            list(GET _coreCppFields 1 _coreCppFlags)
        endif()
        if(_coreCppFlags STREQUAL "")
            message(FATAL_ERROR "CORE_CPP_SANITIZERS: '${_coreCppSanitizer}' is not available with ${CMAKE_CXX_COMPILER_ID}.")
        endif()
        separate_arguments(_coreCppFlags NATIVE_COMMAND "${_coreCppFlags}")
        list(APPEND CORE_CPP_SANITIZER_FLAGS ${_coreCppFlags})
    endforeach()
    if(NOT _coreCppSanitizerKnown)
        message(FATAL_ERROR "CORE_CPP_SANITIZERS: unknown sanitizer '${_coreCppSanitizer}' (known: address, undefined, thread, leak).")
    endif()
endforeach()
list(REMOVE_DUPLICATES CORE_CPP_SANITIZER_FLAGS)

set(CORE_CPP_COVERAGE_FLAGS "")
if(CORE_CPP_COVERAGE)
    core_cpp_first_applicable_flags(CORE_CPP_COVERAGE_TABLE CORE_CPP_COVERAGE_FLAGS)
    if(NOT CORE_CPP_COVERAGE_FLAGS OR CORE_CPP_MSVC_DRIVER)
        message(FATAL_ERROR "CORE_CPP_COVERAGE: no coverage instrumentation for ${CMAKE_CXX_COMPILER_ID} with this driver.")
    endif()
endif()

set(CORE_CPP_CLANG_TIDY_COMMAND "")
if(CORE_CPP_CLANG_TIDY)
    # The plain name first: it is what the pinned PyPI release installs. A distribution's
    # clang-tidy-22 is a rolling snapshot whose findings can differ under the same version.
    find_program(CORE_CPP_CLANG_TIDY_EXE NAMES clang-tidy clang-tidy-22
                 DOC "clang-tidy used by CORE_CPP_CLANG_TIDY (pinned in .clang-tidy-version)")
    if(NOT CORE_CPP_CLANG_TIDY_EXE)
        message(FATAL_ERROR "CORE_CPP_CLANG_TIDY is ON but no clang-tidy was found; "
                            "install the pinned one with: python scripts/tool-versions.py --install")
    endif()
    # find_program keeps a value the user set, so `-DCORE_CPP_CLANG_TIDY_EXE=clang-tidy` stays a bare
    # name. CXX_CLANG_TIDY resolved that through PATH; an OBJECT_DEPENDS (core_cpp_tidy_inputs) does
    # not, and Ninja refuses every analysed compile over an input named `clang-tidy` that no rule
    # makes. So the analyser is a full path from here on.
    if(NOT IS_ABSOLUTE "${CORE_CPP_CLANG_TIDY_EXE}")
        find_program(_coreCppTidyResolved NAMES "${CORE_CPP_CLANG_TIDY_EXE}" NO_CACHE)
        if(NOT _coreCppTidyResolved)
            message(FATAL_ERROR "CORE_CPP_CLANG_TIDY_EXE is '${CORE_CPP_CLANG_TIDY_EXE}', which is not a "
                                "path and names no program on PATH.")
        endif()
        set(CORE_CPP_CLANG_TIDY_EXE "${_coreCppTidyResolved}")
    endif()
    execute_process(COMMAND "${CORE_CPP_CLANG_TIDY_EXE}" --version
                    OUTPUT_VARIABLE _coreCppTidyBanner ERROR_QUIET)
    string(REGEX MATCH "version ([0-9]+\\.[0-9]+\\.[0-9]+)" _coreCppTidyMatch "${_coreCppTidyBanner}")
    set(_coreCppTidyVersion "${CMAKE_MATCH_1}")
    message(STATUS "[core-cpp] clang-tidy ${_coreCppTidyVersion} (${CORE_CPP_CLANG_TIDY_EXE})")
    if(EXISTS "${CORE_CPP_SOURCE_DIR}/.clang-tidy-version")
        file(STRINGS "${CORE_CPP_SOURCE_DIR}/.clang-tidy-version" _coreCppTidyPin REGEX "^version:")
        string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" _coreCppTidyPin "${_coreCppTidyPin}")
        if(NOT _coreCppTidyVersion STREQUAL _coreCppTidyPin)
            message(WARNING
                "[core-cpp] ${CORE_CPP_CLANG_TIDY_EXE} is clang-tidy ${_coreCppTidyVersion}, but "
                ".clang-tidy-version pins ${_coreCppTidyPin}, which is what CI analyses with. Install the pin "
                "(python scripts/tool-versions.py --install) or point CORE_CPP_CLANG_TIDY_EXE at it.")
        endif()
    endif()
    set(CORE_CPP_CLANG_TIDY_COMMAND "${CORE_CPP_CLANG_TIDY_EXE}")
endif()

## @brief Sets @p outVar to what clang-tidy reads besides the source: every `.clang-tidy` from
## @p source's directory up to the source tree's root, and the analyser binary itself.
##
## Why these are dependencies at all (core-cpp#36): Ninja reruns a statement when an input or its
## command line changes, and neither of these is either. So editing `.clang-tidy`, or replacing the
## analyser at an unchanged path, re-analysed nothing whose object was current -- `no work to do`
## then meant "clean under whatever rules were in force when each object was built", not under
## today's. As OBJECT_DEPENDS, a change to any of them rebuilds, and so re-analyses, what it
## governs. A source outside the source tree (a generated one) gets the analyser alone.
##
## The walk runs at configure time, so a `.clang-tidy` ADDED to a directory later is not a dependency
## until the next configure; editing or deleting one that exists is seen at once.
function(core_cpp_tidy_inputs source outVar)
    set(inputs "${CORE_CPP_CLANG_TIDY_EXE}")
    cmake_path(IS_PREFIX CORE_CPP_SOURCE_DIR "${source}" NORMALIZE underSourceTree)
    if(underSourceTree)
        cmake_path(GET source PARENT_PATH directory)
        while(TRUE)
            if(EXISTS "${directory}/.clang-tidy")
                list(APPEND inputs "${directory}/.clang-tidy")
            endif()
            cmake_path(COMPARE "${directory}" EQUAL "${CORE_CPP_SOURCE_DIR}" atRoot)
            cmake_path(GET directory PARENT_PATH parent)
            if(atRoot OR parent STREQUAL directory)
                break()
            endif()
            set(directory "${parent}")
        endwhile()
    endif()
    set(${outVar} "${inputs}" PARENT_SCOPE)
endfunction()

## @brief Applies core-cpp's toolchain policy to @p target, one of its own compiled targets.
##
## The C++ standard is a usage requirement of a library (PUBLIC), because its headers
## need it. Everything else is PRIVATE.
function(core_cpp_apply_toolchain target)
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "EXECUTABLE")
        target_compile_features(${target} PRIVATE cxx_std_23)
    else()
        target_compile_features(${target} PUBLIC cxx_std_23)
    endif()
    set_target_properties(${target} PROPERTIES
        CXX_EXTENSIONS OFF
        CXX_SCAN_FOR_MODULES OFF
        CXX_CLANG_TIDY "${CORE_CPP_CLANG_TIDY_COMMAND}")
    if(CORE_CPP_CLANG_TIDY_COMMAND)
        get_target_property(sources ${target} SOURCES)
        get_target_property(sourceDir ${target} SOURCE_DIR)
        foreach(source IN LISTS sources)
            if(source MATCHES "^\\$<" OR NOT source MATCHES "\\.(c|cc|cpp|cxx)$")
                continue()
            endif()
            cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${sourceDir}" NORMALIZE OUTPUT_VARIABLE absolute)
            core_cpp_tidy_inputs("${absolute}" inputs)
            # A source shared by two targets (a runtime twin) is visited twice; append once.
            get_property(present SOURCE "${absolute}" TARGET_DIRECTORY ${target} PROPERTY OBJECT_DEPENDS)
            list(REMOVE_ITEM inputs ${present})
            if(inputs)
                set_property(SOURCE "${absolute}" TARGET_DIRECTORY ${target} APPEND PROPERTY OBJECT_DEPENDS ${inputs})
            endif()
        endforeach()
    endif()

    if(CORE_CPP_MSVC_DRIVER)
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${CORE_CPP_MSVC_DRIVER_FLAGS}>)
    endif()
    if(WIN32)
        target_compile_definitions(${target} PRIVATE ${CORE_CPP_WINDOWS_DEFINITIONS})
    endif()
    foreach(flags IN ITEMS CORE_CPP_PEDANTIC_FLAGS CORE_CPP_WERROR_FLAGS)
        if(${flags})
            target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${${flags}}>)
        endif()
    endforeach()

    # Instrumentation is compiled in and linked in. link.exe takes neither flag: on the
    # MSVC driver the compiler embeds what the linker needs.
    foreach(flags IN ITEMS CORE_CPP_SANITIZER_FLAGS CORE_CPP_COVERAGE_FLAGS)
        if(${flags})
            target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${${flags}}>)
            if(NOT CORE_CPP_MSVC_DRIVER)
                target_link_options(${target} PRIVATE ${${flags}})
            endif()
        endif()
    endforeach()
endfunction()
