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
option(CONTOUR_FRONTEND_GUI "Enables GUI frontend." ON)
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
# 3rdparty dependencies
include(ThirdParties)

ThirdPartiesAdd_Catch2()
ThirdPartiesAdd_fmtlib()
ThirdPartiesAdd_libunicode()
ThirdPartiesAdd_range_v3()
ThirdPartiesAdd_yaml_cpp()

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
