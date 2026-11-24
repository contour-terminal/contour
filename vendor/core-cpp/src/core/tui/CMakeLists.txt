# SPDX-License-Identifier: Apache-2.0

# core::tui_output: the leaf of endo's terminal UI (endo f774a210), the part that composes escape
# sequences and writes them. Its row in cmake/CoreCppModules.cmake says DEPS base, so the layering
# check refuses a link to libunicode's users, to the coroutines or to core::platform here. That is
# the point of the split: a program that only prints styled text and progress (Lightweight's
# dbtool) links this and nothing else.
#
# The module's root holds only platform-independent code. What goes through the operating system --
# the write, the terminal's size, the XTVERSION probe, the terminal test -- is in posix/ and
# windows/, and the source lists below choose it, so no file there guards itself with an #ifdef of
# its platform. On a platform that is neither (a WebAssembly guest), portable/ supplies the same five
# members with the C++ standard library alone, so the base output needs no operating system. In endo those two files each carried a full copy of the byte composition as well
# (`platform/TerminalOutput.cpp`, `platform/TerminalOutputWin32.cpp`); here it is shared, and what
# is left is only what actually differs.
core_cpp_add_module(tui_output KIND STATIC
    HEADERS
        ClipboardProtocol.hpp
        CursorShape.hpp
        Error.hpp
        MouseTracking.hpp
        SgrBuilder.hpp
        TerminalOutput.hpp
        TerminalProtocols.hpp
    SOURCES
        ClipboardProtocol.cpp
        SgrBuilder.cpp
        TerminalOutput.cpp
        detail/Utf16ToUtf8.hpp
        detail/XtVersion.hpp
    SOURCES_POSIX
        posix/PosixIO.hpp
        posix/TerminalOutput.cpp
    SOURCES_WINDOWS
        windows/TerminalOutput.cpp
    SOURCES_PORTABLE
        portable/TerminalOutput.cpp
    PRIVATE_LIBS core::base)

# portable/ is compiled only where neither POSIX nor Windows is, and no such platform builds the TUI
# yet, so a native tree would never see its body. With tests on it is compiled here as well, as an
# object library nothing links, under the module's toolchain policy: an error in it then fails the
# native build rather than waiting for the first WebAssembly one.
if(CORE_CPP_TESTING AND NOT CORE_CPP_PLATFORM_PORTABLE AND TARGET core-cpp-tui_output)
    add_library(core-cpp-tui_output-portable-check OBJECT portable/TerminalOutput.cpp)
    target_link_libraries(core-cpp-tui_output-portable-check PRIVATE core::base)
    # Compiled as core::tui_output's own code, so its export macro exports: MSVC refuses a definition
    # of a function it reads as imported.
    target_compile_definitions(core-cpp-tui_output-portable-check PRIVATE CORE_TUI_OUTPUT_API_BUILDING)
    core_cpp_apply_toolchain(core-cpp-tui_output-portable-check)
endif()

