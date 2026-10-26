cmake_minimum_required(VERSION 3.10)

find_package(Threads)

option(LIBTERMINAL_TESTING "Enables building of unittests for libterminal [default: ON]" ${CONTOUR_TESTING})
option(LIBTERMINAL_LOG_TRACE "Enables VT sequence tracing. [default: ON]" ON)

# This is an optimization feature that hopefully improves performance when enabled.
# But it's currently disabled by default as I am not fully satisfied with it yet.
option(LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE "Updates the render buffer within the terminal thread if set to ON (otherwise the render buffer is actively refreshed in the render thread)." OFF)

option(LIBTERMINAL_BUILD_BENCH_HEADLESS "Builds bench-headless CLI tool to benchmark libvtbackend [default: OFF]" OFF)

set(vtbackend_HEADERS
    screen/AsciiText.hpp
    vt/Capabilities.hpp
    testing/CatchStringMakers.hpp
    testing/DetectingTerm.hpp
    testing/FakeStatusLineExtensions.hpp
    testing/InMemoryGitFileSystem.hpp
    core/CellFlags.hpp
    grid/CellProxy.hpp
    grid/CellUtil.hpp
    vt/Charset.hpp
    core/Color.hpp
    core/ContextId.hpp
    core/CommandBlockId.hpp
    core/CommandBlockOutcome.hpp
    core/ColorPalette.hpp
    shell/CommandBlock.hpp
    shell/CommandBlockAddress.hpp
    shell/CommandBlockRows.hpp
    shell/CommandBlockPaths.hpp
    shell/CommandLineSanitizer.hpp
    shell/CommandLineText.hpp
    vt/ControlCode.hpp
    screen/Cursor.hpp
    vt/DesktopNotification.hpp
    core/FileUrl.hpp
    shell/Folding.hpp
    vt/Functions.hpp
    core/GraphicsAttributes.hpp
    grid/GraphemeClusters.hpp
    grid/Grid.hpp
    screen/Gutter.hpp
    screen/GutterSettings.hpp
    vt/HierarchicalContext.hpp
    vt/ProgramStatus.hpp
    vt/SemanticBlockQuery.hpp
    screen/ProgramStatusSet.hpp
    vt/JsonString.hpp
    input/vi/HintModeHandler.hpp
    core/Hyperlink.hpp
    core/Image.hpp
    input/ClickCounter.hpp
    input/InputBinding.hpp
    input/InputGenerator.hpp
    input/HeldModifiers.hpp
    input/InputHandler.hpp
    input/vi/JumpHistory.hpp
    vt/KittyClipboard.hpp
    graphics/KittyGraphics.hpp
    grid/Line.hpp
    core/LineFlags.hpp
    grid/LineSoA.hpp
    core/LinkDecoration.hpp
    screen/LinkDetector.hpp
    links/LinkDetectionPolicy.hpp
    links/LinkIndex.hpp
    links/LinkMatcher.hpp
    links/LinkPatterns.hpp
    links/LinkUrlTemplate.hpp
    links/LinkTypes.hpp
    links/PathProbe.hpp
    links/PathResolution.hpp
    shell/LivePromptMarks.hpp
    screen/LinkPresentation.hpp
    screen/LocalTime.hpp
    screen/PrintedLinks.hpp
    Logging.hpp
    vt/MacroDefinition.hpp
    shell/MarkArbiter.hpp
    input/MatchModes.hpp
    graphics/MessageParser.hpp
    testing/MockTerm.hpp
    vt/ModifyKeys.hpp
    vt/PointerShape.hpp
    core/Primitives.hpp
    core/Search.hpp
    vt/ProgressState.hpp
    shell/PromptRegion.hpp
    vt/RectangularAreaChecksum.hpp
    render/RenderBuffer.hpp
    screen/RenderBufferBuilder.hpp
    core/RowMove.hpp
    links/ScanText.hpp
    screen/Screen.hpp
    screen/ScreenTestFixtures.hpp
    shell/ScrollbarMarks.hpp
    input/vi/Selector.hpp
    shell/SemanticBlockTracker.hpp
    vt/SemanticPrompt.hpp
    vt/Sequence.hpp
    vt/SequenceBuilder.hpp
    screen/Settings.hpp
    vt/SgrWriter.hpp
    graphics/SixelParser.hpp
    grid/SoAClusterWriter.hpp
    shell/StableIdSet.hpp
    screen/StatusLineBuilder.hpp
    screen/StatusLineLayout.hpp
    screen/GitHead.hpp
    core/SteadyClockRef.hpp
    shell/StickyHeader.hpp
    core/StickyHeaderKind.hpp
    screen/StatusLineCommandCache.hpp
    screen/StatusLineExtensions.hpp
    screen/Terminal.hpp
    screen/TerminalClocks.hpp
    screen/TerminalTestFixtures.hpp
    testing/TestHelpers.hpp
    core/TerminalContext.hpp
    core/TextScale.hpp
    core/Utf8Prefix.hpp
    vt/TextSizing.hpp
    shell/UserMarkIndex.hpp
    vt/VTType.hpp
    vt/VTWriter.hpp
    input/vi/ViCommands.hpp
    input/vi/ViInputHandler.hpp
    screen/Viewport.hpp
    core/WindowSizeStack.hpp
    grid/WordWalk.hpp
    core/WorkingDirectory.hpp
    testing/Workloads.hpp
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
    vt/ProgramStatus.cpp
    screen/ProgramStatusSet.cpp
    vt/JsonString.cpp
    core/WorkingDirectory.cpp
    shell/CommandBlock.cpp
    shell/CommandBlockRows.cpp
    shell/CommandBlockPaths.cpp
    shell/CommandLineSanitizer.cpp
    shell/Folding.cpp
    shell/PromptRegion.cpp
    shell/ScrollbarMarks.cpp
    shell/StickyHeader.cpp
    vt/Functions.cpp
    grid/GraphemeClusters.cpp
    grid/Grid.cpp
    screen/Gutter.cpp
    core/FileUrl.cpp
    input/vi/HintModeHandler.cpp
    core/Image.cpp
    input/ClickCounter.cpp
    input/InputGenerator.cpp
    grid/Line.cpp
    grid/LineSoA.cpp
    shell/LivePromptMarks.cpp
    screen/LocalTime.cpp
    vt/MacroDefinition.cpp
    graphics/MessageParser.cpp
    testing/MockTerm.cpp
    render/RenderBuffer.cpp
    screen/RenderBufferBuilder.cpp
    vt/KittyClipboard.cpp
    graphics/KittyGraphics.cpp
    vt/TextSizing.cpp
    screen/LinkDetector.cpp
    links/LinkIndex.cpp
    links/LinkMatcher.cpp
    links/LinkPatterns.cpp
    links/LinkUrlTemplate.cpp
    links/PathProbe.cpp
    links/PathResolution.cpp
    links/ScanText.cpp
    screen/PrintedLinks.cpp
    screen/Screen.cpp
    shell/SemanticBlockTracker.cpp
    vt/SemanticPrompt.cpp
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
    screen/GitHead.cpp
    screen/StatusLineCommandCache.cpp
    screen/StatusLineExtensions.cpp
    screen/Terminal.cpp
    vt/VTType.cpp
    vt/VTWriter.cpp
    screen/Viewport.cpp
    input/vi/ViInputHandler.cpp
    input/vi/ViCommands.cpp
    input/vi/JumpHistory.cpp
    core/Primitives.cpp
    core/Search.cpp
)

