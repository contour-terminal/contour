cmake_minimum_required(VERSION 3.10)

#project(libterminal VERSION "0.0.0" LANGUAGES CXX)

find_package(Threads)
include(FilesystemResolver)

option(LIBTERMINAL_TESTING "Enables building of unittests for libterminal [default: ON]" ON)
option(LIBTERMINAL_LOG_RAW "Enables logging of raw VT sequences [default: ON]" OFF)
option(LIBTERMINAL_LOG_TRACE "Enables VT sequence tracing. [default: ON]" OFF)
option(LIBTERMINAL_EXECUTION_PAR "Builds with parallel execution where possible [default: OFF]" OFF)

if(MSVC)
    add_definitions(-DNOMINMAX)
endif()

set(terminal_HEADERS
    Charset.h
    Color.h
    Functions.h
    Image.h
    InputGenerator.h
    Parser.h
    Process.h
    pty/Pty.h
    pty/UnixPty.h
    pty/ConPty.h
    Screen.h
    Selector.h
    Sequencer.h
    SixelParser.h
    Terminal.h
    VTType.h
    Size.h
)

set(terminal_SOURCES
    Charset.cpp
    Color.cpp
    Functions.cpp
    Image.cpp
    InputGenerator.cpp
    Parser.cpp
    Process.cpp
    Screen.cpp
    Sequencer.cpp
    Selector.cpp
    SixelParser.cpp
    Terminal.cpp
    VTType.cpp
)

set(LIBTERMINAL_LIBRARIES crispy::core fmt::fmt-header-only Threads::Threads)
if(UNIX)
    list(APPEND LIBTERMINAL_LIBRARIES util)
    list(APPEND terminal_SOURCES pty/UnixPty.cpp)
else()
    list(APPEND terminal_SOURCES pty/ConPty.cpp)
    #TODO: list(APPEND terminal_SOURCES pty/WinPty.cpp)
endif()
if(LIBTERMINAL_EXECUTION_PAR)
    add_definitions(-DLIBTERMINAL_EXECUTION_PAR=1)
    list(APPEND LIBTERMINAL_LIBRARIES tbb)
endif()

add_library(terminal STATIC ${terminal_SOURCES} ${terminal_HEADERS})
target_compile_definitions(terminal PRIVATE
    LIBTERMINAL_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    LIBTERMINAL_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    LIBTERMINAL_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)
target_include_directories(terminal PUBLIC ${PROJECT_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(terminal PUBLIC ${LIBTERMINAL_LIBRARIES})
if(LIBTERMINAL_LOG_RAW)
    target_compile_definitions(terminal PRIVATE LIBTERMINAL_LOG_RAW=1)
endif()
if(LIBTERMINAL_LOG_TRACE)
    target_compile_definitions(terminal PRIVATE LIBTERMINAL_LOG_TRACE=1)
endif()

# ----------------------------------------------------------------------------
if(LIBTERMINAL_TESTING)
    enable_testing()
    add_executable(terminal_test
        test_main.cpp
		Selector_test.cpp
        Functions_test.cpp
        Parser_test.cpp
        Screen_test.cpp
        Size_test.cpp
        SixelParser_test.cpp
    )
    target_link_libraries(terminal_test fmt::fmt-header-only Catch2::Catch2 terminal)
    add_test(terminal_test ./terminal_test)
endif(LIBTERMINAL_TESTING)

message(STATUS "[libterminal] Compile unit tests: ${LIBTERMINAL_TESTING}")
message(STATUS "[libterminal] Enable raw VT sequence logging: ${LIBTERMINAL_LOG_RAW}")
message(STATUS "[libterminal] Enable VT sequence tracing: ${LIBTERMINAL_LOG_TRACE}")