# Image decoding is the one part with an option of its own: stb is fetched only with
# CORE_CPP_WITH_IMAGES, so the files that decode or scale an image are built only there. What stays
# either way is the ImageProvider interface, which MarkdownRenderer takes by reference; what goes
# is its filesystem implementation and the loader under it.
set(_coreCppTuiImageHeaders "")
set(_coreCppTuiImageSources "")
set(_coreCppTuiImagePosix "")
set(_coreCppTuiImageWindows "")
set(_coreCppTuiImageTests "")
set(_coreCppTuiImageLibs "")
if(CORE_CPP_WITH_IMAGES)
    set(_coreCppTuiImageHeaders FilesystemImageProvider.hpp ImageLoader.hpp)
    set(_coreCppTuiImageSources FilesystemImageProvider.cpp ImageLoader.cpp StbImageImpl.cpp)
    set(_coreCppTuiImagePosix posix/ImageLoader.cpp)
    set(_coreCppTuiImageWindows windows/ImageLoader.cpp)
    set(_coreCppTuiImageTests ImageLoader_test.cpp MarkdownImage_test.cpp)
    # Header-only and needed only to compile StbImageImpl.cpp, so it stays out of the link interface:
    # an installed core::tui would otherwise export a dependency on this build's own stb_image target.
    set(_coreCppTuiImageLibs "$<BUILD_INTERFACE:stb_image>")

    # StbImageImpl.cpp is the one translation unit that compiles stb's implementations. It contains
    # no code of core-cpp's, so core-cpp's warning set says nothing about it and its findings are
    # not core-cpp's to fix: the file is compiled with warnings off rather than annotated with
    # pragmas, which .agent/rules/cpp-guidelines.md does not allow and which would belong in stb's
    # sources anyway. -fno-sanitize=undefined goes with it, because stb_image_resize2's SIMD paths
    # trip UBSan's alignment and integer checks (endo carries the same option at f774a210). Both are
    # PRIVATE, per-source options: neither reaches another file, let alone a consumer.
    #
    # Only for GCC, Clang and clang-cl. cl has nothing to say about stb (endo compiles it at /W4
    # /WX without an exemption), and adding /w there would only earn a D9025 for overriding /W4.
    set(_coreCppStbOptions "")
    if(CORE_CPP_GCC_OR_CLANG)
        list(APPEND _coreCppStbOptions -w -fno-sanitize=undefined)
    endif()
    if(_coreCppStbOptions)
        set_source_files_properties(StbImageImpl.cpp PROPERTIES COMPILE_OPTIONS "${_coreCppStbOptions}")
    endif()
endif()

# core::tui_core: the headless half of endo's terminal UI -- the parser, cells and buffers, the
# components and widgets, the Markdown renderer and the frame driver -- which needs no terminal under
# it (docs/design/tui-headless.md). Screen draws on a ScreenSurface, which core::tui's Terminal
# implements, so nothing here reads a descriptor, installs a signal handler, waits on a clock or
# runs an event loop. Its row in cmake/CoreCppModules.cmake says DEPS base tui_output, and libunicode
# is the one dependency from outside core-cpp. ImageProvider is only the interface MarkdownRenderer
# takes; what reads a file sits in core::tui below. runtime/ModalComponent.hpp is a pure Component
# contract and is here, at its path; the rest of runtime/ is core::tui's.
core_cpp_add_module(tui_core KIND STATIC
    HEADERS
        Box.hpp
        Buffer.hpp
        ByteSinkOutput.hpp
        Canvas.hpp
        Cell.hpp
        CommandPalettePopup.hpp
        CommandRegistry.hpp
        CompletionPopup.hpp
        Component.hpp
        Dialog.hpp
        EditAction.hpp
        Element.hpp
        EndProbe.hpp
        FrameDiff.hpp
        FuzzyPickerPopup.hpp
        GenericSyntaxHighlighter.hpp
        GhostTextHelper.hpp
        HeadlessSurface.hpp
        HoverState.hpp
        HyperlinkEmitter.hpp
        ImageProvider.hpp
        InputEvent.hpp
        InputField.hpp
        KeyBindings.hpp
        KeyCode.hpp
        List.hpp
        LogPanel.hpp
        MarkdownHtml.hpp
        MarkdownInline.hpp
        MarkdownRenderer.hpp
        MarkdownTable.hpp
        MockScreenSurface.hpp
        MockTerminalOutput.hpp
        Modifier.hpp
        PopupKeyDispatch.hpp
        QuestionComponent.hpp
        Rect.hpp
        Responsive.hpp
        Screen.hpp
        ScreenSurface.hpp
        ScrollableSelection.hpp
        Sixel.hpp
        Spinner.hpp
        StatusBar.hpp
        StyledText.hpp
        TestHelpers.hpp
        Text.hpp
        TextDecorator.hpp
        Theme.hpp
        TimerUtils.hpp
        Tooltip.hpp
        TreeTableView.hpp
        Unicode.hpp
        VtParser.hpp
        completer/Completer.hpp
        completer/CompletionItem.hpp
        completer/CompletionProvider.hpp
        completer/FuzzyMatch.hpp
        completer/SmartCaseMatch.hpp
        layout/Solve.hpp
        layout/Style.hpp
        runtime/ModalComponent.hpp
    SOURCES
        Box.cpp
        Buffer.cpp
        ByteSinkOutput.cpp
        Canvas.cpp
        Cell.cpp
        CommandPalettePopup.cpp
        CommandRegistry.cpp
        CompletionPopup.cpp
        Component.cpp
        Dialog.cpp
        FrameDiff.cpp
        FuzzyPickerPopup.cpp
        GenericSyntaxHighlighter.cpp
        HeadlessSurface.cpp
        HoverState.cpp
        HyperlinkEmitter.cpp
        InputField.cpp
        KeyBindings.cpp
        List.cpp
        LogPanel.cpp
        MarkdownHtml.cpp
        MarkdownRenderer.cpp
        MarkdownTable.cpp
        MockScreenSurface.cpp
        MockTerminalOutput.cpp
        QuestionComponent.cpp
        Responsive.cpp
        Screen.cpp
        Sixel.cpp
        Spinner.cpp
        StatusBar.cpp
        StyledText.cpp
        Text.cpp
        Theme.cpp
        Tooltip.cpp
        TreeTableView.cpp
        Unicode.cpp
        VtParser.cpp
        completer/Completer.cpp
        completer/FuzzyMatch.cpp
        completer/SmartCaseMatch.cpp
        layout/Solve.cpp
    PUBLIC_LIBS core::base core::tui_output unicode::unicode)

