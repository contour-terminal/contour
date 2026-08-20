cmake_minimum_required(VERSION 3.10)

find_package(Threads)

option(LIBTERMINAL_TESTING "Enables building of unittests for libterminal [default: ON]" ${CONTOUR_TESTING})
option(LIBTERMINAL_LOG_TRACE "Enables VT sequence tracing. [default: ON]" ON)

# This is an optimization feature that hopefully improves performance when enabled.
# But it's currently disabled by default as I am not fully satisfied with it yet.
option(LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE "Updates the render buffer within the terminal thread if set to ON (otherwise the render buffer is actively refreshed in the render thread)." OFF)

option(LIBTERMINAL_BUILD_BENCH_HEADLESS "Builds bench-headless CLI tool to benchmark libvtbackend [default: OFF]" OFF)

set(vtbackend_HEADERS
    vt/Capabilities.hpp
    core/CellFlags.hpp
    grid/CellProxy.hpp
    grid/CellUtil.hpp
    vt/Charset.hpp
    core/Color.hpp
    core/ContextId.hpp
    core/ColorPalette.hpp
    shell/CommandBlocks.hpp
    vt/ControlCode.hpp
    screen/Cursor.hpp
    vt/DesktopNotification.hpp
    core/FileUrl.hpp
    shell/Folding.hpp
    vt/Functions.hpp
    core/GraphicsAttributes.hpp
    grid/Grid.hpp
    vt/HierarchicalContext.hpp
    input/vi/HintModeHandler.hpp
    core/Hyperlink.hpp
    core/Image.hpp
    input/InputBinding.hpp
    input/InputGenerator.hpp
    input/InputHandler.hpp
    input/vi/JumpHistory.hpp
    vt/KittyClipboard.hpp
    graphics/KittyGraphics.hpp
    grid/Line.hpp
    core/LineFlags.hpp
    grid/LineSoA.hpp
    Logging.hpp
    input/MatchModes.hpp
    graphics/MessageParser.hpp
    testing/MockTerm.hpp
    vt/ModifyKeys.hpp
    vt/PointerShape.hpp
    core/Primitives.hpp
    vt/ProgressState.hpp
    shell/PromptRegion.hpp
    vt/RectangularAreaChecksum.hpp
    render/RenderBuffer.hpp
    screen/RenderBufferBuilder.hpp
    screen/Screen.hpp
    screen/ScreenTestFixtures.hpp
    input/vi/Selector.hpp
    shell/SemanticBlockTracker.hpp
    vt/Sequence.hpp
    vt/SequenceBuilder.hpp
    screen/Settings.hpp
    vt/SgrWriter.hpp
    shell/ShellIntegration.hpp
    graphics/SixelParser.hpp
    grid/SoAClusterWriter.hpp
    screen/StatusLineBuilder.hpp
    screen/Terminal.hpp
    screen/TerminalTestFixtures.hpp
    testing/TestHelpers.hpp
    core/TerminalContext.hpp
    core/TextScale.hpp
    vt/TextSizing.hpp
    vt/VTType.hpp
    vt/VTWriter.hpp
    input/vi/ViCommands.hpp
    input/vi/ViInputHandler.hpp
    screen/Viewport.hpp
    core/WindowSizeStack.hpp
    graphics/regis/ReGISColor.hpp
    graphics/regis/ReGISContext.hpp
    graphics/regis/ReGISFont.hpp
    graphics/regis/ReGISParser.hpp
    graphics/regis/ReGISRasterizer.hpp
    graphics/regis/ReGISTables.hpp
    graphics/regis/ReGISTextRasterizer.hpp
)

set(vtbackend_SOURCES
    vt/Capabilities.cpp
    vt/Charset.cpp
    vt/DesktopNotification.cpp
    vt/ProgressState.cpp
    core/Color.cpp
    core/ColorPalette.cpp
    core/TerminalContext.cpp
    vt/HierarchicalContext.cpp
    shell/CommandBlocks.cpp
    shell/Folding.cpp
    shell/PromptRegion.cpp
    vt/Functions.cpp
    grid/Grid.cpp
    core/FileUrl.cpp
    input/vi/HintModeHandler.cpp
    core/Image.cpp
    input/InputGenerator.cpp
    grid/Line.cpp
    grid/LineSoA.cpp
    graphics/MessageParser.cpp
    testing/MockTerm.cpp
    render/RenderBuffer.cpp
    screen/RenderBufferBuilder.cpp
    vt/KittyClipboard.cpp
    graphics/KittyGraphics.cpp
    vt/TextSizing.cpp
    screen/Screen.cpp
    shell/SemanticBlockTracker.cpp
    input/vi/Selector.cpp
    grid/SoAClusterWriter.cpp
    vt/Sequence.cpp
    graphics/SixelParser.cpp
    graphics/regis/ReGISColor.cpp
    graphics/regis/ReGISRasterizer.cpp
    graphics/regis/ReGISContext.cpp
    graphics/regis/ReGISFont.cpp
    graphics/regis/ReGISTextRasterizer.cpp
    graphics/regis/ReGISParser.cpp
    screen/StatusLineBuilder.cpp
    screen/Terminal.cpp
    vt/VTType.cpp
    vt/VTWriter.cpp
    screen/Viewport.cpp
    input/vi/ViInputHandler.cpp
    input/vi/ViCommands.cpp
    input/vi/JumpHistory.cpp
    core/Primitives.cpp
)

add_library(vtbackend STATIC ${vtbackend_SOURCES} ${vtbackend_HEADERS})
target_compile_definitions(vtbackend PRIVATE
    LIBTERMINAL_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    LIBTERMINAL_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    LIBTERMINAL_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    LIBTERMINAL_VERSION_STRING="${CONTOUR_VERSION_STRING}"
    LIBTERMINAL_NAME="${PROJECT_NAME}"
)

