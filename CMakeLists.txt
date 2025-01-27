cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(Version)
GetVersionInformation(CONTOUR_VERSION CONTOUR_VERSION_STRING)
file(WRITE "${CMAKE_BINARY_DIR}/version.txt" "${CONTOUR_VERSION_STRING}")
project(contour VERSION "${CONTOUR_VERSION}" LANGUAGES CXX)

# setting defaults
if (NOT(CMAKE_CXX_STANDARD))
    set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_COLOR_DIAGNOSTICS ON)

set(QT_VERSION_DEFAULT  "6")

if(NOT CONTOUR_QT_VERSION)
    set(CONTOUR_QT_VERSION "${QT_VERSION_DEFAULT}")
endif()

if((CMAKE_CXX_COMPILER_ID STREQUAL "Clang") OR (CMAKE_CXX_COMPILER_ID STREQUAL "GNU"))
    set(CMAKE_CXX_FLAGS_DEBUG "-Og -g")
endif()

include(EnableCcache)
include(ClangTidy)

if(NOT WIN32 AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Choose the build mode." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release MinSizeRel RelWithDebInfo)
endif()

find_package(Threads)

if(DEFINED MSVC)
    add_definitions(-D_USE_MATH_DEFINES)
    add_definitions(-DNOMINMAX)
    add_compile_options(/utf-8)
    add_compile_options(/nologo)
endif()


# CMAKE_BUILD_TYPE check for None
if(CMAKE_BUILD_TYPE STREQUAL "None")
    add_compile_definitions(-DNDEBUG)
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
option(CONTOUR_FRONTEND_GUI "Enables GUI frontend." ON)
option(CONTOUR_COVERAGE "Builds with codecov [default: OFF]" OFF)
option(CONTOUR_SANITIZE "Builds with Address sanitizer enabled [default: OFF]" "OFF")
option(CONTOUR_STACKTRACE_ADDR2LINE "Uses addr2line to pretty-print SEGV stacktrace." ${ADDR2LINE_DEFAULT})
option(CONTOUR_BUILD_WITH_MIMALLOC "Builds with mimalloc [default: OFF]" OFF)
option(CONTOUR_INSTALL_TOOLS "Installs tools, if built [default: OFF]" OFF)
option(CONTOUR_PACKAGE_TERMINFO "Package terminfo files" ON)
option(CONTOUR_WITH_UTEMPTER "Build with utempter support [default: ON]" ON)
option(CONTOUR_USE_CPM "Use CPM to fetch dependencies [default: OFF]" OFF)
option(CONTOUR_BUILD_STATIC "Link to static libraries [default: OFF]" OFF)


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
    message(STATUS "Enabling ${san} sanitizer.")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-omit-frame-pointer -fsanitize=address -fsanitize-address-use-after-scope")
else()
    message(STATUS "No sanitizer enabled.")
endif()

if(CONTOUR_TESTING)
    enable_testing()
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


include(ContourThirdParties)
add_subdirectory(src)
add_subdirectory(examples)

# ----------------------------------------------------------------------------
# Summary

if("${CCACHE}" STREQUAL "")
    set(USING_CCACHE_STRING "OFF")
else()
    set(USING_CCACHE_STRING "${CCACHE}")
endif()

if("${CMAKE_CXX_CLANG_TIDY}" STREQUAL "")
    set(USING_TIDY_STRING "OFF")
else()
    set(USING_TIDY_STRING "ON (${CMAKE_CXX_CLANG_TIDY})")
endif()

if(CONTOUR_FRONTEND_GUI)
    find_package(Qt${CONTOUR_QT_VERSION} COMPONENTS Core QUIET)
    if (NOT Qt${CONTOUR_QT_VERSION}_FOUND)
        set(QT_VERSION ${Qt5_VERSION})
    else()
        set(QT_VERSION ${Qt6_VERSION})
    endif()
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
    message(STATUS "Using ccache:                                       ${USING_CCACHE_STRING}")
    message(STATUS "Build with sanitizer:                               ${CONTOUR_SANITIZE}")
    message(STATUS "Build unit tests:                                   ${CONTOUR_TESTING}")
    message(STATUS "Enable with code coverage:                          ${CONTOUR_CODE_COVERAGE_ENABLED}")
    message(STATUS "Build contour frontend GUI:                         ${CONTOUR_FRONTEND_GUI}")
    if(CONTOUR_FRONTEND_GUI)
        message(STATUS "Build contour using Qt:                             ${CONTOUR_QT_VERSION} (${QT_VERSION})")
    endif()
    message(STATUS "Build contour using mimalloc:                       ${CONTOUR_BUILD_WITH_MIMALLOC}")
    message(STATUS "Clang Tidy:                                         ${USING_TIDY_STRING}")
    message(STATUS "|> Enable performance metrics:                      ${CONTOUR_PERF_STATS}")
    message(STATUS "------------------------------------------------------------------------------")
endmacro()

ContourThirdPartiesSummary2()
ContourConfigurationSummary()