# core::tui: the terminal half of endo's terminal UI, on top of core::tui_core. Native only -- there
# is no terminal under Emscripten, where CORE_CPP_WITH_TUI is forced off -- and, as above, its root
# holds nothing platform-specific: posix/ has the termios raw mode, the SIGWINCH self-pipe and the
# clipboard tools, windows/ the console modes, the input records and the resize event.
#
# runtime/ is composed on core::net::EventLoop (Task B12), which is why core::net is in PUBLIC_LIBS
# below: TuiRuntime.hpp names its loop, its awaitables and its handle kinds in its own signatures.
# That commit also left this module with NO source that chooses its platform with an #ifdef, and no
# runtime/posix or runtime/windows directory at all -- the multiplexing those two held is the
# loop's, on every platform.
core_cpp_add_module(tui KIND STATIC
    HEADERS
        ${_coreCppTuiImageHeaders}
        ClipboardWriter.hpp
        SemanticBlockClient.hpp
        Terminal.hpp
        TerminalChannel.hpp
        TerminalInput.hpp
        runtime/InputSource.hpp
        runtime/Modal.hpp
        runtime/TerminalInputSource.hpp
        runtime/TuiRuntime.hpp
        runtime/testing/ScriptedInputSource.hpp
        testing/ScriptedTerminalChannel.hpp
    SOURCES
        ${_coreCppTuiImageSources}
        ClipboardWriter.cpp
        SemanticBlockClient.cpp
        Terminal.cpp
        TerminalInput.cpp
        runtime/TuiRuntime.cpp
    SOURCES_POSIX
        ${_coreCppTuiImagePosix}
        posix/Terminal.cpp
        posix/TerminalChannel.cpp
        posix/TerminalChannelPosix.hpp
        posix/TerminalInput.cpp
    SOURCES_WINDOWS
        ${_coreCppTuiImageWindows}
        windows/Terminal.cpp
        windows/TerminalChannel.cpp
        windows/TerminalInput.cpp
    PUBLIC_LIBS core::base core::tui_core core::tui_output core::platform core::async core::net
    PRIVATE_LIBS ${_coreCppTuiImageLibs})