target_include_directories(vtbackend PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)

target_link_libraries(vtbackend PUBLIC
    Microsoft.GSL::GSL
    Threads::Threads
    crispy::core
    unicode::unicode
    vtparser
    vtpty
)

if(LIBTERMINAL_LOG_TRACE)
    target_compile_definitions(vtbackend PUBLIC LIBTERMINAL_LOG_TRACE=1)
endif()
if(CONTOUR_PERF_STATS)
    target_compile_definitions(vtbackend PUBLIC CONTOUR_PERF_STATS=1)
endif()
if(CONTOUR_VERIFY_STATE)
    target_compile_definitions(vtbackend PUBLIC CONTOUR_VERIFY_STATE=1)
endif()

if(LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE AND NOT(WIN32))
    target_compile_definitions(vtbackend PUBLIC LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE=1)
endif()

# ----------------------------------------------------------------------------
if(LIBTERMINAL_TESTING)
    enable_testing()
    add_executable(vtbackend_test
        testing/test_main.cpp
        vt/Capabilities_test.cpp
        vt/DesktopNotification_test.cpp
        vt/ProgressState_test.cpp
        core/Color_test.cpp
        core/ColorPalette_test.cpp
        core/TerminalContext_test.cpp
        vt/HierarchicalContext_test.cpp
        screen/Screen_context_test.cpp
        shell/CommandBlocks_test.cpp
        shell/Folding_test.cpp
        shell/PromptRegion_test.cpp
        graphics/GoodImageProtocol_test.cpp
        graphics/XtSmGraphics_test.cpp
        input/InputGenerator_test.cpp
        input/vi/Selector_test.cpp
        vt/Functions_test.cpp
        grid/Grid_test.cpp
        core/FileUrl_test.cpp
        input/vi/HintModeHandler_test.cpp
        grid/Line_test.cpp
        graphics/MessageParser_test.cpp
        vt/ModifyKeys_test.cpp
        vt/VTType_test.cpp
        vt/RectangularAreaChecksum_test.cpp
        vt/KittyClipboard_test.cpp
        vt/PointerShape_test.cpp
        graphics/KittyGraphics_test.cpp
        vt/TextSizing_test.cpp
        screen/ScreenTestFixtures.cpp
        screen/TerminalTestFixtures.cpp
        screen/Screen_test.cpp
        screen/Screen_charset_test.cpp
        screen/Screen_rect_test.cpp
        screen/Screen_modes_test.cpp
        screen/Screen_osc_test.cpp
        screen/Screen_macros_test.cpp
        screen/Screen_multipage_test.cpp
        core/Image_test.cpp
        vt/Sequence_test.cpp
        screen/StatusLineBuilder_test.cpp
        core/WindowSizeStack_test.cpp
        screen/Terminal_test.cpp
        screen/Terminal_scroll_test.cpp
        screen/Terminal_selection_test.cpp
        screen/Terminal_input_test.cpp
        shell/ShellIntegration_test.cpp
        grid/LineSoA_test.cpp
        grid/SoAClusterWriter_test.cpp
        graphics/SixelParser_test.cpp
        graphics/regis/ReGISScreen_test.cpp
        graphics/regis/ReGISTables_test.cpp
        graphics/regis/ReGISColor_test.cpp
        graphics/regis/ReGISRasterizer_test.cpp
        graphics/regis/ReGISFont_test.cpp
        graphics/regis/ReGISTextRasterizer_test.cpp
        graphics/regis/ReGISParser_test.cpp
        input/vi/ViCommands_test.cpp
        vt/VTWriter_test.cpp
    )
    target_link_libraries(vtbackend_test Catch2::Catch2 vtbackend)
    add_test(NAME vtbackend_test COMMAND $<TARGET_FILE:vtbackend_test>)

    if(NOT WIN32)
        target_compile_options(vtbackend_test PRIVATE -Wno-c2y-extensions)
    endif()

    if (LIBTERMINAL_BUILD_BENCH_HEADLESS)
        add_executable(bench-headless bench-headless.cpp)
        target_compile_definitions(bench-headless PRIVATE
            CONTOUR_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
            CONTOUR_VERSION_MINOR=${PROJECT_VERSION_MINOR}
            CONTOUR_VERSION_PATCH=${PROJECT_VERSION_PATCH}
            CONTOUR_VERSION_STRING="${CONTOUR_VERSION_STRING}"
            CONTOUR_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        )

        target_link_libraries(bench-headless
            termbench::termbench
            vtbackend
        )

        if(CONTOUR_INSTALL_TOOLS)
            if(APPLE)
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
endif()

option(VTBACKEND_DOC_TOOL "Tool to generate documentation for vtbackend [default: OFF]" OFF)
if(VTBACKEND_DOC_TOOL)
    add_executable(vtbackend-doc-tool doc-tool.cpp)
    target_link_libraries(vtbackend-doc-tool vtbackend)
    # install(TARGETS vtbackend-doc-tool DESTINATION bin)
endif()

message(STATUS "[vtbackend] Compile unit tests: ${LIBTERMINAL_TESTING}")
message(STATUS "[vtbackend] Enable VT sequence tracing: ${LIBTERMINAL_LOG_TRACE}")
message(STATUS "[vtbackend] Enable passive render buffer update: ${LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE}")
message(STATUS "[vtbackend] Build bench-headless: ${LIBTERMINAL_BUILD_BENCH_HEADLESS}")
message(STATUS "[vtbackend] Build documentation tool: ${VTBACKEND_DOC_TOOL}")
