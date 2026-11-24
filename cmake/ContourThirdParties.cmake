# This directory structure is being created by `scripts/install-deps.sh`
# and is used to inject all the dependencies the operating system's
# package manager did not provide (not found or too old version).

# {{{ helper: subproject_version(<subproject-name> <result-variable>)
#
# Extract version of a sub-project, which was previously included with add_subdirectory().
function(subproject_version subproject_name VERSION_VAR)
    # Read CMakeLists.txt for subproject and extract project() call(s) from it.
    file(STRINGS "${${subproject_name}_SOURCE_DIR}/CMakeLists.txt" project_calls REGEX "[ \t]*project\\(")
    # For every project() call try to extract its VERSION option
    foreach(project_call ${project_calls})
        string(REGEX MATCH "VERSION[ ]+\"?([^ \")]+)" version_param "${project_call}")
        if(version_param)
            set(version_value "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(version_value)
        set(${VERSION_VAR} "${version_value}" PARENT_SCOPE)
    else()
        message("WARNING: Cannot extract version for subproject '${subproject_name}'")
    endif()
endfunction(subproject_version)
# }}}

# {{{ helper: ContourThirdParties_ClearClangTidy(<directory>)
#
# Clears CXX_CLANG_TIDY on every target defined at or below <directory>, skipping our own src/ and
# examples/ trees.
#
# Vendored code is not ours to lint, but unsetting CMAKE_CXX_CLANG_TIDY around the dependency fetch
# is not sufficient: the embedded projects are CMake projects in their own right and run their own
# ClangTidy.cmake, and the ENABLE_TIDY cache variable this build is configured with is visible in
# their scope too -- so they turn clang-tidy back on for their own targets. Clearing the per-target
# property afterwards is order-independent and wins over any such re-enable.
function(ContourThirdParties_ClearClangTidy directory)
    # Our own trees are added after this module runs, so they should never be reachable here. Guard
    # anyway: silently dropping first-party code from linting is the one failure mode worth ruling out.
    if(directory STREQUAL "${PROJECT_SOURCE_DIR}/src" OR directory STREQUAL "${PROJECT_SOURCE_DIR}/examples")
        return()
    endif()

    get_property(subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(subdirectory IN LISTS subdirectories)
        ContourThirdParties_ClearClangTidy("${subdirectory}")
    endforeach()

    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(target_type ${target} TYPE)
        if(NOT target_type STREQUAL "INTERFACE_LIBRARY" AND NOT target_type STREQUAL "UTILITY")
            set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "")
        endif()
    endforeach()
endfunction(ContourThirdParties_ClearClangTidy)
# }}}

set(ContourThirdParties_SRCDIR ${PROJECT_SOURCE_DIR}/_deps/sources)
if(EXISTS "${ContourThirdParties_SRCDIR}/CMakeLists.txt")
    message(STATUS "Embedding 3rdparty libraries: ${ContourThirdParties_SRCDIR}")
    add_subdirectory(${ContourThirdParties_SRCDIR} EXCLUDE_FROM_ALL)
else()
    message(STATUS "No 3rdparty libraries found at ${ContourThirdParties_SRCDIR}")
endif()

macro(HandleThirdparty _TARGET _CPM_TARGET)
    cmake_parse_arguments(_HTP "" "VERSION" "" ${ARGN})
    if(TARGET ${_TARGET})
        set(THIRDPARTY_BUILTIN_${_TARGET} "embedded")
    else()
        if(_HTP_VERSION)
            find_package(${_TARGET} ${_HTP_VERSION} QUIET)
        else()
            find_package(${_TARGET} QUIET)
        endif()
        if(${_TARGET}_FOUND)
            set(THIRDPARTY_BUILTIN_${_TARGET} "system package")
        elseif(CONTOUR_USE_CPM)
            message(STATUS "Using CPM to fetch ${_TARGET}")
            CPMAddPackage(${_CPM_TARGET})
            set(THIRDPARTY_BUILTIN_${_TARGET} "embedded (CPM)")
        else()
            message(FATAL_ERROR "Could not find ${_TARGET}. "
                "Run install-deps.sh, install system packages, or set CONTOUR_USE_CPM=ON.")
        endif()
    endif()
endmacro()


message(STATUS "==============================================================================")
message(STATUS "    Contour ThirdParties: ${ContourThirdParties}")

set(LIBUNICODE_MINIMAL_VERSION "0.9.5")
set(BOXED_CPP_MINIMAL_VERSION "1.4.3")
set(TERMBENCH_PRO_COMMIT_HASH "3a39a4ad592047dee3038d8bfcce84215ac55032")
set(CATCH_VERSION "3.4.0")
set(YAML_CPP_VERSION "0.8.0")
set(HARFBUZZ_VERSION "8.4.0")
set(GSL_VERSION "3.1.0")
set(REFLECTION_CPP_VERSION "0.4.0")
# Bumping glaze: src/vtcontrol/Json.hpp specializes glz::to<glz::JSON, std::string> ahead of glaze's JSON
# headers, and src/vtcontrol/Reflect.hpp names glz::to_tie and glz::detail::count_members; re-run
# `vtcontrol_test "[json]"` to check the string writer is still the one glaze picks. Update GLAZE_SHA256 (of
# the release tarball), compare glaze_glaze below with the target glaze's own CMakeLists defines, and bump
# the version in scripts/install-deps.sh and scripts/install-deps.ps1 too.
set(GLAZE_VERSION "9.0.0")
set(GLAZE_SHA256 "dd7b033c5bf6e4308615bb7834c541291d5b20f64121d9a68222a39414635517")
set(FREETYPE_VERSION "2.10.0")
set(WAMR_MINIMAL_VERSION "2.4.5")


if(TARGET GSL)
    set(THIRDPARTY_BUILTIN_GSL "embedded")
else()
    if(WIN32)
        find_package(Microsoft.GSL CONFIG QUIET)
    else()
        find_package(Microsoft.GSL QUIET)
    endif()
    if(Microsoft.GSL_FOUND)
        set(THIRDPARTY_BUILTIN_GSL "system package")
    elseif(CONTOUR_USE_CPM)
        message(STATUS "Using CPM to fetch GSL")
        CPMAddPackage(
            NAME GSL
            GITHUB_REPOSITORY microsoft/GSL
            GIT_TAG v${GSL_VERSION}
            EXCLUDE_FROM_ALL YES
            OPTIONS "GSL_TEST=OFF"
        )
        set(THIRDPARTY_BUILTIN_GSL "embedded (CPM)")
    else()
        message(FATAL_ERROR "Could not find GSL. "
            "Run install-deps.sh, install system packages, or set CONTOUR_USE_CPM=ON.")
    endif()
endif()



if(CONTOUR_TESTING)
    HandleThirdparty(Catch2 "gh:catchorg/Catch2@${CATCH_VERSION}" VERSION ${CATCH_VERSION})
endif()
HandleThirdparty(yaml-cpp "gh:jbeder/yaml-cpp#${YAML_CPP_VERSION}" VERSION ${YAML_CPP_VERSION})
HandleThirdparty(Freetype "https://download.savannah.gnu.org/releases/freetype/freetype-${FREETYPE_VERSION}.tar.gz" VERSION ${FREETYPE_VERSION})
# harfbuzz is only needed for non static builds since qt provides own harfbuzz target
HandleThirdparty(HarfBuzz "gh:harfbuzz/harfbuzz#${HARFBUZZ_VERSION}" VERSION ${HARFBUZZ_VERSION})

if(COMMAND ContourThirdParties_Embed_libunicode)
    ContourThirdParties_Embed_libunicode()
    subproject_version(libunicode libunicode_version)
    if(NOT DEFINED libunicode_version OR libunicode_version VERSION_LESS LIBUNICODE_MINIMAL_VERSION)
        message(FATAL_ERROR "Embedded libunicode version must be at least ${LIBUNICODE_MINIMAL_VERSION}, but found ${libunicode_version}")
    endif()
    set(THIRDPARTY_BUILTIN_unicode_core "embedded")
else()
    if(NOT CONTOUR_BUILD_STATIC)
        find_package(libunicode ${LIBUNICODE_MINIMAL_VERSION} QUIET)
    endif()
    if(TARGET unicode::unicode OR TARGET unicode::core)
        set(THIRDPARTY_BUILTIN_libunicode "system package")
    elseif(CONTOUR_USE_CPM)
        message(STATUS "Using CPM to fetch libunicode")
        CPMAddPackage(
            NAME libunicode
            GITHUB_REPOSITORY contour-terminal/libunicode
            GIT_TAG v${LIBUNICODE_MINIMAL_VERSION}
            OPTIONS
                "LIBUNICODE_TESTING OFF"
                "LIBUNICODE_BENCHMARK OFF"
                "LIBUNICODE_TOOLS OFF"
                "LIBUNICODE_EXAMPLES OFF"
                "BUILD_SHARED_LIBS OFF"
            EXCLUDE_FROM_ALL YES
        )
        set(THIRDPARTY_BUILTIN_libunicode "CPM (v${LIBUNICODE_MINIMAL_VERSION}, static)")
    else()
        message(FATAL_ERROR "Could not find libunicode >= ${LIBUNICODE_MINIMAL_VERSION}. "
            "Run install-deps.sh, install system packages, or set CONTOUR_USE_CPM=ON.")
    endif()
endif()

# WAMR (WebAssembly Micro Runtime) hosts the status-line extensions, interpreted only: fast
# interpreter on, every JIT and AOT off, so no executable memory is ever mapped. libc-wasi is on
# (guests are wasm32-wasip1), the libc-builtin shim is off. The thread manager is required although
# guests get no threads: without it the interpreter never polls for wasm_runtime_terminate(), so the
# watchdog could not stop a call that loops. Hardware bounds checks are off (memory and native
# stack): they install a process-wide SIGSEGV handler, which is not a library's to take, and keep
# process-wide signal state that every worker thread's setup writes; the interpreter checks bounds in
# software instead. Waking blocked WASI calls is off as well: it installs a process-wide SIGUSR1
# handler, and guests cannot block (poll_oneoff is refused). An installed WAMR does not say how it was
# built, so a system package is accepted only once cmake/WamrTerminateProbe.cpp has shown that
# terminating works and that initializing the runtime leaves SIGSEGV and SIGUSR1 alone; otherwise WAMR
# is built from source.
#
# The agent orchestrator's guest (CONTOUR_EXTENSIONS_ORCHESTRATOR_GUEST) is compiled with bulk-memory,
# sign-extension and saturating float-to-int conversion, which a system WAMR may have been built without; with
# it, no system WAMR is looked for, also in a build that runs no test that would notice.
#
# First-party code links contour::wamr, whichever WAMR it resolves to.

# ContourThirdParties_WamrTerminates(<target> <result-variable>)
#
# Sets <result-variable> to TRUE if a program linked against <target> stops a looping guest call
# when another thread calls wasm_runtime_terminate() and its runtime initialization installs no
# SIGSEGV or SIGUSR1 handler, FALSE otherwise (including when the probe cannot run, as when cross-compiling).
# Sets <result-variable>_REASON to why a WAMR was rejected.
function(ContourThirdParties_WamrTerminates target resultVar)
    set(${resultVar} FALSE PARENT_SCOPE)
    set(${resultVar}_REASON "wasm_runtime_terminate() did not stop a call (built without WAMR_BUILD_THREAD_MGR?)" PARENT_SCOPE)
    if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
        return()
    endif()
    find_package(Threads QUIET)
    try_run(_wamr_probe_run _wamr_probe_compile
        SOURCES "${PROJECT_SOURCE_DIR}/cmake/WamrTerminateProbe.cpp"
        LINK_LIBRARIES ${target} Threads::Threads
        CXX_STANDARD 20
        NO_CACHE
    )
    if(_wamr_probe_compile AND "${_wamr_probe_run}" STREQUAL "0")
        set(${resultVar} TRUE PARENT_SCOPE)
    elseif(_wamr_probe_compile AND "${_wamr_probe_run}" STREQUAL "8")
        set(${resultVar}_REASON "it installs a SIGSEGV handler (built with hardware bounds checks)" PARENT_SCOPE)
    elseif(_wamr_probe_compile AND "${_wamr_probe_run}" STREQUAL "9")
        set(${resultVar}_REASON "it installs a SIGUSR1 handler (built with wakeup of blocking operations)" PARENT_SCOPE)
    endif()
endfunction()

# ContourThirdParties_FindWamrClangCl(<result-variable>)
#
# Sets <result-variable> to the clang-cl that compiles WAMR in an MSVC build, or to "" with
# <result-variable>_REASON saying why there is none. It is looked for first where the Visual Studio
# installer puts it ("C++ Clang tools for Windows"): cl.exe is <VS>/VC/Tools/MSVC/<version>/bin/Host<arch>/<arch>/,
# clang-cl <VS>/VC/Tools/Llvm/x64/bin/; then on the PATH. CONTOUR_WAMR_CLANG_CL_EXECUTABLE names another.
function(ContourThirdParties_FindWamrClangCl resultVar)
    set(${resultVar} "" PARENT_SCOPE)
    get_property(_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(_multi_config)
        set(${resultVar}_REASON "a multi-config generator, whose configuration WAMR's own build cannot follow" PARENT_SCOPE)
        return()
    endif()
    if(NOT CMAKE_C_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
        set(${resultVar}_REASON "a ${CMAKE_C_COMPILER_ARCHITECTURE_ID} target, and clang-cl is used for x64 only" PARENT_SCOPE)
        return()
    endif()
    cmake_path(GET CMAKE_C_COMPILER PARENT_PATH _tools)
    foreach(_level RANGE 1 5)
        cmake_path(GET _tools PARENT_PATH _tools)
    endforeach()
    find_program(CONTOUR_WAMR_CLANG_CL_EXECUTABLE
        NAMES clang-cl
        HINTS "${_tools}/Llvm/x64/bin"
        DOC "The clang-cl that compiles WAMR in an MSVC build")
    if(CONTOUR_WAMR_CLANG_CL_EXECUTABLE)
        set(${resultVar} "${CONTOUR_WAMR_CLANG_CL_EXECUTABLE}" PARENT_SCOPE)
    else()
        set(${resultVar}_REASON "no clang-cl found (install Visual Studio's \"C++ Clang tools for Windows\", or set CONTOUR_WAMR_CLANG_CL_EXECUTABLE)" PARENT_SCOPE)
    endif()
endfunction()

# ContourThirdParties_PatchWamrForClang(<source-dir>)
#
# Clang, like GCC, joins a line ending in a backslash and whitespace to the next one; MSVC does not. WAMR's
# win_file.c has three // comments ending in a backslash and a space (a path prefix such as `\??\`), so clang
# makes each swallow the line after it and fails to compile the file. There is no flag against it; each such
# comment is rewritten as a /* */ one, which ends before the backslash could join anything. Every Windows build
# applies it, whichever compiler it uses: cl reads the rewritten comments as it read the old ones.
function(ContourThirdParties_PatchWamrForClang sourceDir)
    set(_file "${sourceDir}/core/shared/platform/windows/win_file.c")
    file(READ "${_file}" _text)
    string(REGEX REPLACE "//([^\r\n]*\\\\)[ \t]+(\r?\n)" "/*\\1 */\\2" _patched "${_text}")
    if(NOT _patched STREQUAL _text)
        file(WRITE "${_file}" "${_patched}")
    endif()
endfunction()

# ContourThirdParties_AddWamrWithClangCl(<source-dir> <clang-cl> <options>...)
#
# Builds WAMR from <source-dir> as an external project compiled by <clang-cl>, configured with <options>
# (CPMAddPackage's "NAME VALUE" form), and defines the static library it makes as the imported target vmlib,
# exporting what WAMR's own vmlib target does: its public include directory and ntdll.
#
# clang-cl objects link into an MSVC build as they are: they share its ABI, and the runtime library
# (CMAKE_MSVC_RUNTIME_LIBRARY, /MD by default) and the build type are passed down, so both sides agree.
# What the cl build gives vmlib per target is given here as definitions: the host keeps Winsock
# (WASM_ENABLE_HOST_SOCKET_INIT), and the interpreter dispatches by computed goto
# (WASM_ENABLE_LABELS_AS_VALUES), which WAMR's config.h enables only for __GNUC__, which clang-cl does not
# define. WAMR is not sanitized: the build's sanitizer flags are not passed down.
function(ContourThirdParties_AddWamrWithClangCl sourceDir clangCl)
    include(ExternalProject)
    set(_cmake_args)
    foreach(_option IN LISTS ARGN)
        string(REPLACE " " "=" _definition "${_option}")
        list(APPEND _cmake_args "-D${_definition}")
    endforeach()
    set(_runtime "${CMAKE_MSVC_RUNTIME_LIBRARY}")
    if(NOT _runtime)
        set(_runtime "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
    # MASM assembles the native-call trampoline; ml64 sits beside cl.
    cmake_path(GET CMAKE_C_COMPILER PARENT_PATH _msvc_bin)
    if(EXISTS "${_msvc_bin}/ml64.exe")
        list(APPEND _cmake_args "-DCMAKE_ASM_MASM_COMPILER=${_msvc_bin}/ml64.exe")
    endif()
    set(_binary_dir "${CMAKE_BINARY_DIR}/_deps/wamr-clangcl-build")
    set(_library "${_binary_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}iwasm${CMAKE_STATIC_LIBRARY_SUFFIX}")
    ExternalProject_Add(wamr_clangcl
        SOURCE_DIR "${sourceDir}"
        BINARY_DIR "${_binary_dir}"
        CMAKE_ARGS
            ${_cmake_args}
            "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
            "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
            "-DCMAKE_C_COMPILER=${clangCl}"
            "-DCMAKE_C_FLAGS=/DWIN32 /D_WINDOWS -DWASM_ENABLE_HOST_SOCKET_INIT=1 -DWASM_ENABLE_LABELS_AS_VALUES=1"
            # WAMR asks for CMake 3.14, before which the runtime library was a flag, not a property.
            "-DCMAKE_POLICY_DEFAULT_CMP0091=NEW"
            "-DCMAKE_MSVC_RUNTIME_LIBRARY=${_runtime}"
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --target vmlib
        BUILD_ALWAYS YES
        BUILD_BYPRODUCTS "${_library}"
        INSTALL_COMMAND ""
        EXCLUDE_FROM_ALL YES
    )
    add_library(vmlib STATIC IMPORTED GLOBAL)
    set_target_properties(vmlib PROPERTIES
        IMPORTED_LOCATION "${_library}"
        INTERFACE_INCLUDE_DIRECTORIES "${sourceDir}/core/iwasm/include"
        INTERFACE_LINK_LIBRARIES ntdll)
    add_dependencies(vmlib wamr_clangcl)
endfunction()

if(CONTOUR_EXTENSIONS)
    add_library(contour_wamr INTERFACE)
    add_library(contour::wamr ALIAS contour_wamr)

    set(_wamr_target "")
    set(_wamr_rejected "")
    set(_wamr_clang_cl "")
    if(TARGET iwasm::vmlib)
        set(_wamr_target iwasm::vmlib)
        set(THIRDPARTY_BUILTIN_wamr "embedded")
    elseif(CONTOUR_EXTENSIONS_ORCHESTRATOR_GUEST)
        message(STATUS "Using the bundled WAMR: the orchestrator guest needs bulk-memory, sign-extension and saturating conversion")
    elseif(NOT CONTOUR_BUILD_STATIC)
        find_package(iwasm ${WAMR_MINIMAL_VERSION} QUIET)
        set(_wamr_candidate "")
        if(TARGET iwasm::vmlib)
            set(_wamr_candidate iwasm::vmlib)
        else()
            find_package(PkgConfig QUIET)
            if(PkgConfig_FOUND)
                pkg_check_modules(PC_WAMR QUIET IMPORTED_TARGET GLOBAL iwasm>=${WAMR_MINIMAL_VERSION})
                if(TARGET PkgConfig::PC_WAMR)
                    set(_wamr_candidate PkgConfig::PC_WAMR)
                endif()
            endif()
        endif()
        if(_wamr_candidate)
            ContourThirdParties_WamrTerminates(${_wamr_candidate} _wamr_terminates)
            if(_wamr_terminates)
                set(_wamr_target ${_wamr_candidate})
                set(THIRDPARTY_BUILTIN_wamr "system package (wasm_runtime_terminate verified)")
            else()
                set(_wamr_rejected "; system WAMR rejected: ${_wamr_terminates_REASON}")
                message(STATUS "System WAMR rejected (${_wamr_terminates_REASON}); building WAMR from source instead.")
            endif()
        endif()
    endif()

    if(NOT _wamr_target)
        if(NOT CONTOUR_USE_CPM AND CONTOUR_EXTENSIONS_ORCHESTRATOR_GUEST)
            message(FATAL_ERROR "CONTOUR_EXTENSIONS_ORCHESTRATOR_GUEST builds WAMR from source, which needs "
                "CONTOUR_USE_CPM=ON.")
        elseif(NOT CONTOUR_USE_CPM)
            message(FATAL_ERROR "Could not find a WAMR >= ${WAMR_MINIMAL_VERSION} built with WAMR_BUILD_THREAD_MGR=1 "
                "and without hardware bounds checks. "
                "Install one, set CONTOUR_USE_CPM=ON, or configure with -DCONTOUR_EXTENSIONS=OFF.")
        endif()
        set(_wamr_options
            "WAMR_BUILD_INTERP 1"
            "WAMR_BUILD_FAST_INTERP 1"
            "WAMR_BUILD_AOT 0"
            "WAMR_BUILD_JIT 0"
            "WAMR_BUILD_FAST_JIT 0"
            "WAMR_BUILD_LIBC_WASI 1"
            "WAMR_BUILD_LIBC_BUILTIN 0"
            "WAMR_BUILD_THREAD_MGR 1"
            "WAMR_BUILD_LIB_PTHREAD 0"
            "WAMR_BUILD_LIB_WASI_THREADS 0"
            "WAMR_BUILD_MULTI_MODULE 0"
            "WAMR_DISABLE_HW_BOUND_CHECK 1"
            "WAMR_DISABLE_STACK_HW_BOUND_CHECK 1"
            "WAMR_DISABLE_WAKEUP_BLOCKING_OP 1"
            # WAMR writes the linear memory's base to GS on every call, for the AOT code's sake ("segue");
            # the interpreter never reads it back. valgrind cannot decode wrgsbase, and the daemon's
            # suites run under it.
            "WAMR_DISABLE_WRITE_GS_BASE 1"
            "BUILD_SHARED_LIBS OFF"
        )

        # MSVC compiles WAMR's interpreter about ten times slower than clang: it has no computed goto, so the
        # interpreter falls back to a switch, which it also optimizes poorly. clang-cl shares MSVC's ABI, so an
        # MSVC build has WAMR compiled by clang-cl, in a build of its own, where one is found.
        if(MSVC AND NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
            option(CONTOUR_WAMR_CLANG_CL "Compile the bundled WAMR with clang-cl in an MSVC build" ON)
            if(CONTOUR_WAMR_CLANG_CL)
                ContourThirdParties_FindWamrClangCl(_wamr_clang_cl)
                if(NOT _wamr_clang_cl)
                    message(STATUS "WAMR is compiled by cl, its interpreter about ten times slower: ${_wamr_clang_cl_REASON}.")
                endif()
            endif()
        endif()
        if(_wamr_clang_cl)
            set(_wamr_download_only YES)
        else()
            set(_wamr_download_only NO)
        endif()

        message(STATUS "Using CPM to fetch WAMR")
        CPMAddPackage(
            NAME wamr
            VERSION ${WAMR_MINIMAL_VERSION}
            GITHUB_REPOSITORY bytecodealliance/wasm-micro-runtime
            GIT_TAG 25bd7eb63e828e4bd242cc9b38d260b4b31c6605 # WAMR-${WAMR_MINIMAL_VERSION}
            OPTIONS ${_wamr_options}
            DOWNLOAD_ONLY ${_wamr_download_only}
            EXCLUDE_FROM_ALL YES
            SYSTEM YES
        )
        # WAMR ships a .clang-tidy with a key today's clang-tidy no longer knows (AnalyzeTemporaryDtors). clang-tidy
        # reads it whenever a first-party file it lints includes a WAMR header, and fails on the parse error rather
        # than on anything in our code. Third-party code is not ours to lint, so its configuration is replaced by
        # one that enables nothing.
        if(wamr_ADDED)
            file(WRITE "${wamr_SOURCE_DIR}/.clang-tidy"
                 "# Written by Contour's build (cmake/ContourThirdParties.cmake): third-party code is not linted.\n"
                 "Checks: '-*'\n")
        endif()
        if(WIN32 AND wamr_ADDED)
            ContourThirdParties_PatchWamrForClang("${wamr_SOURCE_DIR}")
        endif()
        if(_wamr_clang_cl)
            ContourThirdParties_AddWamrWithClangCl("${wamr_SOURCE_DIR}" "${_wamr_clang_cl}" ${_wamr_options})
        else()
            if(NOT CONTOUR_SANITIZE STREQUAL "OFF")
                # The fast interpreter reads its pre-decoded operands unaligned on purpose, on CPUs that
                # allow it (WASM_CPU_SUPPORTS_UNALIGNED_ADDR_ACCESS); UBSan would report every one.
                target_compile_options(vmlib PRIVATE -fno-sanitize=alignment)
            endif()
            if(WIN32)
                # Winsock is the host's: WAMR's own deinit calls WSACleanup on every runtime teardown but
                # starts Winsock only on the first init, so each later teardown takes Winsock away from
                # the process's live sockets. Extensions get no sockets, so WAMR needs none of it.
                target_compile_definitions(vmlib PRIVATE WASM_ENABLE_HOST_SOCKET_INIT=1)
            endif()
            if(MSVC)
                # WAMR's headers use C11 alignof in static assertions; MSVC compiles C without a C11
                # standard unless one is requested.
                set_target_properties(vmlib PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
            endif()
            if(MSVC AND CMAKE_C_COMPILER_ID MATCHES "Clang")
                # clang-cl has computed goto, but does not define __GNUC__, which WAMR's config.h asks for.
                target_compile_definitions(vmlib PRIVATE WASM_ENABLE_LABELS_AS_VALUES=1)
            endif()
            if(CMAKE_C_COMPILER_ID MATCHES "Clang")
                # The native-call trampoline is a plain .s file, which the driver assembles without
                # preprocessing; Clang then warns about every -D and dependency flag the build passes.
                target_compile_options(vmlib PRIVATE $<$<COMPILE_LANGUAGE:ASM>:-Wno-unused-command-line-argument>)
            endif()
        endif()
        if(MSVC)
            # wasm_export.h marks the API __declspec(dllimport) under MSVC unless told otherwise, which
            # is right only for a WAMR DLL; this one is linked statically.
            target_compile_definitions(contour_wamr INTERFACE WASM_RUNTIME_API_EXTERN=)
        endif()
        set(_wamr_target vmlib)
        set(THIRDPARTY_BUILTIN_wamr "CPM (WAMR-${WAMR_MINIMAL_VERSION}, static, fast interpreter)${_wamr_rejected}")
        if(_wamr_clang_cl)
            string(APPEND THIRDPARTY_BUILTIN_wamr ", compiled by clang-cl")
        endif()
    endif()

    # Whether the WAMR linked is cl-compiled, whose interpreter runs about five times slower than the
    # orchestrator's deadlines assume (vtextension's DaemonLimits.hpp). An installed WAMR does not say what
    # compiled it, so in an MSVC build it is taken to be cl, as the host is.
    if(MSVC AND NOT CMAKE_C_COMPILER_ID MATCHES "Clang" AND NOT _wamr_clang_cl)
        set(CONTOUR_WAMR_SLOW_INTERPRETER ON)
    else()
        set(CONTOUR_WAMR_SLOW_INTERPRETER OFF)
    endif()
    target_link_libraries(contour_wamr INTERFACE ${_wamr_target})
else()
    set(THIRDPARTY_BUILTIN_wamr "(extensions disabled)")
endif()

# glaze, the JSON (de)serializer behind vtcontrol: structs in, structs out, schemas from the same structs.
#
# It must come before termbench-pro, whose CMakeLists declares its own, older glaze to FetchContent
# under the same name. FetchContent keeps the first declaration of a name and ignores every later one,
# so declaring ours first makes termbench-pro's FetchContent_MakeAvailable(glaze) find it populated and
# do nothing: one glaze in the build, and ours.
#
# glaze is header-only, and its own CMakeLists is never run: glaze 9 requires CMake 3.31, newer than this
# project does. SOURCE_SUBDIR names a directory with no CMakeLists.txt, so FetchContent_MakeAvailable only
# populates the sources, and glaze_glaze below is ours. install-deps.sh only unpacks the archive. A system
# package is declared too, with FIND_PACKAGE_ARGS, so that termbench-pro's MakeAvailable finds that
# package instead of fetching. The download is FetchContent's, not CPM's: from CMake 3.30.3 on, CPM's
# DOWNLOAD_ONLY populates without declaring, and termbench-pro's declaration would then win.
include(FetchContent)
set(_glaze_embedded_dir "${ContourThirdParties_SRCDIR}/glaze-${GLAZE_VERSION}")
set(_glaze_header_only SOURCE_SUBDIR "contour-header-only")
if(EXISTS "${_glaze_embedded_dir}/include/glaze/glaze.hpp")
    FetchContent_Declare(glaze SOURCE_DIR "${_glaze_embedded_dir}" ${_glaze_header_only})
    set(THIRDPARTY_BUILTIN_glaze "embedded")
else()
    find_package(glaze ${GLAZE_VERSION} QUIET)
    if(glaze_FOUND)
        FetchContent_Declare(glaze FIND_PACKAGE_ARGS ${GLAZE_VERSION})
        set(THIRDPARTY_BUILTIN_glaze "system package")
    elseif(CONTOUR_USE_CPM)
        message(STATUS "Fetching glaze ${GLAZE_VERSION}")
        FetchContent_Declare(glaze
            URL "https://github.com/stephenberry/glaze/archive/refs/tags/v${GLAZE_VERSION}.tar.gz"
            URL_HASH SHA256=${GLAZE_SHA256}
            DOWNLOAD_EXTRACT_TIMESTAMP ON
            ${_glaze_header_only}
        )
        set(THIRDPARTY_BUILTIN_glaze "embedded (fetched)")
    else()
        message(FATAL_ERROR "Could not find glaze ${GLAZE_VERSION}. "
            "Run install-deps.sh, install system packages, or set CONTOUR_USE_CPM=ON.")
    endif()
endif()
if(NOT glaze_FOUND)
    FetchContent_MakeAvailable(glaze)
    # What glaze's own CMakeLists defines, less its opt-in options (all off by default). The includes are
    # SYSTEM, as glaze makes them for a consumer, so its headers never warn.
    add_library(glaze_glaze INTERFACE)
    add_library(glaze::glaze ALIAS glaze_glaze)
    set_target_properties(glaze_glaze PROPERTIES EXPORT_NAME glaze)
    target_compile_features(glaze_glaze INTERFACE cxx_std_23)
    target_include_directories(glaze_glaze SYSTEM INTERFACE "$<BUILD_INTERFACE:${glaze_SOURCE_DIR}/include>")
    if(MSVC)
        target_compile_options(glaze_glaze INTERFACE
            $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/Zc:preprocessor>
            $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/permissive->
            $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/Zc:lambda>)
    else()
        target_compile_options(glaze_glaze INTERFACE
            $<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:-Wno-missing-braces>)
    endif()
endif()

if(LIBTERMINAL_BUILD_BENCH_HEADLESS)
    if(COMMAND ContourThirdParties_Embed_termbench_pro)
        ContourThirdParties_Embed_termbench_pro()
    endif()
    if(TARGET termbench)
        set(THIRDPARTY_BUILTIN_termbench "embedded")
    else()
        find_package(termbench-pro QUIET)
        if(termbench-pro_FOUND)
            set(THIRDPARTY_BUILTIN_termbench "system package")
        elseif(CONTOUR_USE_CPM)
            message(STATUS "Using CPM to fetch termbench-pro")
            CPMAddPackage(
                NAME termbench-pro
                GITHUB_REPOSITORY contour-terminal/termbench-pro
                GIT_TAG ${TERMBENCH_PRO_COMMIT_HASH}
                EXCLUDE_FROM_ALL YES
            )
            set(THIRDPARTY_BUILTIN_termbench "embedded (CPM)")
        else()
            message(FATAL_ERROR "Could not find termbench-pro. "
                "Run install-deps.sh, install system packages, or set CONTOUR_USE_CPM=ON.")
        endif()
    endif()
else()
    set(THIRDPARTY_BUILTIN_termbench "(bench-headless disabled)")
endif()

# The vendored termbench-pro must never be run through clang-tidy (it has no .clang-tidy config, so tidy
# aborts the build with "no checks enabled"). Clearing the per-target property is order-independent —
# unlike unsetting the CMAKE_CXX_CLANG_TIDY variable, it holds no matter when/where the target is
# realized. Applied to the library target and, when present, the CPM-added source-tree targets.
if(TARGET termbench)
    set_target_properties(termbench PROPERTIES CXX_CLANG_TIDY "")
endif()

# termbench-pro links glaze publicly and exports itself, and CMake refuses an install(EXPORT) whose
# dependency is in no export set. Our glaze_glaze joins termbench-pro's; it is an interface library, so
# this installs nothing, and EXCLUDE_FROM_ALL keeps even the rule out of a plain install.
if(TARGET termbench AND TARGET glaze_glaze)
    install(TARGETS glaze_glaze EXPORT termbench-targets COMPONENT termbench EXCLUDE_FROM_ALL)
endif()

if(COMMAND ContourThirdParties_Embed_boxed_cpp)
    ContourThirdParties_Embed_boxed_cpp()
    subproject_version(boxed-cpp boxed_cpp_version)
    if(NOT DEFINED boxed_cpp_version OR boxed_cpp_version VERSION_LESS BOXED_CPP_MINIMAL_VERSION)
        message(FATAL_ERROR "Embedded boxed-cpp version must be at least ${BOXED_CPP_MINIMAL_VERSION}, but found ${boxed_cpp_version}")
    endif()
    set(THIRDPARTY_BUILTIN_boxed-cpp "embedded")
else()
    HandleThirdparty(boxed-cpp "gh:contour-terminal/boxed-cpp#v${BOXED_CPP_MINIMAL_VERSION}")
endif()

if(COMMAND ContourThirdParties_Embed_reflection_cpp)
    ContourThirdParties_Embed_reflection_cpp()
    set(THIRDPARTY_BUILTIN_reflection-cpp "embedded")
else()
    HandleThirdparty(reflection-cpp "gh:contour-terminal/reflection-cpp#v${REFLECTION_CPP_VERSION}")
endif()

# core-cpp, the shared foundation of the Contour Terminal projects (coroutines, the event loop and
# sockets, logging, the CLI parser and the generic utilities), is carried as a verbatim copy of a
# tagged release in vendor/core-cpp, so that a distribution build fetches nothing for it. Never edit
# the copy; see vendor/README.md.
#
# Its own dependencies follow CONTOUR_USE_CPM: with CPM off, a missing one stops the configure with
# the option that needed it rather than reaching the network. TLS is on because vthost serves and
# dials TLS (core::net_tls). Of the TUI only the headless half is enabled: core::tui_core (and the
# core::tui_output leaf it links), which needs no terminal, event loop or platform layer and links the
# libunicode resolved above; the terminal half, core::tui, stays off. The OS sleep/wake
# monitor reads logind over libsystemd on Linux, the same library and the same switch as vtpty's
# session scopes, so a build without systemd loses both and needs neither.
set(CORE_CPP_FETCH_DEPS ${CONTOUR_USE_CPM})
set(CORE_CPP_TESTING ${CONTOUR_TESTING_CORE_CPP})
set(CORE_CPP_WITH_TUI OFF)
set(CORE_CPP_WITH_TUI_OUTPUT ON)
set(CORE_CPP_WITH_TUI_CORE ON)
set(CORE_CPP_WITH_TLS ON)
set(CORE_CPP_WITH_SYSTEMD ${CONTOUR_WITH_SYSTEMD})
find_package(OpenSSL REQUIRED)
# vtbackend inflates kitty graphics `o=z` payloads.
find_package(ZLIB REQUIRED)
file(STRINGS "${PROJECT_SOURCE_DIR}/vendor/core-cpp/MANIFEST" _core_cpp_ref REGEX "^# ref ")
string(REPLACE "# ref " "" _core_cpp_ref "${_core_cpp_ref}")
# EXCLUDE_FROM_ALL builds only the core-cpp targets contour links -- and would skip core-cpp's own
# test executables, so it is left off when those are asked for. A vendored copy older than core-cpp
# v0.2.1 cannot configure with its tests on at all (it adds a tests/ directory the copy leaves out),
# so that is refused here, by name, rather than by CMake's error from inside the copy. The version is
# the copy's own project() line, not the MANIFEST's ref, which may be a commit SHA.
file(READ "${PROJECT_SOURCE_DIR}/vendor/core-cpp/CMakeLists.txt" _core_cpp_lists)
string(REGEX MATCH "project\\(core-cpp[^)]*VERSION[ \t\r\n]+([0-9]+[.][0-9]+[.][0-9]+)" _ "${_core_cpp_lists}")
set(_core_cpp_version "${CMAKE_MATCH_1}")
if(CONTOUR_TESTING_CORE_CPP AND (_core_cpp_version STREQUAL "" OR _core_cpp_version VERSION_LESS "0.2.1"))
    message(FATAL_ERROR
        "CONTOUR_TESTING_CORE_CPP needs a vendored core-cpp of v0.2.1 or later, which carries its test "
        "suite; vendor/core-cpp is ${_core_cpp_ref} (version '${_core_cpp_version}'). Re-vendor it "
        "(vendor/README.md), or turn the option off.")
endif()
if(CONTOUR_TESTING_CORE_CPP)
    add_subdirectory(${PROJECT_SOURCE_DIR}/vendor/core-cpp ${PROJECT_BINARY_DIR}/vendor/core-cpp SYSTEM)
else()
    add_subdirectory(${PROJECT_SOURCE_DIR}/vendor/core-cpp ${PROJECT_BINARY_DIR}/vendor/core-cpp SYSTEM EXCLUDE_FROM_ALL)
endif()
set(THIRDPARTY_BUILTIN_core-cpp "vendored ${_core_cpp_ref} (vendor/core-cpp)")

# The copy is verbatim: this test fails on any byte that differs from its MANIFEST, so a local edit
# fails the suite instead of drifting from the release it claims to be.
if(CONTOUR_TESTING)
    add_test(NAME core-cpp-vendored-copy
             COMMAND ${CMAKE_COMMAND} -DMODE=check
                     -DDEST=${PROJECT_SOURCE_DIR}/vendor/core-cpp
                     -P ${PROJECT_SOURCE_DIR}/vendor/core-cpp/cmake/CoreCppVendor.cmake)
    set_tests_properties(core-cpp-vendored-copy PROPERTIES LABELS "lint")
endif()

# Every third-party target now exists, and our own src/ and examples/ trees are not added until after
# this module returns -- so everything reachable from the top level at this point is vendored code.
# The embedded projects attach their targets to the scope that calls the Embed function, i.e. here,
# rather than under _deps/sources, so the sweep has to start at the top level to reach them.
ContourThirdParties_ClearClangTidy("${PROJECT_SOURCE_DIR}")

macro(ContourThirdPartiesSummary2)
    message(STATUS "==============================================================================")
    message(STATUS "    Contour ThirdParties")
    message(STATUS "------------------------------------------------------------------------------")
    message(STATUS "Catch2              ${THIRDPARTY_BUILTIN_Catch2}")
    message(STATUS "GSL                 ${THIRDPARTY_BUILTIN_GSL}")
    message(STATUS "freetype            ${THIRDPARTY_BUILTIN_Freetype}")
    message(STATUS "harfbuzz            ${THIRDPARTY_BUILTIN_HarfBuzz}")
    message(STATUS "yaml-cpp            ${THIRDPARTY_BUILTIN_yaml-cpp}")
    message(STATUS "termbench-pro       ${THIRDPARTY_BUILTIN_termbench}")
    message(STATUS "reflection-cpp      ${THIRDPARTY_BUILTIN_reflection-cpp}")
    message(STATUS "glaze               ${THIRDPARTY_BUILTIN_glaze}")
    if(DEFINED THIRDPARTY_BUILTIN_libunicode)
        message(STATUS "libunicode          ${THIRDPARTY_BUILTIN_libunicode}")
    else()
        message(STATUS "libunicode          ${THIRDPARTY_BUILTIN_unicode_core}")
    endif()
    message(STATUS "boxed-cpp           ${THIRDPARTY_BUILTIN_boxed-cpp}")
    message(STATUS "core-cpp            ${THIRDPARTY_BUILTIN_core-cpp}")
    message(STATUS "WAMR                ${THIRDPARTY_BUILTIN_wamr}")
    message(STATUS "Tracy               ${THIRDPARTY_BUILTIN_Tracy}")
    message(STATUS "------------------------------------------------------------------------------")
endmacro()
