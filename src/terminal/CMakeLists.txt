cmake_minimum_required(VERSION 3.10)

#project(libterminal VERSION "0.0.0" LANGUAGES CXX)

find_package(Threads)
include(FilesystemResolver)

option(LIBTERMINAL_TESTING "Enables building of unittests for libterminal [default: ON]" ON)
option(LIBTERMINAL_LOG_RAW "Enables logging of raw VT sequences [default: ON]" ON)
option(LIBTERMINAL_LOG_TRACE "Enables VT sequence tracing. [default: ON]" ON)

# This is an optimization feature that hopefully improves performance when enabled.
# But it's currently disabled by default as I am not fully satisfied with it yet.
option(LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE "Updates the render buffer within the terminal thread if set to ON (otherwise the render buffer is actively refreshed in the render thread)." OFF)

if(MSVC)
    add_definitions(-DNOMINMAX)
endif()

set(terminal_HEADERS
    Capabilities.h
    Cell.h
    Charset.h
    Color.h
    Functions.h
    GraphicsAttributes.h
    Grid.h
    Hyperlink.h
    Image.h
    InputBinding.h
    InputGenerator.h
    Line.h
    MatchModes.h
    Parser.h
    Process.h
    RenderBuffer.h
    Screen.h
    Selector.h
    Sequence.h
    Sequencer.h
    SixelParser.h
    Terminal.h
    VTType.h
    Viewport.h
    primitives.h
    pty/ConPty.h
    pty/MockPty.h
    pty/MockViewPty.h
    pty/Pty.h
    pty/PtyProcess.h
    pty/UnixPty.h
)

set(terminal_SOURCES
    pty/MockPty.cpp
    pty/MockViewPty.cpp
    pty/PtyProcess.cpp
    Charset.cpp
    Cell.cpp
    Capabilities.cpp
    Color.cpp
    Grid.cpp
    Functions.cpp
    Image.cpp
    InputBinding.cpp
    InputGenerator.cpp
    Line.cpp
    MatchModes.cpp
    Parser.cpp
    Process.cpp
    RenderBuffer.cpp
    Screen.cpp
    Sequence.cpp
    Sequencer.cpp
    Selector.cpp
    SixelParser.cpp
    Terminal.cpp
    VTType.cpp
    primitives.cpp
)

set(LIBTERMINAL_LIBRARIES crispy::core fmt::fmt-header-only range-v3 Threads::Threads GSL)
if(UNIX)
    list(APPEND LIBTERMINAL_LIBRARIES util)
    list(APPEND terminal_SOURCES pty/UnixPty.cpp)
else()
    list(APPEND terminal_SOURCES pty/ConPty.cpp)
    #TODO: list(APPEND terminal_SOURCES pty/WinPty.cpp)
endif()

add_library(terminal STATIC ${terminal_SOURCES} ${terminal_HEADERS})
target_compile_definitions(terminal PRIVATE
    LIBTERMINAL_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    LIBTERMINAL_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    LIBTERMINAL_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    LIBTERMINAL_VERSION_STRING="${CONTOUR_VERSION_STRING}"
    LIBTERMINAL_NAME="${PROJECT_NAME}"
)
target_include_directories(terminal PUBLIC ${PROJECT_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(terminal PUBLIC ${LIBTERMINAL_LIBRARIES})
if(LIBTERMINAL_LOG_RAW)
    target_compile_definitions(terminal PRIVATE LIBTERMINAL_LOG_RAW=1)
endif()
if(LIBTERMINAL_LOG_TRACE)
    target_compile_definitions(terminal PRIVATE LIBTERMINAL_LOG_TRACE=1)
endif()
if(CONTOUR_PERF_STATS)
    target_compile_definitions(terminal PUBLIC CONTOUR_PERF_STATS=1)
endif()


if(LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE AND NOT(WIN32))
    target_compile_definitions(terminal PUBLIC LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE=1)
endif()

# ----------------------------------------------------------------------------
if(LIBTERMINAL_TESTING)
    enable_testing()
    add_executable(terminal_test
        test_main.cpp
        Capabilities_test.cpp
        Color_test.cpp
        InputGenerator_test.cpp
		Selector_test.cpp
        Functions_test.cpp
        Grid_test.cpp
        Parser_test.cpp
        Screen_test.cpp
        Terminal_test.cpp
        SixelParser_test.cpp
    )
    target_link_libraries(terminal_test fmt::fmt-header-only Catch2::Catch2 terminal)
    add_test(terminal_test ./terminal_test)

    add_executable(bench-headless bench-headless.cpp)
    target_compile_definitions(bench-headless PRIVATE
        CONTOUR_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        CONTOUR_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        CONTOUR_VERSION_PATCH=${PROJECT_VERSION_PATCH}
        CONTOUR_VERSION_STRING="${CONTOUR_VERSION_STRING}"
        CONTOUR_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    target_link_libraries(bench-headless fmt::fmt-header-only terminal termbench)
endif()

message(STATUS "[libterminal] Compile unit tests: ${LIBTERMINAL_TESTING}")
message(STATUS "[libterminal] Enable raw VT sequence logging: ${LIBTERMINAL_LOG_RAW}")
message(STATUS "[libterminal] Enable VT sequence tracing: ${LIBTERMINAL_LOG_TRACE}")
