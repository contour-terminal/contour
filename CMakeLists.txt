cmake_minimum_required(VERSION 3.10)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(COMMAND git describe --always --tags --dirty=-modified
        OUTPUT_VARIABLE CONTOUR_VERSION_SUFFIX
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(CONTOUR_VERSION_SUFFIX "-${CONTOUR_VERSION_SUFFIX}")
else()
    set(CONTOUR_VERSION_SUFFIX "")
endif()

project(contour VERSION "0.0.1${CMAKE_VERSION_SUFFIX}" LANGUAGES CXX)

# setting defaults
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(("${CMAKE_CXX_COMPILER_ID}" MATCHES "GNU") OR ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang"))
    add_compile_options(-Wall)
    add_compile_options(-Wextra)
elseif(DEFINED MSVC)
    add_definitions(-DNOMINMAX)
    add_definitions(-D_USE_MATH_DEFINES)
endif()

option(USE_BOOST_FILESYSTEM "Compiles and links against boost::filesystem instead of std::filesystem [default: OFF]." OFF)

if("${OpenGL_GL_PREFERENCE}" STREQUAL "")
    # possible values are: GLVND, LEGACY
    set(OpenGL_GL_PREFERENCE "GLVND")
endif()

include(EnableCcache)
include(ClangTidy)
include(CIncludeMe)
find_package(Threads)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CONTOUR_TESTING "Enables building of unittests for libterminal [default: ON" ON)
option(CONTOUR_EMBEDDED_FMTLIB "Uses embedded fmtlib [default: ON]" ON)
option(CONTOUR_EMBEDDED_YAML_CPP "Uses embedded yaml-cpp [default: ON]" ON)
option(CONTOUR_EMBEDDED_CATCH2 "Uses embedded catch2 for testing [default: ON]" ON)
option(CONTOUR_EXAMPLES "Enables building of example programs. [default: ON]" ON)
option(CONTOUR_CLIENT "Enables building of OpenGL terminal view. [default: ON]" ON)
option(CONTOUR_COVERAGE "Builds with codecov [default: OFF]" OFF)
option(CONTOUR_SANITIZE "Builds with Address sanitizer enabled [default: OFF]" OFF)

# ----------------------------------------------------------------------------
# code coverage

if(CONTOUR_COVERAGE AND NOT MSVC)
    add_compile_options(-g --coverage)
    set(CMAKE_EXE_LINKER_FLAGS "--coverage ${CMAKE_EXE_LINKER_FLAGS}")
    set(CONTOUR_CODE_COVERAGE_ENABLED ON)
else()
    set(CONTOUR_CODE_COVERAGE_ENABLED OFF)
endif()

# ----------------------------------------------------------------------------
# 3rdparty dependencies

if(CONTOUR_EMBEDDED_FMTLIB)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/fmt" EXCLUDE_FROM_ALL)
    add_definitions(-DFMT_USE_WINDOWS_H=0)
else()
    # master project must provide its own fmtlib
endif()

#set(LIBTERMINAL_TESTING ${CONTOUR_TESTING})
if(CONTOUR_TESTING AND CONTOUR_EMBEDDED_CATCH2)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/catch2")
else()
    # master project must provide its own fmtlib
endif()

if(CONTOUR_EMBEDDED_YAML_CPP)
	add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/yaml-cpp")
else()
	# master project must provide its own fmtlib
endif()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/libunicode")

# ----------------------------------------------------------------------------
add_subdirectory(src/crispy)
add_subdirectory(src/terminal)
add_subdirectory(src/terminal_view)

if(CONTOUR_CLIENT)
    add_subdirectory(src/contour)
endif()

if(CONTOUR_EXAMPLES)
    add_subdirectory(examples)
endif()

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
message(STATUS "    Contour (version ${contour_VERSION}${CONTOUR_VERSION_SUFFIX})")
message(STATUS "------------------------------------------------------------------------------")
message(STATUS "Build type:                  ${CMAKE_BUILD_TYPE}")
message(STATUS "Build with sanitizer:        ${CONTOUR_SANITIZE}")
message(STATUS "Build unit tests:            ${CONTOUR_TESTING}")
message(STATUS "Build contour client:        ${CONTOUR_CLIENT}")
message(STATUS "Enable blur effect on KWin:  ${CONTOUR_BLUR_PLATFORM_KWIN}")
message(STATUS "Enable performance metrics:  ${CONTOUR_PERF_STATS}")
message(STATUS "Enable with code coverage:   ${CONTOUR_CODE_COVERAGE_ENABLED}")
message(STATUS "OpenGL preference:           ${OpenGL_GL_PREFERENCE}")
message(STATUS "Using filesystem API:        ${USING_FILESYSTEM_API_STRING}")
message(STATUS "Using ccache:                ${USING_CCACHE_STRING}")
message(STATUS "------------------------------------------------------------------------------")
