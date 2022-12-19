cmake_minimum_required(VERSION 3.10)

#project(libterminal VERSION "0.0.0" LANGUAGES CXX)

find_package(Threads)
include(FilesystemResolver)

option(LIBTERMINAL_TESTING "Enables building of unittests for libterminal [default: ON]" ON)
option(LIBTERMINAL_LOG_TRACE "Enables VT sequence tracing. [default: ON]" ON)
option(LIBTERMINAL_CACHE_CURRENT_LINE_POINTER "Enables caching the pointer to the current line, which should improve performance. [default: OFF]" OFF)

# This is an optimization feature that hopefully improves performance when enabled.
# But it's currently disabled by default as I am not fully satisfied with it yet.
option(LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE "Updates the render buffer within the terminal thread if set to ON (otherwise the render buffer is actively refreshed in the render thread)." OFF)

set(terminal_HEADERS
    Capabilities.h
    cell/CellConcept.h
    cell/CellConfig.h
    cell/SimpleCell.h
    cell/CompactCell.h
    CellUtil.h
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
    RenderBuffer.h
    RenderBufferBuilder.h
    Screen.h
    Selector.h
    Sequence.h
    Sequencer.h
    SixelParser.h
    Terminal.h
    VTType.h
    VTWriter.h
    Viewport.h
    ViInputHandler.h
    ViCommands.h
    primitives.h
)

set(terminal_SOURCES
    Capabilities.cpp
    cell/CompactCell.cpp
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
    VTWriter.cpp
    Viewport.cpp
    ViInputHandler.cpp
    ViCommands.cpp
    primitives.cpp
)

add_library(terminal STATIC ${terminal_SOURCES} ${terminal_HEADERS})
target_compile_definitions(terminal PRIVATE
    LIBTERMINAL_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    LIBTERMINAL_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    LIBTERMINAL_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    LIBTERMINAL_VERSION_STRING="${CONTOUR_VERSION_STRING}"
    LIBTERMINAL_NAME="${PROJECT_NAME}"
)

target_include_directories(terminal PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)
target_link_libraries(terminal PUBLIC vtparser vtpty crispy::core fmt::fmt-header-only range-v3::range-v3 Threads::Threads Microsoft.GSL::GSL)
if(LIBTERMINAL_LOG_TRACE)
    target_compile_definitions(terminal PUBLIC LIBTERMINAL_LOG_TRACE=1)
endif()
if(LIBTERMINAL_CACHE_CURRENT_LINE_POINTER)
    target_compile_definitions(terminal PUBLIC LIBTERMINAL_CACHE_CURRENT_LINE_POINTER=1)
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
        #TODO(pr): Grid_test.cpp
        Line_test.cpp
        #TODO(pr): Screen_test.cpp
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
            # See: https://stackoverflow.com/questions/35612687/cmake-macos-x-bundle-with-bundleutiliies-for-qt-application/48035834#48035834
            set(APP_NAME contour)
            set(App_Contents "${APP_NAME}.app/Contents")
            set(INSTALL_RUNTIME_DIR "${App_Contents}/MacOS")
            set(INSTALL_CMAKE_DIR "${App_Contents}/Resources")

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
