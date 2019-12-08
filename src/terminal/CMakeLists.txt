cmake_minimum_required(VERSION 3.10)

#project(libterminal VERSION "0.0.0" LANGUAGES CXX)

find_package(Threads)
include(FilesystemResolver)

option(LIBTERMINAL_TESTING "Enables building of unittests for libterminal [default: ON]" ON)

if(MSVC)
    add_definitions(-DNOMINMAX)
endif()

set(terminal_HEADERS
    util/UTF8.h
    util/overloaded.h
    util/stdfs.h
    Color.h
    Commands.h
    FunctionDef.h
    InputGenerator.h
    OutputGenerator.h
    OutputHandler.h
    Parser.h
    Process.h
    PseudoTerminal.h
    Screen.h
	Selector.h
    Terminal.h
    TerminalProcess.h
    Util.h
    VTType.h
    WindowSize.h
)

set(terminal_SOURCES
    Color.cpp
    Commands.cpp
    FunctionDef.cpp
    InputGenerator.cpp
    OutputGenerator.cpp
    OutputHandler.cpp
    Parser.cpp
    Process.cpp
    PseudoTerminal.cpp
    Screen.cpp
	Selector.cpp
    Terminal.cpp
    TerminalProcess.cpp
    Util.cpp
    VTType.cpp
)

set(LIBTERMINAL_LIBRARIES fmt::fmt-header-only Threads::Threads)
if(UNIX)
    list(APPEND LIBTERMINAL_LIBRARIES util)
endif()

add_library(terminal STATIC ${terminal_SOURCES} ${terminal_HEADERS})
target_include_directories(terminal PUBLIC ${PROJECT_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(terminal PUBLIC ${LIBTERMINAL_LIBRARIES})

# ----------------------------------------------------------------------------
if(LIBTERMINAL_TESTING)
    enable_testing()
    add_executable(terminal_test
        Parser_test.cpp
        Screen_test.cpp
        OutputHandler_test.cpp
		Selector_test.cpp
        terminal_test.cpp
    )
    target_link_libraries(terminal_test fmt::fmt-header-only Catch2::Catch2 terminal)
    add_test(terminal_test ./terminal_test)
endif(LIBTERMINAL_TESTING)
