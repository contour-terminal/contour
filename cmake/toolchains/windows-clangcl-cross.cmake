# SPDX-License-Identifier: Apache-2.0
#
# Cross-compiles Windows x64 executables on a Linux host: clang-cl, lld-link and llvm-rc from the host's
# LLVM, against the MSVC headers and libraries and the Windows SDK of an msvc-wine style install.
#
# Where that install lives is MSVC_WINE_ROOT -- a cache variable, else the environment variable of the
# same name, else /opt/msvc. The MSVC toolset and SDK versions are the newest found there unless
# MSVC_WINE_TOOLSET_VERSION / MSVC_WINE_SDK_VERSION name one.
#
# The GUI builds against an official Qt for Windows (MSVC 2022 x64) in QT_WINDOWS_ROOT, with the host's Qt
# tools (QT_HOST_PATH); see below.
#
# Test binaries run under wine: CMAKE_CROSSCOMPILING_EMULATOR is set, so ctest runs them as it would
# on Windows.
#
# @see docs/internals/windows-cross-build.md

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT DEFINED MSVC_WINE_ROOT)
    if(DEFINED ENV{MSVC_WINE_ROOT})
        set(MSVC_WINE_ROOT "$ENV{MSVC_WINE_ROOT}")
    else()
        set(MSVC_WINE_ROOT "/opt/msvc")
    endif()
endif()
set(MSVC_WINE_ROOT "${MSVC_WINE_ROOT}" CACHE PATH "The msvc-wine style MSVC + Windows SDK install")
if(NOT IS_DIRECTORY "${MSVC_WINE_ROOT}/vc/tools/msvc" OR NOT IS_DIRECTORY "${MSVC_WINE_ROOT}/kits/10/lib")
    message(FATAL_ERROR "MSVC_WINE_ROOT=${MSVC_WINE_ROOT} is no msvc-wine style install "
                        "(expected vc/tools/msvc/ and kits/10/lib/ beneath it)")
endif()

# Picks the newest version directory under @p parent unless @p variable already names one.
function(_msvc_wine_pick_version variable parent)
    if(${variable})
        return()
    endif()
    file(GLOB _versions LIST_DIRECTORIES true RELATIVE "${parent}" "${parent}/*")
    list(FILTER _versions INCLUDE REGEX "^[0-9.]+$")
    list(SORT _versions COMPARE NATURAL ORDER DESCENDING)
    list(GET _versions 0 _newest)
    set(${variable} "${_newest}" CACHE STRING "" FORCE)
endfunction()
_msvc_wine_pick_version(MSVC_WINE_TOOLSET_VERSION "${MSVC_WINE_ROOT}/vc/tools/msvc")
_msvc_wine_pick_version(MSVC_WINE_SDK_VERSION "${MSVC_WINE_ROOT}/kits/10/lib")

set(_msvc "${MSVC_WINE_ROOT}/vc/tools/msvc/${MSVC_WINE_TOOLSET_VERSION}")
set(_sdkInclude "${MSVC_WINE_ROOT}/kits/10/include/${MSVC_WINE_SDK_VERSION}")
set(_sdkLib "${MSVC_WINE_ROOT}/kits/10/lib/${MSVC_WINE_SDK_VERSION}")

find_program(CMAKE_C_COMPILER NAMES clang-cl REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES clang-cl REQUIRED)
find_program(CMAKE_LINKER NAMES lld-link REQUIRED)
find_program(CMAKE_RC_COMPILER NAMES llvm-rc REQUIRED)
find_program(CMAKE_MT NAMES llvm-mt REQUIRED)
find_program(CMAKE_AR NAMES llvm-lib REQUIRED)

# The system headers are -imsvc, so the pedantic warnings stay on Contour's own code, as on Windows.
set(_includes "")
foreach(_dir IN ITEMS "${_msvc}/include" "${_sdkInclude}/ucrt" "${_sdkInclude}/shared" "${_sdkInclude}/um"
                      "${_sdkInclude}/winrt")
    string(APPEND _includes " /imsvc \"${_dir}\"")
endforeach()
# _MSC_VER is the toolset's, not clang-cl's default (1933, VS 2022 17.3): headers decide by it, as Qt's do
# which name stdext::make_checked_array_iterator only below 1938, and the STL of a newer toolset has none.
# Toolset 14.XY is _MSC_VER 19XY.
string(REGEX REPLACE "^14\\.([0-9]+).*$" "19.\\1" _msvcCompatibilityVersion "${MSVC_WINE_TOOLSET_VERSION}")
set(_target "--target=x86_64-pc-windows-msvc -fuse-ld=lld-link")
string(APPEND _target " -fms-compatibility-version=${_msvcCompatibilityVersion}")
set(CMAKE_C_FLAGS_INIT "${_target}${_includes}")
set(CMAKE_CXX_FLAGS_INIT "${_target}${_includes}")
set(CMAKE_RC_FLAGS_INIT "/I \"${_sdkInclude}/um\" /I \"${_sdkInclude}/shared\"")

