cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(Version)
GetVersionInformation(CONTOUR_VERSION CONTOUR_VERSION_STRING)
file(WRITE "${CMAKE_BINARY_DIR}/version.txt" "${CONTOUR_VERSION_STRING}")
project(contour VERSION "${CONTOUR_VERSION}" LANGUAGES CXX)

# setting defaults
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(EnableCcache)
include(ClangTidy)
include(CIncludeMe)

if(NOT WIN32 AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Choose the build mode." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release MinSizeRel RelWithDebInfo)
endif()

find_package(Threads)

if(DEFINED MSVC)
    add_definitions(-D_USE_MATH_DEFINES)
    add_definitions(-DNOMINMAX)
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

option(CONTOUR_TESTING "Enables building of unittests for libterminal [default: ON" ON)
option(CONTOUR_FRONTEND_GUI "Enables GUI frontend." ON)
option(CONTOUR_COVERAGE "Builds with codecov [default: OFF]" OFF)
option(CONTOUR_SANITIZE "Builds with Address sanitizer enabled [default: OFF]" OFF)
option(CONTOUR_STACKTRACE_ADDR2LINE "Uses addr2line to pretty-print SEGV stacktrace." ${ADDR2LINE_DEFAULT})
option(CONTOUR_BUILD_WITH_EMBEDDED_FT_HB "Builds with embedded FreeType and HarfBuzz [default: OFF]" OFF)
option(CONTOUR_BUILD_WITH_MIMALLOC "Builds with mimalloc [default: OFF]" OFF)

# ----------------------------------------------------------------------------
# code coverage

if(CONTOUR_COVERAGE AND NOT MSVC)
    add_compile_options(-g --coverage)
    set(CMAKE_EXE_LINKER_FLAGS "--coverage ${CMAKE_EXE_LINKER_FLAGS}")
    set(CONTOUR_CODE_COVERAGE_ENABLED ON)
else()
    set(CONTOUR_CODE_COVERAGE_ENABLED OFF)
endif()

if(SANITIZE)
    string(TOLOWER "${SANITIZE}" san)
    if (san STREQUAL "address")
        message(STATUS "Enabling ${san} sanitizer.")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-omit-frame-pointer -fsanitize=address -fsanitize-address-use-after-scope")
    else()
        message(STATUS "No sanitizer enabled.")
    endif()
endif()

# ----------------------------------------------------------------------------
add_subdirectory(src)

include(ThirdParties)

# ----------------------------------------------------------------------------
# Summary

if(${USING_BOOST_FILESYSTEM})
    set(USING_FILESYSTEM_API_STRING "boost::filesystem")
else()
    set(USING_FILESYSTEM_API_STRING "C++fs")
endif()

if("${CCACHE}" STREQUAL "")
    set(USING_CCACHE_STRING "OFF")
else()
    set(USING_CCACHE_STRING "${CCACHE}")
endif()

message(STATUS "------------------------------------------------------------------------------")
message(STATUS "    Contour (${CONTOUR_VERSION_STRING})")
message(STATUS "------------------------------------------------------------------------------")
message(STATUS "Build type:                                         ${CMAKE_BUILD_TYPE}")
message(STATUS "Build with sanitizer:                               ${CONTOUR_SANITIZE}")
message(STATUS "Build unit tests:                                   ${CONTOUR_TESTING}")
message(STATUS "Enable with code coverage:                          ${CONTOUR_CODE_COVERAGE_ENABLED}")
message(STATUS "Build contour frontend GUI:                         ${CONTOUR_FRONTEND_GUI}")
message(STATUS "Build contour using Qt 6:                           ${CONTOUR_BUILD_WITH_QT6}")
message(STATUS "Build contour using mimalloc:                       ${CONTOUR_BUILD_WITH_MIMALLOC}")
message(STATUS "Build contour using embedded FreeType and HarfBuzz: ${CONTOUR_BUILD_WITH_EMBEDDED_FT_HB}")
message(STATUS "|> Enable blur effect on KWin:                      ${CONTOUR_BLUR_PLATFORM_KWIN}")
message(STATUS "|> Enable performance metrics:                      ${CONTOUR_PERF_STATS}")
message(STATUS "Using filesystem API:                               ${USING_FILESYSTEM_API_STRING}")
message(STATUS "Using ccache:                                       ${USING_CCACHE_STRING}")
message(STATUS "------------------------------------------------------------------------------")