add_library(vtbackend STATIC ${vtbackend_SOURCES} ${vtbackend_HEADERS})
# The version string embeds the branch name and HEAD sha. The compile cache keys on the command line,
# so it must not sit on every vtbackend source: only Screen.cpp reads it.
set_property(SOURCE screen/Screen.cpp APPEND PROPERTY COMPILE_DEFINITIONS
    LIBTERMINAL_VERSION_STRING="${CONTOUR_VERSION_STRING}")
target_compile_definitions(vtbackend PRIVATE
    LIBTERMINAL_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    LIBTERMINAL_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    LIBTERMINAL_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    LIBTERMINAL_NAME="${PROJECT_NAME}"
)

target_include_directories(vtbackend PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)

# contour::tracy is PUBLIC, not PRIVATE: Terminal.hpp declares _stateMutex through TracyLockable,
# so every consumer of vtbackend compiles that macro and needs the header it comes from.
target_link_libraries(vtbackend PUBLIC
    Microsoft.GSL::GSL
    boxed-cpp::boxed-cpp
    Threads::Threads
    contour::tracy
    core::async
    core::base
    core::cli
    core::log
    core::platform
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

# Makes Catch2 print UTF-32 text readably in the failure output of every file of test binary TARGET.
#
# Force-included rather than included by the tests that want it: a printer that only some files of a
# binary see is an ODR violation the linker settles by keeping one definition for all of them (see
# testing/CatchStringMakers.hpp). Outside the testing block below, as other modules' tests call it too.
function(vtbackend_force_include_test_printers target)
    set(printers "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/testing/CatchStringMakers.hpp")
    if(MSVC)
        target_compile_options(${target} PRIVATE "/FI${printers}")
    else()
        target_compile_options(${target} PRIVATE "SHELL:-include ${printers}")
    endif()
endfunction()

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
        vt/ProgramStatus_test.cpp
        screen/ProgramStatusSet_test.cpp
        screen/Terminal_programStatus_test.cpp
        vt/JsonString_test.cpp
        screen/Screen_context_test.cpp
        shell/MarkArbiter_test.cpp
        core/WorkingDirectory_test.cpp
        shell/CommandBlock_test.cpp
        shell/CommandBlockRows_test.cpp
        shell/CommandBlockPaths_test.cpp
        shell/CommandLineSanitizer_test.cpp
        shell/CommandLineText_test.cpp
        shell/Folding_test.cpp
        screen/Gutter_test.cpp
        shell/PromptRegion_test.cpp
        screen/LinkDetector_test.cpp
        links/LinkDetectionPolicy_test.cpp
        links/LinkIndex_test.cpp
        links/LinkMatcher_test.cpp
        links/LinkPatterns_test.cpp
        links/LinkUrlTemplate_test.cpp
        screen/LinkPresentation_test.cpp
        links/PathResolution_test.cpp
        links/PathProbe_test.cpp
        links/ScanText_test.cpp
        screen/PrintedLinks_test.cpp
        shell/ScrollbarMarks_test.cpp
        shell/StableIdSet_test.cpp
        shell/UserMarkIndex_test.cpp
        shell/StickyHeader_test.cpp
        render/RenderBuffer_test.cpp
        screen/RenderBufferBuilder_test.cpp
        screen/Terminal_stickyheader_test.cpp
        shell/Osc133Reporting_test.cpp
        graphics/GoodImageProtocol_test.cpp
        graphics/XtSmGraphics_test.cpp
        input/ClickCounter_test.cpp
        input/HeldModifiers_test.cpp
        input/InputGenerator_test.cpp
        input/vi/Selector_test.cpp
        vt/Functions_test.cpp
        grid/Grid_test.cpp
        core/FileUrl_test.cpp
        input/vi/HintModeHandler_test.cpp
        grid/Line_test.cpp
        shell/LivePromptMarks_test.cpp
        graphics/MessageParser_test.cpp
        vt/ModifyKeys_test.cpp
        vt/VTType_test.cpp
        vt/RectangularAreaChecksum_test.cpp
        vt/KittyClipboard_test.cpp
        vt/MacroDefinition_test.cpp
        vt/PointerShape_test.cpp
        graphics/KittyGraphics_test.cpp
        vt/TextSizing_test.cpp
        screen/ScreenTestFixtures.cpp
        screen/TerminalTestFixtures.cpp
        screen/AsciiText_test.cpp
        screen/Screen_test.cpp
        screen/Screen_charset_test.cpp
        screen/Screen_rect_test.cpp
        screen/Screen_modes_test.cpp
        screen/Screen_osc_test.cpp
        screen/Screen_macros_test.cpp
        screen/Screen_multipage_test.cpp
        core/Image_test.cpp
        core/Search_test.cpp
        vt/SemanticPrompt_test.cpp
        vt/Sequence_test.cpp
        screen/StatusLineBuilder_test.cpp
        screen/StatusLineLayout_test.cpp
        screen/GitHead_test.cpp
        screen/StatusLineCommandCache_test.cpp
        core/WindowSizeStack_test.cpp
        screen/Terminal_test.cpp
        screen/Terminal_extensions_test.cpp
        screen/Terminal_scroll_test.cpp
        screen/Terminal_selection_test.cpp
        screen/Terminal_block_actions_test.cpp
        screen/Terminal_input_test.cpp
        screen/Terminal_scrollbar_marks_test.cpp
        screen/Terminal_gutter_test.cpp
        screen/Terminal_mirrorSeams_test.cpp
        screen/Terminal_blocks_test.cpp
        screen/Terminal_links_test.cpp
        screen/BulkScan_test.cpp
        shell/ShellIntegration_test.cpp
        grid/GraphemeClusters_test.cpp
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
        testing/Workloads_test.cpp
        vt/VTWriter_test.cpp
    )
    target_link_libraries(vtbackend_test Catch2::Catch2 core::testing vtbackend)
    add_test(NAME vtbackend_test COMMAND $<TARGET_FILE:vtbackend_test>)
    vtbackend_force_include_test_printers(vtbackend_test)

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
        if(CONTOUR_EXTENSIONS)
            # `throughput extensions LIST` runs status-line extensions beside the measured terminal. The tool
            # links them; the vtbackend library does not.
            # A composition root: it creates the WebAssembly runtime backend (makeDefaultBackend()).
            target_link_libraries(bench-headless vtextension vtextension_runtime)
        endif()

        # A short run of throughput, snapshot, keypress and pty each, so that a crash like the one
        # `bench-headless pty` had -- it used its PTY before opening it -- fails the suite.
        add_test(NAME bench_headless_throughput_smoke COMMAND bench-headless throughput size 1 runs 1)
        # A name that is not a workload must fail, naming the ones that are.
        add_test(NAME bench_headless_throughput_rejects_unknown_kind
                 COMMAND bench-headless throughput size 1 runs 1 kinds no-such-workload)
        set_tests_properties(bench_headless_throughput_rejects_unknown_kind PROPERTIES PASS_REGULAR_EXPRESSION
                             "Unknown workload 'no-such-workload'\\. Known: ascii, colored, sgr, utf8, utf8sparse, tui, link-prose, link-line\\.")
        add_test(NAME bench_headless_snapshot_smoke COMMAND bench-headless snapshot frames 10 runs 1)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            # Reads the resident set from /proc; what it reports is not asserted.
            add_test(NAME bench_headless_memory_smoke COMMAND bench-headless memory panes 2 seconds 0)
        endif()
        # The benchmarks with an application on a real PTY: a script under /bin/sh on POSIX, and
        # `bench-headless pty-child` through ConPTY on Windows.
        add_test(NAME bench_headless_throughput_pty_smoke
                 COMMAND bench-headless throughput size 1 runs 1 kinds ascii screens primary pty)
        # Link detection on, as the GUI runs it: the row reports the match jobs inside the run and after it, and
        # the links indexed. How many, and whether the run was too short for detection to scan inside it, is
        # the machine's speed to say; only the warning's wording is checked, where it appears. Through ConPTY
        # the row also says how much of the output arrived, and the lines may end in CRLF.
        add_test(NAME bench_headless_throughput_links_smoke
                 COMMAND bench-headless throughput size 1 runs 1 kinds ascii screens primary pty refresh-hz 60 links)
        set_tests_properties(bench_headless_throughput_links_smoke PROPERTIES PASS_REGULAR_EXPRESSION
                             "link detection: on\r?\nascii +primary +[0-9.]+ MiB/s +[0-9]+ bytes/read( +[0-9.]+% arrived)? +[0-9]+ match jobs in the run, +[0-9]+ after it, +[0-9]+ links(  \\(under [0-9]+ ms, detection's longest wait: it could not scan inside the measurement\\))?\r?\n")
        add_test(NAME bench_headless_keypress_smoke COMMAND bench-headless keypress seconds 1 rate 100)
        set(BENCH_HEADLESS_SMOKE_TESTS
            bench_headless_throughput_smoke
            bench_headless_throughput_rejects_unknown_kind
            bench_headless_snapshot_smoke
            bench_headless_throughput_pty_smoke
            bench_headless_throughput_links_smoke
            bench_headless_keypress_smoke)
        # ConPTY has no slave end that `bench-headless pty` could write into.
        if(NOT WIN32)
            add_test(NAME bench_headless_pty_smoke COMMAND bench-headless pty seconds 1)
            list(APPEND BENCH_HEADLESS_SMOKE_TESTS bench_headless_pty_smoke)
        endif()
        # A hang -- a PTY nobody drains any more, say -- fails within a minute, not at CI's step timeout.
        set_tests_properties(${BENCH_HEADLESS_SMOKE_TESTS} PROPERTIES LABELS bench TIMEOUT 60)

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