set(_libpaths "")
foreach(_dir IN ITEMS "${_msvc}/lib/x64" "${_sdkLib}/ucrt/x64" "${_sdkLib}/um/x64")
    string(APPEND _libpaths " /libpath:\"${_dir}\"")
endforeach()
foreach(_kind IN ITEMS EXE SHARED MODULE)
    set(CMAKE_${_kind}_LINKER_FLAGS_INIT "${_libpaths}")
endforeach()

# Qt for Windows, for the GUI: an official Qt MSVC 2022 x64 install, as the Qt installer lays it out
# (<version>/msvc2022_64). Where it lives is QT_WINDOWS_ROOT -- a cache variable, else the environment variable
# of the same name, else /opt/qt-windows/6.11.1/msvc2022_64. Its tools (moc, rcc, qmlcachegen, ...) are
# Windows executables, so Qt takes them from a Qt of the same minor release on the host instead: QT_HOST_PATH
# (default /usr), with its CMake packages in QT_HOST_PATH_CMAKE_DIR (default <QT_HOST_PATH>/lib64/cmake where
# that exists, as on Fedora, else <QT_HOST_PATH>/lib/cmake). A missing QT_WINDOWS_ROOT is not an error here:
# a build without the GUI (CONTOUR_FRONTEND_GUI=OFF) does not need it.
if(NOT DEFINED QT_WINDOWS_ROOT)
    if(DEFINED ENV{QT_WINDOWS_ROOT})
        set(QT_WINDOWS_ROOT "$ENV{QT_WINDOWS_ROOT}")
    else()
        set(QT_WINDOWS_ROOT "/opt/qt-windows/6.11.1/msvc2022_64")
    endif()
endif()
set(QT_WINDOWS_ROOT "${QT_WINDOWS_ROOT}" CACHE PATH "Qt for Windows (MSVC 2022 x64) to build the GUI against")
if(IS_DIRECTORY "${QT_WINDOWS_ROOT}/lib/cmake/Qt6")
    list(APPEND CMAKE_PREFIX_PATH "${QT_WINDOWS_ROOT}")
    list(APPEND CMAKE_FIND_ROOT_PATH "${QT_WINDOWS_ROOT}")
    set(QT_HOST_PATH "/usr" CACHE PATH "The host's Qt, whose tools a cross build runs")
    if(IS_DIRECTORY "${QT_HOST_PATH}/lib64/cmake/Qt6")
        set(QT_HOST_PATH_CMAKE_DIR "${QT_HOST_PATH}/lib64/cmake" CACHE PATH "The host Qt's CMake packages")
    else()
        set(QT_HOST_PATH_CMAKE_DIR "${QT_HOST_PATH}/lib/cmake" CACHE PATH "The host Qt's CMake packages")
    endif()
endif()

# Windows binaries run under wine, with Microsoft's own C++ runtime rather than wine's reimplementation of
# it (the debug one exists only as Microsoft's), and the DLLs of every CMAKE_PREFIX_PATH entry on the DLL
# search path. WINEPREFIX and WINEDEBUG come from the environment as usual.
#
# The emulator is a script written into the build directory, <build>/wine-run.sh, rather than an
# `env WINEPATH=... wine` list: WINEPATH is ;-separated, and a ; inside a CMake list does not survive being
# expanded into a custom command (windeployqt's, see DeployQt.cmake). It also runs a binary by hand.
find_program(MSVC_WINE_WINE NAMES wine wine64)
if(MSVC_WINE_WINE)
    set(_redistRoot "${MSVC_WINE_ROOT}/vc/Redist/MSVC/${MSVC_WINE_TOOLSET_VERSION}")
    file(GLOB _redist LIST_DIRECTORIES true "${_redistRoot}/x64/*.CRT" "${_redistRoot}/debug_nonredist/x64/*.DebugCRT")
    set(_winePath ${_redist} "${MSVC_WINE_ROOT}/kits/10/bin/${MSVC_WINE_SDK_VERSION}/x64/ucrt")
    foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
        list(APPEND _winePath "${_prefix}/bin")
    endforeach()
    list(REMOVE_DUPLICATES _winePath)
    list(JOIN _winePath ";" _winePath)
    file(WRITE "${CMAKE_BINARY_DIR}/wine-run.sh"
         "#!/bin/sh\n"
         "# Written by cmake/toolchains/windows-clangcl-cross.cmake: runs a Windows executable of this build "
         "under wine.\n"
         "export WINEPATH='${_winePath}'\n"
         "export WINEDLLOVERRIDES='msvcp140,msvcp140_1,msvcp140_2,vcruntime140,vcruntime140_1=n,b'\n"
         "exec '${MSVC_WINE_WINE}' \"$@\"\n")
    file(CHMOD "${CMAKE_BINARY_DIR}/wine-run.sh" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ
                                                             GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${CMAKE_BINARY_DIR}/wine-run.sh")
endif()

# Dependencies come from CMAKE_PREFIX_PATH / CMAKE_FIND_ROOT_PATH, never from the Linux host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
