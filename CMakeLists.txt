cmake_minimum_required(VERSION 3.10)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(Version)
GetVersionInformation(CONTOUR_VERSION CONTOUR_VERSION_STRING)
file(WRITE "${CMAKE_BINARY_DIR}/version.txt" "${CONTOUR_VERSION_STRING}")

project(contour VERSION "${CONTOUR_VERSION}" LANGUAGES CXX)

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
option(CONTOUR_EMBEDDED_RANGE_V3 "Uses embedded range-v3 [default: ON]" ON)
option(CONTOUR_EMBEDDED_YAML_CPP "Uses embedded yaml-cpp [default: ON]" ON)
option(CONTOUR_EMBEDDED_LIBUNICODE "Uses embedded libunicode [default: ON]" ON)
option(CONTOUR_EMBEDDED_CATCH2 "Uses embedded catch2 for testing [default: ON]" ON)
option(CONTOUR_EXAMPLES "Enables building of example programs. [default: ON]" ON)
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

include(3rdparty)
set(3rdparty_prefix "${CMAKE_CURRENT_BINARY_DIR}/3rdparty")

if(NOT TARGET fmt::fmt-header-only)
    if(CONTOUR_EMBEDDED_FMTLIB)
        set(CONTOUR_EMBEDDED_FMTLIB_VERSION 7.1.3                              CACHE INTERNAL "Embedded fmtlib version")
        set(CONTOUR_EMBEDDED_FMTLIB_MD5SUM  2522ec65070c0bda0ca288677ded2831   CACHE INTERNAL "Embedded fmtlib md5sum")
        contour_add_fmtlib()
    else()
        find_package(fmt REQUIRED)
    endif()
endif()

if(NOT TARGET range-v3::range-v3)
    if(CONTOUR_EMBEDDED_RANGE_V3)
        set(CONTOUR_EMBEDDED_RANGE_V3_VERSION 0.11.0 CACHE INTERNAL "Embedded range-v3 version")
        set(CONTOUR_EMBEDDED_RANGE_V3_MD5SUM 97ab1653f3aa5f9e3d8200ee2a4911d3 CACHE INTERNAL "Embedded range-v3 md5sum")
        contour_add_range_v3()
    else()
        find_package(range-v3 REQUIRED)
    endif()
endif()

if(NOT TARGET yaml-cpp::yaml-cpp)
    if(CONTOUR_EMBEDDED_YAML_CPP)
        set(CONTOUR_EMBEDDED_YAML_CPP_VERSION 0.6.3                            CACHE INTERNAL "Embedded yaml-cpp version")
        set(CONTOUR_EMBEDDED_YAML_CPP_MD5SUM  b45bf1089a382e81f6b661062c10d0c2 CACHE INTERNAL "Embedded yaml-cpp md5sum")
        contour_add_yaml_cpp()
    else()
        find_package(yaml-cpp REQUIRED)
        add_library(yaml-cpp-proxy INTERFACE)
        target_include_directories(yaml-cpp-proxy INTERFACE ${YAML_CPP_INCLUDE_DIR})
        target_link_libraries(yaml-cpp-proxy INTERFACE ${YAML_CPP_LIBRARIES})
        add_library(yaml-cpp::yaml-cpp ALIAS yaml-cpp-proxy)
    endif()
endif()

if(NOT TARGET unicode::core)
    if(CONTOUR_EMBEDDED_LIBUNICODE)
        set(CONTOUR_EMBEDDED_LIBUNICODE_VERSION master                         CACHE INTERNAL "Embedded libunicode version")
        set(CONTOUR_EMBEDDED_LIBUNICODE_MD5SUM  SKIP                           CACHE INTERNAL "Embedded libunicode md5sum")
        contour_add_libunicode()
    else()
        # find_package(unicode REQUIRED) # TODO
        message(FATAL_ERROR "System version of libunicode does not exist yet.")
    endif()
endif()

if(NOT TARGET Catch2::Catch2)
    if(CONTOUR_EMBEDDED_CATCH2)
        set(CONTOUR_EMBEDDED_CATCH2_VERSION 2.13.4                             CACHE INTERNAL "Embedded catch2 version")
        set(CONTOUR_EMBEDDED_CATCH2_MD5SUM  7854bb4974e8e88dcdfcde21bfcda963   CACHE INTERNAL "Embedded catch2 md5sum")
        contour_add_catch2()
    else()
        find_package(Catch2 REQUIRED)
    endif()
endif()

# ----------------------------------------------------------------------------
add_subdirectory(src/crispy)
add_subdirectory(src/text_shaper)
add_subdirectory(src/terminal)

if(CONTOUR_FRONTEND_GUI)
    add_subdirectory(src/terminal_renderer)
    add_subdirectory(src/terminal_view)
    add_subdirectory(src/terminal_renderer/opengl)
endif()

add_subdirectory(src/contour)

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
message(STATUS "    Contour (${CONTOUR_VERSION_STRING})")
message(STATUS "------------------------------------------------------------------------------")
message(STATUS "Build type:                         ${CMAKE_BUILD_TYPE}")
message(STATUS "Build with sanitizer:               ${CONTOUR_SANITIZE}")
message(STATUS "Build unit tests:                   ${CONTOUR_TESTING}")
message(STATUS "Enable with code coverage:          ${CONTOUR_CODE_COVERAGE_ENABLED}")
message(STATUS "Build contour frontend GUI:         ${CONTOUR_FRONTEND_GUI}")
message(STATUS "|> Enable blur effect on KWin:      ${CONTOUR_BLUR_PLATFORM_KWIN}")
message(STATUS "|> Enable performance metrics:      ${CONTOUR_PERF_STATS}")
message(STATUS "|> OpenGL preference:               ${OpenGL_GL_PREFERENCE}")
message(STATUS "Using filesystem API:               ${USING_FILESYSTEM_API_STRING}")
message(STATUS "Using ccache:                       ${USING_CCACHE_STRING}")
message(STATUS "------------------------------------------------------------------------------")
message(STATUS "Using embedded fmtlib:              ${CONTOUR_EMBEDDED_FMTLIB}")
message(STATUS "Using embedded yaml-cpp:            ${CONTOUR_EMBEDDED_YAML_CPP}")
message(STATUS "Using embedded libunicode:          ${CONTOUR_EMBEDDED_LIBUNICODE}")
message(STATUS "Using embedded catch2 for testing:  ${CONTOUR_EMBEDDED_CATCH2}")
message(STATUS "------------------------------------------------------------------------------")
