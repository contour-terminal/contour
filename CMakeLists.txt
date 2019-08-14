# vim:ts=4:sw=4:et
cmake_minimum_required(VERSION 3.13)
project(libterminal VERSION "0.0.0" LANGUAGES CXX)

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(ClangTidy)
include(EnableCcache)

find_package(Threads)

set(MASTER_PROJECT OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(MASTER_PROJECT ON)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ${MASTER_PROJECT})
option(LIBTERMINAL_TESTING "Enables building of unittests for libterminal [default: ${MASTER_PROJECT}" ${MASTER_PROJECT})
option(LIBTERMINAL_EMBEDDED_FMTLIB "Uses embedded fmtlib [default: ${MASTER_PROJECT}" ${MASTER_PROJECT})
option(LIBTERMINAL_EMBEDDED_CATCH2 "Uses embedded catch2 for testing [default: ${MASTER_PROJECT}" ${MASTER_PROJECT})

# ----------------------------------------------------------------------------
# fmtlib

if(LIBTERMINAL_EMBEDDED_FMTLIB)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/fmt" EXCLUDE_FROM_ALL)
    add_definitions(-DFMT_USE_WINDOWS_H=0)
else()
    # master project must provide its own fmtlib
endif()

if(LIBTERMINAL_TESTING AND LIBTERMINAL_EMBEDDED_CATCH2)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/catch2")
else()
    # master project must provide its own fmtlib
endif()

# ----------------------------------------------------------------------------
if(NOT MSVC)
    add_definitions(-Wall)
    add_definitions(-Werror)
    add_definitions(-pedantic)
endif()

# ----------------------------------------------------------------------------
set(terminal_HEADERS
    src/terminal/Color.h
    src/terminal/Commands.h
    src/terminal/Generator.h
    src/terminal/OutputHandler.h
    src/terminal/Parser.h
    src/terminal/Process.h
    src/terminal/Screen.h
    src/terminal/Terminal.h
    src/terminal/VTType.h
)

set(terminal_SOURCES
    src/terminal/Color.cpp
    src/terminal/Commands.cpp
    src/terminal/Generator.cpp
    src/terminal/OutputHandler.cpp
    src/terminal/Parser.cpp
    src/terminal/Process.cpp
    src/terminal/Screen.cpp
    src/terminal/Terminal.cpp
    src/terminal/VTType.cpp
)

add_library(terminal STATIC ${terminal_SOURCES} ${terminal_HEADERS})
set_target_properties(terminal PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(terminal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/src)
target_link_libraries(terminal PUBLIC fmt::fmt-header-only util)

# ----------------------------------------------------------------------------
if(LIBTERMINAL_TESTING)
    enable_testing()
    add_executable(terminal_test
        src/terminal/Parser_test.cpp
        src/terminal/Screen_test.cpp
        src/terminal/OutputHandler_test.cpp
        src/terminal/UTF8_test.cpp
    )
    set_target_properties(terminal_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    target_link_libraries(terminal_test fmt::fmt-header-only Catch2::Catch2 terminal)
    add_test(terminal_test ./terminal_test)
endif(LIBTERMINAL_TESTING)
