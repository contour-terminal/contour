cmake_minimum_required(VERSION 3.10)

#project(libterminal VERSION "0.0.0" LANGUAGES CXX)

find_package(Threads)
include(FilesystemResolver)

option(LIBTERMINAL_TESTING "Enables building of unittests for libterminal [default: ON]" ON)
option(LIBTERMINAL_LOG_TRACE "Enables VT sequence tracing. [default: ON]" ON)

# This is an optimization feature that hopefully improves performance when enabled.
# But it's currently disabled by default as I am not fully satisfied with it yet.
option(LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE "Updates the render buffer within the terminal thread if set to ON (otherwise the render buffer is actively refreshed in the render thread)." OFF)

# Tries to avoid unnecessary copies by reusing BufferObject fragments
# in terminal grid lines buffer.
option(LIBTERMINAL_PTY_BUFFER_OBJECTS "PTY Buffer Objects storage [default: ON]" ON)

if(MSVC)
    add_definitions(-DNOMINMAX)
endif()

set(terminal_HEADERS
    Capabilities.h
    Cell.h
    Charset.h
    Color.h
    ColorPalette.h
    Functions.h
    GraphicsAttributes.h
    Grid.h
    Hyperlink.h
    Image.h
    InputBinding.h
    InputGenerator.h
    Line.h
    MatchModes.h
    MockTerm.h
    Parser.h
    Process.h
    RenderBuffer.h
    RenderBufferBuilder.h
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
    pty/UnixPty.h
)

# Platform suffix can be either _win32 or _unix.
if(WIN32)
    set(PLATFORM_SUFFIX "_win32")
else()
    set(PLATFORM_SUFFIX "_unix")
endif()

set(terminal_SOURCES
    Capabilities.cpp
    Cell.cpp
    Charset.cpp
    Color.cpp
    ColorPalette.cpp
    Functions.cpp
    Grid.cpp
    Image.cpp
    InputBinding.cpp
    InputGenerator.cpp
    Line.cpp
    MatchModes.cpp
    MockTerm.cpp
    Parser.cpp
    Process${PLATFORM_SUFFIX}.cpp
    RenderBuffer.cpp
    RenderBufferBuilder.cpp
    Screen.cpp
    Selector.cpp
    Sequence.cpp
    Sequencer.cpp
    SixelParser.cpp
    Terminal.cpp
    TerminalState.cpp
    VTType.cpp
    Viewport.cpp
    primitives.cpp
    pty/MockPty.cpp
    pty/MockViewPty.cpp
    pty/Pty.cpp
)

set(LIBTERMINAL_LIBRARIES crispy::core fmt::fmt-header-only range-v3::range-v3 Threads::Threads Microsoft.GSL::GSL)
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

if(LIBTERMINAL_PTY_BUFFER_OBJECTS)
    target_compile_definitions(terminal PUBLIC LIBTERMINAL_PTY_BUFFER_OBJECTS=1)
endif()

target_include_directories(terminal PUBLIC ${PROJECT_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(terminal PUBLIC ${LIBTERMINAL_LIBRARIES})
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
        Line_test.cpp
        Parser_test.cpp
        Screen_test.cpp
        Sequence_test.cpp
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

    if(CONTOUR_INSTALL_TOOLS)
        if(WIN32)
            install(TARGETS bench-headless DESTINATION bin)
        elseif(APPLE)
            set(INSTALL_RUNTIME_DIR "${App_Contents}/MacOS")
            install(TARGETS bench-headless
                BUNDLE DESTINATION . COMPONENT Runtime
                RUNTIME DESTINATION "${INSTALL_RUNTIME_DIR}" COMPONENT Runtime
            )
        else()
            install(TARGETS bench-headless DESTINATION bin)
        endif()
    endif()
endif()

message(STATUS "[libterminal] Compile unit tests: ${LIBTERMINAL_TESTING}")
message(STATUS "[libterminal] Enable VT sequence tracing: ${LIBTERMINAL_LOG_TRACE}")