core_cpp_add_test(tui NAME tui_output
    SOURCES
        ClipboardProtocol_test.cpp
        TerminalOutput_test.cpp
        TerminalProtocols_test.cpp
        detail/Utf16ToUtf8_test.cpp)

# The headless half's own binary, which builds with the terminal half switched off
# (CORE_CPP_WITH_TUI off, CORE_CPP_WITH_TUI_CORE on): each case here links core::tui_core and nothing
# above it, which is what proves the split holds for the code the cases exercise.
core_cpp_add_test(tui NAME tui_core
    SOURCES
        Borders_test.cpp
        CommandPalettePopup_test.cpp
        CommandRegistry_test.cpp
        Completer_test.cpp
        CompletionPopup_test.cpp
        Dialog_test.cpp
        FrameDiff_test.cpp
        FuzzyMatch_test.cpp
        FuzzyPickerPopup_test.cpp
        GenericSyntaxHighlighter_test.cpp
        HeadlessSurface_test.cpp
        InputField_test.cpp
        KeyBindings_test.cpp
        MarkdownHtml_test.cpp
        MarkdownRenderer_test.cpp
        MarkdownTable_test.cpp
        MockTerminalOutput_test.cpp
        PopupKeyDispatch_test.cpp
        QuestionComponent_test.cpp
        Renderer_test.cpp
        Responsive_test.cpp
        Screen_test.cpp
        ScrollableSelection_test.cpp
        Sixel_test.cpp
        SmartCaseMatch_test.cpp
        Spinner_test.cpp
        StyledText_test.cpp
        TimerUtils_test.cpp
        TreeTableView_test.cpp
        Unicode_test.cpp
        VtParser_test.cpp
        layout/Solve_test.cpp)

# The terminal half. Screen_test.cpp is not here: it drives Screen over a MockScreenSurface, so it
# runs above, with the terminal half off. windows/TerminalInput_test.cpp is Windows-only: it drives
# the console API directly, and serialises its cases on a named mutex because they share the one
# console the test process has. posix/TerminalHangup_test.cpp and
# posix/TerminalInput_test.cpp are POSIX-only: one hangs up a pseudo-terminal, the other swaps
# standard output for a pipe with dup2.
core_cpp_add_test(tui
    SOURCES
        ${_coreCppTuiImageTests}
        ClipboardWriter_test.cpp
        SemanticBlockClient_test.cpp
        TerminalInput_test.cpp
        TerminalQuery_test.cpp
        runtime/Modal_test.cpp
        runtime/TuiRuntime_test.cpp
    SOURCES_POSIX
        posix/TerminalChannel_test.cpp
        posix/TerminalHangup_test.cpp
        posix/TerminalInput_test.cpp
    SOURCES_WINDOWS
        windows/TerminalInput_test.cpp)

# A null output handed to Terminal is a failed precondition, which ends the process -- so it is
# watched from outside, one process per constructor, judged by the precondition's own text and
# refused if construction returned (TerminalNullOutputCanary.cpp says why not by exit code). Behind
# CORE_CPP_TESTING as a core_cpp_add_test would be, so a consumer's ctest registers none of it.
if(CORE_CPP_TESTING AND TARGET core-cpp-tui)
    add_executable(core-cpp-tui-null-output-canary TerminalNullOutputCanary.cpp)
    target_link_libraries(core-cpp-tui-null-output-canary PRIVATE core::tui core::testing_dialogs)
    core_cpp_apply_toolchain(core-cpp-tui-null-output-canary)
    foreach(mode IN ITEMS output queryInput)
        add_test(NAME core-cpp.tui-null-output-canary.${mode}
                 COMMAND core-cpp-tui-null-output-canary ${mode})
        set_tests_properties(core-cpp.tui-null-output-canary.${mode} PROPERTIES
            PASS_REGULAR_EXPRESSION "Precondition failed\\. output != nullptr"
            FAIL_REGULAR_EXPRESSION "was accepted"
            TIMEOUT 60
            LABELS "core-cpp;tui;canary")
    endforeach()
endif()
