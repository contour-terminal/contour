// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/grid/CellUtil.hpp>
#include <vtbackend/input/vi/HintModeHandler.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/screen/TerminalTestFixtures.hpp>
#include <vtbackend/testing/DetectingTerm.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/CommandCapture.hpp>
#include <vtpty/MockPty.hpp>
#include <vtpty/test/NonAsciiSample.hpp>

#include <core/Escape.hpp>
#include <core/Times.hpp>
#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>
#include <core/async/testing/ManualExecutor.hpp>
#include <core/cli/App.hpp>
#include <core/platform/NativeFileSystem.hpp>
#include <core/platform/PathUtils.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>
#include <core/testing/Environment.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <libunicode/convert.h>
#include <libunicode/width.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <latch>
#include <mutex>
#include <optional>
#include <ranges>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>
using namespace std;
using namespace std::chrono_literals;
using vtbackend::CellFlag;
using vtbackend::CellLocation;
using vtbackend::ColumnCount;
using vtbackend::ColumnOffset;
using vtbackend::LineCount;
using vtbackend::LineOffset;
using vtbackend::MockTerm;
using vtbackend::Modifier;
using vtbackend::PageSize;
using namespace vtbackend::test;

// NOLINTBEGIN(misc-const-correctness)

TEST_CASE("Terminal.BlinkingCursor", "[terminal]")
{
    auto mc = MockTerm { ColumnCount { 6 }, LineCount { 4 } };
    auto& terminal = mc.terminal;
    terminal.setCursorDisplay(vtbackend::CursorDisplay::Blink);
    auto constexpr BlinkInterval = chrono::milliseconds(500);
    terminal.setCursorBlinkingInterval(BlinkInterval);

    auto const clockBase = chrono::steady_clock::time_point();

    SECTION("over time")
    {
        auto const clockBeforeTurn = clockBase + BlinkInterval - chrono::milliseconds(1);
        terminal.tick(clockBeforeTurn);
        terminal.ensureFreshRenderBuffer();
        CHECK(terminal.cursorCurrentlyVisible());

        auto const clockAfterTurn = clockBase + BlinkInterval + chrono::milliseconds(1);
        terminal.tick(clockAfterTurn);
        terminal.ensureFreshRenderBuffer();
        CHECK(!terminal.cursorCurrentlyVisible());
    }

    SECTION("force show on keyboard input")
    {
        // get a state where the blinking cursor is not visible
        auto const clockBeforeTurn = clockBase + BlinkInterval + chrono::milliseconds(1);
        terminal.tick(clockBeforeTurn);
        terminal.ensureFreshRenderBuffer();
        CHECK(!terminal.cursorCurrentlyVisible());

        // type something into the terminal
        auto const clockAtInputEvent = clockBase + BlinkInterval + chrono::milliseconds(10);
        mc.sendCharEvent('x', vtbackend::Modifier {}, clockAtInputEvent);

        // now the cursor is visible before the interval has passed
        terminal.tick(clockBeforeTurn);
        terminal.ensureFreshRenderBuffer();
        CHECK(terminal.cursorCurrentlyVisible());
    }
}

TEST_CASE("Terminal.ModifierKeysDoNotScrollViewport", "[terminal]")
{
    // Set up a terminal with history capacity to allow scrollback
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(6) }, LineCount(10) };
    auto& terminal = mc.terminal;

    // Enable ReportAllKeysAsEscapeCodes (kitty keyboard protocol)
    terminal.changeKeyboardProtocol(
        [](auto& protocol) { protocol.enter(vtbackend::KeyboardEventFlag::ReportAllKeysAsEscapeCodes); });

    // Fill terminal and generate scrollback history
    mc.writeToScreen("line1\r\n"
                     "line2\r\n"
                     "line3\r\n"
                     "line4\r\n"
                     "line5\r\n"
                     "line6\r\n");

    // Scroll up so viewport is not at bottom
    terminal.viewport().scrollUp(LineCount(2));
    REQUIRE(terminal.viewport().scrolled());
    auto const scrollOffsetBeforeKey = terminal.viewport().scrollOffset();

    SECTION("modifier-only press does not scroll")
    {
        mc.resetReplyData();
        terminal.sendKeyEvent(vtbackend::Key::LeftShift,
                              vtbackend::Modifiers { vtbackend::Modifier::Shift },
                              vtbackend::KeyboardEventType::Press,
                              mc.steadyClock.now());

        // Viewport should remain scrolled
        CHECK(terminal.viewport().scrolled());
        CHECK(terminal.viewport().scrollOffset() == scrollOffsetBeforeKey);
        // Escape sequence should still have been sent to the application
        CHECK(!mc.replyData().empty());
    }

    SECTION("non-modifier press does scroll")
    {
        mc.resetReplyData();
        terminal.sendKeyEvent(vtbackend::Key::Enter,
                              vtbackend::Modifiers { vtbackend::Modifier::None },
                              vtbackend::KeyboardEventType::Press,
                              mc.steadyClock.now());

        // Viewport should scroll to bottom
        CHECK(!terminal.viewport().scrolled());
    }

    SECTION("various modifier keys")
    {
        auto const modifierKeys = std::vector<vtbackend::Key> {
            vtbackend::Key::LeftShift,      vtbackend::Key::RightShift,     vtbackend::Key::LeftControl,
            vtbackend::Key::RightControl,   vtbackend::Key::LeftAlt,        vtbackend::Key::RightAlt,
            vtbackend::Key::LeftSuper,      vtbackend::Key::RightSuper,     vtbackend::Key::LeftHyper,
            vtbackend::Key::RightHyper,     vtbackend::Key::LeftMeta,       vtbackend::Key::RightMeta,
            vtbackend::Key::IsoLevel3Shift, vtbackend::Key::IsoLevel5Shift, vtbackend::Key::CapsLock,
            vtbackend::Key::NumLock,
        };

        for (auto const modKey: modifierKeys)
        {
            INFO("Testing modifier key: " << std::format("{}", modKey));

            // Reset viewport to scrolled position
            terminal.viewport().scrollUp(LineCount(2));
            REQUIRE(terminal.viewport().scrolled());

            terminal.sendKeyEvent(modKey,
                                  vtbackend::Modifiers { vtbackend::Modifier::None },
                                  vtbackend::KeyboardEventType::Press,
                                  mc.steadyClock.now());

            CHECK(terminal.viewport().scrolled());
        }
    }
}

TEST_CASE("Terminal.sendMouseLeaveEvent.hoversNothingUntilThePointerIsBack", "[terminal][hyperlink]")
{
    // Off the grid the pointer is on no cell: the link it left is no longer the hovered one -- which is what
    // the frame underlines -- and nothing answers for the pointer's cell. Back on the grid, on the very cell
    // it left from too, it hovers again.
    auto mc = MockTerm { PageSize { LineCount(2), ColumnCount(20) } };
    auto& terminal = mc.terminal;
    mc.writeToScreen("\033]8;;https://example.com/\033\\link\033]8;;\033\\");
    auto const onLink = CellLocation { .line = LineOffset(0), .column = ColumnOffset(1) };
    terminal.sendMouseMoveEvent(Modifier::None, onLink, vtbackend::PixelCoordinate {}, false);
    REQUIRE(terminal.isMouseHoveringHyperlink());

    terminal.sendMouseLeaveEvent();
    CHECK_FALSE(terminal.isMouseHoveringHyperlink());
    CHECK(terminal.tryGetHoveringHyperlink() == nullptr);
    CHECK_FALSE(terminal.currentMouseGridPosition().has_value());

    SECTION("a move back onto the cell it left from")
    {
        terminal.sendMouseMoveEvent(Modifier::None, onLink, vtbackend::PixelCoordinate {}, false);
        CHECK(terminal.isMouseHoveringHyperlink());
        CHECK(terminal.tryGetHoveringHyperlink() != nullptr);
        CHECK(terminal.currentMouseGridPosition() == onLink);
    }

    SECTION("a press there")
    {
        std::ignore = terminal.sendMousePressEvent(
            Modifier::None, vtbackend::MouseButton::Left, onLink, vtbackend::PixelCoordinate {}, false);
        CHECK(terminal.isMouseHoveringHyperlink());
        CHECK(terminal.currentMouseGridPosition() == onLink);
    }
}

TEST_CASE("Terminal.firstPointerEventOntoTheTopLeftCellHoversItsLink", "[terminal][hyperlink]")
{
    // The first pointer event names the pointer's cell, whichever cell that is: the top-left one too, which a
    // fresh terminal's pointer position reads as before any event.
    auto mc = MockTerm { PageSize { LineCount(2), ColumnCount(20) } };
    auto& terminal = mc.terminal;
    mc.writeToScreen("\033]8;;https://example.com/\033\\link\033]8;;\033\\");
    auto const topLeft = CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) };

    SECTION("a move")
    {
        terminal.sendMouseMoveEvent(Modifier::None, topLeft, vtbackend::PixelCoordinate {}, false);
    }

    SECTION("a press")
    {
        std::ignore = terminal.sendMousePressEvent(
            Modifier::None, vtbackend::MouseButton::Left, topLeft, vtbackend::PixelCoordinate {}, false);
    }

    CHECK(terminal.isMouseHoveringHyperlink());
    CHECK(terminal.currentMouseGridPosition() == topLeft);
}

TEST_CASE("Terminal.pathUnderThePointer", "[terminal]")
{
    namespace fs = std::filesystem;

    // The follow gesture finds the path without looking (Terminal::lookUpLinkAt()); the probe thread then
    // looks in a filesystem, an in-memory one here, which the lookup itself never reads.
    auto files = core::platform::testing::InMemoryFileSystem {};
#ifdef _WIN32
    auto const tmpRoot = fs::path { "C:/contour-local-path" };
#else
    auto const tmpRoot = fs::path { "/contour-local-path" };
#endif
    files.addFile(tmpRoot / "nested" / "file.txt", "test");

    auto constexpr PixelCoordinate = vtbackend::PixelCoordinate {};
    auto constexpr UiHandledHint = false;

    // Use forward-slash (generic) path forms throughout: that is how OSC-7 delivers the
    // working directory URL and how the path-detection regex expects paths to look, on every
    // platform (including Windows, where native paths use backslashes the regex would reject).
    auto const tmpRootUrl = "file://" + tmpRoot.generic_string();
    auto const expectedFile = (tmpRoot / "nested" / "file.txt").lexically_normal().string();

    SECTION("relative path")
    {
        auto mc = MockTerm { PageSize { LineCount(2), ColumnCount(80) } };
        auto& terminal = mc.terminal;
        terminal.setCurrentWorkingDirectory(tmpRootUrl);
        mc.writeToScreen("open nested/file.txt now");

        terminal.sendMouseMoveEvent(Modifier::None,
                                    CellLocation { .line = LineOffset(0), .column = ColumnOffset(10) },
                                    PixelCoordinate,
                                    UiHandledHint);

        auto const path = followedPathUnderPointer(terminal, files);
        REQUIRE(path.has_value());
        CHECK(*path == expectedFile);
    }

    SECTION("absolute path")
    {
        auto mc = MockTerm { PageSize { LineCount(2), ColumnCount(240) } };
        auto& terminal = mc.terminal;
        auto const absolutePath = (tmpRoot / "nested" / "file.txt").generic_string();
        mc.writeToScreen("open " + absolutePath);

        terminal.sendMouseMoveEvent(Modifier::None,
                                    CellLocation { .line = LineOffset(0), .column = ColumnOffset(8) },
                                    PixelCoordinate,
                                    UiHandledHint);

        auto const path = followedPathUnderPointer(terminal, files);
        REQUIRE(path.has_value());
        CHECK(*path == expectedFile);
    }

    SECTION("absolute path with tilde in a component")
    {
        // Windows resolves long user names to 8.3 short names containing a '~'
        // (e.g. C:\Users\RUNNER~1\...). The detection regex must keep such a component
        // intact instead of truncating the match at the tilde. A directory literally
        // named "short~1" reproduces the same shape on every platform.
        auto const shortDir = tmpRoot / "short~1";
        files.addFile(shortDir / "file.txt", "test");
        auto const tildePath = (shortDir / "file.txt").generic_string();
        auto const expectedTildeFile = (shortDir / "file.txt").lexically_normal().string();

        auto mc = MockTerm { PageSize { LineCount(2), ColumnCount(240) } };
        auto& terminal = mc.terminal;
        mc.writeToScreen("open " + tildePath);

        terminal.sendMouseMoveEvent(Modifier::None,
                                    CellLocation { .line = LineOffset(0), .column = ColumnOffset(8) },
                                    PixelCoordinate,
                                    UiHandledHint);

        auto const path = followedPathUnderPointer(terminal, files);
        REQUIRE(path.has_value());
        CHECK(*path == expectedTildeFile);
    }

    SECTION("relative path in a working directory beyond ASCII")
    {
        // OSC 7 reports the directory in UTF-8. Read as a narrow path, Windows took it in the ANSI code page
        // and found no file in it.
        auto const directory = tmpRoot / vtpty::testing::NonAsciiName;
        files.addFile(directory / "nested" / "file.txt", "test");

        auto mc = MockTerm { PageSize { LineCount(2), ColumnCount(80) } };
        auto& terminal = mc.terminal;
        terminal.setCurrentWorkingDirectory(fileUrlOf(directory));
        mc.writeToScreen("open nested/file.txt now");

        terminal.sendMouseMoveEvent(Modifier::None,
                                    CellLocation { .line = LineOffset(0), .column = ColumnOffset(10) },
                                    PixelCoordinate,
                                    UiHandledHint);

        auto const path = followedPathUnderPointer(terminal, files).value_or(std::nullopt);
        REQUIRE(path.has_value());
        CHECK(core::pathFromUtf8(*path) == (directory / "nested" / "file.txt").lexically_normal());
    }

    SECTION("missing path")
    {
        auto mc = MockTerm { PageSize { LineCount(2), ColumnCount(80) } };
        auto& terminal = mc.terminal;
        terminal.setCurrentWorkingDirectory(tmpRootUrl);
        mc.writeToScreen("open nested/missing.txt now");

        terminal.sendMouseMoveEvent(Modifier::None,
                                    CellLocation { .line = LineOffset(0), .column = ColumnOffset(10) },
                                    PixelCoordinate,
                                    UiHandledHint);

        CHECK(followedPathUnderPointer(terminal, files) == std::nullopt);
    }

    SECTION("a working directory on another host resolves no relative path here")
    {
        // file://otherhost/dir names otherhost's directory: one of that name here is another directory.
        auto const generic = tmpRoot.generic_string();
        auto mc = MockTerm { PageSize { LineCount(2), ColumnCount(80) } };
        auto& terminal = mc.terminal;
        terminal.setCurrentWorkingDirectory("file://otherhost.invalid"
                                            + (generic.starts_with('/') ? "" : "/"s) + generic);
        mc.writeToScreen("open nested/file.txt now");

        terminal.sendMouseMoveEvent(Modifier::None,
                                    CellLocation { .line = LineOffset(0), .column = ColumnOffset(10) },
                                    PixelCoordinate,
                                    UiHandledHint);

        CHECK(printedPathUnderPointer(terminal) == "nested/file.txt");
        CHECK(followedPathUnderPointer(terminal, files) == std::nullopt);
    }

    SECTION("a replica resolves a path here only when its daemon runs on this machine")
    {
        // On another machine, the path names a file there: one that exists here too is another file.
        auto const role = GENERATE(vtbackend::ReplicaRole::OtherMachine, vtbackend::ReplicaRole::SameMachine);
        auto mc = MockTerm { PageSize { LineCount(2), ColumnCount(80) } };
        auto& terminal = mc.terminal;
        terminal.bindReplica(role, vtbackend::HostIdentity {});
        terminal.setCurrentWorkingDirectory(tmpRootUrl);
        mc.writeToScreen("open nested/file.txt now");

        terminal.sendMouseMoveEvent(Modifier::None,
                                    CellLocation { .line = LineOffset(0), .column = ColumnOffset(10) },
                                    PixelCoordinate,
                                    UiHandledHint);

        CHECK(printedPathUnderPointer(terminal) == "nested/file.txt");
        if (role == vtbackend::ReplicaRole::OtherMachine)
            CHECK(followedPathUnderPointer(terminal, files)
                  == std::unexpected(vtbackend::PathRefusal::ReplicaOfAnotherMachine));
        else
            CHECK(followedPathUnderPointer(terminal, files) == expectedFile);
    }
}

TEST_CASE("Terminal.AutoScrollOnUpdate", "[terminal]")
{
    // Set up a terminal with history capacity to allow scrollback.
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(6) }, LineCount(10) };
    auto& terminal = mc.terminal;

    // Fill terminal and generate scrollback history.
    mc.writeToScreen("line1\r\n"
                     "line2\r\n"
                     "line3\r\n"
                     "line4\r\n"
                     "line5\r\n"
                     "line6\r\n");

    auto const anyModifiers = vtbackend::Modifiers { vtbackend::Modifier::None };

    SECTION("keypress always scrolls to bottom, whatever autoScrollOnUpdate says")
    {
        // User input must reveal the cursor regardless of the output-scroll setting: sending a key
        // snaps the viewport back to the bottom even where autoScrollOnUpdate (which governs
        // output-driven scrolling only) is disabled.
        for (auto const autoScrollOnUpdate: { false, true })
        {
            INFO("autoScrollOnUpdate " << autoScrollOnUpdate);
            terminal.settings().autoScrollOnUpdate = autoScrollOnUpdate;
            terminal.viewport().scrollUp(LineCount(2));
            REQUIRE(terminal.viewport().scrolled());

            terminal.sendKeyEvent(vtbackend::Key::Enter,
                                  anyModifiers,
                                  vtbackend::KeyboardEventType::Press,
                                  mc.steadyClock.now());

            CHECK(!terminal.viewport().scrolled());
        }
    }

    SECTION("char input always scrolls to bottom, whatever autoScrollOnUpdate says")
    {
        // Same input-independence as the keypress case above, exercised through the char path.
        for (auto const autoScrollOnUpdate: { false, true })
        {
            INFO("autoScrollOnUpdate " << autoScrollOnUpdate);
            terminal.settings().autoScrollOnUpdate = autoScrollOnUpdate;
            terminal.viewport().scrollUp(LineCount(2));
            REQUIRE(terminal.viewport().scrolled());

            terminal.sendCharEvent(U'a',
                                   vtbackend::KeyIdentity { .unshiftedKey = U'a' },
                                   anyModifiers,
                                   vtbackend::KeyboardEventType::Press,
                                   mc.steadyClock.now());

            CHECK(!terminal.viewport().scrolled());
        }
    }

    // A key/char *release* is never typed content. When the active keyboard protocol reports
    // releases to the application (win32-input-mode here, or the Kitty keyboard protocol) the release
    // still produces PTY input, but it must NOT snap the viewport back to the bottom -- otherwise
    // releasing a viewport-scroll shortcut such as Shift+Up (whose press the GUI already consumed as
    // a ScrollOneUp action) would immediately undo the scroll. See Terminal::scrollToBottomOnInput().
    SECTION("key release does not scroll to bottom even when it generates input")
    {
        // Win32 input mode reports both presses and releases to the application, so the release below
        // actually reaches the input generator (in legacy mode releases generate nothing at all).
        terminal.setMode(vtbackend::DECMode::Win32InputMode, true);

        // Sanity check: a *press* in this mode really does generate input and snap to the bottom, so
        // the release assertion below is not vacuous.
        terminal.viewport().scrollUp(LineCount(2));
        REQUIRE(terminal.viewport().scrolled());
        terminal.sendKeyEvent(
            vtbackend::Key::UpArrow, anyModifiers, vtbackend::KeyboardEventType::Press, mc.steadyClock.now());
        REQUIRE(!terminal.viewport().scrolled());

        // The release generates input too, but must leave the viewport parked where the user put it.
        terminal.viewport().scrollUp(LineCount(2));
        REQUIRE(terminal.viewport().scrolled());
        auto const offsetBefore = terminal.viewport().scrollOffset();

        terminal.sendKeyEvent(vtbackend::Key::UpArrow,
                              anyModifiers,
                              vtbackend::KeyboardEventType::Release,
                              mc.steadyClock.now());

        CHECK(terminal.viewport().scrolled());
        CHECK(terminal.viewport().scrollOffset() == offsetBefore);
    }

    SECTION("char release does not scroll to bottom even when it generates input")
    {
        terminal.setMode(vtbackend::DECMode::Win32InputMode, true);

        terminal.viewport().scrollUp(LineCount(2));
        REQUIRE(terminal.viewport().scrolled());
        auto const offsetBefore = terminal.viewport().scrollOffset();

        terminal.sendCharEvent(U'a',
                               vtbackend::KeyIdentity { .unshiftedKey = U'a' },
                               anyModifiers,
                               vtbackend::KeyboardEventType::Release,
                               mc.steadyClock.now());

        CHECK(terminal.viewport().scrolled());
        CHECK(terminal.viewport().scrollOffset() == offsetBefore);
    }

    SECTION("CSI 3 J leaves no offset into the rows it dropped, even with autoScrollOnUpdate=false")
    {
        // Not scrolling on output does not keep the viewport on history that is gone: the offset is held
        // to what is left of it, here nothing.
        terminal.settings().autoScrollOnUpdate = false;
        terminal.viewport().scrollUp(LineCount(2));
        REQUIRE(terminal.viewport().scrolled());

        mc.writeToScreen("\x1b[3J");

        CHECK(terminal.viewport().scrollOffset() == vtbackend::ScrollOffset(0));
    }

    // Note: we exercise `bufferChanged` directly rather than feeding DECSET 1049,
    // because the full alt-screen entry sequence also clears the screen which in
    // turn triggers `onBufferScrolled`, clamping the viewport to the (empty) alt
    // screen history independently of our flag.
    SECTION("bufferChanged honors autoScrollOnUpdate=false")
    {
        terminal.settings().autoScrollOnUpdate = false;
        terminal.viewport().scrollUp(LineCount(2));
        REQUIRE(terminal.viewport().scrolled());
        auto const offsetBefore = terminal.viewport().scrollOffset();

        terminal.bufferChanged(vtbackend::ScreenType::Primary);

        CHECK(terminal.viewport().scrollOffset() == offsetBefore);
    }

    SECTION("bufferChanged honors autoScrollOnUpdate=true")
    {
        terminal.settings().autoScrollOnUpdate = true;
        terminal.viewport().scrollUp(LineCount(2));
        REQUIRE(terminal.viewport().scrolled());

        terminal.bufferChanged(vtbackend::ScreenType::Primary);

        CHECK(!terminal.viewport().scrolled());
    }
}

TEST_CASE("Terminal.DECCARA", "[terminal]")
{
    auto mock = MockTerm { ColumnCount(5), LineCount(5) };
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    mock.terminal.tick(ClockBase);
    mock.terminal.ensureFreshRenderBuffer();
    CHECK(trimmedTextScreenshot(mock).empty());

    mock.writeToScreen("12345\r\n"
                       "67890\r\n"
                       "ABCDE\r\n"
                       "abcde\r\n"
                       "fghij");

    mock.terminal.tick(ClockBase + chrono::seconds(1));
    mock.terminal.ensureFreshRenderBuffer();
    CHECK("12345\n67890\nABCDE\nabcde\nfghij" == trimmedTextScreenshot(mock));

    auto const top = 2;
    auto const left = 3;
    auto const bottom = 4;
    auto const right = 5;
    mock.writeToScreen(
        std::format("\033[{};{};{};{};{}$r", top, left, bottom, right, "1;38:2::171:178:191;4"));

    mock.terminal.tick(ClockBase + chrono::seconds(2));
    mock.terminal.ensureFreshRenderBuffer();
    CHECK("12345\n67890\nABCDE\nabcde\nfghij" == trimmedTextScreenshot(mock));

    // Just peak into it and test. That's not really 100% precise, tbh., but
    // i'd like to keep on going and have fun doing the project and not die
    // early due to a overdose of TDD. :-)
    for (auto line = top; line <= bottom; ++line)
        for (auto column = left; column <= right; ++column)
        {
            // clang-format off
            auto const& someCell = mock.terminal.primaryScreen().at(LineOffset(line - 1), ColumnOffset(column - 1));
            auto const rgb = someCell.foregroundColor().rgb();
            auto const colorDec = std::format("{}/{}/{}", unsigned(rgb.red), unsigned(rgb.green), unsigned(rgb.blue));
            INFO(std::format("at line {} column {}, flags {}", line, column, someCell.flags()));
            CHECK(colorDec == "171/178/191");
            CHECK(someCell.isFlagEnabled(vtbackend::CellFlag::Bold));
            CHECK(someCell.isFlagEnabled(vtbackend::CellFlag::Underline));
            // clang-format on
        }
}

TEST_CASE("Terminal.CaptureScreenBuffer")
{
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    auto constexpr NoLogicalLines = 0; // 0=false
    auto constexpr NumberOfLinesToCapture = 7;
    auto constexpr MaxHistoryLineCount = LineCount(20);

    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) }, MaxHistoryLineCount };

    logScreenText(mock.terminal, "init");

    mock.terminal.tick(ClockBase);
    mock.terminal.ensureFreshRenderBuffer();

    // fill screen buffer (5 lines into history + full 5 lines page buffer)
    for (int i = 1; i <= 10; ++i)
    {
        mock.writeToScreen(std::format("\r\n{}", i));
        logScreenText(mock.terminal, std::format("write i {}", i));
    }

    mock.terminal.tick(ClockBase + chrono::seconds(1));
    mock.terminal.ensureFreshRenderBuffer();
    auto const actualScreen1 = trimmedTextScreenshot(mock);
    REQUIRE("6\n7\n8\n9\n10" == actualScreen1);
    logScreenText(mock.terminal, "fini");

    // XTCAPTURE now carries a ',' intermediate (`CSI > Ps ; Ps , t`) so the bare `CSI > Ps t` can be
    // the standard xterm XTSMTITLE. @see Functions.h XTCAPTURE / XTSMTITLE.
    mock.writeToScreen(std::format("\033[>{};{},t", NoLogicalLines, NumberOfLinesToCapture));
    mock.terminal.flushInput();
    logScreenText(mock.terminal, "after flush");

    mock.terminal.tick(ClockBase + chrono::seconds(1));
    mock.terminal.ensureFreshRenderBuffer();
    auto const actualScreen2 = trimmedTextScreenshot(mock);
    CHECK(actualScreen1 == actualScreen2);

    CHECK(e(mock.replyData()) == e("\033^314;4\n5\n6\n7\n8\n9\n10\n\033\\\033^314;\033\\"));

    // I just realized we have a test as Screen.captureBuffer already.
    // But here we test the full cycle.
}

TEST_CASE("Terminal.RIS", "[terminal]")
{
    using namespace vtbackend;

    constexpr auto RIS = "\033c"sv;

    auto mc = MockTerm { ColumnCount(20), LineCount(5) };
    mc.terminal.ensureFreshRenderBuffer();

    mc.terminal.tick(mc.terminal.currentTime() + chrono::milliseconds(500));
    mc.terminal.ensureFreshRenderBuffer();

    mc.terminal.setStatusDisplay(StatusDisplayType::Indicator);
    mc.terminal.tick(mc.terminal.currentTime() + chrono::milliseconds(500));
    mc.terminal.ensureFreshRenderBuffer();
    mc.writeToScreen(RIS);
    mc.terminal.forceRedraw({});

    CHECK(mc.terminal.statusDisplayType() == StatusDisplayType::None);
}

TEST_CASE("Terminal.RIS.keepsFrozenModesAppliedToInputGenerator", "[terminal]")
{
    using namespace vtbackend;

    // DECCKM, DECNKM and DECBKM are mirrored inside the input generator rather than read back from
    // the mode register, so hardReset() has to clear that mirror BEFORE it replays `frozenModes` --
    // not after. Clearing it afterwards silently undoes the replay, and because setMode() early-
    // returns on a frozen mode, nothing can ever resync the two again: the arrow keys would emit
    // `ESC [ A` for the rest of the session while DECRQM kept reporting DECCKM as set.
    auto mc = MockTerm { ColumnCount(20), LineCount(5) };
    mc.terminal.settings().frozenModes[DECMode::UseApplicationCursorKeys] = true;

    SECTION("a frozen DECCKM survives RIS")
    {
        mc.writeToScreen("\033[?1h"sv); // DECSET DECCKM -- refused, the mode is frozen on
        mc.terminal.freezeMode(DECMode::UseApplicationCursorKeys, true);
        REQUIRE(mc.terminal.applicationCursorKeys());

        mc.writeToScreen("\033c"sv); // RIS

        CHECK(mc.terminal.isModeEnabled(DECMode::UseApplicationCursorKeys));
        CHECK(mc.terminal.applicationCursorKeys()); // the mirror must agree with the mode register
    }

    SECTION("an unfrozen DECCKM is still reset by RIS")
    {
        mc.terminal.settings().frozenModes.clear();
        mc.writeToScreen("\033[?1h"sv); // DECSET DECCKM
        REQUIRE(mc.terminal.applicationCursorKeys());

        mc.writeToScreen("\033c"sv); // RIS

        CHECK(!mc.terminal.isModeEnabled(DECMode::UseApplicationCursorKeys));
        CHECK(!mc.terminal.applicationCursorKeys());
    }
}

TEST_CASE("Terminal.RIS.resetsPassiveMouseTracking", "[terminal]")
{
    using namespace vtbackend;

    // DECMode 2029 is mirrored in the input generator as well. It was the one mirror hardReset() did
    // not clear, so after RIS the mode register read "off" while the generator still appended the
    // passive-tracking parameter to every SGR mouse report -- a fourth field the application cannot
    // parse -- and kept the alternate-scroll path gated shut.
    auto mc = MockTerm { ColumnCount(20), LineCount(5) };

    mc.writeToScreen("\033[?2029h"sv);
    REQUIRE(mc.terminal.isModeEnabled(DECMode::MousePassiveTracking));

    mc.writeToScreen("\033c"sv); // RIS

    CHECK(!mc.terminal.isModeEnabled(DECMode::MousePassiveTracking));

    // Re-enabling must take effect, which it cannot if the mirror never went back to false.
    mc.writeToScreen("\033[?2029h"sv);
    CHECK(mc.terminal.isModeEnabled(DECMode::MousePassiveTracking));
}

TEST_CASE("Terminal.forceRedraw keeps the cell size", "[terminal]")
{
    using namespace vtbackend;

    // forceRedraw() briefly resizes to one extra column to make applications redraw, then back.
    // resizeScreen() derives the cell size as pixels/page, so each call must carry the pixel size OF
    // THE PAGE IT NAMES: handing the real page's pixels to the one-column-wider page derived a cell
    // width of cellW*columns/(columns+1) and pushed that to the child on the first SIGWINCH. A
    // program that reads TIOCGWINSZ once at startup -- img2sixel and chafa both do -- sizes its image
    // canvas from that, and the restoring resize issues no third SIGWINCH to correct it.
    //
    // The artificialSleep hook runs between the two resizes, which is the only place the transient
    // state is observable at all.
    auto constexpr CellSize = ImageSize { Width(9), Height(18) };

    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    mc.terminal.setCellPixelSize(CellSize);
    REQUIRE(mc.terminal.cellPixelSize() == CellSize);

    auto cellSizeWhileWidened = ImageSize {};
    mc.terminal.forceRedraw([&]() { cellSizeWhileWidened = mc.terminal.cellPixelSize(); });

    CHECK(cellSizeWhileWidened == CellSize);
    CHECK(mc.terminal.cellPixelSize() == CellSize);
}

TEST_CASE("Terminal.clampedTotalPageSize", "[terminal]")
{
    using namespace vtbackend;

    // clampedTotalPageSize() is the single authority for the resize lower-bound: the total page must leave
    // at least one main-display line ON TOP of the visible status line(s). Frontend callers (helper.cpp's
    // resize early-out, TerminalSessionManager's renderer-geometry sync) query it so their bookkeeping
    // matches what resizeScreen() actually applies. This pins that contract for both status-line states.
    auto mc = MockTerm { ColumnCount(20), LineCount(5) };

    SECTION("no status line: clamps only to a 1x1 floor")
    {
        mc.terminal.setStatusDisplay(StatusDisplayType::None);
        REQUIRE(mc.terminal.statusLineHeight() == LineCount(0));

        // A sub-1 request is raised to the 1x1 minimum; a comfortably-sized request passes through.
        CHECK(mc.terminal.clampedTotalPageSize(PageSize { LineCount(0), ColumnCount(0) })
              == PageSize { LineCount(1), ColumnCount(1) });
        CHECK(mc.terminal.clampedTotalPageSize(PageSize { LineCount(10), ColumnCount(40) })
              == PageSize { LineCount(10), ColumnCount(40) });
    }

    SECTION("indicator status line: floor rises to statusLineHeight()+1")
    {
        mc.terminal.setStatusDisplay(StatusDisplayType::Indicator);
        REQUIRE(mc.terminal.statusLineHeight() == LineCount(1));

        // The GUI-default case behind the resize-early-out finding: a 1-line request clamps up to 2, so a
        // caller comparing its own LineCount(1)-clamped value against the applied size must query this to
        // agree (otherwise 1 != 2 defeats the early-out forever below two cell-rows).
        CHECK(mc.terminal.clampedTotalPageSize(PageSize { LineCount(1), ColumnCount(20) })
              == PageSize { LineCount(2), ColumnCount(20) });
        // A total already above the floor is unchanged.
        CHECK(mc.terminal.clampedTotalPageSize(PageSize { LineCount(5), ColumnCount(20) })
              == PageSize { LineCount(5), ColumnCount(20) });
    }

    SECTION("the clamp matches what resizeScreen() actually applies")
    {
        mc.terminal.setStatusDisplay(StatusDisplayType::Indicator);
        auto const requested = PageSize { LineCount(1), ColumnCount(20) };
        {
            auto const _ = std::scoped_lock { mc.terminal };
            mc.terminal.resizeScreen(requested, std::nullopt);
        }
        // resizeScreen() clamped the total internally; clampedTotalPageSize() predicts that exact result.
        CHECK(mc.terminal.totalPageSize() == mc.terminal.clampedTotalPageSize(requested));
    }
}

TEST_CASE("Terminal.DECCOLM.doesNotDoubleCountStatusLine", "[terminal]")
{
    using namespace vtbackend;

    // DECCOLM changes only the column count. The line count it hands the frontend must be the USABLE
    // main-page count: the frontend adds the status-line height back itself (exactly as it does for
    // XTWINOPS `CSI 8 t`). Passing the *total* double-counts the status line, so the window grows one
    // row on every DECCOLM whenever the indicator status line is shown (29 -> 30 -> 31). Regression for
    // that, driven through the same requestWindowResize()/resizeScreen() path as the real frontend.
    auto mc = MockTerm { PageSize { LineCount(24), ColumnCount(80) } };
    mc.terminal.setStatusDisplay(StatusDisplayType::Indicator);
    REQUIRE(mc.terminal.statusLineHeight() == LineCount(1));

    auto const usableBefore = mc.terminal.pageSize().lines; // main page only; excludes the status line

    // Allow 80<->132, then switch to 132 columns.
    mc.writeToScreen("\033[?40h\033[?3h");

    REQUIRE(mc.requestedPageSize.has_value());
    CHECK(mc.requestedPageSize->columns == ColumnCount(132));
    // The request names the USABLE line count, unchanged — NOT the total (usable + status line).
    CHECK(mc.requestedPageSize->lines == usableBefore);

    // The main page did not creep: only the columns changed.
    CHECK(mc.terminal.pageSize().lines == usableBefore);
    CHECK(mc.terminal.pageSize().columns == ColumnCount(132));

    // A second DECCOLM (back to 80 columns) still does not creep the line count.
    mc.writeToScreen("\033[?3l");
    CHECK(mc.terminal.pageSize().lines == usableBefore);
    CHECK(mc.terminal.pageSize().columns == ColumnCount(80));
}

TEST_CASE("Terminal.DECSCPP.doesNotDoubleCountStatusLine", "[terminal]")
{
    using namespace vtbackend;

    // DECSCPP (`CSI Ps $ |`) selects 80/132 columns per page and shares DECCOLM's frontend contract: the
    // USABLE line count is passed and the frontend adds the status-line height back. Passing the total
    // double-counts it. Same regression as DECCOLM, different sequence.
    auto mc = MockTerm { PageSize { LineCount(24), ColumnCount(80) } };
    mc.terminal.setStatusDisplay(StatusDisplayType::Indicator);
    REQUIRE(mc.terminal.statusLineHeight() == LineCount(1));

    auto const usableBefore = mc.terminal.pageSize().lines;

    mc.writeToScreen("\033[132$|");
    REQUIRE(mc.requestedPageSize.has_value());
    CHECK(mc.requestedPageSize->columns == ColumnCount(132));
    CHECK(mc.requestedPageSize->lines == usableBefore);
    CHECK(mc.terminal.pageSize().lines == usableBefore);
    CHECK(mc.terminal.pageSize().columns == ColumnCount(132));
}

TEST_CASE("Terminal.DECCOLM.resizesGridSynchronously", "[terminal]")
{
    using namespace vtbackend;

    // DECCOLM must resize the grid authoritatively and synchronously with sequence processing, so that
    // an application which switches to 132 columns and immediately draws to the new width — vttest's
    // cursor-movement test addresses its box border at absolute column 132 right after the switch —
    // lands its cells on the new width, even when the frontend cannot resize the window yet. Model that
    // frontend with refuseWindowResize: the grid must be 132 wide right after DECCOLM regardless.
    auto mc = MockTerm { PageSize { LineCount(24), ColumnCount(80) } };
    mc.refuseWindowResize = true;

    mc.writeToScreen("\033[?40h\033[?3h"); // allow 80<->132, then DECCOLM 132
    // Authoritative: the grid is 132 wide now, not one GUI round-trip later.
    REQUIRE(mc.terminal.pageSize().columns == ColumnCount(132));

    // Absolute addressing to the far right lands on the new width: a marker at column 132 is at
    // column-offset 131. On the old 80-column grid it would have clamped to column 80.
    mc.writeToScreen("\033[1;132HX");
    CHECK(mc.terminal.currentScreen().at(LineOffset(0), ColumnOffset(131)).toUtf8() == "X");

    // And back to 80 columns, synchronously.
    mc.writeToScreen("\033[?3l");
    CHECK(mc.terminal.pageSize().columns == ColumnCount(80));
}

TEST_CASE("Terminal.DECCOLM.homesTheCursor", "[terminal]")
{
    using namespace vtbackend;

    // DEC STD 070: selecting either column mode sets the Active Position to the display origin (1,1).
    auto mc = MockTerm { PageSize { LineCount(24), ColumnCount(80) } };
    mc.writeToScreen("\033[?40h"); // allow the 80<->132 switch
    auto const& screen = mc.terminal.primaryScreen();

    SECTION("switching to 132 columns")
    {
        mc.writeToScreen("\033[5;10H\033[?3h");
        CHECK(screen.cursor().position == CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) });
    }

    SECTION("switching back to 80 from past column 80 wraps no row")
    {
        // vttest's "Bug C: wide to narrow screen": the cursor at column 81 of a 132-column page, then 80
        // columns. Nothing on the page may be left continuing onto a row below.
        mc.writeToScreen("\033[?3h\033[1;81H\033[?3l");
        CHECK(screen.cursor().position == CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) });
        for (auto const line: std::views::iota(0, 24))
            CHECK_FALSE(screen.grid().lineAt(LineOffset(line)).wrapped());
    }

    SECTION("with DECNCSM keeping the page")
    {
        mc.writeToScreen("\033[?95h\033[5;10H\033[?3h");
        CHECK(screen.cursor().position == CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) });
    }
}

TEST_CASE("Terminal.RIS.resetsDECCOLM", "[terminal]")
{
    using namespace vtbackend;

    // esctest RISTests.test_RIS_ResetDECCOLM: RIS returns a DECCOLM 132-column switch to 80 columns,
    // provided 80/132 switching was allowed and the terminal is currently in 132 columns. The check must
    // precede the mode reset (older xterm reset the mode first and could no longer tell).
    SECTION("RIS returns a 132-column DECCOLM switch to 80")
    {
        auto mc = MockTerm { PageSize { LineCount(24), ColumnCount(80) } };
        mc.writeToScreen("\033[?40h\033[?3h"); // allow 80<->132, then DECCOLM 132
        REQUIRE(mc.terminal.pageSize().columns == ColumnCount(132));
        mc.writeToScreen("\033c"); // RIS
        CHECK(mc.terminal.pageSize().columns == ColumnCount(80));
    }

    SECTION("RIS leaves an 80-column terminal at 80 (no spurious resize)")
    {
        auto mc = MockTerm { PageSize { LineCount(24), ColumnCount(80) } };
        mc.writeToScreen("\033c"); // RIS with no DECCOLM in effect
        CHECK(mc.terminal.pageSize().columns == ColumnCount(80));
    }
}

TEST_CASE("Terminal.DECNCSM", "[terminal]")
{
    using namespace vtbackend;

    // DECNCSM (DEC private mode 95): when set, DECCOLM (80<->132) preserves page memory; when reset
    // (the default), a column-width change clears the screen (VT100 behaviour).
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(80) } };
    mc.writeToScreen("\033[?40h"); // allow the 80<->132 switch

    SECTION("default (reset): DECCOLM clears the screen")
    {
        mc.writeToScreen("HELLO");
        mc.writeToScreen("\033[?3h"); // DECCOLM -> 132
        CHECK(!mc.terminal.primaryScreen().grid().lineText(LineOffset(0)).contains('H'));
    }

    SECTION("DECNCSM set: DECCOLM preserves the screen")
    {
        mc.writeToScreen("\033[?95h"); // DECNCSM
        mc.writeToScreen("HELLO");
        mc.writeToScreen("\033[?3h"); // DECCOLM -> 132
        CHECK(mc.terminal.primaryScreen().grid().lineText(LineOffset(0)).substr(0, 5) == "HELLO");
    }
}

TEST_CASE("Terminal.SynchronizedOutputTimesOut", "[terminal]")
{
    // An application that opens a synchronized block and never closes it must not be able to freeze
    // the display. Rendering is suppressed while the block is young and resumes once it has outlived
    // Settings::synchronizedOutputTimeout, even though the mode itself is still set -- DECRQM must
    // still report it enabled, because that is what the application asked for.
    constexpr auto BatchOn = "\033[?2026h"sv;

    auto mc = MockTerm { ColumnCount(20), LineCount(1) };
    mc.steadyClock.advance(1s); // a second into the session: past the render throttle's interval
    auto const now = mc.steadyClock.now();

    mc.writeToScreen(BatchOn);
    mc.writeToScreen("Hello");
    mc.terminal.tick(now);
    mc.terminal.ensureFreshRenderBuffer();
    CHECK(trimmedTextScreenshot(mc).empty());
    CHECK(mc.terminal.isModeEnabled(vtbackend::DECMode::BatchedRendering));
    CHECK(mc.terminal.isRenderingSuppressed());

    // A zero timeout expires the block that is already open, without the application closing it.
    mc.terminal.settings().synchronizedOutputTimeout = chrono::milliseconds { 0 };

    CHECK(mc.terminal.isModeEnabled(vtbackend::DECMode::BatchedRendering));
    CHECK_FALSE(mc.terminal.isRenderingSuppressed());

    mc.terminal.tick(now);
    mc.terminal.ensureFreshRenderBuffer();
    CHECK(trimmedTextScreenshot(mc) == "Hello");
}

TEST_CASE("Terminal.SynchronizedOutputTimesOutByTheTerminalsSteadyClock", "[terminal]")
{
    // The deadline is kept on the steady clock the terminal was given, like every other time it keeps, so
    // whoever drives that clock moves the deadline -- a test need not wait it out in real time.
    auto mc = MockTerm { ColumnCount(20), LineCount(1) };

    mc.writeToScreen("\033[?2026h");
    REQUIRE(mc.terminal.isRenderingSuppressed());

    mc.steadyClock.advance(mc.terminal.settings().synchronizedOutputTimeout);
    CHECK_FALSE(mc.terminal.isRenderingSuppressed());
}

TEST_CASE("Terminal.SynchronizedOutput", "[terminal]")
{
    constexpr auto BatchOn = "\033[?2026h"sv;
    constexpr auto BatchOff = "\033[?2026l"sv;

    auto mc = MockTerm { ColumnCount(20), LineCount(1) };
    mc.steadyClock.advance(1s); // a second into the session: past the render throttle's interval
    auto const now = mc.steadyClock.now();

    mc.writeToScreen(BatchOn);
    mc.writeToScreen("Hello ");
    mc.terminal.tick(now);
    mc.terminal.ensureFreshRenderBuffer();
    CHECK(trimmedTextScreenshot(mc).empty());

    mc.writeToScreen(" World");
    mc.terminal.tick(now);
    mc.terminal.ensureFreshRenderBuffer();
    CHECK(trimmedTextScreenshot(mc).empty());

    mc.writeToScreen(BatchOff);
    mc.terminal.tick(now);
    mc.terminal.ensureFreshRenderBuffer();
    CHECK("Hello  World" == trimmedTextScreenshot(mc));
}

TEST_CASE("Terminal.XTPUSHCOLORS_and_XTPOPCOLORS", "[terminal]")
{
    using namespace vtbackend;

    auto mc = MockTerm { ColumnCount(20), LineCount(1) };
    auto& vt = mc.terminal;

    auto const originalPalette = vt.colorPalette();

    auto modifiedPalette = ColorPalette {};
    modifiedPalette.palette[0] = 0xFF6600_rgb;

    SECTION("pop on empty")
    {
        mc.writeToScreen("\033[#Q");
        REQUIRE(vt.savedColorPalettes().empty());
        REQUIRE(vt.colorPalette().palette == originalPalette.palette);
    }

    SECTION("default")
    {
        mc.writeToScreen("\033[#P"); // XTPUSHCOLORS (default)
        REQUIRE(vt.savedColorPalettes().size() == 1);
        REQUIRE(vt.savedColorPalettes().back().palette == originalPalette.palette);
        vt.colorPalette().palette[0] = 0x123456_rgb;
        REQUIRE(vt.colorPalette().palette != originalPalette.palette);
        mc.writeToScreen("\033[#Q"); // XTPOPCOLORS
        REQUIRE(vt.colorPalette().palette == originalPalette.palette);
    }

    SECTION("0")
    {
        mc.writeToScreen("\033[0#P"); // push current color palette to slot 1 (default).
        REQUIRE(vt.savedColorPalettes().size() == 1);
    }

    SECTION("1")
    {
        REQUIRE(vt.savedColorPalettes().empty());
        mc.writeToScreen("\033[1#P"); // push current color palette to slot 1.
        REQUIRE(vt.savedColorPalettes().size() == 1);
    }

    SECTION("2")
    {
        REQUIRE(vt.savedColorPalettes().empty());
        mc.writeToScreen("\033[2#P"); // push current color palette to slot 1.
        REQUIRE(vt.savedColorPalettes().size() == 2);
        mc.writeToScreen("\033[#R");
        mc.terminal.flushInput();
        REQUIRE(e("\033[2;2#Q") == e(mc.replyData()));
    }

    SECTION("10")
    {
        REQUIRE(vt.savedColorPalettes().empty());
        mc.writeToScreen("\033[10#P"); // push current color palette to slot 10.
        REQUIRE(vt.savedColorPalettes().size() == 10);
        mc.writeToScreen("\033[#R");
        mc.terminal.flushInput();
        REQUIRE(e("\033[10;10#Q") == e(mc.replyData()));
    }

    SECTION("11")
    {
        REQUIRE(vt.savedColorPalettes().empty());
        mc.writeToScreen("\033[11#P"); // push current color palette to slot 11: overflow.
        REQUIRE(vt.savedColorPalettes().empty());
    }

    SECTION("push and direct copy")
    {
        vt.colorPalette().palette[1] = 0x101010_rgb;
        auto const p1 = vt.colorPalette();
        mc.writeToScreen("\033[#P");

        vt.colorPalette().palette[3] = 0x303030_rgb;
        auto const p3 = vt.colorPalette();
        mc.writeToScreen("\033[3#P");

        vt.colorPalette().palette[2] = 0x202020_rgb;
        auto const p2 = vt.colorPalette();
        mc.writeToScreen("\033[2#P");

        REQUIRE(vt.savedColorPalettes().size() == 3);
        REQUIRE(vt.colorPalette().palette == vt.savedColorPalettes()[2 - 1].palette);

        mc.writeToScreen("\033[1#Q"); // XTPOPCOLORS
        REQUIRE(vt.savedColorPalettes().size() == 3);
        REQUIRE(vt.colorPalette().palette == vt.savedColorPalettes()[1 - 1].palette);

        mc.writeToScreen("\033[2#Q"); // XTPOPCOLORS
        REQUIRE(vt.savedColorPalettes().size() == 3);
        REQUIRE(vt.colorPalette().palette == vt.savedColorPalettes()[2 - 1].palette);

        mc.writeToScreen("\033[3#Q"); // XTPOPCOLORS
        REQUIRE(vt.savedColorPalettes().size() == 3);
        REQUIRE(vt.colorPalette().palette == vt.savedColorPalettes()[3 - 1].palette);

        mc.writeToScreen("\033[#Q"); // XTPOPCOLORS
        REQUIRE(vt.savedColorPalettes().size() == 2);
        REQUIRE(vt.colorPalette().palette == p3.palette);

        mc.writeToScreen("\033[#Q"); // XTPOPCOLORS
        REQUIRE(vt.savedColorPalettes().size() == 1);
        REQUIRE(vt.colorPalette().palette == p2.palette);

        mc.writeToScreen("\033[#Q"); // XTPOPCOLORS
        REQUIRE(vt.savedColorPalettes().empty());
        REQUIRE(vt.colorPalette().palette == p1.palette);

        mc.writeToScreen("\033[#Q"); // XTPOPCOLORS (underflow)
        REQUIRE(vt.savedColorPalettes().empty());
        REQUIRE(vt.colorPalette().palette == p1.palette);
    }
}

TEST_CASE("Terminal.DECAC", "[terminal]")
{
    using namespace vtbackend;

    auto mc = MockTerm { ColumnCount(20), LineCount(1) };
    auto& vt = mc.terminal;

    auto const originalPalette = vt.colorPalette();

    SECTION("item 1: normal text sets default fg/bg from palette indices")
    {
        // CSI 1 ; 7 ; 4 , |  -> default fg = palette[7], default bg = palette[4].
        mc.writeToScreen("\033[1;7;4,|");
        REQUIRE(vt.colorPalette().defaultForeground == originalPalette.indexedColor(7));
        REQUIRE(vt.colorPalette().defaultBackground == originalPalette.indexedColor(4));
    }

    SECTION("item 1: bare form resets default fg/bg")
    {
        mc.writeToScreen("\033[1;2;5,|"); // set something first
        REQUIRE(vt.colorPalette().defaultForeground == originalPalette.indexedColor(2));
        mc.writeToScreen("\033[1,|"); // bare item -> reset
        REQUIRE(vt.colorPalette().defaultForeground == vt.defaultColorPalette().defaultForeground);
        REQUIRE(vt.colorPalette().defaultBackground == vt.defaultColorPalette().defaultBackground);
    }

    SECTION("item 2: window frame fires setWindowFrameColor with the background index")
    {
        REQUIRE(mc.windowFrameColorChangeCount == 0);
        REQUIRE(!mc.windowFrameColor.has_value());
        // CSI 2 ; 15 ; 1 , |  -> frame/tab color = palette[1] (the background index).
        mc.writeToScreen("\033[2;15;1,|");
        REQUIRE(mc.windowFrameColorChangeCount == 1);
        REQUIRE(mc.windowFrameColor == originalPalette.indexedColor(1));
    }

    SECTION("item 2: bare form resets the window frame color")
    {
        mc.writeToScreen("\033[2;15;1,|");
        REQUIRE(mc.windowFrameColor.has_value());
        mc.writeToScreen("\033[2,|"); // bare item -> reset
        REQUIRE(mc.windowFrameColorChangeCount == 2);
        REQUIRE(!mc.windowFrameColor.has_value());
    }

    SECTION("item 2: hard reset (RIS) clears the window frame color")
    {
        mc.writeToScreen("\033[2;15;1,|");
        REQUIRE(mc.windowFrameColor.has_value());
        mc.writeToScreen("\033c"); // RIS
        REQUIRE(!mc.windowFrameColor.has_value());
    }

    SECTION("invalid item is rejected without touching state")
    {
        mc.writeToScreen("\033[3;7;4,|"); // item 3 does not exist
        REQUIRE(vt.colorPalette().defaultForeground == originalPalette.defaultForeground);
        REQUIRE(vt.colorPalette().defaultBackground == originalPalette.defaultBackground);
        REQUIRE(mc.windowFrameColorChangeCount == 0);
    }

    SECTION("out-of-range palette index is rejected without touching state")
    {
        mc.writeToScreen("\033[1;300;4,|"); // fg index > 255
        REQUIRE(vt.colorPalette().defaultForeground == originalPalette.defaultForeground);
        mc.writeToScreen("\033[1;4;300,|"); // bg index > 255
        REQUIRE(vt.colorPalette().defaultBackground == originalPalette.defaultBackground);
    }

    SECTION("item 2 validates the foreground index too, even though it consumes only the background")
    {
        // Deliberate strictness: the DEC window-frame item carries a foreground of its own (Windows
        // Terminal keeps a FrameForeground alias for it), and Contour's tab strip simply derives its
        // label color by contrast instead of using it. An out-of-range index is therefore a malformed
        // sequence, not a parameter to quietly ignore.
        mc.writeToScreen("\033[2;300;5,|");
        REQUIRE(mc.windowFrameColorChangeCount == 0);
        REQUIRE(!mc.windowFrameColor.has_value());
    }

    SECTION("a two-parameter form is malformed and neither sets nor resets")
    {
        // First set a frame color, then send the ambiguous 2-param form: it must be rejected, leaving
        // the previously-set color intact (NOT reset).
        mc.writeToScreen("\033[2;15;1,|");
        REQUIRE(mc.windowFrameColor == originalPalette.indexedColor(1));
        auto const changesBefore = mc.windowFrameColorChangeCount;
        mc.writeToScreen("\033[2;5,|");                           // item + one color: malformed
        REQUIRE(mc.windowFrameColorChangeCount == changesBefore); // no set, no reset
        REQUIRE(mc.windowFrameColor == originalPalette.indexedColor(1));
    }
}

TEST_CASE("Terminal.DECATC", "[terminal]")
{
    using namespace vtbackend;

    auto mc = MockTerm { ColumnCount(20), LineCount(1) };
    auto& vt = mc.terminal;

    // DECATC colors only take effect in DECSTGLT "Alternate color" mode; enter it so the resolver
    // applies the overrides. (A dedicated section below verifies the default AnsiSgr mode ignores them.)
    mc.writeToScreen("\033[1){"); // DECSTGLT 1 = alternate color
    REQUIRE(vt.colorPalette().colorLookupTable == ColorLookupTable::Alternate);

    // Resolve a cell's colors the way the renderer does, for a given attribute combination, optional
    // screen-wide reverse-video (DECSCNM) state, and optional SGR colors on the cell itself.
    auto resolve = [&](CellFlags flags,
                       bool reverseVideo = false,
                       Color foreground = defaultColor(),
                       Color background = defaultColor()) {
        return CellUtil::makeColors(vt.colorPalette(),
                                    vt.colorPalette().colorLookupTable,
                                    flags,
                                    reverseVideo,
                                    foreground,
                                    background,
                                    /*blinkingState*/ 1.0f,
                                    /*rapidBlinkState*/ 1.0f);
    };

    SECTION("attribute 0 (normal text) overrides plain, unattributed cells")
    {
        // CSI 0 ; 2 ; 5 , }  -> plain text: fg = palette[2], bg = palette[5].
        mc.writeToScreen("\033[0;2;5,}");
        REQUIRE(vt.colorPalette().alternateTextColors[0].has_value());
        auto const plain = resolve(CellFlags {});
        REQUIRE(plain.foreground == vt.colorPalette().indexedColor(2));
        REQUIRE(plain.background == vt.colorPalette().indexedColor(5));
        // A cell WITH an attribute is a different combination, so the attribute-0 entry does not apply;
        // having no entry of its own, it falls back to the default text colors.
        auto const bold = resolve(CellFlags { CellFlag::Bold });
        REQUIRE(bold.foreground == vt.colorPalette().defaultForeground);
        REQUIRE(bold.background == vt.colorPalette().defaultBackground);
    }

    SECTION("reverse bit tracks visual state under DECSCNM (SGR 7 XOR screen reverse)")
    {
        mc.writeToScreen("\033[2;7;1,}"); // attribute 2 = reverse
        // Under DECSCNM, a plain cell is visually reversed, so the reverse override fires.
        auto const plainReversed = resolve(CellFlags {}, /*reverseVideo*/ true);
        REQUIRE(plainReversed.foreground == vt.colorPalette().indexedColor(7));
        REQUIRE(plainReversed.background == vt.colorPalette().indexedColor(1));
        // A cell WITH SGR 7 under DECSCNM is visually NON-reversed, so the override does NOT fire.
        auto const doublyReversed = resolve(CellFlags { CellFlag::Inverse }, /*reverseVideo*/ true);
        REQUIRE(doublyReversed.foreground != vt.colorPalette().indexedColor(7));
    }

    SECTION("a two-parameter form is malformed and does not clear an existing entry")
    {
        mc.writeToScreen("\033[1;7;4,}");
        REQUIRE(vt.colorPalette().alternateTextColors[1].has_value());
        mc.writeToScreen("\033[1;5,}");                                // attribute + one color: malformed
        REQUIRE(vt.colorPalette().alternateTextColors[1].has_value()); // still set
        REQUIRE(vt.colorPalette().alternateTextColors[1]->foreground == vt.colorPalette().indexedColor(7));
    }

    SECTION("assigns colors to the bold attribute combination")
    {
        // CSI 1 ; 7 ; 4 , }  -> bold text: fg = palette[7], bg = palette[4].
        mc.writeToScreen("\033[1;7;4,}");
        auto const& stored = vt.colorPalette().alternateTextColors[1];
        REQUIRE(stored.has_value());
        REQUIRE(stored->foreground == vt.colorPalette().indexedColor(7));
        REQUIRE(stored->background == vt.colorPalette().indexedColor(4));

        // A bold cell renders with the DECATC colors...
        auto const bold = resolve(CellFlags { CellFlag::Bold });
        REQUIRE(bold.foreground == vt.colorPalette().indexedColor(7));
        REQUIRE(bold.background == vt.colorPalette().indexedColor(4));

        // ...but a plain (non-bold) cell is unaffected.
        auto const plain = resolve(CellFlags {});
        REQUIRE(plain.foreground != vt.colorPalette().indexedColor(7));
    }

    SECTION("underline matches all underline variants")
    {
        mc.writeToScreen("\033[3;2;1,}"); // attribute 3 = Underline (DEC enumerated index)
        for (auto const flag: { CellFlag::Underline,
                                CellFlag::DoublyUnderlined,
                                CellFlag::CurlyUnderlined,
                                CellFlag::DottedUnderline,
                                CellFlag::DashedUnderline })
        {
            auto const c = resolve(CellFlags { flag });
            REQUIRE(c.foreground == vt.colorPalette().indexedColor(2));
            REQUIRE(c.background == vt.colorPalette().indexedColor(1));
        }
    }

    SECTION("blink matches both blink speeds")
    {
        mc.writeToScreen("\033[4;3;5,}"); // attribute 4 = Blink (DEC enumerated index)
        // blinkingState=1.0 so the mix returns the (overridden) full color.
        auto const slow = resolve(CellFlags { CellFlag::Blinking });
        REQUIRE(slow.foreground == vt.colorPalette().indexedColor(3));
        auto const rapid = resolve(CellFlags { CellFlag::RapidBlinking });
        REQUIRE(rapid.foreground == vt.colorPalette().indexedColor(3));
    }

    SECTION("reverse-video override is the final appearance (not swapped again)")
    {
        // attribute 2 = reverse. The override must be the final fg/bg of the reversed cell, i.e.
        // applied AFTER the inverse swap, so fg stays palette[7] and bg stays palette[1].
        mc.writeToScreen("\033[2;7;1,}");
        auto const c = resolve(CellFlags { CellFlag::Inverse });
        REQUIRE(c.foreground == vt.colorPalette().indexedColor(7));
        REQUIRE(c.background == vt.colorPalette().indexedColor(1));
    }

    SECTION("attribute indices follow the DEC enumerated table, not a bitmask")
    {
        // Per VT525 §5-22 the Ps1 index is an enumerated combination, NOT an OR of bits:
        // 3 = Underline (a bitmask would read 3 as Bold+Reverse), 6 = Bold underline (not 1|4=5).
        mc.writeToScreen("\033[3;2;1,}"); // attribute 3 = Underline
        auto const underline = resolve(CellFlags { CellFlag::Underline });
        REQUIRE(underline.foreground == vt.colorPalette().indexedColor(2));

        mc.writeToScreen("\033[6;10;11,}"); // attribute 6 = Bold underline
        auto const boldUnderline = resolve(CellFlags { CellFlag::Bold } | CellFlag::Underline);
        REQUIRE(boldUnderline.foreground == vt.colorPalette().indexedColor(10));
        REQUIRE(boldUnderline.background == vt.colorPalette().indexedColor(11));

        // Bold alone (index 1) is a different, here-unset entry, so it is NOT affected by either.
        auto const boldOnly = resolve(CellFlags { CellFlag::Bold });
        REQUIRE(boldOnly.foreground != vt.colorPalette().indexedColor(2));
        REQUIRE(boldOnly.foreground != vt.colorPalette().indexedColor(10));
    }

    SECTION("in the default ANSI SGR mode DECATC overrides are ignored")
    {
        mc.writeToScreen("\033[3){");     // DECSTGLT 3 = ANSI SGR color (the power-up default)
        mc.writeToScreen("\033[1;7;4,}"); // assign bold colors — stored, but must not render
        REQUIRE(vt.colorPalette().alternateTextColors[1].has_value());
        auto const bold = resolve(CellFlags { CellFlag::Bold });
        REQUIRE(bold.foreground != vt.colorPalette().indexedColor(7));
        REQUIRE(bold.background != vt.colorPalette().indexedColor(4));
    }

    SECTION("bare form resets an assigned combination")
    {
        mc.writeToScreen("\033[1;7;4,}");
        REQUIRE(vt.colorPalette().alternateTextColors[1].has_value());
        mc.writeToScreen("\033[1,}"); // bare attribute -> reset
        REQUIRE_FALSE(vt.colorPalette().alternateTextColors[1].has_value());
    }

    SECTION("attribute 0 can be reset individually")
    {
        // Regression: a lone '0' parameter collapses to "no parameters" under the VT convention, so
        // `CSI 0 , }` reaches the handler with an empty parameter list. Registering DECATC with a
        // minimum of one parameter dropped the sequence entirely, leaving the normal-text entry the one
        // combination that could be assigned but never individually cleared.
        mc.writeToScreen("\033[0;2;5,}");
        REQUIRE(vt.colorPalette().alternateTextColors[0].has_value());
        mc.writeToScreen("\033[0,}"); // bare attribute 0 -> reset
        REQUIRE_FALSE(vt.colorPalette().alternateTextColors[0].has_value());

        // The wholly parameterless form defaults the attribute to 0 and means the same thing.
        mc.writeToScreen("\033[0;2;5,}");
        REQUIRE(vt.colorPalette().alternateTextColors[0].has_value());
        mc.writeToScreen("\033[,}");
        REQUIRE_FALSE(vt.colorPalette().alternateTextColors[0].has_value());

        // Resetting entry 0 must not disturb any other entry.
        mc.writeToScreen("\033[1;7;4,}");
        mc.writeToScreen("\033[0;2;5,}");
        mc.writeToScreen("\033[0,}");
        REQUIRE_FALSE(vt.colorPalette().alternateTextColors[0].has_value());
        REQUIRE(vt.colorPalette().alternateTextColors[1].has_value());
    }

    SECTION("an unassigned combination uses the default text colors, not the cell's SGR colors")
    {
        // In Alternate mode the ANSI SGR color parameters are ignored *entirely*: a combination the
        // application never assigned renders in the default foreground/background, exactly as if the
        // application had assigned those. (xterm expresses the same rule by seeding all sixteen entries
        // from the default pair.) Anything else would let SGR colors leak into a mode whose whole point
        // is that attribute combinations, and only those, choose the color.
        auto const& palette = vt.colorPalette();
        auto const red = IndexedColor::Red;
        auto const blue = IndexedColor::Blue;

        auto const unassigned = resolve(CellFlags { CellFlag::Bold }, false, red, blue);
        REQUIRE(unassigned.foreground == palette.defaultForeground);
        REQUIRE(unassigned.background == palette.defaultBackground);

        // Assigning that combination makes it, and only it, follow the assignment.
        mc.writeToScreen("\033[1;7;4,}"); // attribute 1 = bold
        auto const assigned = resolve(CellFlags { CellFlag::Bold }, false, red, blue);
        REQUIRE(assigned.foreground == palette.indexedColor(7));
        REQUIRE(assigned.background == palette.indexedColor(4));

        // Clearing it again returns the cell to the default colors, never to its SGR colors.
        mc.writeToScreen("\033[1,}");
        auto const cleared = resolve(CellFlags { CellFlag::Bold }, false, red, blue);
        REQUIRE(cleared.foreground == palette.defaultForeground);
        REQUIRE(cleared.background == palette.defaultBackground);
    }

    SECTION("out-of-range attribute is rejected")
    {
        mc.writeToScreen("\033[16;7;4,}"); // attribute 16 is out of the 0..15 range
        for (auto const& entry: vt.colorPalette().alternateTextColors)
            REQUIRE_FALSE(entry.has_value());
    }

    SECTION("out-of-range palette index is rejected without touching the entry")
    {
        mc.writeToScreen("\033[1;7;4,}"); // set something first
        REQUIRE(vt.colorPalette().alternateTextColors[1].has_value());
        mc.writeToScreen("\033[1;300;4,}"); // fg index > 255: rejected, entry NOT cleared
        REQUIRE(vt.colorPalette().alternateTextColors[1]->foreground == vt.colorPalette().indexedColor(7));
        mc.writeToScreen("\033[1;7;300,}"); // bg index > 255: likewise
        REQUIRE(vt.colorPalette().alternateTextColors[1]->background == vt.colorPalette().indexedColor(4));
    }

    SECTION("hard reset (RIS) clears all assignments")
    {
        mc.writeToScreen("\033[1;7;4,}");
        mc.writeToScreen("\033[8;3;5,}");
        mc.writeToScreen("\033c"); // RIS
        for (auto const& entry: vt.colorPalette().alternateTextColors)
            REQUIRE_FALSE(entry.has_value());
    }

    SECTION("soft reset (DECSTR) also clears assignments")
    {
        mc.writeToScreen("\033[1;7;4,}");
        mc.writeToScreen("\033[!p"); // DECSTR
        for (auto const& entry: vt.colorPalette().alternateTextColors)
            REQUIRE_FALSE(entry.has_value());
    }
}

TEST_CASE("Terminal.DECSTGLT", "[terminal]")
{
    using namespace vtbackend;

    auto mc = MockTerm { ColumnCount(20), LineCount(1) };
    auto& vt = mc.terminal;

    // Power-up default is ANSI SGR color.
    REQUIRE(vt.colorPalette().colorLookupTable == ColorLookupTable::AnsiSgr);

    SECTION("parameters 1 and 2 select alternate color, 3 selects ANSI SGR")
    {
        mc.writeToScreen("\033[1){");
        CHECK(vt.colorPalette().colorLookupTable == ColorLookupTable::Alternate);
        mc.writeToScreen("\033[2){");
        CHECK(vt.colorPalette().colorLookupTable == ColorLookupTable::Alternate);
        mc.writeToScreen("\033[3){");
        CHECK(vt.colorPalette().colorLookupTable == ColorLookupTable::AnsiSgr);
    }

    SECTION("an omitted or lone-zero parameter selects the default ANSI SGR color table")
    {
        // The parser collapses a lone 0 to "no parameters" (VT convention), so CSI 0 ) { and CSI ) {
        // are identical and both mean the default. This is why the spec's monochrome table, numbered 0,
        // is not modelled: it could never be selected.
        mc.writeToScreen("\033[1){"); // move away from the default first
        REQUIRE(vt.colorPalette().colorLookupTable == ColorLookupTable::Alternate);
        mc.writeToScreen("\033[){"); // no parameter -> default
        CHECK(vt.colorPalette().colorLookupTable == ColorLookupTable::AnsiSgr);
        mc.writeToScreen("\033[1){");
        mc.writeToScreen("\033[0){"); // lone zero -> also default
        CHECK(vt.colorPalette().colorLookupTable == ColorLookupTable::AnsiSgr);
    }

    SECTION("an out-of-range parameter is rejected and leaves the mode untouched")
    {
        mc.writeToScreen("\033[1){"); // alternate
        REQUIRE(vt.colorPalette().colorLookupTable == ColorLookupTable::Alternate);
        mc.writeToScreen("\033[4){"); // no such table
        CHECK(vt.colorPalette().colorLookupTable == ColorLookupTable::Alternate);
    }

    SECTION("hard reset (RIS) restores the default color look-up table")
    {
        mc.writeToScreen("\033[1){");
        REQUIRE(vt.colorPalette().colorLookupTable == ColorLookupTable::Alternate);
        mc.writeToScreen("\033c"); // RIS
        CHECK(vt.colorPalette().colorLookupTable == ColorLookupTable::AnsiSgr);
    }
}

TEST_CASE("Terminal.DECATC.doesNotRecolorTheStatusLine", "[terminal]")
{
    using namespace vtbackend;

    // The status line is host-owned chrome, painted from the colors the host configured for it. An
    // application that selects DECSTGLT Alternate mode and assigns DECATC colors recolors its own text,
    // never the terminal's furniture. Regression guard: colorLookupTable and the DECATC assignments live
    // on the terminal-global palette, which the status-line screens share with the main screen, so the
    // color mode has to be chosen per rendered screen rather than merely read off the palette.
    auto mc = MockTerm { PageSize { .lines = LineCount(4), .columns = ColumnCount(20) }, LineCount(0) };
    auto& vt = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    vt.tick(ClockBase);
    vt.setStatusDisplay(StatusDisplayType::Indicator);

    // The indicator status line renders below the main page, at the first line offset past it.
    auto const statusLine = LineOffset::cast_from(unbox(vt.pageSize().lines));

    // Every rendered (foreground, background) pair on @p line, covering both the per-cell and the
    // batched trivial-line render paths.
    auto sample = [&](LineOffset line) {
        vt.refreshRenderBuffer();
        auto const buf = vt.renderBuffer();
        auto colors = std::vector<std::pair<RGBColor, RGBColor>> {};
        for (auto const& cell: buf.get().cells)
            if (cell.position.line == line)
                colors.emplace_back(cell.attributes.foregroundColor, cell.attributes.backgroundColor);
        for (auto const& renderLine: buf.get().lines)
            if (renderLine.lineOffset == line)
            {
                colors.emplace_back(renderLine.textAttributes.foregroundColor,
                                    renderLine.textAttributes.backgroundColor);
                colors.emplace_back(renderLine.fillAttributes.foregroundColor,
                                    renderLine.fillAttributes.backgroundColor);
            }
        return colors;
    };

    mc.writeToScreen("AB");
    auto const statusBefore = sample(statusLine);
    auto const pageBefore = sample(LineOffset(0));
    REQUIRE_FALSE(statusBefore.empty()); // the status line must actually be rendered, or this proves nothing
    REQUIRE_FALSE(pageBefore.empty());

    mc.writeToScreen("\033[1){");     // DECSTGLT 1 = alternate color
    mc.writeToScreen("\033[0;2;5,}"); // DECATC: normal text -> palette[2] on palette[5]

    // The application's own text takes the assigned colors ...
    CHECK(sample(LineOffset(0)) != pageBefore);

    // ... while the status line keeps the colors the host configured for it.
    CHECK(sample(statusLine) == statusBefore);
}

TEST_CASE("Terminal.statusLine.takesNoHighlightOfTheMainPage", "[terminal][statusline]")
{
    using namespace vtbackend;

    // A selection, a yank highlight and a search match belong to the page they were made on, the main
    // display's. The status line is a screen of its own -- DEC STD 070 ch. 14's status display -- and none
    // of them may colour its cells, whatever rows they cover: not row 0, whose coordinates the status line's
    // own row shares, nor the main page's last row, which it sits beside.
    auto mc = MockTerm { PageSize { .lines = LineCount(4), .columns = ColumnCount(20) }, LineCount(0) };
    auto& vt = mc.terminal;
    vt.tick(chrono::steady_clock::time_point());

    auto const type = GENERATE(StatusDisplayType::Indicator, StatusDisplayType::HostWritable);
    INFO("status line: " << (type == StatusDisplayType::Indicator ? "indicator" : "host-writable"));
    switch (type)
    {
        case StatusDisplayType::Indicator: vt.setStatusDisplay(StatusDisplayType::Indicator); break;
        case StatusDisplayType::HostWritable:
            mc.writeToScreen("\033[2$~"); // DECSSDT host-writable
            vt.answerHostWritableStatusLineRequest(HostWritableStatusLineAnswer::Shown);
            mc.writeToScreen("\033[1$}match on the status line\033[0$}"); // DECSASD there and back
            break;
        default: FAIL("not a status line type the test covers");
    }
    REQUIRE(vt.statusDisplayType() == type);
    mc.writeToScreen("\033[Hmatch on row 0\033[4;1Hmatch on the last row");

    // Every rendered (foreground, background) pair on the status line, of the per-cell and the batched paths.
    auto const statusLine = LineOffset::cast_from(unbox(vt.pageSize().lines));
    auto sample = [&] {
        vt.refreshRenderBuffer();
        auto const buf = vt.renderBuffer();
        auto colors = std::vector<std::pair<RGBColor, RGBColor>> {};
        for (auto const& cell: buf.get().cells)
            if (cell.position.line == statusLine)
                colors.emplace_back(cell.attributes.foregroundColor, cell.attributes.backgroundColor);
        for (auto const& renderLine: buf.get().lines)
            if (renderLine.lineOffset == statusLine)
            {
                colors.emplace_back(renderLine.textAttributes.foregroundColor,
                                    renderLine.textAttributes.backgroundColor);
                colors.emplace_back(renderLine.fillAttributes.foregroundColor,
                                    renderLine.fillAttributes.backgroundColor);
            }
        return colors;
    };
    auto const unmarked = sample();
    REQUIRE_FALSE(unmarked.empty()); // the status line must actually be rendered, or this proves nothing

    auto const row = GENERATE(LineOffset(0), LineOffset(3));
    INFO("the main page's row " << row.value);
    auto const from = CellLocation { .line = row, .column = ColumnOffset(0) };
    auto const to = CellLocation { .line = row, .column = ColumnOffset(19) };

    auto const mark = GENERATE(std::string_view { "a selection" },
                               std::string_view { "a yank highlight" },
                               std::string_view { "a search match" });
    INFO(mark);
    if (mark == "a selection")
    {
        vt.setSelector(std::make_unique<LinearSelection>(vt.selectionHelper(), from, [] {}));
        (void) vt.selector()->extend(to);
        REQUIRE(vt.isSelected(vt.primaryScreen(), from));
    }
    else if (mark == "a yank highlight")
    {
        vt.setHighlightRange(LinearHighlight { .from = from, .to = to });
        REQUIRE(vt.isHighlighted(from));
    }
    else
        (void) vt.setNewSearchTerm(U"match", SearchOrigin::Typed);

    CHECK(sample() == unmarked);
}

TEST_CASE("Terminal.UnderlineStyleClearing", "[terminal]")
{
    // Each subsequent underline style should clear the former if present.

    auto mc = MockTerm { ColumnCount(20), LineCount(1) };
    mc.steadyClock.advance(1s); // a second into the session: past the render throttle's interval
    auto const now = mc.steadyClock.now();

    mc.writeToScreen("\033[4:1mAB\033[21mCD\033[4:3mEF\033[24mGH\033[4:2mIJ\033[mKL");
    mc.terminal.tick(now);
    mc.terminal.ensureFreshRenderBuffer();
    CHECK("ABCDEFGHIJKL" == trimmedTextScreenshot(mc));

    auto& screen = mc.terminal.primaryScreen();

    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).isFlagEnabled(CellFlag::Underline));
    CHECK(screen.at(LineOffset(0), ColumnOffset(1)).isFlagEnabled(CellFlag::Underline));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(2)).isFlagEnabled(CellFlag::Underline));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(3)).isFlagEnabled(CellFlag::Underline));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(4)).isFlagEnabled(CellFlag::Underline));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(5)).isFlagEnabled(CellFlag::Underline));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(6)).isFlagEnabled(CellFlag::Underline));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(7)).isFlagEnabled(CellFlag::Underline));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(8)).isFlagEnabled(CellFlag::Underline));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(9)).isFlagEnabled(CellFlag::Underline));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(10)).isFlagEnabled(CellFlag::Underline));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(11)).isFlagEnabled(CellFlag::Underline));

    CHECK(!screen.at(LineOffset(0), ColumnOffset(0)).isFlagEnabled(CellFlag::DoublyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(1)).isFlagEnabled(CellFlag::DoublyUnderlined));
    CHECK(screen.at(LineOffset(0), ColumnOffset(2)).isFlagEnabled(CellFlag::DoublyUnderlined));
    CHECK(screen.at(LineOffset(0), ColumnOffset(3)).isFlagEnabled(CellFlag::DoublyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(4)).isFlagEnabled(CellFlag::DoublyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(5)).isFlagEnabled(CellFlag::DoublyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(6)).isFlagEnabled(CellFlag::DoublyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(7)).isFlagEnabled(CellFlag::DoublyUnderlined));
    CHECK(screen.at(LineOffset(0), ColumnOffset(8)).isFlagEnabled(CellFlag::DoublyUnderlined));
    CHECK(screen.at(LineOffset(0), ColumnOffset(9)).isFlagEnabled(CellFlag::DoublyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(10)).isFlagEnabled(CellFlag::DoublyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(11)).isFlagEnabled(CellFlag::DoublyUnderlined));

    CHECK(!screen.at(LineOffset(0), ColumnOffset(0)).isFlagEnabled(CellFlag::CurlyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(1)).isFlagEnabled(CellFlag::CurlyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(2)).isFlagEnabled(CellFlag::CurlyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(3)).isFlagEnabled(CellFlag::CurlyUnderlined));
    CHECK(screen.at(LineOffset(0), ColumnOffset(4)).isFlagEnabled(CellFlag::CurlyUnderlined));
    CHECK(screen.at(LineOffset(0), ColumnOffset(5)).isFlagEnabled(CellFlag::CurlyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(6)).isFlagEnabled(CellFlag::CurlyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(7)).isFlagEnabled(CellFlag::CurlyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(8)).isFlagEnabled(CellFlag::CurlyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(9)).isFlagEnabled(CellFlag::CurlyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(10)).isFlagEnabled(CellFlag::CurlyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(11)).isFlagEnabled(CellFlag::CurlyUnderlined));

    CHECK(!screen.at(LineOffset(0), ColumnOffset(0)).isFlagEnabled(CellFlag::Italic));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(1)).isFlagEnabled(CellFlag::Italic));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(2)).isFlagEnabled(CellFlag::Italic));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(3)).isFlagEnabled(CellFlag::Italic));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(4)).isFlagEnabled(CellFlag::Italic));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(5)).isFlagEnabled(CellFlag::Italic));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(6)).isFlagEnabled(CellFlag::Italic));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(7)).isFlagEnabled(CellFlag::Italic));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(8)).isFlagEnabled(CellFlag::Italic));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(9)).isFlagEnabled(CellFlag::Italic));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(10)).isFlagEnabled(CellFlag::Italic));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(11)).isFlagEnabled(CellFlag::Italic));
}

TEST_CASE("Terminal.a_one_column_selection_still_breaks_lines", "[terminal]")
{
    // The line-break test asks "has anything been written yet", and used the accumulated text to
    // answer -- the same proxy flushLine was fixed away from, in the other of the two places that
    // used it. A one-column selection emits every callback at the same column, so the only thing that
    // could trigger a flush is that proxy, and it stays false because text only becomes non-empty
    // inside the flush it is gating.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(4) } };
    mock.writeToScreen("a\r\nb\r\nc"sv);

    // Rectangular: every callback lands on the same column, which is what leaves the line-break test
    // with nothing else to go on.
    mock.terminal.setSelector(std::make_unique<vtbackend::RectangularSelection>(
        mock.terminal.selectionHelper(),
        CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) },
        []() {}));
    (void) mock.terminal.selector()->extend(
        CellLocation { .line = LineOffset(2), .column = ColumnOffset(0) });
    mock.terminal.selector()->complete();

    CHECK(mock.terminal.extractSelectionText() == "a\nb\nc");
}

TEST_CASE("Terminal.CurlyUnderline", "[terminal]")
{
    auto mc = MockTerm { ColumnCount(20), LineCount(1) };
    mc.steadyClock.advance(1s); // a second into the session: past the render throttle's interval
    auto const now = mc.steadyClock.now();

    mc.writeToScreen("\033[4:3mAB\033[mCD");
    mc.terminal.tick(now);
    mc.terminal.ensureFreshRenderBuffer();
    CHECK("ABCD" == trimmedTextScreenshot(mc));

    auto& screen = mc.terminal.primaryScreen();

    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).isFlagEnabled(CellFlag::CurlyUnderlined));
    CHECK(screen.at(LineOffset(0), ColumnOffset(1)).isFlagEnabled(CellFlag::CurlyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(2)).isFlagEnabled(CellFlag::CurlyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(3)).isFlagEnabled(CellFlag::CurlyUnderlined));

    CHECK(!screen.at(LineOffset(0), ColumnOffset(0)).isFlagEnabled(CellFlag::Italic));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(1)).isFlagEnabled(CellFlag::Italic));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(2)).isFlagEnabled(CellFlag::Italic));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(3)).isFlagEnabled(CellFlag::Italic));
}

TEST_CASE("Terminal.ParsingBuffer", "[terminal]")
{
    // Outside a parse there is no buffer being parsed: text flushed then (queued trace events) may lie in the
    // reader's buffer or in writeToScreen()'s, so neither may have its hot end moved for it.

    auto mock = MockTerm { ColumnCount { 10 }, LineCount { 3 } };
    auto& terminal = mock.terminal;

    CHECK(terminal.parsingBuffer() == nullptr);

    // Write some text - this will exercise the parsing path
    mock.writeToScreen("Hello");

    // _parsingBuffer is reset after each parse
    CHECK(terminal.parsingBuffer() == nullptr);
}

TEST_CASE("Terminal.TrivialLineBufferIntegrity", "[terminal]")
{
    // Test that TrivialLineBuffer correctly stores text when written through terminal.
    // This tests the fast path where text is stored directly in a BufferFragment.

    auto mock = MockTerm { ColumnCount { 20 }, LineCount { 3 } };
    auto& terminal = mock.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    // Write a simple ASCII string that should use the TrivialLineBuffer fast path
    mock.writeToScreen("ABCDEFGHIJ");

    terminal.tick(ClockBase + chrono::seconds(1));
    terminal.ensureFreshRenderBuffer();

    // Verify the text was stored correctly
    auto const& line = terminal.primaryScreen().currentLine();

    // With SoA storage, all lines use LineSoA. Verify via toUtf8.
    CHECK(line.toUtf8().substr(0, 10) == "ABCDEFGHIJ");
}

TEST_CASE("Terminal.BoxDrawingCharacters", "[terminal]")
{
    // Test that box-drawing characters (3-byte UTF-8) are handled correctly.
    // This is a regression test for the corruption seen in `tree /` output.

    auto mock = MockTerm { ColumnCount { 20 }, LineCount { 5 } };
    auto& terminal = mock.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    // Write a line with box-drawing characters similar to tree output
    // "│── file" using box drawing chars
    mock.writeToScreen("\xE2\x94\x82\xE2\x94\x80\xE2\x94\x80 file\r\n");
    mock.writeToScreen("\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 dir\r\n");

    terminal.tick(ClockBase + chrono::seconds(1));
    terminal.ensureFreshRenderBuffer();

    // Verify the text contains the expected content
    auto const line0 = terminal.primaryScreen().grid().lineAt(LineOffset(0)).toUtf8();
    auto const line1 = terminal.primaryScreen().grid().lineAt(LineOffset(1)).toUtf8();

    // Check that box-drawing characters are present (not corrupted to replacement chars)
    CHECK(line0.contains("\xE2\x94\x82")); // │
    CHECK(line1.contains("\xE2\x94\x9C")); // ├
    CHECK(line0.contains("file"));
    CHECK(line1.contains("dir"));
}

TEST_CASE("Terminal.cursorAnimationProgress.no_animation_returns_1", "[terminal]")
{
    auto mc = MockTerm { ColumnCount { 10 }, LineCount { 4 } };
    auto& terminal = mc.terminal;
    // With no animation, cursor at current position should return 1.0 (complete).
    auto const gridPos = terminal.currentScreen().cursor().position;
    CHECK(terminal.cursorAnimationProgress(gridPos) == 1.0f);
}

TEST_CASE("Terminal.onBufferScrolled.preserves_viewport_with_pixel_offset", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Write enough lines to generate some history.
    for (auto i = 0; i < 8; ++i)
        mc.writeToScreen("line\r\n");

    // Scroll up and set a non-zero pixel offset.
    terminal.applySmoothScrollPixelDelta(5.0f);
    auto const offsetBefore = terminal.viewport().scrollOffset().value;

    // Write more content, triggering onBufferScrolled.
    for (auto i = 0; i < 4; ++i)
        mc.writeToScreen("more\r\n");

    // The viewport scroll offset should have increased to keep the view stable.
    CHECK(terminal.viewport().scrollOffset().value > offsetBefore);
}

TEST_CASE("Terminal.resizeScreen.minimal_one_by_one", "[terminal]")
{
    // Regression: when the render surface collapses below one cell (e.g. a pane shrunk to nothing,
    // or a transient layout state) the frontend clamps the page size to a minimum of 1x1 before
    // calling resizeScreen(). The backend must accept a 1x1 page without tripping the
    // clampToScreen() bounds assert (which fires when a page dimension reaches zero).
    auto mc = MockTerm { PageSize { LineCount { 10 }, ColumnCount { 20 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    terminal.tick(chrono::steady_clock::time_point());

    REQUIRE_NOTHROW(terminal.resizeScreen(PageSize { LineCount { 1 }, ColumnCount { 1 } }));
    CHECK(terminal.pageSize().lines == LineCount { 1 });
    CHECK(terminal.pageSize().columns == ColumnCount { 1 });

    // And it can grow back from the degenerate size.
    REQUIRE_NOTHROW(terminal.resizeScreen(PageSize { LineCount { 10 }, ColumnCount { 20 } }));
    CHECK(terminal.pageSize().lines == LineCount { 10 });
}

TEST_CASE("Terminal.resizeScreen.minimal_one_by_one.with_status_line", "[terminal]")
{
    // Regression: the same degenerate 1x1 resize, but with the indicator status line VISIBLE — the
    // contour GUI default. resizeScreen derives the main page as `totalPageSize - statusLineHeight()`,
    // so a 1x1 total here would leave a ZERO-line main page (1 - 1) and trip
    // applyPageSizeToCurrentBuffer()/verifyState(). The plain test above misses this because MockTerm
    // defaults statusDisplayType to None (statusLineHeight() == 0). resizeScreen must clamp the total
    // up so at least one main-display line survives on top of the status line.
    auto mc = MockTerm { PageSize { LineCount { 10 }, ColumnCount { 20 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    terminal.setStatusDisplay(vtbackend::StatusDisplayType::Indicator);
    terminal.tick(chrono::steady_clock::time_point());
    REQUIRE(terminal.statusLineHeight() == LineCount { 1 });

    REQUIRE_NOTHROW(terminal.resizeScreen(PageSize { LineCount { 1 }, ColumnCount { 1 } }));
    // The total was clamped up to leave one main line above the one status line; the main page
    // (what the PTY/shell sees) never collapses to zero.
    CHECK(terminal.pageSize().lines >= LineCount { 1 });
    CHECK(terminal.pageSize().columns == ColumnCount { 1 });
    CHECK(terminal.totalPageSize().lines >= LineCount { 2 });

    // And it can grow back from the degenerate size.
    REQUIRE_NOTHROW(terminal.resizeScreen(PageSize { LineCount { 10 }, ColumnCount { 20 } }));
    CHECK(terminal.pageSize().lines == LineCount { 9 }); // 10 total - 1 status line
}

TEST_CASE("Terminal.resizeScreen.narrowing_keeps_the_status_line_cursor_on_its_row", "[terminal]")
{
    // The status lines' grids were resized behind their screens' backs, so a status-line cursor beyond the
    // new right margin stayed there, and the screen's cached cursor line was never re-pointed.
    auto mc = MockTerm { PageSize { LineCount { 5 }, ColumnCount { 20 } } };
    auto& terminal = mc.terminal;
    auto const& statusLine = terminal.hostWritableStatusLineDisplay();
    mc.writeToScreen("\033[2$~");   // DECSSDT: show the host-writable status line
    mc.writeToScreen("\033[1$}");   // DECSASD: write to it
    mc.writeToScreen("\033[16GAB"); // CHA 16, then two cells: the cursor ends at column 17
    mc.writeToScreen("\033[0$}");   // back to the main display

    terminal.resizeScreen(PageSize { LineCount { 5 }, ColumnCount { 10 } });

    CHECK(statusLine.cursor().position.column < ColumnOffset { 10 });
    CHECK(&statusLine.currentLine() == &statusLine.grid().lineAt(statusLine.cursor().position.line));

    mc.writeToScreen("\033[1$}X\033[0$}"); // lands on the status line, within it
    CHECK(statusLine.grid().lineText(LineOffset { 0 }).contains('X'));
}

TEST_CASE("Terminal.answerHostWritableStatusLineRequest.a_denied_line_titles_the_window", "[terminal]")
{
    // With the host-writable status line denied, what the application writes there goes to the window
    // title as it returns to the main display (DECSASD 0) -- once per denial, and only while the
    // configuration asks for it at that moment.
    auto mc = MockTerm { PageSize { LineCount { 5 }, ColumnCount { 20 } } };
    auto& terminal = mc.terminal;
    terminal.settings().syncWindowTitleWithHostWritableStatusDisplay = true;
    terminal.setWindowTitle("before");
    mc.writeToScreen("\033[2$~"); // DECSSDT: the application asks for the host-writable status line
    auto const writeStatus = [&](std::string_view text) {
        mc.writeToScreen(std::format("\033[1$}}\033[2K\033[G{}\033[0$}}", text));
    };

    SECTION("denied: the next return titles the window, and only that one")
    {
        terminal.answerHostWritableStatusLineRequest(vtbackend::HostWritableStatusLineAnswer::Denied);
        writeStatus("first");
        CHECK(terminal.windowTitle() == "first");
        writeStatus("second");
        CHECK(terminal.windowTitle() == "first");
        CHECK(terminal.statusDisplayType() == vtbackend::StatusDisplayType::None);
    }

    SECTION("the configuration is read at the return, not at the denial")
    {
        terminal.answerHostWritableStatusLineRequest(vtbackend::HostWritableStatusLineAnswer::Denied);
        terminal.settings().syncWindowTitleWithHostWritableStatusDisplay = false;
        writeStatus("status");
        CHECK(terminal.windowTitle() == "before");

        // Still owed: the setting turned back on is honoured at the next return.
        terminal.settings().syncWindowTitleWithHostWritableStatusDisplay = true;
        writeStatus("status");
        CHECK(terminal.windowTitle() == "status");
    }

    SECTION("shown: the line keeps its text to itself")
    {
        terminal.answerHostWritableStatusLineRequest(vtbackend::HostWritableStatusLineAnswer::Denied);
        terminal.answerHostWritableStatusLineRequest(vtbackend::HostWritableStatusLineAnswer::Shown);
        CHECK(terminal.statusDisplayType() == vtbackend::StatusDisplayType::HostWritable);
        writeStatus("status");
        CHECK(terminal.windowTitle() == "before");
    }
}

TEST_CASE("Terminal.DECSASD.selects_only_a_status_line_DECSSDT_selected", "[terminal]")
{
    // DEC STD 070 ch. 14 §3 DECSASD: "Note that the Host Writable Status Line must be selected through
    // DECSSDT for DECSASD to be able to select the Host Writable Status Line." Selected, not yet shown:
    // MockTerm never answers the request DECSSDT 2 makes. A DECSASD 1 that came first is not remembered, as
    // in mintty. DECSSDT's note, "The type of Status Line can also be changed through a Set-Up field": the
    // configuration showing the host-writable status line selects it as well.
    using vtbackend::ActiveStatusDisplay;
    using vtbackend::StatusDisplayType;
    auto const [shown, setup, active] =
        GENERATE(table<StatusDisplayType, std::string_view, ActiveStatusDisplay>({
            { StatusDisplayType::None, "\033[1$}", ActiveStatusDisplay::Main },         // no DECSSDT
            { StatusDisplayType::None, "\033[1$~\033[1$}", ActiveStatusDisplay::Main }, // the indicator
            { StatusDisplayType::None,
              "\033[2$~\033[0$~\033[1$}",
              ActiveStatusDisplay::Main },                                              // selected, then none
            { StatusDisplayType::None, "\033[1$}\033[2$~", ActiveStatusDisplay::Main }, // DECSASD first
            { StatusDisplayType::None, "\033[2$~\033[1$}", ActiveStatusDisplay::StatusLine }, // host-writable
            { StatusDisplayType::None,
              "\033[2$~\033[1$~\033[2$~\033[1$}",
              ActiveStatusDisplay::StatusLine }, // selected again
            { StatusDisplayType::HostWritable,
              "\033[1$}",
              ActiveStatusDisplay::StatusLine }, // by the configuration
        }));
    INFO(std::format("shown {}, then {}", static_cast<int>(shown), e(setup)));
    auto mc = MockTerm { PageSize { LineCount { 5 }, ColumnCount { 20 } } };
    auto& terminal = mc.terminal;
    terminal.setStatusDisplay(shown);
    mc.writeToScreen(setup);
    mc.writeToScreen("X");

    CHECK(terminal.activeStatusDisplay() == active);
    auto const& written = active == ActiveStatusDisplay::Main ? terminal.primaryScreen()
                                                              : terminal.hostWritableStatusLineDisplay();
    CHECK(written.grid().lineText(LineOffset { 0 }).starts_with('X'));

    mc.resetReplyData();
    mc.writeToScreen("\033P$q$}\033\\"); // DECRQSS DECSASD
    terminal.flushInput();
    CHECK(e(mc.replyData())
          == e(std::format("\033P1$r{}$}}\033\\", active == ActiveStatusDisplay::Main ? 0 : 1)));
}

TEST_CASE("Terminal.DECSSDT.another_type_leaves_the_host_writable_status_line", "[terminal]")
{
    // DEC STD 070 ch. 14 §2.3: "If the Status Display Type is chosen to be something other than "Host
    // Writable", the Status Display will be exited".
    auto const type =
        GENERATE(std::string_view { "\033[0$~" }, std::string_view { "\033[1$~" }); // none, indicator
    INFO(e(type));
    auto mc = MockTerm { PageSize { LineCount { 5 }, ColumnCount { 20 } } };
    auto& terminal = mc.terminal;
    mc.writeToScreen("\033[2$~\033[1$}"); // DECSSDT host-writable, DECSASD: write to it
    REQUIRE(terminal.activeStatusDisplay() == vtbackend::ActiveStatusDisplay::StatusLine);
    mc.writeToScreen(type);
    mc.writeToScreen("X");

    CHECK(terminal.activeStatusDisplay() == vtbackend::ActiveStatusDisplay::Main);
    CHECK(terminal.primaryScreen().grid().lineText(LineOffset { 0 }).starts_with('X'));
    CHECK_FALSE(terminal.hostWritableStatusLineDisplay().grid().lineText(LineOffset { 0 }).contains('X'));

    mc.resetReplyData();
    mc.writeToScreen("\033P$q$}\033\\"); // DECRQSS DECSASD
    terminal.flushInput();
    CHECK(e(mc.replyData()) == e("\033P1$r0$}\033\\"));
}

TEST_CASE("Terminal.DECSASD.margins_received_on_the_status_line_are_ignored", "[terminal]")
{
    // DEC STD 070 ch. 14 §2: "controls which affect some portion of the Main Display do not affect the
    // contents or the ANSI state of the Status Display", §2.3: "most ANSI control functions received will
    // affect only the Status Display", and §2.2 keeps no margins among the status line's own state. xterm
    // drops DECSTBM there.
    auto const [mode, margins, request, reply] =
        GENERATE(table<std::string_view, std::string_view, std::string_view, std::string_view>({
            { "", "\033[2;4r", "r", "\033P1$r1;5r\033\\" },           // DECSTBM
            { "\033[?69h", "\033[3;6s", "s", "\033P1$r1;20s\033\\" }, // DECSLRM, DECLRMM set
        }));
    INFO(e(margins));
    auto mc = MockTerm { PageSize { LineCount { 5 }, ColumnCount { 20 } } };
    auto& terminal = mc.terminal;
    mc.writeToScreen(mode);
    mc.writeToScreen("\033[2$~\033[1$}AB"); // DECSSDT host-writable, DECSASD: write to it
    mc.writeToScreen(margins);
    mc.writeToScreen("C\033[0$}"); // where the status line's cursor was; back to the main display
    CHECK(terminal.hostWritableStatusLineDisplay().grid().lineText(LineOffset { 0 }).starts_with("ABC"));

    mc.resetReplyData();
    mc.writeToScreen(std::format("\033P$q{}\033\\", request)); // DECRQSS, on the main display
    terminal.flushInput();
    CHECK(e(mc.replyData()) == e(reply));
}

TEST_CASE("Terminal.DECSTR.received_on_the_status_line_resets_the_main_display", "[terminal]")
{
    // DEC STD 070 ch. 4 DECSTR, soft_terminal_reset(): "active_display = MAIN_DISPLAY;" comes before the
    // margins, the rendition and the cursor save buffer are reset, so they are the main display's. xterm's
    // ReallyReset leaves the status line first as well.
    auto mc = MockTerm { PageSize { LineCount { 5 }, ColumnCount { 20 } } };
    auto& terminal = mc.terminal;
    mc.writeToScreen("\033[1m\033[?6h");         // SGR bold, DECOM, on the main display
    mc.writeToScreen("\033[2$~\033[1$}\033[4m"); // the status line, underlined
    mc.writeToScreen("\033[!p");                 // DECSTR, received on the status line

    CHECK(terminal.activeStatusDisplay() == vtbackend::ActiveStatusDisplay::Main);
    auto const& cursor = terminal.primaryScreen().cursor();
    CHECK_FALSE(cursor.graphicsRendition.flags.contains(vtbackend::CellFlag::Bold));
    CHECK_FALSE(cursor.originMode);
}

TEST_CASE("Terminal.DECSTR.keeps_the_status_display_type", "[terminal][statusline]")
{
    // DEC STD 070 ch. 14 §2.3: DECSTR -- "The Status Display will be exited, but not erased" (RIS: "erased
    // and exited"); ch. 4's DECSTR state table has "Active Display -- Main Display (status display is
    // exited)" and no status display type. xterm's soft reset only exits (handle_DECSASD), and mintty changes
    // the type on a full reset only.
    auto mc = MockTerm { PageSize { LineCount { 5 }, ColumnCount { 20 } } };
    auto& terminal = mc.terminal;

    SECTION("the indicator")
    {
        mc.writeToScreen("\033[1$~"); // DECSSDT: the indicator
        REQUIRE(terminal.statusDisplayType() == vtbackend::StatusDisplayType::Indicator);
        mc.writeToScreen("\033[!p");
        CHECK(terminal.statusDisplayType() == vtbackend::StatusDisplayType::Indicator);
    }

    SECTION("the host-writable status line, exited but not erased")
    {
        mc.writeToScreen("\033[2$~"); // DECSSDT: the host-writable status line
        terminal.answerHostWritableStatusLineRequest(vtbackend::HostWritableStatusLineAnswer::Shown);
        mc.writeToScreen("\033[1$}kept");
        REQUIRE(terminal.statusDisplayType() == vtbackend::StatusDisplayType::HostWritable);

        mc.writeToScreen("\033[!p");
        CHECK(terminal.activeStatusDisplay() == vtbackend::ActiveStatusDisplay::Main);
        CHECK(terminal.statusDisplayType() == vtbackend::StatusDisplayType::HostWritable);
        CHECK(terminal.hostWritableStatusLineDisplay().renderMainPageText().contains("kept"));

        // Still selected, so DECSASD may enter it again.
        mc.writeToScreen("\033[1$}");
        CHECK(terminal.activeStatusDisplay() == vtbackend::ActiveStatusDisplay::StatusLine);

        // RIS, by contrast, erases and exits it, and returns to the configured type.
        mc.writeToScreen("\033c");
        CHECK(terminal.activeStatusDisplay() == vtbackend::ActiveStatusDisplay::Main);
        CHECK(terminal.statusDisplayType() == terminal.factorySettings().statusDisplayType);
        CHECK(terminal.statusDisplayType() != vtbackend::StatusDisplayType::HostWritable);
        CHECK_FALSE(terminal.hostWritableStatusLineDisplay().renderMainPageText().contains("kept"));
    }
}

TEST_CASE("Terminal.revisions.rowsThatScrolledOutBeforeTheStatusLineAppearsAreStamped",
          "[terminal][statusline]")
{
    // Vi mode shows the indicator status line, which takes a row from the page: a page resize. The rows the
    // output scrolled into the history before it, with no finalize in between, are stamped by the next one
    // as the page's rows are.
    auto mc = MockTerm { PageSize { LineCount(7), ColumnCount(40) }, LineCount(30) };
    auto& terminal = mc.terminal;
    for (auto const i: std::views::iota(0, 18))
        mc.writeToScreen(std::format("line {}\r\n", i));
    REQUIRE(terminal.statusDisplayType() == vtbackend::StatusDisplayType::None);

    terminal.inputHandler().setMode(vtbackend::ViMode::Normal);
    auto& grid = terminal.displayedPage().grid();
    REQUIRE(grid.pageSize().lines == LineCount(6));
    grid.finalizeRevisions();

    auto const history = unbox<int>(grid.historyLineCount());
    REQUIRE(history > 0);
    for (auto const offset: std::views::iota(-history, unbox<int>(grid.pageSize().lines)))
    {
        INFO("row " << offset);
        auto const& line = std::as_const(grid).lineAt(vtbackend::LineOffset(offset));
        CHECK_FALSE(line.isDirty());
        CHECK(line.revision() != 0);
    }
}

TEST_CASE("Terminal.statusNotices.RIS_drops_the_notice_of_a_locked_keyboard", "[terminal][statusline]")
{
    // RIS resets KAM with every other mode, so the locked keyboard's notice goes too: a notice put up later,
    // vi mode's, is then the only one, and taking it down shows the type RIS left.
    auto mc = MockTerm { PageSize { LineCount { 5 }, ColumnCount { 20 } } };
    auto& terminal = mc.terminal;
    REQUIRE(terminal.statusDisplayType() == vtbackend::StatusDisplayType::None);
    mc.writeToScreen("\033[2h"); // KAM: the keyboard locked, its notice up
    REQUIRE(terminal.statusDisplayType() == vtbackend::StatusDisplayType::Indicator);
    mc.writeToScreen("\033c");
    REQUIRE(terminal.statusDisplayType() == vtbackend::StatusDisplayType::None);

    terminal.inputHandler().setMode(vtbackend::ViMode::Normal);
    CHECK(terminal.statusDisplayType() == vtbackend::StatusDisplayType::Indicator);
    terminal.inputHandler().setMode(vtbackend::ViMode::Insert);
    CHECK(terminal.statusDisplayType() == vtbackend::StatusDisplayType::None);
}

TEST_CASE("Terminal.statusNotices.the_host_may_not_take_a_notice_down", "[terminal][statusline]")
{
    // DEC STD 070 ch. 14 §2.1.1: "The host may not directly program or affect the Error Line", DEC's own
    // notice. RIS, DECSSDT and the frontend's grant of the host-writable status line leave a notice up; the
    // type the host chose is what comes back once the last notice is down.
    // Wide enough for the whole "key bindings disabled" notice: the indicator line is clipped to the page.
    auto mc = MockTerm { PageSize { LineCount { 5 }, ColumnCount { 40 } } };
    auto& terminal = mc.terminal;
    REQUIRE(terminal.statusDisplayType() == vtbackend::StatusDisplayType::None);

    SECTION("RIS during vi mode")
    {
        terminal.inputHandler().setMode(vtbackend::ViMode::Normal);
        REQUIRE(terminal.statusDisplayType() == vtbackend::StatusDisplayType::Indicator);
        mc.writeToScreen("\033c");
        CHECK(terminal.statusDisplayType() == vtbackend::StatusDisplayType::Indicator);

        terminal.inputHandler().setMode(vtbackend::ViMode::Insert);
        CHECK(terminal.statusDisplayType() == terminal.factorySettings().statusDisplayType);
    }

    SECTION("DECSSDT while the key bindings are disabled")
    {
        terminal.setStatusLineDefinition(vtbackend::parseStatusLineDefinition(
            "{}", "{Text:text=key bindings disabled,Left= « ,Right= » }", "{}"));
        terminal.pushStatusDisplay(vtbackend::StatusNotice::KeyBindingsDisabled,
                                   vtbackend::StatusDisplayType::Indicator);
        mc.writeToScreen("\033[1$~\033[0$~"); // DECSSDT indicator, then none
        CHECK(terminal.statusDisplayType() == vtbackend::StatusDisplayType::Indicator);
        terminal.refreshRenderBuffer(); // the frame writes the indicator status line
        CHECK(terminal.indicatorStatusLineDisplay().renderMainPageText().contains("key bindings disabled"));

        terminal.popStatusDisplay(vtbackend::StatusNotice::KeyBindingsDisabled);
        CHECK(terminal.statusDisplayType() == vtbackend::StatusDisplayType::None);
    }

    SECTION("the host-writable status line granted during vi mode")
    {
        terminal.inputHandler().setMode(vtbackend::ViMode::Normal);
        mc.writeToScreen("\033[2$~"); // DECSSDT host-writable: asked of the frontend
        terminal.answerHostWritableStatusLineRequest(vtbackend::HostWritableStatusLineAnswer::Shown);
        CHECK(terminal.statusDisplayType() == vtbackend::StatusDisplayType::Indicator);

        terminal.inputHandler().setMode(vtbackend::ViMode::Insert);
        CHECK(terminal.statusDisplayType() == vtbackend::StatusDisplayType::HostWritable);
    }
}

TEST_CASE("Terminal.userInputLock.noHostResetUnlocksIt", "[terminal][statusline]")
{
    // KAM is the host's, which DECSTR, RM and RIS reset (DEC STD 070 ch. 6 §6.2); the user's lock is not KAM.
    // It, and its notice, outlast them, and only the user lifts it.
    auto const reset = GENERATE(
        std::string_view { "\033[!p" }, std::string_view { "\033[2l" }, std::string_view { "\033c" });
    INFO("reset: " << core::escape(reset));
    auto mc = MockTerm { PageSize { LineCount { 5 }, ColumnCount { 40 } } };
    auto& terminal = mc.terminal;
    REQUIRE(terminal.statusDisplayType() == vtbackend::StatusDisplayType::None);

    terminal.setUserInputLock(vtbackend::UserInputLock::Locked);
    REQUIRE(terminal.statusDisplayType() == vtbackend::StatusDisplayType::Indicator);
    mc.writeToScreen(reset);

    CHECK(terminal.userInputLock() == vtbackend::UserInputLock::Locked);
    CHECK_FALSE(terminal.allowInput());
    CHECK(terminal.statusDisplayType() == vtbackend::StatusDisplayType::Indicator);
    mc.resetReplyData();
    mc.sendCharSequence("x");
    CHECK(mc.replyData().empty());

    terminal.setUserInputLock(vtbackend::UserInputLock::Unlocked);
    CHECK(terminal.allowInput());
    CHECK(terminal.statusDisplayType() == vtbackend::StatusDisplayType::None);
    mc.sendCharSequence("x");
    CHECK(mc.replyData() == "x");
}

TEST_CASE("Terminal.userInputLock.andTheHostsKAMAreIndependent", "[terminal]")
{
    // Either lock holds the keyboard; neither lifts the other.
    auto mc = MockTerm { PageSize { LineCount { 5 }, ColumnCount { 40 } } };
    auto& terminal = mc.terminal;

    SECTION("the host locks, the user locks and unlocks")
    {
        mc.writeToScreen("\033[2h"); // KAM set
        terminal.setUserInputLock(vtbackend::UserInputLock::Locked);
        terminal.setUserInputLock(vtbackend::UserInputLock::Unlocked);
        CHECK(terminal.isModeEnabled(vtbackend::AnsiMode::KeyboardAction));
        CHECK_FALSE(terminal.allowInput());
        mc.writeToScreen("\033[2l"); // KAM reset
        CHECK(terminal.allowInput());
    }

    SECTION("the user locks, the host locks and unlocks")
    {
        terminal.setUserInputLock(vtbackend::UserInputLock::Locked);
        mc.writeToScreen("\033[2h\033[2l"); // KAM set, then reset
        CHECK_FALSE(terminal.allowInput());
        terminal.setUserInputLock(vtbackend::UserInputLock::Unlocked);
        CHECK(terminal.allowInput());
    }

    SECTION("the host's KAM is reported as the host set it")
    {
        // DECRQM answers for KAM alone: the user's lock is no mode of the host's.
        terminal.setUserInputLock(vtbackend::UserInputLock::Locked);
        mc.discardPendingReplies();
        mc.writeToScreen("\033[2$p"); // DECRQM KAM
        terminal.flushInput();
        CHECK(mc.replyData() == "\033[2;2$y"); // reset
    }
}

TEST_CASE("Terminal.CancelSelection_no_selection", "[terminal]")
{
    // Reproducer for #1839: On a fresh terminal with no selection,
    // selectionAvailable() must return false so CancelSelection
    // does not consume the key event.
    auto mock = MockTerm { ColumnCount { 10 }, LineCount { 3 } };
    auto& terminal = mock.terminal;

    mock.writeToScreen("Hello World");
    terminal.ensureFreshRenderBuffer();

    CHECK_FALSE(terminal.selectionAvailable());

    // clearSelection() should be a safe no-op when no selection exists.
    terminal.clearSelection();
    CHECK_FALSE(terminal.selectionAvailable());
}

TEST_CASE("Terminal.CancelSelection_with_selection", "[terminal]")
{
    // Verify that creating a selection sets selectionAvailable() to true,
    // and clearSelection() resets it to false.
    auto mock = MockTerm { ColumnCount { 5 }, LineCount { 3 } };
    auto& terminal = mock.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();

    terminal.tick(ClockBase);
    terminal.ensureFreshRenderBuffer();
    mock.writeToScreen("Hello\r\nWorld\r\nTest!");
    terminal.tick(ClockBase + 1s);
    terminal.ensureFreshRenderBuffer();

    using namespace vtbackend;
    auto constexpr UiHandledHint = false;
    auto constexpr PixelCoordinate = vtbackend::PixelCoordinate {};

    // Initiate a mouse selection across lines to avoid division-by-zero
    // on cellPixelWidth in mock terminal (no real renderer).
    terminal.tick(ClockBase + 2s);
    terminal.sendMouseMoveEvent(
        Modifier::None, 0_lineOffset + 1_columnOffset, PixelCoordinate, UiHandledHint);
    terminal.tick(ClockBase + 3s);
    terminal.sendMousePressEvent(
        Modifier::None, MouseButton::Left, 0_lineOffset + 1_columnOffset, PixelCoordinate, UiHandledHint);
    terminal.tick(ClockBase + 4s);
    terminal.sendMouseMoveEvent(
        Modifier::None, 1_lineOffset + 2_columnOffset, PixelCoordinate, UiHandledHint);
    terminal.tick(ClockBase + 5s);
    terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoordinate, UiHandledHint);

    REQUIRE(terminal.selectionAvailable());
    CHECK_FALSE(terminal.extractSelectionText().empty());

    // Now clear it, simulating what CancelSelection does.
    terminal.clearSelection();
    CHECK_FALSE(terminal.selectionAvailable());
}

TEST_CASE("Terminal.CancelSelection_double_clear", "[terminal]")
{
    // Edge case: calling clearSelection() twice must not crash or
    // have unexpected side effects.
    auto mock = MockTerm { ColumnCount { 5 }, LineCount { 3 } };
    auto& terminal = mock.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();

    terminal.tick(ClockBase);
    terminal.ensureFreshRenderBuffer();
    mock.writeToScreen("Hello\r\nWorld\r\nTest!");
    terminal.tick(ClockBase + 1s);
    terminal.ensureFreshRenderBuffer();

    using namespace vtbackend;
    auto constexpr UiHandledHint = false;
    auto constexpr PixelCoordinate = vtbackend::PixelCoordinate {};

    // Create a selection across lines.
    terminal.tick(ClockBase + 2s);
    terminal.sendMouseMoveEvent(
        Modifier::None, 0_lineOffset + 1_columnOffset, PixelCoordinate, UiHandledHint);
    terminal.tick(ClockBase + 3s);
    terminal.sendMousePressEvent(
        Modifier::None, MouseButton::Left, 0_lineOffset + 1_columnOffset, PixelCoordinate, UiHandledHint);
    terminal.tick(ClockBase + 4s);
    terminal.sendMouseMoveEvent(
        Modifier::None, 1_lineOffset + 2_columnOffset, PixelCoordinate, UiHandledHint);
    terminal.tick(ClockBase + 5s);
    terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoordinate, UiHandledHint);

    REQUIRE(terminal.selectionAvailable());

    // First clear.
    terminal.clearSelection();
    CHECK_FALSE(terminal.selectionAvailable());

    // Second clear — must be a safe no-op.
    terminal.clearSelection();
    CHECK_FALSE(terminal.selectionAvailable());
}

TEST_CASE("Terminal.ScrollWhileSelecting", "[terminal]")
{
    // Create TE with scrollback history.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) }, LineCount(10) };
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    mock.terminal.tick(ClockBase);
    // Write enough lines to create history.
    mock.writeToScreen("AAAAA\r\n"
                       "BBBBB\r\n"
                       "CCCCC\r\n"
                       "DDDDD\r\n"
                       "EEEEE\r\n"
                       "FFFFF");
    // History: AAAAA, BBBBB, CCCCC. Main page: DDDDD, EEEEE, FFFFF

    using namespace vtbackend;
    auto constexpr UiHandledHint = false;
    auto constexpr PixelCoord = vtbackend::PixelCoordinate {};

    SECTION("wheel scroll up extends selection into history")
    {
        // Start a selection in the middle of the main page.
        mock.terminal.tick(1s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 1_lineOffset + 2_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(2s);
        mock.terminal.sendMousePressEvent(
            Modifier::None, MouseButton::Left, 1_lineOffset + 2_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(3s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);
        // Selection is InProgress.
        REQUIRE(mock.terminal.selectionAvailable());
        CHECK(mock.terminal.selector()->state() == Selection::State::InProgress);

        // Simulate wheel-scroll up (viewport change without mouse move).
        mock.terminal.viewport().scrollUp(LineCount(2));
        // extendSelectionAfterScroll is called from onViewportChanged.

        // The selection should have been extended to the new mouse position
        // relative to the scrolled viewport.
        CHECK(mock.terminal.viewport().scrollOffset() == ScrollOffset(2));
        // The selection endpoint should now be in history.
        auto const text = mock.terminal.extractSelectionText();
        CHECK_FALSE(text.empty());
    }

    SECTION("scroll without selection is no-op")
    {
        // No selection active, scroll should not crash or create a selection.
        CHECK_FALSE(mock.terminal.selectionAvailable());
        mock.terminal.viewport().scrollUp(LineCount(1));
        CHECK_FALSE(mock.terminal.selectionAvailable());
    }

    SECTION("scroll with completed selection is no-op")
    {
        // Create and complete a selection.
        mock.terminal.tick(1s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(2s);
        mock.terminal.sendMousePressEvent(
            Modifier::None, MouseButton::Left, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(3s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 1_lineOffset + 2_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(4s);
        mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        auto const textBefore = mock.terminal.extractSelectionText();
        CHECK(mock.terminal.selector()->state() == Selection::State::Complete);

        // Scroll should NOT extend the completed selection.
        mock.terminal.viewport().scrollUp(LineCount(1));
        CHECK(mock.terminal.extractSelectionText() == textBefore);
    }
}

TEST_CASE("Terminal.PerformAutoScroll", "[terminal]")
{
    // Create TE with scrollback history.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) }, LineCount(10) };
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    mock.terminal.tick(ClockBase);
    // Write enough lines to fill history.
    mock.writeToScreen("AAAAA\r\n"
                       "BBBBB\r\n"
                       "CCCCC\r\n"
                       "DDDDD\r\n"
                       "EEEEE\r\n"
                       "FFFFF");
    // History: AAAAA, BBBBB, CCCCC. Main page: DDDDD, EEEEE, FFFFF

    using namespace vtbackend;
    auto constexpr UiHandledHint = false;
    auto constexpr PixelCoord = vtbackend::PixelCoordinate {};

    SECTION("scrolls up and extends selection")
    {
        // Start a selection on the main page.
        mock.terminal.tick(1s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 1_lineOffset + 2_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(2s);
        mock.terminal.sendMousePressEvent(
            Modifier::None, MouseButton::Left, 1_lineOffset + 2_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(3s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);
        // Selection should be in progress now.
        REQUIRE(mock.terminal.selectionAvailable());
        CHECK(mock.terminal.selector()->state() == Selection::State::InProgress);

        // Perform auto-scroll up by 1 line.
        mock.terminal.performAutoScroll(-1, LineCount(1));
        CHECK(mock.terminal.viewport().scrollOffset() == ScrollOffset(1));

        // Perform auto-scroll up by 2 more lines.
        mock.terminal.performAutoScroll(-1, LineCount(2));
        CHECK(mock.terminal.viewport().scrollOffset() == ScrollOffset(3));
    }

    SECTION("does nothing without active selection")
    {
        // No selection → performAutoScroll should be a no-op.
        CHECK_FALSE(mock.terminal.selectionAvailable());
        mock.terminal.performAutoScroll(-1, LineCount(1));
        CHECK(mock.terminal.viewport().scrollOffset() == ScrollOffset(0));
    }

    SECTION("does nothing with completed selection")
    {
        // Create and complete a selection.
        mock.terminal.tick(1s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(2s);
        mock.terminal.sendMousePressEvent(
            Modifier::None, MouseButton::Left, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(3s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 1_lineOffset + 2_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(4s);
        mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        CHECK(mock.terminal.selector()->state() == Selection::State::Complete);

        // Auto-scroll should be a no-op for completed selections.
        mock.terminal.performAutoScroll(-1, LineCount(1));
        CHECK(mock.terminal.viewport().scrollOffset() == ScrollOffset(0));
    }

    SECTION("stops at history boundary")
    {
        // Start a selection.
        mock.terminal.tick(1s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 1_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(2s);
        mock.terminal.sendMousePressEvent(
            Modifier::None, MouseButton::Left, 1_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(3s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);

        // Try to scroll way past history.
        mock.terminal.performAutoScroll(-1, LineCount(100));
        // Should be capped at the available history.
        CHECK(mock.terminal.viewport().scrollOffset() <= ScrollOffset(3));
    }
}

TEST_CASE("Terminal.PassiveMouseTracking_Selection", "[terminal]")
{
    using namespace vtbackend;

    auto mock = MockTerm { ColumnCount(5), LineCount(3) };
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    mock.terminal.tick(ClockBase);
    mock.terminal.ensureFreshRenderBuffer();

    mock.writeToScreen("Hello\r\n"
                       "World\r\n"
                       "Test!");

    mock.terminal.tick(ClockBase + chrono::seconds(1));
    mock.terminal.ensureFreshRenderBuffer();
    CHECK("Hello\nWorld\nTest!" == trimmedTextScreenshot(mock));

    auto constexpr UiHandledHint = false;
    auto constexpr PixelCoord = vtbackend::PixelCoordinate {};

    // Enable passive mouse tracking (DEC mode 2029).
    // This also implicitly enables MouseSGR (1006) and MouseProtocolButtonTracking (1002).
    mock.terminal.setMode(DECMode::MousePassiveTracking, true);
    CHECK(mock.terminal.isModeEnabled(DECMode::MousePassiveTracking));
    CHECK(mock.terminal.isModeEnabled(DECMode::MouseSGR));
    CHECK(mock.terminal.isModeEnabled(DECMode::MouseProtocolButtonTracking));

    // Clear any reply data generated by mode-setting.
    mock.resetReplyData();

    // Helper: perform a click-drag selection of "Hello" on line 0.
    auto const selectHello = [&] {
        mock.terminal.tick(1s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(2s);
        mock.terminal.sendMousePressEvent(
            Modifier::None, MouseButton::Left, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(3s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 0_lineOffset + 4_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(4s);
        mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        REQUIRE(mock.terminal.extractSelectionText() == "Hello");
    };

    SECTION("click-drag creates selection while passive tracking is on")
    {
        // Move cursor to start position (row 0, col 0)
        mock.terminal.tick(1s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);

        // Press left button — should start selection AND forward to app
        mock.terminal.tick(2s);
        auto const handled = mock.terminal.sendMousePressEvent(
            Modifier::None, MouseButton::Left, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);

        // Passive tracking should return Handled{false} so session can also process action mappings
        CHECK(handled == Handled { false });

        // Selection should be in Waiting state (waiting for drag to start)
        REQUIRE(mock.terminal.selector());
        CHECK(mock.terminal.selector()->state() == Selection::State::Waiting);

        // Drag to extend selection (row 0, col 4 → selects "Hello")
        mock.terminal.tick(3s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 0_lineOffset + 4_columnOffset, PixelCoord, UiHandledHint);
        CHECK(mock.terminal.extractSelectionText() == "Hello");

        // Release
        mock.terminal.tick(4s);
        mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        CHECK(mock.terminal.extractSelectionText() == "Hello");

        // Verify mouse events were forwarded to the app via the PTY (SGR format: ESC [ < ... M/m)
        auto const& reply = mock.replyData();
        CHECK(reply.contains("\033[<"));
    }

    SECTION("click-to-deselect works with passive tracking")
    {
        selectHello();

        // Now click to deselect
        mock.terminal.tick(5s);
        mock.terminal.sendMousePressEvent(
            Modifier::None, MouseButton::Left, 0_lineOffset + 4_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        CHECK(mock.terminal.extractSelectionText().empty());
    }
}

TEST_CASE("Terminal.Cursorline.trivialLineUnderCursorIsHighlighted", "[terminal][vi]")
{
    // Regression: a plain-text (trivial) line under the vi cursor must be highlighted.
    // A tall page keeps all four content lines visible above the bottom status line that
    // normal mode pushes in.
    auto mc = MockTerm { PageSize { LineCount(8), ColumnCount(10) }, LineCount(0) };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    // grid line 0: plain text (trivial), 1: colorized (inflated), 2: empty (trivial),
    // 3: plain text (trivial).
    mc.writeToScreen("plain0\r\n");
    mc.writeToScreen("\033[38;2;255;0;0m"); // red foreground
    mc.writeToScreen("tinted1");
    mc.writeToScreen("\033[m\r\n"); // reset SGR
    mc.writeToScreen("\r\n");
    mc.writeToScreen("plain3");

    terminal.inputHandler().setMode(vtbackend::ViMode::Normal);
    auto const defaultBg = terminal.colorPalette().defaultBackground;

    // Every line type (plain trivial, colorized inflated, empty trivial) must highlight when the
    // cursor lands on it. Assert the tint appears on exactly the cursor's screen line.
    auto seconds = 2;
    for (auto const gridLine: { 0, 1, 2, 3 })
    {
        terminal.moveNormalModeCursorTo(
            vtbackend::CellLocation { .line = LineOffset(gridLine), .column = ColumnOffset(0) });
        terminal.tick(ClockBase + chrono::seconds(seconds++));
        terminal.refreshRenderBuffer();
        auto const buf = terminal.renderBuffer();

        auto const cursorLine = renderedCursorLine(buf);
        REQUIRE(cursorLine.has_value());
        auto const bgOnCursorLine = screenLineBackground(buf, *cursorLine);
        INFO(std::format("gridLine={} renderedCursorLine={} bgOnCursorLine={}",
                         gridLine,
                         cursorLine->value,
                         bgOnCursorLine));
        // The cursor line's background must be tinted away from the default background.
        CHECK(bgOnCursorLine != defaultBg);
    }
}

TEST_CASE("Terminal.Cursorline.notShownInInsertMode", "[terminal][vi]")
{
    // The cursorline is a normal-mode affordance; insert mode must not tint any content line.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(10) }, LineCount(0) };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);
    mc.writeToScreen("plainA\r\n");
    mc.writeToScreen("plainB\r\n");
    mc.writeToScreen("plainC");

    // Stays in the default insert mode (no status line, no cursorline).
    terminal.tick(ClockBase + chrono::seconds(1));
    terminal.refreshRenderBuffer();
    auto const buf = terminal.renderBuffer();
    auto const defaultBg = terminal.colorPalette().defaultBackground;

    for (auto const line: { 0, 1, 2 })
        CHECK(screenLineBackground(buf, LineOffset(line)) == defaultBg);
}

TEST_CASE("Terminal.Cursorline.nonCursorTrivialLineNotHighlighted", "[terminal][vi]")
{
    // Only the cursor's line is tinted; a sibling plain-text trivial line keeps the default bg.
    // Use a tall page so content lines stay clear of the bottom indicator status line.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(10) }, LineCount(0) };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);
    mc.writeToScreen("plainA\r\n");
    mc.writeToScreen("plainB");

    terminal.inputHandler().setMode(vtbackend::ViMode::Normal);
    terminal.moveNormalModeCursorTo(
        vtbackend::CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) });
    terminal.tick(ClockBase + chrono::seconds(1));
    terminal.refreshRenderBuffer();
    auto const buf = terminal.renderBuffer();
    auto const defaultBg = terminal.colorPalette().defaultBackground;

    auto const cursorLine = renderedCursorLine(buf);
    REQUIRE(cursorLine.has_value());
    CHECK(screenLineBackground(buf, *cursorLine) != defaultBg);
    // The immediately following content line (not the cursor line, well above the status line)
    // must keep the default background.
    CHECK(screenLineBackground(buf, LineOffset(cursorLine->value + 1)) == defaultBg);
}

TEST_CASE("Terminal.GraphemeCluster.selectionDoesNotShiftLayout", "[terminal][unicode]")
{
    // Each case names the layout it must produce, unselected AND selected, and which path the
    // unselected state must have taken. Naming the layout rather than comparing the two states
    // against each other matters: both derive from the same line, so two equally wrong layouts
    // would satisfy a self-consistency check without drawing the right thing. Naming the path
    // matters just as much -- a case that never reaches the batched form is not testing the
    // batched-vs-per-cell agreement it appears to be testing.
    struct TestCase
    {
        std::string_view name;
        std::string_view text;
        std::string_view layout;
        RenderPath unselectedPath;
    };

    // The first three are #1752's reproduction lines. The two that broke are the two whose BASE
    // codepoint is one column wide and which only reach two columns through VS16 -- 1F441 EYE and
    // 1F3F3 WAVING WHITE FLAG; 1F44D THUMBS UP is wide already, which is why the reporter saw it
    // survive. In all three the closing bracket belongs at column 3. They widen through a
    // continuation cell, which is what takes them off the batched path in BOTH states.
    auto constexpr Cases = std::array {
        TestCase { .name = "thumbs up + skin tone modifier",
                   .text = "[\U0001F44D\U0001F3FB]"sv,
                   .layout = "0:U+005B(w1) 1:U+1F44D+U+1F3FB(w2) 3:U+005D(w1)"sv,
                   .unselectedPath = RenderPath::PerCell },
        TestCase { .name = "rainbow flag: VS16, ZWJ, rainbow",
                   .text = "[\U0001F3F3\uFE0F\u200D\U0001F308]"sv,
                   .layout = "0:U+005B(w1) 1:U+1F3F3+U+FE0F+U+200D+U+1F308(w2) 3:U+005D(w1)"sv,
                   .unselectedPath = RenderPath::PerCell },
        TestCase { .name = "eye + VS16",
                   .text = "[\U0001F441\uFE0F]"sv,
                   .layout = "0:U+005B(w1) 1:U+1F441+U+FE0F(w2) 3:U+005D(w1)"sv,
                   .unselectedPath = RenderPath::PerCell },
        // Plain text stays on the batched path, so this is the case that actually compares the two
        // representations against each other -- the comparison #1752 is about.
        TestCase { .name = "plain text",
                   .text = "abc"sv,
                   .layout = "0:U+0061(w1) 1:U+0062(w1) 2:U+0063(w1)"sv,
                   .unselectedPath = RenderPath::Batched },
        // A cluster that stays ONE column wide writes no continuation cell, so only the cluster rule
        // itself takes its line off the batched path -- and that path stores one codepoint per
        // column (@see Line::trivialBuffer), silently discarding everything after the base. A
        // decomposed alpha + COMBINING ACUTE therefore reached the renderer as a bare alpha.
        TestCase { .name = "alpha + combining acute",
                   .text = "\u03B1\u0301x"sv,
                   .layout = "0:U+03B1+U+0301(w1) 1:U+0078(w1)"sv,
                   .unselectedPath = RenderPath::PerCell },
    };

    for (auto const& testCase: Cases)
    {
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) }, LineCount(0) };
        // Park the cursor on the NEXT line. Left on line 0, gridLineContainsCursor() forces the
        // per-cell path even unselected, and the batched half of every comparison below silently
        // stops happening.
        mock.writeToScreen(testCase.text);
        mock.writeToScreen("\r\n"sv);

        auto const [unselected, unselectedPath] = renderLineOf(mock.terminal, LineOffset(0));
        selectColumns(mock.terminal, LineOffset(0), ColumnOffset(0), ColumnOffset(19));
        auto const [selected, selectedPath] = renderLineOf(mock.terminal, LineOffset(0));

        INFO(testCase.name);
        CHECK(unselectedPath == testCase.unselectedPath);
        CHECK(selectedPath == RenderPath::PerCell); // a selection always forces per-cell
        CHECK(unselected == testCase.layout);
        CHECK(selected == testCase.layout);
    }
}

TEST_CASE("Terminal.GraphemeCluster.aWideCellOnTheLastColumnKeepsItsWidth", "[terminal][unicode]")
{
    // A wide character leaves the batched path only through the continuation cell it writes, and
    // Screen::clearAndAdvance skips that fill when a single column is left -- so a full-width
    // character on the last writable column stays on a line the batched path still claims, as a
    // TWO-column cell. The selection fallback must measure it the same way the batched path does;
    // reporting one column shrinks the selection background and any underline behind the glyph.
    //
    // Text written there no longer leaves such a cell -- the character moves on to the next row, or
    // is dropped without autowrap -- so the cell is put in place the way that write used to put it.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) }, LineCount(0) };
    mock.terminal.primaryScreen()
        .grid()
        .lineAt(LineOffset(0))
        .useCellAt(ColumnOffset(4))
        .write(vtbackend::GraphicsAttributes {},
               U'\u4E16',
               2,
               vtbackend::HyperlinkId {}); // last column of a 5-column page
    mock.writeToScreen("\033[2;1H"sv);     // cursor off the line under test

    REQUIRE(mock.terminal.primaryScreen().grid().lineAt(LineOffset(0)).isTrivialBuffer());
    REQUIRE(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(4)).width() == 2);

    auto const [unselected, unselectedPath] = renderLineOf(mock.terminal, LineOffset(0));
    selectColumns(mock.terminal, LineOffset(0), ColumnOffset(0), ColumnOffset(4));
    auto const [selected, selectedPath] = renderLineOf(mock.terminal, LineOffset(0));

    CHECK(unselectedPath == RenderPath::Batched);
    CHECK(selectedPath == RenderPath::PerCell);
    CHECK(unselected == "4:U+4E16(w2)");
    CHECK(selected == unselected);
}

TEST_CASE("Terminal.GraphemeCluster.batchedFallbackDoesNotInheritTheCursor", "[terminal][unicode]")
{
    // makeColorsForCell extends a block cursor over the second column of a wide glyph by consulting
    // _prevWidth/_prevHasCursor, which every emitter must reset per cell. startLine() resets them
    // for a per-cell line, but renderTrivialLine has no such entry point -- so if its fallback
    // resets only once, a line following one that ENDS in a wide glyph under the cursor is painted
    // entirely in cursor colours.
    //
    // Asserted against a control that differs only in the previous line's last cell, so no palette
    // value is hard-coded.
    auto const colorsOfSecondLine = [](std::string_view lastCellOfFirstLine, ColumnOffset cursorColumn) {
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) }, LineCount(0) };
        auto constexpr ClockBase = chrono::steady_clock::time_point();
        mock.terminal.tick(ClockBase);
        // The block-cursor inversion this test is about is suppressed while a cursor MOTION
        // animation is in flight (makeColorsForCell requires animationProgress >= 1). Writing text
        // is exactly what starts one, so switch the animation off rather than race it.
        mock.terminal.settings().cursorMotionAnimationDuration = chrono::milliseconds(0);

        mock.writeToScreen("\033[2;1H"sv);
        mock.writeToScreen("xyz"sv);                       // line 1: uniform text
        mock.writeToScreen("\033[1;1H\033[31mab\033[m"sv); // line 0: mixed SGR -> per-cell path
        mock.writeToScreen(lastCellOfFirstLine);

        // The cursor must end ON the glyph that ends line 0, so renderCell leaves _prevHasCursor set.
        REQUIRE(mock.terminal.primaryScreen().cursor().position
                == CellLocation { .line = LineOffset(0), .column = cursorColumn });
        // Line 1 is selected in part, which sends it to renderTrivialLine's fallback; columns 0..2
        // are outside the selection, so nothing may recolor them.
        selectColumns(mock.terminal, LineOffset(1), ColumnOffset(3), ColumnOffset(4));

        mock.terminal.tick(ClockBase + chrono::seconds(1));
        mock.terminal.refreshRenderBuffer();
        auto const buf = mock.terminal.renderBuffer();
        auto result = std::vector<vtbackend::RGBColor> {};
        for (auto const& cell: buf.get().cells)
            if (cell.position.line == LineOffset(1) && cell.position.column < ColumnOffset(3))
                result.push_back(cell.attributes.backgroundColor);
        return result;
    };

    // Two columns ending the line, the cursor on its head; one column, the cursor on it.
    auto const afterWideGlyph = colorsOfSecondLine("\033[1;4H\u4E16"sv, ColumnOffset(3));
    auto const afterNarrowGlyph = colorsOfSecondLine("\033[1;5HZ"sv, ColumnOffset(4));

    REQUIRE(afterNarrowGlyph.size() == 3);
    CHECK(afterWideGlyph == afterNarrowGlyph);
}

TEST_CASE("Terminal.GraphemeCluster.aClusterLeavesTheBatchedPath", "[terminal][unicode]")
{
    // Why the layouts above can agree at all: a line the batched RenderLine path cannot express
    // must not take it. That path stores one codepoint per column, so a cell holding more than one
    // -- whether or not the cluster also grew wider -- has to leave it, exactly as a wide cell and
    // an image fragment already do. Stated on its own so a change that re-admits such a line fails
    // HERE, naming the cause, rather than as a layout mismatch above or only in the GUI.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) }, LineCount(0) };
    auto const& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("[\U0001F441\uFE0F]"sv);
    CHECK_FALSE(screen.grid().lineAt(LineOffset(0)).isTrivialBuffer());
    // The eye is two columns: its own, plus the one VS16 claimed for it.
    CHECK(screen.at(LineOffset(0), ColumnOffset(1)).width() == 2);

    // The same must hold when the cluster stays one column wide, which is the case that has no
    // continuation cell to break SGR uniformity on its behalf.
    mock.writeToScreen("\r\n\u03B1\u0301x"sv);
    CHECK(screen.at(LineOffset(1), ColumnOffset(0)).codepointCount() == 2);
    CHECK_FALSE(screen.grid().lineAt(LineOffset(1)).isTrivialBuffer());
    CHECK(screen.at(LineOffset(1), ColumnOffset(0)).width() == 1);
}

TEST_CASE("Terminal.hint_mode_accepts_labels_while_lock_keys_are_latched", "[terminal][locks]")
{
    for (auto const locks: LockCombinations)
    {
        INFO(std::format("lock modifiers {}", locks));
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(40) } };
        mock.writeToScreen("visit https://example.com now");

        mock.terminal.activateHintMode(
            vtbackend::HintModeRequest { .patterns = {}, .action = vtbackend::HintAction::Copy });
        REQUIRE(mock.terminal.isHintModeActive());
        REQUIRE(mock.terminal.hintMatches().size() == 1);

        auto const label = mock.terminal.hintMatches().at(0).label;
        REQUIRE(label.size() == 1);
        mock.resetReplyData();

        mock.sendCharEvent(static_cast<char32_t>(label[0]), locks);

        // The label character is consumed by hint mode, never forwarded to the application.
        CHECK(mock.replyData().empty());
        CHECK(!mock.terminal.isHintModeActive());
        CHECK(mock.clipboardData == "https://example.com");
    }
}

TEST_CASE("Terminal.DECUDK_fires_while_lock_keys_are_latched", "[terminal][locks]")
{
    for (auto const locks: LockCombinations)
    {
        INFO(std::format("lock modifiers {}", locks));
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };

        // Program F6 (UDK id 17) with "Hello" (hex: 48656C6C6F).
        mock.writeToScreen("\033P0;1|17/48656C6C6F\033\\");
        mock.terminal.flushInput();
        REQUIRE(mock.terminal.udkString(17).has_value());
        mock.resetReplyData();

        mock.sendKeyEvent(vtbackend::Key::F6, locks);

        CHECK(e(mock.replyData()) == e("Hello"));
    }
}

TEST_CASE("Terminal.DECMode.numberMappingRoundTrips", "[terminal]")
{
    using namespace vtbackend;

    // The DECMode<->number mapping is single-sourced in DECModeNumbers; SM/RM and DECRQM/DECRPM read it
    // in opposite directions. Guard that every row round-trips both ways, so a mode added to the table
    // is always both settable AND reportable -- never one without the other.
    for (auto const& [mode, number]: DECModeNumbers)
    {
        CHECK(toDECModeNum(mode) == number);
        CHECK(fromDECModeNum(number) == mode);
    }

    // A number with no row is simply unrecognised, in either query.
    CHECK(fromDECModeNum(38) == std::nullopt); // DECTEK, not implemented
    CHECK(fromDECModeNum(44) == std::nullopt); // margin bell, not implemented
    CHECK_FALSE(isValidDECMode(38));
}

TEST_CASE("TraceHandler.an_APC_body_waits_its_turn_like_every_other_sequence", "[terminal][trace]")
{
    // TraceHandler is a BUFFERING decorator: its whole job is to hold what the application sent, in
    // order, so the user can step through it. Forwarding an APC straight to the display let it
    // overtake everything still queued ahead of it -- so a kitty image was placed against the cursor
    // position that a queued CUP had not moved yet, and tracing a graphics problem showed behaviour
    // that never occurs outside trace mode.
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(8) } };
    auto trace = vtbackend::TraceHandler { mock.terminal };

    auto cup = vtbackend::Sequence {};
    cup.setCategory(vtbackend::FunctionCategory::CSI);
    cup.setFinalChar('H');
    trace.processSequence(cup);

    trace.processAPC("Ga=T,f=32,s=2,v=2;AAAA"sv);

    REQUIRE(trace.pendingSequences().size() == 2);
    CHECK(std::holds_alternative<vtbackend::Sequence>(trace.pendingSequences()[0]));

    // Queued, and BEHIND the sequence that preceded it -- not executed on arrival.
    auto const* apc =
        std::get_if<vtbackend::TraceHandler::ApplicationProgramCommand>(&trace.pendingSequences()[1]);
    REQUIRE(apc != nullptr);
    CHECK(apc->body == "Ga=T,f=32,s=2,v=2;AAAA");

    // The body is OWNED, not viewed: it outlives the parser buffer it arrived in, because nothing
    // runs it until the user steps the trace forward.
    CHECK(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment() == nullptr);
}

TEST_CASE("Terminal.focus.events_reach_the_pty_only_under_DECMode_1004", "[terminal][focus]")
{
    // Focus state is tracked unconditionally (the renderer needs it for the inactive cursor shape and
    // the inactive indicator status line), but the CSI I / CSI O notification is gated on DECMode 1004.
    // The return value reports whether bytes were actually produced.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    SECTION("mode off: focus is tracked, nothing reaches the PTY")
    {
        mock.discardPendingReplies();

        CHECK_FALSE(mock.terminal.sendFocusOutEvent());
        CHECK_FALSE(mock.terminal.focused());
        CHECK(mock.replyData().empty());

        CHECK_FALSE(mock.terminal.sendFocusInEvent());
        CHECK(mock.terminal.focused());
        CHECK(mock.replyData().empty());
    }

    SECTION("mode on: CSI O on focus loss, CSI I on focus gain")
    {
        mock.writeToScreen("\033[?1004h");
        mock.discardPendingReplies();

        CHECK(mock.terminal.sendFocusOutEvent());
        CHECK_FALSE(mock.terminal.focused());
        CHECK(e(mock.replyData()) == e("\033[O"s));

        mock.resetReplyData();

        CHECK(mock.terminal.sendFocusInEvent());
        CHECK(mock.terminal.focused());
        CHECK(e(mock.replyData()) == e("\033[I"s));
    }
}

TEST_CASE("Terminal.contains is exclusive on both axes", "[terminal]")
{
    // The column bound used to be `<=`, accepting a coordinate one cell past the right edge while the
    // line bound was correctly exclusive. Nothing called it yet, which is exactly why it was worth
    // fixing: the first caller to guard an index with it would have read out of bounds.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) } };
    auto const& terminal = mock.terminal;

    auto const at = [](int line, int column) {
        return CellLocation { .line = LineOffset(line), .column = ColumnOffset(column) };
    };

    CHECK(terminal.contains(at(0, 0)));
    CHECK(terminal.contains(at(2, 4))); // last addressable cell

    CHECK_FALSE(terminal.contains(at(0, 5)));  // one past the right edge
    CHECK_FALSE(terminal.contains(at(3, 0)));  // one past the bottom edge
    CHECK_FALSE(terminal.contains(at(0, -1))); // negative column
    CHECK_FALSE(terminal.contains(at(-1, 0))); // negative line (scrollback is not the page)

    // The screen-level predicate answers identically for the main page.
    CHECK(terminal.currentScreen().contains(at(2, 4)));
    CHECK_FALSE(terminal.currentScreen().contains(at(2, 5)));
}

TEST_CASE("Terminal.flushInput drops pending input on a fatal PTY write error", "[terminal][pty]")
{
    // The frontend retries flushInput() for as long as hasInput() reports bytes are still pending
    // (TerminalSession::flushInput posts itself again). That is right for backpressure and wrong for a
    // dead device: bytes that can never be delivered would keep hasInput() true forever, turning the
    // retry into an unbounded loop that logs one error per iteration -- the "Failed to write to SSH
    // channel" flood seen on a corrupted SSH session (#1495).
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    auto& pty = static_cast<vtpty::MockPty&>(mock.terminal.device());

    SECTION("EAGAIN is backpressure: the bytes stay pending for the next attempt")
    {
        pty.setWriteBehavior(vtpty::PtyWriteBehavior::FailAgain);
        mock.terminal.sendRawInput("hello"sv);

        CHECK(mock.terminal.hasInput());

        // Once the device accepts again, the very same bytes go out.
        pty.setWriteBehavior(vtpty::PtyWriteBehavior::Accept);
        mock.terminal.flushInput();
        CHECK_FALSE(mock.terminal.hasInput());
        CHECK(pty.stdinBuffer() == "hello");
    }

    SECTION("a fatal error drops them, so the caller's retry terminates")
    {
        pty.setWriteBehavior(vtpty::PtyWriteBehavior::FailFatally);
        mock.terminal.sendRawInput("hello"sv);

        CHECK_FALSE(mock.terminal.hasInput());

        // A second flush has nothing left to send, so no further error is produced.
        mock.terminal.flushInput();
        CHECK_FALSE(mock.terminal.hasInput());
        CHECK(pty.stdinBuffer().empty());
    }
}

TEST_CASE("Terminal.hint_mode_matches_a_url_wrapped_across_rows", "[terminal][hintmode]")
{
    // 20 columns forces "https://example.com/wrapped" to wrap after column 20. Scanning per
    // physical row would find at most the truncated head; the logical line yields the whole URL.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(10) };
    mock.writeToScreen("https://example.com/wrapped");
    mock.terminal.flushInput();

    REQUIRE(mock.terminal.currentScreen().isLineWrapped(LineOffset(1)));

    mock.terminal.activateHintMode(
        vtbackend::HintModeRequest { .patterns = {}, .action = vtbackend::HintAction::Copy });

    REQUIRE(mock.terminal.hintMatches().size() == 1);
    auto const& match = mock.terminal.hintMatches().at(0);
    CHECK(match.matchedText == "https://example.com/wrapped");
    CHECK(match.start == CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) });
    CHECK(match.end == CellLocation { .line = LineOffset(1), .column = ColumnOffset(6) });
}

TEST_CASE("Terminal.hint_mode_clips_a_logical_line_beyond_the_scan_budget", "[terminal][hintmode][links]")
{
    // A crash and budget test, not the freeze guard (that is HintModeHandler.FilepathIsLinearOnALongWordRun).
    // 64 KB is more than one scan budget: the logical line is cut into two scan texts, and the one word run
    // in each touches a clipped edge, so no match is offered rather than a truncated one.
    auto mock = MockTerm { PageSize { LineCount(50), ColumnCount(200) }, LineCount(400) };
    mock.terminal.setMode(vtbackend::DECMode::AutoWrap,
                          true); // one 64 KB logical line, as bench-headless sets up
    mock.writeToScreen(std::string(std::size_t { 64 } * 1024, 'a'));
    mock.terminal.flushInput();
    mock.terminal.activateHintMode(
        vtbackend::HintModeRequest { .patterns = {}, .action = vtbackend::HintAction::Copy });
    CHECK(mock.terminal.hintMatches().empty()); // each match touches a clipped edge and is dropped
}

TEST_CASE("Terminal.hint_mode_matches_a_cjk_url_whole_at_its_columns", "[terminal][hintmode][links]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(60) } };
    mock.writeToScreen("東京 https://例え.jp/パス。次へ");
    mock.terminal.flushInput();
    mock.terminal.activateHintMode(vtbackend::HintModeRequest {
        .patterns = { .table = vtbackend::LinkPatterns::builtins(), .names = { "url" }, .resolve = {} },
        .action = vtbackend::HintAction::Copy });
    REQUIRE(mock.terminal.hintMatches().size() == 1);
    auto const& match = mock.terminal.hintMatches().front();
    CHECK(match.matchedText == "https://例え.jp/パス");
    CHECK(match.start == CellLocation { .line = LineOffset(0), .column = ColumnOffset(5) });
    CHECK(match.end
          == CellLocation { .line = LineOffset(0), .column = ColumnOffset(24) }); // ス's second column
}

TEST_CASE("Terminal.hint_mode_counts_a_cluster_as_one_column", "[terminal][hintmode][links]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) } };
    mock.writeToScreen("e\xCC\x81 https://x.y/z");
    mock.terminal.flushInput();
    mock.terminal.activateHintMode(vtbackend::HintModeRequest {
        .patterns = { .table = vtbackend::LinkPatterns::builtins(), .names = { "url" }, .resolve = {} },
        .action = vtbackend::HintAction::Copy });
    REQUIRE(mock.terminal.hintMatches().size() == 1);
    CHECK(mock.terminal.hintMatches().front().start.column == ColumnOffset(2));
}

TEST_CASE("Terminal.hint_mode_counts_two_clusters_as_two_columns_at_both_ends", "[terminal][hintmode][links]")
{
    // Two `e` + U+0301 clusters take one column each, so the path begins at column 3 and ends at column 11;
    // the old scanner counted codepoints and put them at 5 and 13.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) } };
    mock.writeToScreen("e\xcc\x81"
                       "e\xcc\x81 src/a.txt");
    mock.terminal.flushInput();
    mock.terminal.activateHintMode(vtbackend::HintModeRequest {
        .patterns = { .table = vtbackend::LinkPatterns::builtins(), .names = { "filepath" }, .resolve = {} },
        .action = vtbackend::HintAction::Copy });
    REQUIRE(mock.terminal.hintMatches().size() == 1);
    auto const& match = mock.terminal.hintMatches().front();
    CHECK(match.matchedText == "src/a.txt");
    CHECK(match.start == CellLocation { .line = LineOffset(0), .column = ColumnOffset(3) });
    CHECK(match.end == CellLocation { .line = LineOffset(0), .column = ColumnOffset(11) });
}

TEST_CASE("Terminal.hint_mode_keeps_a_parenthesised_url_whole", "[terminal][hintmode][links]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(60) } };
    mock.writeToScreen("see https://en.wikipedia.org/wiki/Foo_(bar).");
    mock.terminal.flushInput();
    mock.terminal.activateHintMode(vtbackend::HintModeRequest {
        .patterns = { .table = vtbackend::LinkPatterns::builtins(), .names = { "url" }, .resolve = {} },
        .action = vtbackend::HintAction::Copy });
    REQUIRE(mock.terminal.hintMatches().size() == 1);
    CHECK(mock.terminal.hintMatches().front().matchedText == "https://en.wikipedia.org/wiki/Foo_(bar)");
}

TEST_CASE("Terminal.hint_mode_offers_no_slashless_tilde_word_as_a_path", "[terminal][hintmode][links]")
{
    // Only a leading `~/` names the home directory: `~10` is prose, like any other bare word.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) } };
    mock.writeToScreen("approx ~10 ms");
    mock.terminal.flushInput();
    mock.terminal.activateHintMode(vtbackend::HintModeRequest {
        .patterns = { .table = vtbackend::LinkPatterns::builtins(), .names = { "filepath" }, .resolve = {} },
        .action = vtbackend::HintAction::Copy });
    CHECK(mock.terminal.hintMatches().empty());
}

TEST_CASE("Terminal.hint_mode_resolves_absolute_and_home_paths_with_no_directory_known",
          "[terminal][hintmode][links]")
{
    // No OSC 7, no block: no base directory to resolve a relative path against. An absolute path and a `~/`
    // path do not need one, so they resolve through the injected filesystem all the same (§4.4); a path that
    // names nothing is offered as written, and a bare word is no path.
    auto const root = std::filesystem::temp_directory_path() / "contour-in-memory-only";
    auto const home = root / "home";
    auto const absolute = root / "proj" / "doc.txt";
    auto files = core::platform::testing::InMemoryFileSystem {};
    REQUIRE(files.createDirectories(home).has_value());
    REQUIRE(files.createDirectories(absolute.parent_path()).has_value());
    files.addFile(home / "notes.txt", "x");
    files.addFile(absolute, "x");

    auto events = vtbackend::Terminal::NullEvents {};
    auto const pageSize = PageSize { LineCount(3), ColumnCount(120) };
    auto settings = vtbackend::Settings {};
    settings.pageSize = pageSize;
    auto const environment = core::testing::FakeEnvironment { { { "HOME", core::pathToUtf8(home) } } };
    auto terminal = vtbackend::Terminal { events,
                                          environment,
                                          files,
                                          std::make_unique<vtpty::MockPty>(pageSize),
                                          settings,
                                          std::nullopt,
                                          vtbackend::TerminalClocks::system(),
                                          vtbackend::randomSemanticBlockTokens() };

    terminal.writeToScreen(std::format(
        "see {} and ~/notes.txt and {}/gone.txt", core::pathToUtf8(absolute), core::pathToUtf8(root)));
    terminal.activateHintMode(vtbackend::HintModeRequest {
        .patterns = { .table = vtbackend::LinkPatterns::builtins(), .names = { "filepath" }, .resolve = {} },
        .action = vtbackend::HintAction::Copy });

    auto const& matches = terminal.hintMatches();
    REQUIRE(matches.size() == 3); // "see" and "and" are bare words: dropped
    CHECK(matches[0].target.target == core::pathToUtf8(absolute.lexically_normal()));
    CHECK(matches[0].target.pathKind == vtbackend::PathKind::File);
    CHECK(matches[1].target.target == core::pathToUtf8((home / "notes.txt").lexically_normal()));
    CHECK(matches[1].target.pathKind == vtbackend::PathKind::File);
    CHECK(matches[1].matchedText == matches[1].target.target);
    CHECK(matches[2].target.target == core::pathToUtf8(root) + "/gone.txt"); // nothing resolves: as printed
    CHECK(matches[2].target.pathKind == vtbackend::PathKind::Missing);
}

TEST_CASE("Terminal.hint_mode_visible_scope_ignores_scrollback", "[terminal][hintmode]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) }, LineCount(20) };
    mock.writeToScreen("https://scrolled-away.example\r\n\r\n\r\n\r\n\r\n");
    mock.terminal.flushInput();

    // The URL is now above the page: it must not be offered under the default scope.
    REQUIRE(mock.terminal.currentScreen().historyLineCount() > LineCount(0));

    mock.terminal.activateHintMode(vtbackend::HintModeRequest {
        .patterns = {}, .action = vtbackend::HintAction::Copy, .scope = vtbackend::HintScope::Visible });

    CHECK(mock.terminal.hintMatches().empty());
}

TEST_CASE("Terminal.hint_mode_scrollback_scope_finds_history", "[terminal][hintmode]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) }, LineCount(20) };
    mock.writeToScreen("https://scrolled-away.example\r\n\r\n\r\n\r\n\r\n");
    mock.terminal.flushInput();

    mock.terminal.activateHintMode(vtbackend::HintModeRequest { .patterns = {},
                                                                .action = vtbackend::HintAction::Copy,
                                                                .scope = vtbackend::HintScope::Scrollback,
                                                                .scrollbackLimit = LineCount(1000) });

    REQUIRE(mock.terminal.hintMatches().size() == 1);
    auto const& match = mock.terminal.hintMatches().at(0);
    CHECK(match.matchedText == "https://scrolled-away.example");
    // A history row: a negative, scroll-invariant line offset.
    CHECK(match.start.line < LineOffset(0));
}

TEST_CASE("Terminal.hint_mode_scrollback_labels_survive_scrolling", "[terminal][hintmode]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) }, LineCount(20) };
    // The two URLs go in separate calls so that each line-break escape ends a literal rather than
    // sitting in front of a word: check-spelling splits identifiers on case boundaries, so an escape
    // glued ahead of a word forms a nonsense token and fails the spell gate.
    mock.writeToScreen("https://one.example\r\n");
    mock.writeToScreen("https://two.example\r\n\r\n\r\n\r\n\r\n");
    mock.terminal.flushInput();

    mock.terminal.activateHintMode(vtbackend::HintModeRequest { .patterns = {},
                                                                .action = vtbackend::HintAction::Copy,
                                                                .scope = vtbackend::HintScope::Scrollback,
                                                                .scrollbackLimit = LineCount(1000) });

    auto const before = mock.terminal.hintMatches();
    REQUIRE(before.size() == 2);

    // Scrolling reveals other rows' labels; it must not rename the ones already on screen, or a
    // label the user has read becomes wrong under their fingers.
    mock.terminal.viewport().scrollUp(LineCount(2));

    auto const after = mock.terminal.hintMatches();
    REQUIRE(after.size() == before.size());
    for (auto const i: std::views::iota(size_t { 0 }, after.size()))
    {
        CHECK(after[i].label == before[i].label);
        CHECK(after[i].start == before[i].start);
        CHECK(after[i].matchedText == before[i].matchedText);
    }
}

TEST_CASE("Terminal.hint_mode_visible_scope_scans_again_on_scroll", "[terminal][hintmode]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) }, LineCount(20) };
    mock.writeToScreen("https://scrolled-away.example\r\n\r\n\r\n\r\n\r\n");
    mock.terminal.flushInput();

    mock.terminal.activateHintMode(vtbackend::HintModeRequest {
        .patterns = {}, .action = vtbackend::HintAction::Copy, .scope = vtbackend::HintScope::Visible });
    REQUIRE(mock.terminal.hintMatches().empty());

    // The visible scope's scan region *is* the viewport, so scrolling the URL back into view must
    // surface it. Scroll to the very top rather than by a hardcoded count: the URL is on the oldest
    // history row, wherever the scroll accounting puts it.
    REQUIRE(mock.terminal.viewport().scrollToTop());

    REQUIRE(mock.terminal.hintMatches().size() == 1);
    CHECK(mock.terminal.hintMatches().at(0).matchedText == "https://scrolled-away.example");
}

TEST_CASE("Terminal.hint_mode_overlay_draws_labels_into_the_render_buffer", "[terminal][hintmode]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) }, LineCount(10) };
    mock.writeToScreen("visit https://example.com now");
    mock.terminal.flushInput();

    mock.terminal.activateHintMode(
        vtbackend::HintModeRequest { .patterns = {}, .action = vtbackend::HintAction::Copy });
    REQUIRE(mock.terminal.hintMatches().size() == 1);
    auto const& match = mock.terminal.hintMatches().at(0);
    REQUIRE(match.label == "a");

    mock.terminal.refreshRenderBuffer();
    auto const buf = mock.terminal.renderBuffer();

    // The label replaces the match's first cell.
    auto const labelCell =
        std::ranges::find_if(buf.get().cells, [&](auto const& cell) { return cell.position == match.start; });
    REQUIRE(labelCell != buf.get().cells.end());
    CHECK(labelCell->codepoints == U"a");
}

TEST_CASE("Terminal.hint_mode_overlay_reaches_a_line_without_the_cursor", "[terminal][hintmode]")
{
    // A plain, uniformly-coloured line with no cursor and no selection is rendered through the
    // trivial-line fast path, which emits one RenderLine instead of per-cell RenderCells. The hint
    // overlay only rewrites cells, so the label must not be lost to that path.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) }, LineCount(10) };
    mock.writeToScreen("visit https://example.com now\r\n");
    mock.writeToScreen("second line holds the cursor");
    mock.terminal.flushInput();

    mock.terminal.activateHintMode(
        vtbackend::HintModeRequest { .patterns = {}, .action = vtbackend::HintAction::Copy });
    REQUIRE(mock.terminal.hintMatches().size() == 1);
    auto const& match = mock.terminal.hintMatches().at(0);
    REQUIRE(match.start.line == LineOffset(0));

    mock.terminal.refreshRenderBuffer();
    auto const buf = mock.terminal.renderBuffer();

    auto const labelCell =
        std::ranges::find_if(buf.get().cells, [&](auto const& cell) { return cell.position == match.start; });
    REQUIRE(labelCell != buf.get().cells.end());
    CHECK(labelCell->codepoints == U"a");
}

TEST_CASE("Terminal.hint_mode_overlay_highlights_a_wrapped_match_on_both_rows", "[terminal][hintmode]")
{
    // The overlay's match body is a run of text, not a rectangle: a wrapped match must be
    // highlighted on its continuation row too, and only up to the column it actually ends on.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) }, LineCount(10) };
    mock.writeToScreen("https://example.com/wrapped");
    mock.terminal.flushInput();

    mock.terminal.activateHintMode(
        vtbackend::HintModeRequest { .patterns = {}, .action = vtbackend::HintAction::Copy });
    REQUIRE(mock.terminal.hintMatches().size() == 1);
    auto const& match = mock.terminal.hintMatches().at(0);
    REQUIRE(match.start.line == LineOffset(0));
    REQUIRE(match.end.line == LineOffset(1));

    mock.terminal.refreshRenderBuffer();
    auto const buf = mock.terminal.renderBuffer();

    auto const backgroundAt = [&](CellLocation pos) -> std::optional<vtbackend::RGBColor> {
        auto const it =
            std::ranges::find_if(buf.get().cells, [&](auto const& cell) { return cell.position == pos; });
        if (it == buf.get().cells.end())
            return std::nullopt;
        return it->attributes.backgroundColor;
    };

    // Compared within the continuation row, so cursorline colouring (the Vi cursor sits on the
    // last written row) cannot confound it: the cells the match covers are highlighted, and the
    // first cell past its end is not.
    auto const firstInside = backgroundAt(CellLocation { .line = match.end.line, .column = ColumnOffset(0) });
    auto const lastInside = backgroundAt(CellLocation { .line = match.end.line, .column = match.end.column });
    auto const firstOutside =
        backgroundAt(CellLocation { .line = match.end.line, .column = match.end.column + ColumnOffset(1) });

    REQUIRE(firstInside.has_value());
    REQUIRE(lastInside.has_value());
    REQUIRE(firstOutside.has_value());
    CHECK(*firstInside == *lastInside);   // the whole run on this row is highlighted
    CHECK(*firstOutside != *firstInside); // and the highlight stops where the match does
}

TEST_CASE("Terminal.hint_mode_acts_on_the_printed_text_and_on_a_paths_resolved_form", "[terminal][hintmode]")
{
    // Copy, Paste and Select take a URL and a custom match as printed: a template's expansion is what Open
    // opens, never what is copied. A path takes the absolute form it resolved to, as it always has.
#ifdef _WIN32
    auto const file = std::filesystem::path { "C:/hint-copy/a.cpp" };
#else
    auto const file = std::filesystem::path { "/hint-copy/a.cpp" };
#endif
    auto term = vtbackend::test::DetectingTerm { PageSize { LineCount(3), ColumnCount(80) } };
    term.files.addFile(file, "x");
    auto& mock = term.mock;
    mock.writeToScreen(std::format("J-1 https://x.y/z {}", file.generic_string()));
    mock.terminal.flushInput();
    auto const specs =
        std::array { vtbackend::LinkPatternSpec { .name = "jira",
                                                  .regex = R"(\bJ-\d+\b)",
                                                  .activation = vtbackend::LinkActivation::Clickable,
                                                  .urlTemplate = "https://jira.example/{0}" } };
    mock.terminal.activateHintMode(vtbackend::HintModeRequest {
        .patterns = vtbackend::HintPatternSet { .table = std::make_shared<vtbackend::LinkPatterns const>(
                                                    vtbackend::LinkPatterns::compile(specs)),
                                                .names = {},
                                                .resolve = {} },
        .action = vtbackend::HintAction::Copy });
    auto const& matches = mock.terminal.hintMatches();
    auto const at = [&](int column) -> vtbackend::HintMatch const& {
        auto const found =
            std::ranges::find(matches, ColumnOffset(column), [](vtbackend::HintMatch const& match) {
                return match.start.column;
            });
        REQUIRE(found != matches.end());
        return *found;
    };

    CHECK(at(0).matchedText == "J-1");
    CHECK(at(0).target.target == "https://jira.example/J-1");
    CHECK(at(4).matchedText == "https://x.y/z");
    CHECK(core::pathFromUtf8(at(18).matchedText) == file.lexically_normal());
    CHECK(at(18).target.kind == vtbackend::LinkKind::Path);
}

TEST_CASE("Terminal.hint_mode_copies_but_never_opens_a_scheme_the_printed_text_chose", "[terminal][hintmode]")
{
    // §7.6: printed text whose scheme no link may open opens nothing through hint mode either; it can still
    // be copied, as printed.
    auto const specs =
        std::array { vtbackend::LinkPatternSpec { .name = "doc",
                                                  .regex = R"(\S+\.pdf)",
                                                  .activation = vtbackend::LinkActivation::Clickable,
                                                  .urlTemplate = std::nullopt } };
    auto const table =
        std::make_shared<vtbackend::LinkPatterns const>(vtbackend::LinkPatterns::compile(specs));
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) } };
    mock.writeToScreen("smb://evil/x.pdf");
    mock.terminal.flushInput();
    auto const pick = [&](vtbackend::HintAction action) {
        mock.terminal.activateHintMode(vtbackend::HintModeRequest {
            .patterns = vtbackend::HintPatternSet { .table = table, .names = { "doc" }, .resolve = {} },
            .action = action });
        REQUIRE(mock.terminal.hintMatches().size() == 1);
        mock.sendCharEvent(U'a');
    };

    pick(vtbackend::HintAction::Open);
    CHECK_FALSE(mock.openedLink.has_value());
    CHECK(mock.openedDocument.empty());

    pick(vtbackend::HintAction::Copy);
    CHECK(mock.clipboardData == "smb://evil/x.pdf");
}

TEST_CASE("Terminal.hint_mode_dispatches_each_action", "[terminal][hintmode]")
{
    // Every HintAction routed through HintModeExecutor::onHintSelected, so a new one cannot be
    // added without a home here.
    auto const activate = [](auto& mock, vtbackend::HintAction action) {
        mock.terminal.activateHintMode(vtbackend::HintModeRequest { .patterns = {}, .action = action });
        REQUIRE(mock.terminal.hintMatches().size() == 1);
        mock.sendCharEvent(U'a');
    };

    SECTION("Copy")
    {
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) } };
        mock.writeToScreen("go https://example.com now");
        mock.terminal.flushInput();
        activate(mock, vtbackend::HintAction::Copy);
        CHECK(mock.clipboardData == "https://example.com");
    }

    SECTION("Open")
    {
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) } };
        mock.writeToScreen("go https://example.com now");
        mock.terminal.flushInput();
        activate(mock, vtbackend::HintAction::Open);
        // As Ctrl+click opens it: through openLink(), whose frontend applies the follow gesture's refusals.
        REQUIRE(mock.openedLink.has_value());
        CHECK(mock.openedLink->kind == vtbackend::LinkKind::Url);
        CHECK(mock.openedLink->target == "https://example.com");
        CHECK(mock.openedDocument.empty());
    }

    SECTION("Open, a path that names a file: as Ctrl+click opens it, at its location")
    {
        // In the filesystem the terminal was given, which hint mode looks paths up in: an in-memory one.
#ifdef _WIN32
        auto const file = std::filesystem::path { "C:/hint-open/a.cpp" };
#else
        auto const file = std::filesystem::path { "/hint-open/a.cpp" };
#endif
        auto term = vtbackend::test::DetectingTerm { PageSize { LineCount(3), ColumnCount(80) } };
        term.files.addFile(file, "x");
        auto& mock = term.mock;
        mock.writeToScreen(std::format("see {}:12:5 now", file.generic_string()));
        mock.terminal.flushInput();
        mock.terminal.activateHintMode(
            vtbackend::HintModeRequest { .patterns = {}, .action = vtbackend::HintAction::Open });
        REQUIRE(mock.terminal.hintMatches().size() == 1);
        mock.sendCharEvent(U'a');
        REQUIRE(mock.openedLink.has_value());
        CHECK(mock.openedLink->kind == vtbackend::LinkKind::Path);
        CHECK(core::pathFromUtf8(mock.openedLink->target) == file.lexically_normal());
        CHECK(mock.openedLink->pathKind == vtbackend::PathKind::File);
        CHECK(mock.openedLink->location == vtbackend::LinkLocation { .line = 12, .column = 5 });
        CHECK(mock.openedDocument.empty());
    }

    SECTION("Paste")
    {
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) } };
        mock.writeToScreen("go https://example.com now");
        mock.terminal.flushInput();
        mock.resetReplyData();
        activate(mock, vtbackend::HintAction::Paste);
        CHECK(mock.replyData() == "https://example.com");
    }

    SECTION("Paste, under bracketed paste")
    {
        // A hint is pasted, as the clipboard is: the application sees it as one paste, not as keys.
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) } };
        mock.writeToScreen("\033[?2004hgo https://example.com now");
        mock.terminal.flushInput();
        mock.resetReplyData();
        activate(mock, vtbackend::HintAction::Paste);
        CHECK(mock.replyData() == "\033[200~https://example.com\033[201~");
    }

    SECTION("CopyAndPaste")
    {
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) } };
        mock.writeToScreen("go https://example.com now");
        mock.terminal.flushInput();
        mock.resetReplyData();
        activate(mock, vtbackend::HintAction::CopyAndPaste);
        CHECK(mock.clipboardData == "https://example.com");
        CHECK(mock.replyData() == "https://example.com");
    }

    SECTION("Select")
    {
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) } };
        mock.writeToScreen("go https://example.com now");
        mock.terminal.flushInput();
        activate(mock, vtbackend::HintAction::Select);

        // Visual mode with the match pre-selected, ready for manual adjustment before yanking.
        CHECK(mock.terminal.inputHandler().mode() == vtbackend::ViMode::Visual);
        REQUIRE(mock.terminal.selector() != nullptr);
        CHECK(mock.terminal.selector()->from()
              == CellLocation { .line = LineOffset(0), .column = ColumnOffset(3) });
    }
}

TEST_CASE("Terminal.pastesUnchanged_asks_about_the_way_the_paste_would_arrive", "[terminal]")
{
    // What a hint's path and a command line run again are both held to: a text that would lose a character
    // is not pasted at all, and which characters it loses depends on whether the paste is bracketed.
    auto mc = MockTerm { PageSize { LineCount(2), ColumnCount(20) } };
    CHECK(mc.terminal.pastesUnchanged("ls -l ~/src"));

    SECTION("typed, a shell reads TAB and a line break as keys")
    {
        CHECK_FALSE(mc.terminal.pastesUnchanged("a\tb"));
        CHECK_FALSE(mc.terminal.pastesUnchanged("ls\nrm -rf ~"));
    }

    SECTION("bracketed, they are text; ESC is still taken out")
    {
        mc.writeToScreen("\033[?2004h");
        CHECK(mc.terminal.pastesUnchanged("a\tb"));
        CHECK(mc.terminal.pastesUnchanged("ls\nrm -rf ~"));
        CHECK_FALSE(mc.terminal.pastesUnchanged("ls\033[201~rm -rf ~"));
    }
}

TEST_CASE("Terminal.hint_mode_validates_and_resolves_paths_against_the_working_directory",
          "[terminal][hintmode]")
{
    // With a working directory known (OSC 7), the filepath pattern broadens to bare names and a
    // filesystem-existence validator keeps only the ones that are really there; the match is then
    // transformed to an absolute path so Copy and Open get something usable. The validator is
    // memoized per activation, so a name repeated across rows is stat()ed once.
    namespace fs = std::filesystem;

    auto const tmpDir = core::testing::ScopedTempDir { "contour-hint-cwd" };
    auto const& tmpRoot = tmpDir.path();
    fs::create_directories(tmpRoot);
    {
        auto file = std::ofstream(tmpRoot / "Makefile");
        file << "all:\n";
    }

    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(60) }, LineCount(10) };
    mock.terminal.setCurrentWorkingDirectory("file://" + tmpRoot.generic_string());

    // "Makefile" exists and must be offered; "Nonexistent" does not and must be filtered out. The
    // repeat is what the memo cache collapses.
    mock.writeToScreen("edit Makefile please\r\n");
    mock.writeToScreen("also Makefile again\r\n");
    mock.writeToScreen("but Nonexistent is absent");
    mock.terminal.flushInput();

    mock.terminal.activateHintMode(
        vtbackend::HintModeRequest { .patterns = {}, .action = vtbackend::HintAction::Copy });

    auto const& matches = mock.terminal.hintMatches();
    auto const expected = (tmpRoot / "Makefile").lexically_normal().string(); // as the mouse spells it
    REQUIRE(matches.size() == 2); // the two Makefile mentions, not the missing name
    CHECK(matches[0].matchedText == expected);
    CHECK(matches[1].matchedText == expected);
    CHECK(std::ranges::none_of(matches, [](auto const& m) { return m.matchedText.contains("Nonexistent"); }));
}

TEST_CASE("Terminal.hint_mode_resolves_a_path_as_the_mouse_does", "[terminal][hintmode]")
{
    // Hint mode and the Ctrl+mouse lookup resolve the same text against the same directory, so they name
    // the same file, spelt the same way: normalised, without the "./" or ".." the text was written with.
    namespace fs = std::filesystem;

    auto const tmpDir = core::testing::ScopedTempDir { "contour-hint-mouse" };
    auto const& tmpRoot = tmpDir.path();
    fs::create_directories(tmpRoot / "src");
    std::ofstream(tmpRoot / "Makefile") << "all:\n";

    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(60) }, LineCount(10) };
    mock.terminal.setCurrentWorkingDirectory("file://" + tmpRoot.generic_string());
    mock.writeToScreen("edit src/../Makefile please");
    mock.terminal.flushInput();

    mock.terminal.sendMouseMoveEvent(Modifier::None,
                                     CellLocation { .line = LineOffset(0), .column = ColumnOffset(8) },
                                     vtbackend::PixelCoordinate {},
                                     false);
    auto const byMouse = followedPathUnderPointer(mock.terminal, core::platform::NativeFileSystem::instance())
                             .value_or(std::nullopt);
    REQUIRE(byMouse == (tmpRoot / "Makefile").lexically_normal().string());

    mock.terminal.activateHintMode(
        vtbackend::HintModeRequest { .patterns = {}, .action = vtbackend::HintAction::Copy });
    auto const& matches = mock.terminal.hintMatches();
    REQUIRE(matches.size() == 1);
    CHECK(matches[0].matchedText == *byMouse);
}

TEST_CASE("Terminal.hint_mode_resolves_no_path_in_another_hosts_working_directory", "[terminal][hintmode]")
{
    // file://otherhost/dir names otherhost's directory. A directory of that name here is another one, so its
    // files are not offered as hints.
    namespace fs = std::filesystem;

    auto const tmpDir = core::testing::ScopedTempDir { "contour-hint-remote" };
    auto const& tmpRoot = tmpDir.path();
    fs::create_directories(tmpRoot);
    std::ofstream(tmpRoot / "Makefile") << "all:\n";

    auto const generic = tmpRoot.generic_string();
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(60) }, LineCount(10) };
    mock.terminal.setCurrentWorkingDirectory("file://otherhost.invalid"
                                             + (generic.starts_with('/') ? "" : "/"s) + generic);
    mock.writeToScreen("edit Makefile please");
    mock.terminal.flushInput();

    mock.terminal.activateHintMode(
        vtbackend::HintModeRequest { .patterns = {}, .action = vtbackend::HintAction::Copy });

    CHECK(mock.terminal.hintMatches().empty());
}

TEST_CASE("Terminal.hint_mode_resolves_paths_in_a_working_directory_beyond_ascii", "[terminal][hintmode]")
{
    // OSC 7 reports the directory in UTF-8. Read as a narrow path, Windows took it in the ANSI code page,
    // found none of its files, and offered no hint for them.
    namespace fs = std::filesystem;

    auto const tmpDir = core::testing::ScopedTempDir { "contour-hint-utf8" };
    auto const& tmpRoot = tmpDir.path();
    auto const directory = tmpRoot / vtpty::testing::NonAsciiName;
    fs::create_directories(directory);
    std::ofstream(directory / "Makefile") << "all:\n";

    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(60) }, LineCount(10) };
    mock.terminal.setCurrentWorkingDirectory(fileUrlOf(directory));
    mock.writeToScreen("edit Makefile please");
    mock.terminal.flushInput();

    mock.terminal.activateHintMode(
        vtbackend::HintModeRequest { .patterns = {}, .action = vtbackend::HintAction::Copy });

    auto const& matches = mock.terminal.hintMatches();
    REQUIRE(matches.size() == 1);
    CHECK(core::pathFromUtf8(matches[0].matchedText) == (directory / "Makefile").lexically_normal());
}

#ifndef _WIN32 // no control character is legal in a Windows file name
TEST_CASE("Terminal.hint_mode_types_no_path_a_shell_would_read_as_keys", "[terminal][hintmode]")
{
    // A directory name may hold ESC, BEL or a line feed, and OSC 7 reports it percent-encoded, which the
    // terminal decodes. Hint mode resolves a bare file name against that directory: typed raw, the path
    // would reach the shell as keys. It is still copied, but it is not typed.
    namespace fs = std::filesystem;

    auto const tmpDir = core::testing::ScopedTempDir { "contour-hint-keys" };
    auto const& tmpRoot = tmpDir.path();

    struct Typed
    {
        std::string input;     ///< What reached the PTY.
        std::string clipboard; ///< What was copied.
    };
    auto bracketed = std::string_view {}; // what the shell turned on before the hint was picked
    auto const pickIn = [&](std::string const& directoryName, vtbackend::HintAction action) {
        auto const directory = tmpRoot / directoryName;
        fs::create_directories(directory);
        std::ofstream(directory / "notes.txt") << "x\n";

        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) } };
        mock.writeToScreen(bracketed);
        mock.writeToScreen(
            std::format("\033]7;file://{}\033\\", core::platform::percentEncodeUriPath(directory.string())));
        mock.writeToScreen("see notes.txt");
        mock.terminal.flushInput();
        mock.resetReplyData();
        mock.terminal.activateHintMode(vtbackend::HintModeRequest { .patterns = {}, .action = action });
        REQUIRE(mock.terminal.hintMatches().size() == 1);
        mock.sendCharEvent(U'a');
        return Typed { .input = mock.replyData(), .clipboard = mock.clipboardData };
    };

    SECTION("a plain directory: the path is typed")
    {
        auto const expected = (tmpRoot / "plain" / "notes.txt").string();
        CHECK(pickIn("plain", vtbackend::HintAction::Paste).input == expected);
    }

    SECTION("ESC and BEL in the directory: copied, not typed")
    {
        auto const name = std::string { "esc\x1b]2;x\x07" };
        auto const picked = pickIn(name, vtbackend::HintAction::CopyAndPaste);
        CHECK(picked.input.empty());
        CHECK(picked.clipboard == (tmpRoot / name / "notes.txt").string());
    }

    SECTION("a line feed in the directory: not typed")
    {
        CHECK(pickIn("lf\nrm -rf x", vtbackend::HintAction::Paste).input.empty());
    }

    SECTION("under bracketed paste, a line feed in the directory is pasted as text")
    {
        // Inside the brackets the shell inserts a line feed rather than running what precedes it.
        bracketed = "\033[?2004h";
        auto const expected = (tmpRoot / "lf\nrm -rf x" / "notes.txt").string();
        CHECK(pickIn("lf\nrm -rf x", vtbackend::HintAction::Paste).input
              == std::format("\033[200~{}\033[201~", expected));
    }

    SECTION("under bracketed paste, ESC in the directory: still not typed")
    {
        // The paste would drop the ESC, and a path with a character taken out names another file.
        bracketed = "\033[?2004h";
        CHECK(pickIn("esc\x1b]2;x\x07", vtbackend::HintAction::Paste).input.empty());
    }
}
#endif

TEST_CASE("Terminal.hint_mode_extends_the_scan_past_the_viewport_to_finish_a_wrapped_line",
          "[terminal][hintmode]")
{
    // A one-row viewport so a two-row wrapped match can only ever straddle its edge.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(20) }, LineCount(10) };
    mock.writeToScreen("https://example.com/wrapped");
    mock.writeToScreen("\r\n\r\n\r\n");
    mock.terminal.flushInput();

    auto const history = unbox<int>(mock.terminal.currentScreen().historyLineCount());
    REQUIRE(history >= 3);

    // Grid rows: the URL head, then its continuation, then the blank rows the line feeds added.
    auto const head = LineOffset(-history);
    REQUIRE_FALSE(mock.terminal.currentScreen().isLineWrapped(head));
    REQUIRE(mock.terminal.currentScreen().isLineWrapped(head + LineOffset(1)));

    auto const activateAt = [&](int scrollOffset) {
        mock.terminal.viewport().scrollTo(vtbackend::ScrollOffset(scrollOffset));
        mock.terminal.activateHintMode(
            vtbackend::HintModeRequest { .patterns = {}, .action = vtbackend::HintAction::Copy });
    };

    SECTION("the head is visible: the scan reaches down for the tail and yields the whole URL")
    {
        activateAt(history);

        REQUIRE(mock.terminal.hintMatches().size() == 1);
        auto const& match = mock.terminal.hintMatches().at(0);
        CHECK(match.matchedText == "https://example.com/wrapped");
        CHECK(match.start.line == head);
        CHECK(match.end.line == head + LineOffset(1)); // the tail is off screen, but it is matched
    }

    SECTION("only the tail is visible: the hint is not offered, because its label cannot be drawn")
    {
        activateAt(history - 1);

        CHECK(mock.terminal.hintMatches().empty());
    }
}

TEST_CASE("Terminal.hint_mode_maps_columns_past_a_wide_character", "[terminal][hintmode]")
{
    // A double-width glyph before the URL must not shift the label/highlight left: the scan text is
    // column-aligned (the wide character's continuation cell becomes a space), so the URL's grid
    // column accounts for the two cells the glyph occupies.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) }, LineCount(10) };
    mock.writeToScreen("\xe4\xb8\xad https://example.com"); // 中 (2 columns) + space + URL
    mock.terminal.flushInput();

    // 中 really is double-width here, otherwise the column arithmetic below would not be exercised.
    REQUIRE(mock.terminal.currentScreen().cellWidthAt(
                CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) })
            == 2);

    mock.terminal.activateHintMode(
        vtbackend::HintModeRequest { .patterns = {}, .action = vtbackend::HintAction::Copy });

    REQUIRE(mock.terminal.hintMatches().size() == 1);
    auto const& match = mock.terminal.hintMatches().at(0);
    CHECK(match.matchedText == "https://example.com");
    // 中 occupies columns 0-1 and the space column 2, so the URL starts at column 3 — not column 2,
    // which is where a codepoint-counted (wide-character-collapsing) mapping would wrongly place it.
    CHECK(match.start == CellLocation { .line = LineOffset(0), .column = ColumnOffset(3) });
}

TEST_CASE("Terminal.hint_mode_survives_a_negative_scrollback_limit", "[terminal][hintmode]")
{
    // A negative hint_scrollback_lines (an out-of-range config value) must not push the labelable
    // range past the grid and underflow the row reservation, which would terminate the terminal.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) }, LineCount(20) };
    mock.writeToScreen("https://one.example\r\n\r\n\r\n\r\n\r\n");
    mock.terminal.flushInput();

    mock.terminal.activateHintMode(
        vtbackend::HintModeRequest { .patterns = {},
                                     .action = vtbackend::HintAction::Copy,
                                     .scope = vtbackend::HintScope::Scrollback,
                                     .scrollbackLimit = vtbackend::LineCount(-100) });

    // It did not crash; the negative limit clamped to "no history", so the scrolled-away URL is out
    // of range and nothing is offered.
    CHECK(mock.terminal.isHintModeActive());
    CHECK(mock.terminal.hintMatches().empty());
}

TEST_CASE("Terminal.hint_mode_matches_track_content_scrolling", "[terminal][hintmode]")
{
    // A hint session's positions are grid-absolute. When new output scrolls the grid while the
    // session is open, each match must move up with its own text rather than staying at a now-stale
    // row that other text has scrolled into.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) }, LineCount(50) };
    mock.writeToScreen("https://tracked.example\r\n");
    mock.terminal.flushInput();

    mock.terminal.activateHintMode(
        vtbackend::HintModeRequest { .patterns = {},
                                     .action = vtbackend::HintAction::Copy,
                                     .scope = vtbackend::HintScope::Scrollback,
                                     .scrollbackLimit = vtbackend::LineCount(1000) });
    REQUIRE(mock.terminal.hintMatches().size() == 1);
    auto const before = mock.terminal.hintMatches().at(0);

    // Three more lines scroll the grid up by two; the match keeps its label and text but moves up.
    // Each line goes in its own write so a line-break escape ends its literal rather than gluing to
    // the next word (which check-spelling would read as a nonsense token).
    mock.writeToScreen("more\r\n");
    mock.writeToScreen("lines\r\n");
    mock.writeToScreen("here\r\n");
    mock.terminal.flushInput();

    REQUIRE(mock.terminal.hintMatches().size() == 1);
    auto const after = mock.terminal.hintMatches().at(0);
    CHECK(after.matchedText == before.matchedText);
    CHECK(after.label == before.label);
    CHECK(after.start.line < before.start.line); // followed its text up into history
}

TEST_CASE("Terminal.hint_mode_overlay_wraps_a_label_past_the_row_edge", "[terminal][hintmode]")
{
    // 27 matches force two-character labels. One URL starts at the very last column, so its label
    // cannot fit on that row: the second character must appear on the wrapped continuation row
    // rather than being silently dropped.
    auto urlPatterns = vtbackend::HintPatternSet { .table = vtbackend::LinkPatterns::builtins(),
                                                   .names = { "url" },
                                                   .resolve = {} };

    auto mock = MockTerm { PageSize { LineCount(30), ColumnCount(20) }, LineCount(50) };
    for (auto const i: std::views::iota(0, 26))
        mock.writeToScreen(std::format("https://s{}.io\r\n", i));
    // 19 blank filler columns then the URL, so its match starts at column 19 (the last column).
    mock.writeToScreen(std::string(19, ' ') + "https://edge.example");
    mock.terminal.flushInput();

    mock.terminal.activateHintMode(vtbackend::HintModeRequest { .patterns = std::move(urlPatterns),
                                                                .action = vtbackend::HintAction::Copy });

    auto const& matches = mock.terminal.hintMatches();
    REQUIRE(matches.size() == 27);
    auto const edge =
        std::ranges::find_if(matches, [](auto const& m) { return m.matchedText == "https://edge.example"; });
    REQUIRE(edge != matches.end());
    REQUIRE(edge->label.size() == 2);
    REQUIRE(edge->start.column == ColumnOffset(19)); // the last column of a 20-wide page

    mock.terminal.refreshRenderBuffer();
    auto const buf = mock.terminal.renderBuffer();
    auto const codepointsAt = [&](CellLocation pos) -> std::optional<std::u32string> {
        auto const it =
            std::ranges::find_if(buf.get().cells, [&](auto const& cell) { return cell.position == pos; });
        if (it == buf.get().cells.end())
            return std::nullopt;
        return it->codepoints;
    };

    // First label character on the start cell, second wrapped onto the next row's first column.
    auto const wrapped = CellLocation { .line = edge->start.line + LineOffset(1), .column = ColumnOffset(0) };
    CHECK(codepointsAt(edge->start) == std::u32string(1, static_cast<char32_t>(edge->label[0])));
    CHECK(codepointsAt(wrapped) == std::u32string(1, static_cast<char32_t>(edge->label[1])));
}

TEST_CASE("Terminal reports the identity its settings named", "[terminal]")
{
    // Settings::terminalId used to reach nothing: _terminalId was default-initialized to VT525 and
    // written only by setTerminalId(), which the GUI calls from configureTerminal(). Any terminal
    // nobody reconfigures after construction -- every daemon-hosted session, and vtconformance's --
    // therefore reported VT525 whatever its profile said, and DA1/DA2/DECSCL answered accordingly.
    auto events = vtbackend::Terminal::NullEvents {};
    auto const pageSize = vtbackend::PageSize { vtbackend::LineCount(5), vtbackend::ColumnCount(20) };

    auto settings = vtbackend::Settings {};
    settings.pageSize = pageSize;
    settings.terminalId = vtbackend::VTType::VT340;

    auto const environment = core::testing::FakeEnvironment {};
    auto terminal = vtbackend::Terminal { events,
                                          environment,
                                          core::platform::NativeFileSystem::instance(),
                                          std::make_unique<vtpty::MockPty>(pageSize),
                                          settings,
                                          std::nullopt,
                                          vtbackend::TerminalClocks::system(),
                                          vtbackend::randomSemanticBlockTokens() };

    CHECK(terminal.terminalId() == vtbackend::VTType::VT340);
    // The operating level follows the identity at birth, as setTerminalId() also makes it do -- a
    // terminal must not start out claiming conformance above the model it reports.
    CHECK(terminal.operatingLevel() == vtbackend::VTType::VT340);
    CHECK(vtbackend::conformanceLevelOf(terminal.operatingLevel()) == 3);
}

TEST_CASE("a terminal constructed below VT525 narrows its sequence table too", "[terminal][conformance]")
{
    // Seeding _operatingLevel from Settings::terminalId without ALSO resetting the dispatch table
    // leaves the conformance gate and the table disagreeing for any terminal nobody reconfigures
    // afterwards — which is every daemon-hosted session (vthost::SessionHost builds each Terminal
    // straight from the profile's settings and never calls setTerminalId, unlike the GUI's
    // configureTerminal()). Such a session answered DA1 as a VT220 and refused the VT420 DEC modes,
    // yet still executed VT420-only sequences off the full VT525 table.
    auto const hasFunction = [](vtbackend::Terminal const& terminal, vtbackend::Function const& wanted) {
        return std::ranges::any_of(terminal.activeSequences(),
                                   [&](vtbackend::Function const& fn) { return fn.id() == wanted.id(); });
    };

    auto events = vtbackend::Terminal::NullEvents {};
    auto const pageSize = vtbackend::PageSize { vtbackend::LineCount(5), vtbackend::ColumnCount(20) };

    auto const environment = core::testing::FakeEnvironment {};
    auto makeTerminal = [&](vtbackend::VTType id) {
        auto settings = vtbackend::Settings {};
        settings.pageSize = pageSize;
        settings.terminalId = id;
        return vtbackend::Terminal { events,
                                     environment,
                                     core::platform::NativeFileSystem::instance(),
                                     std::make_unique<vtpty::MockPty>(pageSize),
                                     settings,
                                     std::nullopt,
                                     vtbackend::TerminalClocks::system(),
                                     vtbackend::randomSemanticBlockTokens() };
    };

    SECTION("a VT220 identity excludes the VT420-only sequences")
    {
        auto terminal = makeTerminal(vtbackend::VTType::VT220);
        CHECK(terminal.operatingLevel() == vtbackend::VTType::VT220);
        CHECK_FALSE(hasFunction(terminal, vtbackend::DECFRA)); // VT420
        CHECK(hasFunction(terminal, vtbackend::DECSCL));       // never gated: it SETS the level
        // The table the constructor produced must be the one setTerminalId would have produced —
        // that equality is the whole invariant, and what an explicit call used to be needed for.
        auto const activeCount = terminal.activeSequences().size();
        terminal.setTerminalId(vtbackend::VTType::VT220);
        CHECK(terminal.activeSequences().size() == activeCount);
    }

    SECTION("the default identity keeps the full table")
    {
        auto terminal = makeTerminal(vtbackend::Settings {}.terminalId);
        CHECK(hasFunction(terminal, vtbackend::DECFRA));
    }
}

// NOLINTEND(misc-const-correctness)

TEST_CASE("Terminal.sixelScrolling.is_DECSDM_at_power_up_and_after_RIS", "[terminal][sixel]")
{
    // A configured setting, not DECSDM's own reset state: xterm 407, measured with its sixelScrolling
    // resource off, starts with DECSDM set and sets it again on RIS after a program reset it, while DECSTR
    // leaves the mode as the program left it.
    auto events = vtbackend::Terminal::NullEvents {};
    auto const pageSize = vtbackend::PageSize { vtbackend::LineCount(5), vtbackend::ColumnCount(20) };
    auto const environment = core::testing::FakeEnvironment {};
    auto const makeTerminal = [&](vtbackend::SixelScrolling sixelScrolling) {
        auto settings = vtbackend::Settings {};
        settings.pageSize = pageSize;
        settings.sixelScrolling = sixelScrolling;
        return std::make_unique<vtbackend::Terminal>(events,
                                                     environment,
                                                     core::platform::NativeFileSystem::instance(),
                                                     std::make_unique<vtpty::MockPty>(pageSize),
                                                     settings,
                                                     std::nullopt,
                                                     vtbackend::TerminalClocks::system(),
                                                     vtbackend::randomSemanticBlockTokens());
    };
    auto const scrollingDisabled = [](vtbackend::Terminal const& terminal) {
        return terminal.isModeEnabled(vtbackend::DECMode::NoSixelScrolling);
    };

    SECTION("configured off")
    {
        auto const terminal = makeTerminal(vtbackend::SixelScrolling::Disabled);
        CHECK(scrollingDisabled(*terminal));

        terminal->writeToScreen("\033[?80l\033[!p"); // DECRST 80, then DECSTR
        CHECK_FALSE(scrollingDisabled(*terminal));

        terminal->writeToScreen("\033c");
        CHECK(scrollingDisabled(*terminal));
    }

    SECTION("configured on, the default")
    {
        auto const terminal = makeTerminal(vtbackend::SixelScrolling::Enabled);
        CHECK_FALSE(scrollingDisabled(*terminal));

        terminal->writeToScreen("\033[?80h\033c"); // DECSET 80, then RIS
        CHECK_FALSE(scrollingDisabled(*terminal));
    }
}

// {{{ block-atomic history eviction: the consumers a shrinking history reaches (issue #836)
namespace
{
/// Writes one OSC 133 prompt-marked command whose output is @p outputLines lines long.
void writeCommandBlock(MockTerm<vtpty::MockPty>& mock, int index, int outputLines)
{
    mock.writeToScreen(std::format("\033]133;A\033\\P{}\r\n", index));
    mock.writeToScreen("\033]133;C\033\\");
    for (auto const i: std::views::iota(0, outputLines))
        mock.writeToScreen(std::format("o{}{}\r\n", index, i));
    mock.writeToScreen("\033]133;D;0\033\\");
}

/// Writes the command blocks numbered [@p first, @p last), each with @p outputLines of output.
void writeCommandBlocks(MockTerm<vtpty::MockPty>& mock, int first, int last, int outputLines)
{
    for (auto const block: std::views::iota(first, last))
        writeCommandBlock(mock, block, outputLines);
}

/// The headroom the eviction tests share: four guaranteed rows, twelve at most.
constexpr auto TestHistoryLimits =
    vtbackend::HistoryLimits { .guaranteed = LineCount(4), .capacity = LineCount(12) };
} // namespace

TEST_CASE("Terminal.historyEviction.clampsAViewportScrolledIntoTheEvictedBlock",
          "[terminal][history-eviction]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(2), ColumnCount(8) }, TestHistoryLimits };
    writeCommandBlocks(mock, 0, 3, 3);

    // Scroll all the way up, so the viewport sits on rows the next trim is about to take.
    mock.terminal.viewport().scrollToTop();
    REQUIRE(mock.terminal.viewport().scrolled());

    // The offset must never name more history than there is: Grid::render asserts exactly this, and
    // in a release build the ring would wrap modulo its size onto unrelated rows instead. Checked
    // after every block, because a dangling offset is transient -- the buffer grows back past it.
    for (auto const block: std::views::iota(3, 8))
    {
        writeCommandBlock(mock, block, 3);
        CHECK(unbox<int>(mock.terminal.viewport().scrollOffset())
              <= unbox<int>(mock.terminal.primaryScreen().historyLineCount()));
    }
}

TEST_CASE("Terminal.historyEviction.clampsAViewportWhenAPartialRegionFeedsTheScrollback",
          "[terminal][history-eviction]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) }, TestHistoryLimits };
    writeCommandBlocks(mock, 0, 3, 3);

    mock.terminal.viewport().scrollToTop();
    REQUIRE(mock.terminal.viewport().scrolled());

    // A TOP-ANCHORED partial DECSTBM region still feeds rows into the scrollback -- Grid routes it
    // through the very same scrollUp -- but Screen deliberately withholds onBufferScrolled, because
    // the live area below the region did not move. So the one path that clamps the viewport on an
    // ordinary scroll is not reached here, and the trim's floor jump goes unnoticed.
    mock.writeToScreen("\033[1;3r");

    // Checked after every write, not just at the end: the buffer oscillates between the guarantee
    // and the capacity, so an offset left dangling by a trim is back in range by the time the next
    // few lines have been written. It is the moment right after a trim that renders wrongly.
    for (auto const i: std::views::iota(0, 40))
    {
        mock.writeToScreen(std::format("x{}\r\n", i % 10));
        CHECK(unbox<int>(mock.terminal.viewport().scrollOffset())
              <= unbox<int>(mock.terminal.primaryScreen().historyLineCount()));
    }
}

TEST_CASE("Terminal.historyEviction.foldRangesForgetAnEvictedBlock", "[terminal][history-eviction]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(2), ColumnCount(8) }, TestHistoryLimits };
    writeCommandBlocks(mock, 0, 3, 3);

    // Populate the fold cache while the first block is still there.
    auto const floorBefore = mock.terminal.primaryScreen().grid().stableRangeFloor();
    REQUIRE(!mock.terminal.foldRanges().empty());

    writeCommandBlocks(mock, 3, 8, 3);

    REQUIRE(mock.terminal.primaryScreen().grid().stableRangeFloor() > floorBefore);

    // A trim moves neither the generation nor the stable base nor the mark revision, so a fold cache
    // keyed on those alone would go on serving ranges whose head has been evicted.
    auto const floor = mock.terminal.primaryScreen().grid().stableRangeFloor();
    for (auto const& range: mock.terminal.foldRanges())
        CHECK(range.firstStableId >= floor);
}

TEST_CASE("Terminal.historyEviction.keepsTheViCursorInsideTheAddressableGrid", "[terminal][history-eviction]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(2), ColumnCount(8) }, TestHistoryLimits };
    writeCommandBlocks(mock, 0, 3, 3);

    mock.terminal.inputHandler().setMode(vtbackend::ViMode::Normal);
    mock.terminal.moveNormalModeCursorTo(CellLocation {
        .line = LineOffset::cast_from(-unbox<int>(mock.terminal.primaryScreen().historyLineCount())),
        .column = ColumnOffset(0) });

    writeCommandBlocks(mock, 3, 8, 3);

    auto const top = -unbox<int>(mock.terminal.primaryScreen().historyLineCount());
    CHECK(unbox<int>(mock.terminal.normalModeCursorPosition().line) >= top);
}
// }}}

TEST_CASE("Terminal.indicatorStatusLine.anOverflowingIndicatorMovesNothingOnThePage", "[terminal]")
{
    // The indicator status line is a one-row screen of its own, written anew by every frame. Text wider than
    // the page is cut at its last column (anOverflowingIndicatorShowsItsHead), but a line feed in the text
    // still scrolls the one row -- which is no output to the page on display: the vi cursor and the window
    // onto the page stay where they are, frame after frame.
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(8) }, LineCount(10) };
    auto& terminal = mc.terminal;
    printLines(mc, "line", 10);
    terminal.setStatusLineDefinition(
        vtbackend::parseStatusLineDefinition("an indicator\nwider than\nthe page", "", ""));
    terminal.inputHandler().setMode(vtbackend::ViMode::Normal); // shows the indicator status line
    auto const viCursor = CellLocation { .line = LineOffset(1), .column = ColumnOffset(0) };
    terminal.moveNormalModeCursorTo(viCursor);
    REQUIRE(terminal.viewport().scrollOffset() == vtbackend::ScrollOffset(0));

    for ([[maybe_unused]] auto const frame: std::views::iota(0, 5))
        terminal.refreshRenderBuffer();

    CHECK(terminal.normalModeCursorPosition() == viCursor);
    CHECK(terminal.viewport().scrollOffset() == vtbackend::ScrollOffset(0));
}

namespace
{
/// Whether the application leaves autowrap (DECAWM) set.
enum class AutoWrap : uint8_t
{
    Reset = 0,
    Set,
};
} // namespace

TEST_CASE("Terminal.indicatorStatusLine.anOverflowingIndicatorShowsItsHead", "[terminal]")
{
    // The indicator status line ends at the page's last column, and a segment wider than what is left of the
    // row is cut there, with its head shown. Autowrap carried the overflow onto the next row, which on the
    // one-row line is a scroll: the tail was shown. Without autowrap the last column took every character
    // past it, and showed the last one.
    auto const [definition, head] = GENERATE(table<std::string_view, std::string_view>({
        { "0123456789", "01234567" },
        { "0123456界9", "0123456" },                      // a wide character with one column left
        { "012345\u2764\uFE0Fyz", "012345\u2764\uFE0F" }, // VS16 widens the heart into the last column
        { "abcdefgh\u0301z", "abcdefgh\u0301" }, // a combining mark joins the character in the last column
        { "012345\U0001F468\u200D\U0001F469z",
          "012345\U0001F468\u200D\U0001F469" }, // a joined sequence stays whole
    }));
    auto const autoWrap = GENERATE(AutoWrap::Set, AutoWrap::Reset);
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(8) } };
    auto& terminal = mc.terminal;
    if (autoWrap == AutoWrap::Reset)
        mc.writeToScreen("\033[?7l"); // DECAWM reset; the indicator status line writes under it too
    terminal.setStatusLineDefinition(vtbackend::parseStatusLineDefinition(definition, "", ""));
    terminal.setStatusDisplay(vtbackend::StatusDisplayType::Indicator);
    terminal.refreshRenderBuffer();

    auto text = terminal.indicatorStatusLineDisplay().renderMainPageText();
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n'))
        text.pop_back();
    CHECK(text == head);
}

TEST_CASE("Terminal.indicatorStatusLine.theRightSegmentEndsInTheLastColumn", "[terminal][statusline]")
{
    // The right segment is placed by its width in columns: "│" is one column and three bytes, and a byte
    // count put it two columns short of the last one. A segment shown is one narrower than the line (the rule
    // kept): twelve bytes of CJK are eight columns, and fit on a ten-column line.
    auto const [columns, right, line] = GENERATE(table<int, std::string_view, std::string>({
        { 20, "ab│", std::string(17, ' ') + "ab│" },
        { 10, "界界界界", "  界界界界" },
    }));
    INFO(right);
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(columns) } };
    auto& terminal = mc.terminal;
    terminal.setStatusLineDefinition(vtbackend::parseStatusLineDefinition("", "", right));
    terminal.setStatusDisplay(vtbackend::StatusDisplayType::Indicator);
    terminal.refreshRenderBuffer();
    CHECK(terminal.indicatorStatusLineDisplay().grid().lineText(LineOffset(0)) == line);
}

TEST_CASE("Terminal.indicatorStatusLine.aSegmentIsMeasuredAsTheGridLaysItOut", "[terminal][statusline]")
{
    // The grid gives every grapheme cluster at least one column, so a zero-width space takes one; and with
    // DEC mode 2027 reset a cluster is as wide as its first codepoint, so a heart that VS16 would widen stays
    // one column. Either way the right segment ends in the last column.
    auto const [unicodeMode, right, columns, line] =
        GENERATE(table<std::string_view, std::string_view, int, std::string>({
            { "\033[?2027h", "ab\u200B", 20, std::string(17, ' ') + "ab\u200B" },
            { "\033[?2027l", "\u2764\uFE0F", 10, std::string(9, ' ') + "\u2764\uFE0F" },
        }));
    INFO(e(right));
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(columns) } };
    auto& terminal = mc.terminal;
    mc.writeToScreen(unicodeMode);
    terminal.setStatusLineDefinition(vtbackend::parseStatusLineDefinition("", "", right));
    terminal.setStatusDisplay(vtbackend::StatusDisplayType::Indicator);
    terminal.refreshRenderBuffer();
    CHECK(terminal.indicatorStatusLineDisplay().grid().lineText(LineOffset(0)) == line);
}

TEST_CASE("Terminal.indicatorStatusLine.theMiddleSegmentIsCentred", "[terminal][statusline]")
{
    // The middle segment starts at (columns - its width) / 2, its width in columns. A byte count put "│││"
    // (nine bytes, three columns) one column left of the middle of a 20-column line, and hid fourteen columns
    // of CJK (21 bytes) that fit.
    auto const [middle, line] = GENERATE(table<std::string_view, std::string>({
        { "│││", std::string(8, ' ') + "│││" + std::string(9, ' ') },
        { "界界界界界界界", std::string(3, ' ') + "界界界界界界界" + std::string(3, ' ') },
    }));
    INFO(middle);
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(20) } };
    auto& terminal = mc.terminal;
    terminal.setStatusLineDefinition(vtbackend::parseStatusLineDefinition("", middle, ""));
    terminal.setStatusDisplay(vtbackend::StatusDisplayType::Indicator);
    terminal.refreshRenderBuffer();
    CHECK(terminal.indicatorStatusLineDisplay().grid().lineText(LineOffset(0)) == line);
}

TEST_CASE("Terminal.indicatorStatusLine.theRightSegmentLeavesTheOnesBeforeItWhole", "[terminal][statusline]")
{
    // The right segment is not shown where it would overlap what the left segment, or a middle one shown,
    // takes, in columns: on a 20-column line "RIGHT" starts at column 15.
    auto const [left, middle, right, line] =
        GENERATE(table<std::string_view, std::string_view, std::string_view, std::string>({
            { "0123456789abcdef", "", "RIGHT", "0123456789abcdef    " }, // the left segment reaches column 15
            { "界界界界界界界界", "", "RIGHT", "界界界界界界界界    " }, // so does a CJK one, in columns
            { "0123456789abcde", "", "RIGHT", "0123456789abcdeRIGHT" },  // it ends where the right one starts
            { "", "0123456789", "RIGHTS", "     0123456789     " },      // the middle one ends at column 15
            { "", "012345678", "RIGHTS", "     012345678RIGHTS" },       // it ends where the right one starts
        }));
    INFO(std::format("left {}, middle {}, right {}", left, middle, right));
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(20) } };
    auto& terminal = mc.terminal;
    terminal.setStatusLineDefinition(vtbackend::parseStatusLineDefinition(left, middle, right));
    terminal.setStatusDisplay(vtbackend::StatusDisplayType::Indicator);
    terminal.refreshRenderBuffer();
    CHECK(terminal.indicatorStatusLineDisplay().grid().lineText(LineOffset(0)) == line);
}

TEST_CASE("Terminal.indicatorStatusLine.theMiddleSegmentLeavesTheLeftOneWhole", "[terminal][statusline]")
{
    // The middle segment is not shown where it would overlap what the left segment takes, in columns: on a
    // 20-column line "XY" starts at column 9, "XYZW" at 8. A middle segment not shown takes no columns.
    auto const [left, middle, right, line] =
        GENERATE(table<std::string_view, std::string_view, std::string_view, std::string>({
            { "0123456789abcdef", "XY", "", "0123456789abcdef    " },  // the left segment reaches column 9
            { "界界界界界", "XY", "", "界界界界界          " },        // so does a CJK one, in columns
            { "012345678", "XY", "", "012345678XY         " },         // it ends where the middle one starts
            { "0123456789", "XYZW", "RIGHT", "0123456789     RIGHT" }, // the right one after a hidden middle
        }));
    INFO(std::format("left {}, middle {}, right {}", left, middle, right));
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(20) } };
    auto& terminal = mc.terminal;
    terminal.setStatusLineDefinition(vtbackend::parseStatusLineDefinition(left, middle, right));
    terminal.setStatusDisplay(vtbackend::StatusDisplayType::Indicator);
    terminal.refreshRenderBuffer();
    CHECK(terminal.indicatorStatusLineDisplay().grid().lineText(LineOffset(0)) == line);
}

TEST_CASE("Terminal.historyEviction.theOldestScrollbackLineIsAlwaysAPrompt", "[terminal][history-eviction]")
{
    // What the issue actually asks for, driven end to end through real OSC 133 sequences.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) },
                                           vtbackend::HistoryLimits { LineCount(10), LineCount(24) } };

    auto const outputLengths = std::array { 4, 9, 2, 7, 3, 11, 5, 6 };
    for (auto const [block, length]: core::views::enumerate(outputLengths))
    {
        writeCommandBlock(mock, static_cast<int>(block), length);

        auto const& grid = mock.terminal.primaryScreen().grid();
        if (grid.historyLineCount() < LineCount(10))
            continue; // not deep enough yet for anything to have been evicted

        // The oldest line still addressable is a prompt, with its command's output whole beneath it.
        auto const top = grid.addressableTop();
        UNSCOPED_INFO(std::format("block {}: top={} text=\"{}\" history={}",
                                  block,
                                  top,
                                  grid.lineText(top),
                                  grid.historyLineCount()));
        CHECK(grid.lineAt(top).marked());
        CHECK(grid.lineText(top).starts_with("P"));
    }
}

TEST_CASE("Terminal.historyEviction.aShellWithoutOsc133SimplyBoundsAtTheHardLimit",
          "[terminal][history-eviction]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) },
                                           vtbackend::HistoryLimits { LineCount(10), LineCount(24) } };

    for (auto const i: std::views::iota(0, 200))
        mock.writeToScreen(std::format("line{}\r\n", i));

    // No mark anywhere, so no boundary can be honoured: the ceiling is what bounds the buffer, and
    // eviction is line-wise exactly as it was before this existed.
    CHECK(mock.terminal.primaryScreen().historyLineCount() == LineCount(24));
    CHECK(mock.terminal.primaryScreen().grid().lineText(LineOffset(-24)) == "line174   ");
}

namespace
{

/// Whether a count a test thread polled for got there in time.
enum class Awaited : uint8_t
{
    TimedOut,
    Reached,
};

/// Polls @p count until it reaches @p target, or @p timeout passes.
/// @return Whether it got there.
template <typename T>
[[nodiscard]] Awaited awaitCount(std::atomic<T> const& count, T target, chrono::milliseconds timeout)
{
    auto const deadline = chrono::steady_clock::now() + timeout;
    while (count.load(std::memory_order_acquire) < target)
    {
        if (chrono::steady_clock::now() >= deadline)
            return Awaited::TimedOut;
        std::this_thread::sleep_for(1ms);
    }
    return Awaited::Reached;
}

/// A PTY that hands over at most @p PieceSize bytes per read -- the way a kernel PTY hands a fast
/// writer's output over in pieces -- and counts its reads, the zero-timeout ones that delivered bytes,
/// and the bytes it served, for a test thread to wait on.
template <size_t PieceSize>
class PiecewisePty: public vtpty::MockPty
{
  public:
    using vtpty::MockPty::MockPty;

    [[nodiscard]] std::optional<ReadResult> read(crispy::BufferObject<char>& storage,
                                                 std::optional<std::chrono::milliseconds> timeout,
                                                 size_t size) override
    {
        auto result = MockPty::read(storage, timeout, std::min(size, PieceSize));
        if (result && !result->data.empty() && timeout == std::chrono::milliseconds::zero())
            _readsAhead.fetch_add(1, std::memory_order_relaxed);
        if (result)
            _served.fetch_add(result->data.size(), std::memory_order_release);
        _reads.fetch_add(1, std::memory_order_release);
        return result;
    }

    /// @return How many reads were served.
    [[nodiscard]] int reads() const noexcept { return _reads.load(std::memory_order_acquire); }

    /// @return How many zero-timeout reads -- reads ahead -- delivered bytes.
    [[nodiscard]] int readsAhead() const noexcept { return _readsAhead.load(std::memory_order_relaxed); }

    /// Waits until @p count reads were served, or @p timeout passes.
    [[nodiscard]] Awaited awaitReads(int count, chrono::milliseconds timeout) const
    {
        return awaitCount(_reads, count, timeout);
    }

    /// Waits until @p bytes were served in total, or @p timeout passes.
    [[nodiscard]] Awaited awaitServed(size_t bytes, chrono::milliseconds timeout) const
    {
        return awaitCount(_served, bytes, timeout);
    }

  private:
    std::atomic<int> _reads = 0;
    std::atomic<int> _readsAhead = 0;
    std::atomic<size_t> _served = 0;
};

/// A PiecewisePty whose second read finds bytes another writer appended to the buffer since the first --
/// the way writeToScreen() on the GUI thread appends, and its parse moves the hot end past them, while the
/// parser thread reads ahead.
class OvertakenPty final: public PiecewisePty<8>
{
  public:
    using PiecewisePty<8>::PiecewisePty;

    [[nodiscard]] std::optional<ReadResult> read(crispy::BufferObject<char>& storage,
                                                 std::optional<std::chrono::milliseconds> timeout,
                                                 size_t size) override
    {
        if (reads() == 1)
        {
            auto const appended = storage.writeAtEnd("XYZ"sv);
            storage.advanceHotEndUntil(appended.data() + appended.size());
        }
        return PiecewisePty<8>::read(storage, timeout, size);
    }
};

/// A PiecewisePty serving three bytes per read, whose second read comes through the stdout fast pipe:
/// there a line feed acts as CR LF, and through the PTY it does not.
class FastPipePty final: public PiecewisePty<3>
{
  public:
    using PiecewisePty<3>::PiecewisePty;

    [[nodiscard]] std::optional<ReadResult> read(crispy::BufferObject<char>& storage,
                                                 std::optional<std::chrono::milliseconds> timeout,
                                                 size_t size) override
    {
        auto result = PiecewisePty<3>::read(storage, timeout, size);
        if (result && reads() == 2)
            result->fromStdoutFastPipe = true;
        return result;
    }
};

/// A PTY whose reads fail without saying why in errno.
class SilentlyFailingPty final: public vtpty::MockPty
{
  public:
    using vtpty::MockPty::MockPty;

    [[nodiscard]] std::optional<ReadResult> read(crispy::BufferObject<char>& /*storage*/,
                                                 std::optional<std::chrono::milliseconds> /*timeout*/,
                                                 size_t /*size*/) override
    {
        return std::nullopt;
    }
};

} // namespace

TEST_CASE("Terminal.processInputOnce.aReadFailingWithoutErrnoClosesThePty", "[terminal]")
{
    // A read ahead that found nothing waiting leaves EAGAIN in errno. A later read that fails without
    // setting errno must not pass for another "nothing yet", or the parser thread reads forever.
    auto mock =
        MockTerm<SilentlyFailingPty> { PageSize { LineCount(3), ColumnCount(10) }, LineCount(10), 1024 };
    errno = EAGAIN; // what an earlier read ahead left behind

    CHECK_FALSE(mock.terminal.processInputOnce());
    CHECK(mock.mockPty().isClosed());
}

#ifdef LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE
namespace
{
/// A PTY that, its output read, has nothing more: a read then fails with EAGAIN, as a real PTY's does when
/// its wait times out. Records the wait each read was given.
class DrainedPty final: public vtpty::MockPty
{
  public:
    using vtpty::MockPty::MockPty;

    [[nodiscard]] std::optional<ReadResult> read(crispy::BufferObject<char>& storage,
                                                 std::optional<std::chrono::milliseconds> timeout,
                                                 size_t size) override
    {
        _lastTimeout = timeout;
        if (!isStdoutDataAvailable())
        {
            errno = EAGAIN;
            return std::nullopt;
        }
        return MockPty::read(storage, timeout, size);
    }

    /// @return The wait the last read was given; milliseconds::max() for none, a wait without end.
    [[nodiscard]] std::chrono::milliseconds lastTimeout() const noexcept
    {
        return _lastTimeout.value_or(std::chrono::milliseconds::max());
    }

    /// Counts the wake-up, which ends the wait of a read in progress, or of the next one.
    void wakeupReader() override { _wakeups.fetch_add(1, std::memory_order_relaxed); }

    /// @return How often another thread woke the reader.
    [[nodiscard]] int wakeups() const noexcept { return _wakeups.load(std::memory_order_relaxed); }

  private:
    std::optional<std::chrono::milliseconds> _lastTimeout;
    std::atomic<int> _wakeups = 0;
};

/// Whether the display paints a frame -- which sets the terminal's frame clock (Terminal::tick()) -- each
/// time a test lets time pass. A passive build's display paints only when told to, and nothing tells it while
/// a fill waits.
enum class DisplayPaints : uint8_t
{
    No = 0,
    Yes,
};
} // namespace

TEST_CASE("Terminal.processInputOnce.passive.aThrottledFillRunsOnceItIsDue", "[terminal]")
{
    // A passive build fills the render buffer on the reader thread after each batch, at most once per refresh
    // interval. A batch that lands inside the interval leaves its fill for later: the reader waits for no
    // more than the rest of the interval, and runs the fill when that wait finds nothing to read.
    auto mock = MockTerm<DrainedPty> { PageSize { LineCount(3), ColumnCount(20) }, LineCount(10), 1024 };
    auto& terminal = mock.terminal;
    terminal.setRefreshRate(vtbackend::RefreshRate { 50.0 });
    constexpr auto Interval = std::chrono::milliseconds(20);
    auto const paints = GENERATE(DisplayPaints::No, DisplayPaints::Yes);
    auto const letTimePass = [&](std::chrono::milliseconds duration) {
        mock.steadyClock.advance(duration);
        if (paints == DisplayPaints::Yes)
            terminal.tick(mock.steadyClock.now());
    };
    letTimePass(Interval); // the first fill is due at once

    mock.mockPty().appendStdOutBuffer("first");
    REQUIRE(terminal.processInputOnce());
    REQUIRE(trimmedTextScreenshot(mock) == "first");

    mock.mockPty().appendStdOutBuffer("\r\nsecond");
    REQUIRE(terminal.processInputOnce());
    REQUIRE(trimmedTextScreenshot(mock) == "first"); // the fill waits for the interval

    // Nothing more to read, at the same instant: the reader waits for the rest of the interval, not for none.
    REQUIRE(terminal.processInputOnce());
    CHECK(mock.mockPty().lastTimeout() == Interval);
    CHECK(trimmedTextScreenshot(mock) == "first");

    // The interval has passed: the read finds nothing again, and the fill runs.
    letTimePass(Interval);
    REQUIRE(terminal.processInputOnce());
    CHECK(mock.mockPty().lastTimeout() == std::chrono::milliseconds(0));
    CHECK(trimmedTextScreenshot(mock) == "first\nsecond");

    // With nothing left to fill, the reader waits a whole interval, and fills nothing.
    REQUIRE(terminal.processInputOnce());
    CHECK(mock.mockPty().lastTimeout() == Interval);
}

TEST_CASE("Terminal.processInputOnce.passive.aHeldFrontBufferIsRetriedWhenTheDisplayLetsGo",
          "[terminal][threading]")
{
    // A frame filled while the display holds the front buffer waits to be swapped in. The reader does not
    // poll to retry the swap: it waits a whole interval, which new output or the display letting go -- a
    // wake-up -- ends, and swaps then.
    auto mock = MockTerm<DrainedPty> { PageSize { LineCount(3), ColumnCount(20) }, LineCount(10), 1024 };
    auto& terminal = mock.terminal;
    terminal.setRefreshRate(vtbackend::RefreshRate { 50.0 });
    constexpr auto Interval = std::chrono::milliseconds(20);
    mock.steadyClock.advance(Interval); // the first fill is due at once

    // The display holds the front buffer on a thread of its own: a thread may not try a lock it owns.
    auto held = std::promise<void> {};
    auto letGo = std::promise<void> {};
    auto display = std::thread([&terminal, &held, released = letGo.get_future()] {
        auto const frame = terminal.renderBuffer();
        held.set_value();
        released.wait();
    });
    held.get_future().wait();

    mock.mockPty().appendStdOutBuffer("first");
    REQUIRE(terminal.processInputOnce());
    REQUIRE(terminal.renderBufferState() == vtbackend::RenderBufferState::TrySwapBuffers);

    // Nothing more to read: a whole interval's wait, not none.
    REQUIRE(terminal.processInputOnce());
    CHECK(mock.mockPty().lastTimeout() == Interval);
    CHECK(mock.mockPty().wakeups() == 0);

    letGo.set_value();
    display.join();
    CHECK(mock.mockPty().wakeups() == 1);

    // Woken, the reader finds nothing to read, and swaps the frame in.
    REQUIRE(terminal.processInputOnce());
    CHECK(terminal.renderBufferState() == vtbackend::RenderBufferState::WaitingForRefresh);
    CHECK(trimmedTextScreenshot(mock) == "first");
}
#endif

TEST_CASE("Terminal.screenUpdated.aRetriedSwapThatSucceedsIsAnnounced", "[terminal][threading]")
{
    // The display's last frame lost its swap to a paint, and the display then went away -- its tab was
    // switched from -- leaving the frame waiting to be swapped in. The first output afterwards retries the
    // swap, which now succeeds, and must still say the screen changed: a background tab learns of its
    // output from nothing else.
    struct CountingTerm final: MockTerm<>
    {
        using MockTerm<>::MockTerm;
        int screenUpdates = 0; ///< How often the terminal said the screen changed.
        void screenUpdated() override { ++screenUpdates; }
    };
    auto mock = CountingTerm { PageSize { LineCount(3), ColumnCount(20) }, LineCount(10) };
    {
        auto const painting = mock.terminal.renderBuffer();
        // On a thread of its own: the swap's try_lock must fail as it does against the render thread, and a
        // mutex may not be tried by the thread that holds it.
        std::thread { [&mock] { std::ignore = mock.terminal.refreshRenderBuffer(); } }.join();
        REQUIRE(mock.terminal.renderBufferState() == vtbackend::RenderBufferState::TrySwapBuffers);
    }
    mock.screenUpdates = 0;

    mock.writeToScreen("x");

    CHECK(mock.terminal.renderBufferState() == vtbackend::RenderBufferState::WaitingForRefresh);
    CHECK(mock.screenUpdates == 1);
}

TEST_CASE("Terminal.processInputOnce.readsAheadWhileTheStateIsTaken", "[terminal][threading]")
{
    // While another thread -- the render thread, building a frame -- holds the terminal's state, the
    // parser thread keeps reading instead of waiting idle, and parses all of it in its next hold.
    auto mock = MockTerm<PiecewisePty<8>> { PageSize { LineCount(3), ColumnCount(40) }, LineCount(10), 1024 };
    mock.mockPty().appendStdOutBuffer("0123456789abcdefghijklmnopqrstuv"); // four pieces of eight

    mock.terminal.lock(); // the render thread's hold
    auto parse = std::async(std::launch::async, [&mock] { return mock.terminal.processInputOnce(); });
    auto const served = mock.mockPty().awaitServed(32, 2s);
    mock.terminal.unlock();
    REQUIRE(parse.get());

    CHECK(served == Awaited::Reached);
    CHECK(mock.terminal.primaryScreen()
              .grid()
              .lineText(LineOffset(0))
              .starts_with("0123456789abcdefghijklmnopqrstuv"));
}

TEST_CASE("Terminal.processInputOnce.readAheadLeavesTheHotEndWhereAPlainReadWould", "[terminal][threading]")
{
    // readAhead() claims the bytes it reads by moving the buffer's hot end over them; the parse then
    // moves it on exactly as for a batch that was never read ahead. releaseReadAhead() is what hands
    // the claim back first -- without it the hot end stays at the batch's end, which a plain read of
    // the same input never leaves it at, and the next read allocates a fresh buffer too early.
    constexpr auto Text = "0123456789abcdefghijklmnopqrstuv\r\n"sv; // four pieces of eight and a tail

    auto plain = MockTerm<> { PageSize { LineCount(3), ColumnCount(40) }, LineCount(10), 1024 };
    plain.mockPty().appendStdOutBuffer(Text);
    REQUIRE(plain.terminal.processInputOnce());
    auto const expected = plain.terminal.currentPtyBuffer()->bytesUsed();

    auto mock = MockTerm<PiecewisePty<8>> { PageSize { LineCount(3), ColumnCount(40) }, LineCount(10), 1024 };
    mock.mockPty().appendStdOutBuffer(Text);
    mock.terminal.lock(); // the render thread's hold
    auto parse = std::async(std::launch::async, [&mock] { return mock.terminal.processInputOnce(); });
    auto const served = mock.mockPty().awaitServed(Text.size(), 2s);
    mock.terminal.unlock();
    REQUIRE(parse.get());

    CHECK(served == Awaited::Reached);
    CHECK(mock.terminal.currentPtyBuffer()->bytesUsed() == expected);
}

TEST_CASE("Terminal.processInputOnce.readAheadStepsAroundBytesAppendedMeanwhile", "[terminal][threading]")
{
    // Another writer appended to the buffer while the parser thread read ahead, so the next read lands
    // after those bytes rather than after the batch. The parse must take exactly what the PTY served --
    // the other writer parses its own bytes -- and the read-ahead stops there.
    auto mock = MockTerm<OvertakenPty> { PageSize { LineCount(3), ColumnCount(40) }, LineCount(10), 1024 };
    mock.mockPty().appendStdOutBuffer("0123456789abcdefghijklmnopqrstuv");

    mock.terminal.lock(); // the render thread's hold
    auto parse = std::async(std::launch::async, [&mock] { return mock.terminal.processInputOnce(); });
    auto const readTwice = mock.mockPty().awaitReads(2, 2s);
    mock.terminal.unlock();
    REQUIRE(parse.get());

    CHECK(readTwice == Awaited::Reached);
    CHECK(mock.mockPty().reads() == 2);
    CHECK(mock.terminal.primaryScreen().grid().lineText(LineOffset(0)).substr(0, 17) == "0123456789abcdef ");
}

TEST_CASE("Terminal.processInputOnce.readAheadKeepsEachSourceApart", "[terminal][threading]")
{
    // The PTY's "ab\n", the fast pipe's "cd\n", then the PTY's "ef", all read ahead into one batch. Each
    // piece must be parsed as its own source made it: the PTY's line feed keeps the column, the fast
    // pipe's returns the carriage as well.
    auto mock = MockTerm<FastPipePty> { PageSize { LineCount(3), ColumnCount(10) }, LineCount(10), 1024 };
    mock.mockPty().appendStdOutBuffer("ab\ncd\nef");

    mock.terminal.lock(); // the render thread's hold
    auto parse = std::async(std::launch::async, [&mock] { return mock.terminal.processInputOnce(); });
    auto const served = mock.mockPty().awaitServed(8, 2s);
    mock.terminal.unlock();
    REQUIRE(parse.get());

    CHECK(served == Awaited::Reached);
    auto const& grid = mock.terminal.primaryScreen().grid();
    CHECK(grid.lineText(LineOffset(0)) == "ab        ");
    CHECK(grid.lineText(LineOffset(1)) == "  cd      ");
    CHECK(grid.lineText(LineOffset(2)) == "ef        ");
}

TEST_CASE("Terminal.processInputOnce.readsOnceWhenTheStateIsFree", "[terminal]")
{
    // Nobody holds the state: one read, one parse, as before.
    auto mock = MockTerm<PiecewisePty<8>> { PageSize { LineCount(3), ColumnCount(40) }, LineCount(10), 1024 };
    mock.mockPty().appendStdOutBuffer("0123456789abcdefghijklmnopqrstuv");

    REQUIRE(mock.terminal.processInputOnce());

    CHECK(mock.mockPty().reads() == 1);
    CHECK(mock.terminal.primaryScreen().grid().lineText(LineOffset(0)).substr(0, 9) == "01234567 ");
}

TEST_CASE("Terminal.processInputOnce.readAheadKeepsTheScreenExactWhileRendering", "[terminal][threading]")
{
    // The parser thread floods the terminal while another thread refreshes the render buffer as fast as
    // it can, so most reads find the state taken and read ahead. Run under the clang-tsan tree for the
    // races; anywhere, the screen must come out exactly as a single-threaded parse leaves it.
    // The first frame holds the state until the parser has read ahead, so that the contended path runs
    // whatever the scheduler does, and every frame after it yields: valgrind runs one thread at a time, and
    // a thread that took the state straight back kept the parser waiting for minutes.
    auto constexpr TermSize = PageSize { LineCount(10), ColumnCount(40) };
    auto const flood = [] {
        auto text = std::string {};
        for (auto const i: std::views::iota(0, 3000))
            text += std::format("\033[3{}mline {}\033[m plain tail\r\n", i % 8, i);
        return text;
    }();

    auto threaded = MockTerm<PiecewisePty<64>> { TermSize, LineCount(100), 4096 };
    threaded.mockPty().appendStdOutBuffer(flood);
    auto parsed = std::atomic<bool> { false };
    threaded.terminal.lock(); // the first frame's hold
    auto parser = std::thread { [&] {
        while (threaded.mockPty().isStdoutDataAvailable())
            threaded.terminal.processInputOnce();
        parsed.store(true, std::memory_order_release);
    } };
    while (threaded.mockPty().readsAhead() == 0)
        std::this_thread::yield();
    threaded.terminal.unlock();
    while (!parsed.load(std::memory_order_acquire))
    {
        threaded.terminal.refreshRenderBuffer();
        std::this_thread::yield();
    }
    parser.join();

    auto reference = MockTerm { TermSize, LineCount(100), 4096 };
    reference.writeToScreen(flood);

    CHECK(threaded.mockPty().readsAhead() > 0); // else the contended path never ran
    CHECK(screenText(threaded.terminal.primaryScreen()) == screenText(reference.terminal.primaryScreen()));
}

TEST_CASE("Terminal.processInputOnce.sizesItsReadByAWidthAnotherThreadResizes", "[terminal][threading]")
{
    // The parser thread looks at the page width to size its read before it takes the terminal's state;
    // the GUI thread, holding that state, resizes the page meanwhile. The resize waits for the read
    // ahead through a relaxed count, which orders nothing, so under the clang-tsan tree the width the
    // parser looked at must be one published for readers without the lock.
    auto mock = MockTerm<PiecewisePty<8>> { PageSize { LineCount(3), ColumnCount(40) }, LineCount(10), 1024 };
    mock.mockPty().appendStdOutBuffer("0123456789abcdef"); // two pieces of eight

    mock.terminal.lock(); // the GUI thread's hold
    auto parse = std::async(std::launch::async, [&mock] { return mock.terminal.processInputOnce(); });
    while (mock.mockPty().readsAhead() == 0)
        std::this_thread::yield();
    mock.terminal.resizeScreen(PageSize { LineCount(3), ColumnCount(30) }, std::nullopt);
    mock.terminal.unlock();
    REQUIRE(parse.get());

    CHECK(mock.terminal.primaryScreen().grid().lineText(LineOffset(0)).starts_with("0123456789abcdef"));
}

TEST_CASE("Terminal.flushInput.concurrentFlushesDeliverEveryReplyExactlyOnce", "[terminal][input][threading]")
{
    // Both threads flush the input queue: the parser thread from reportInBandWindowResize() (enabling
    // DEC mode 2048 replies and flushes on the spot), the GUI thread from TerminalSession::flushInput().
    // Unsynchronized, two flushes could send the same bytes and both consume() them, leaving the consumed
    // offset past the emptied queue -- the next reply then lost its first bytes, and Neovim received the
    // tail of a DA1 reply as keystrokes. Run under `ctest --preset=clang-tsan` for the data race itself;
    // elsewhere this checks the observable contract: every reply arrives once, whole, in order.
    auto constexpr TermSize = PageSize { LineCount(4), ColumnCount(10) };
    auto constexpr Queries = "\033[5n\033[?2048h\033[?2048l"sv;
    auto constexpr Iterations = 2000;

    auto mc = MockTerm { TermSize, LineCount(10) };
    auto& terminal = mc.terminal;

    {
        auto const gui = vtbackend::test::writeOnOwnThread([&] { terminal.flushInput(); });
        for ([[maybe_unused]] auto const i: std::views::iota(0, Iterations))
            mc.writeToScreen(Queries);
    }
    terminal.flushInput();

    // The queue is first in, first out, so however the flushes interleave, the wire must carry exactly
    // what a single thread sends: each iteration's DSR reply and in-band resize report, whole and in
    // order. A duplicated flush shows up as extra bytes, a truncated reply as missing ones.
    auto reference = MockTerm { TermSize, LineCount(10) };
    reference.writeToScreen(Queries);
    reference.terminal.flushInput();
    auto expected = std::string {};
    for ([[maybe_unused]] auto const i: std::views::iota(0, Iterations))
        expected += reference.replyData();

    CHECK(core::escape(mc.replyData()) == core::escape(expected));
}

namespace
{

/// A PTY whose first write() parks until the test releases it.
///
/// That holds one thread inside Terminal::flushInput() with the bytes it is sending already taken from
/// the queue but not yet consumed -- the window UnixPty::write() opens for real whenever a short write
/// falls back to a blocking one.
class GatedPty final: public vtpty::MockPty
{
  public:
    using vtpty::MockPty::MockPty;

    int write(std::string_view data) override
    {
        if (!_gatePassed.exchange(true))
        {
            _entered.release();
            _released.wait();
        }
        auto const _ = std::scoped_lock { _mutex };
        return MockPty::write(data);
    }

    /// Waits for a thread to park inside the first write().
    /// @param timeout How long to wait.
    /// @return Whether one did within @p timeout.
    [[nodiscard]] bool awaitFirstWrite(chrono::milliseconds timeout)
    {
        return _entered.try_acquire_for(timeout);
    }

    /// Lets the parked write() proceed.
    void release() { _released.count_down(); }

  private:
    std::atomic<bool> _gatePassed = false;
    std::binary_semaphore _entered { 0 };
    std::latch _released { 1 };
    std::mutex _mutex;
};

/// Presses the Up arrow key on one thread, the way the GUI thread does, and while its PTY write is parked,
/// parses @p vt on another, the way the parser thread does. Then lets the write finish.
///
/// The parse is given a moment to run into the parked write rather than awaited: a terminal that
/// serializes flushes keeps a reply's flush waiting on the key press's until the write is let go.
void parseDuringKeyPressWrite(MockTerm<GatedPty>& mock, std::string_view vt)
{
    auto keyPress = std::async(std::launch::async, [&] {
        mock.terminal.sendKeyEvent(vtbackend::Key::UpArrow,
                                   vtbackend::KeyboardModifiers {},
                                   vtbackend::KeyboardEventType::Press,
                                   mock.steadyClock.now());
    });
    if (!mock.mockPty().awaitFirstWrite(5s))
    {
        mock.mockPty().release();
        FAIL("The key press never reached the PTY.");
    }

    auto parse = std::async(std::launch::async, [&] { mock.terminal.writeToScreen(vt); });
    std::ignore = parse.wait_for(50ms);

    mock.mockPty().release();
    keyPress.get();
    parse.get();
}

} // namespace

TEST_CASE("Terminal.flushInput.overlappingFlushNeitherResendsNorOverconsumes", "[terminal][input][threading]")
{
    // The crash this guards against: a key press is flushed on the GUI thread, and DSR 996 replies and
    // flushes on the parser thread. With the parser thread's flush inside the GUI thread's -- after the
    // GUI thread took the key's bytes, before it consumed them -- the key went out twice and was consumed
    // twice, leaving the consumed offset past the end of the emptied queue. peek() derives its length as
    // size minus that offset, so the next, shorter key press made it wrap, and flushInput() threw
    // std::length_error out of a Qt event handler, aborting the terminal.
    //
    // Unlike the test above, this places the overlap deterministically rather than hoping for it.
    auto mock = MockTerm<GatedPty> { PageSize { LineCount(3), ColumnCount(10) } };
    parseDuringKeyPressWrite(mock, "\033[?996n");

    CHECK_NOTHROW(mock.sendCharEvent(U'x'));
    CHECK_FALSE(mock.terminal.hasInput());
    CHECK(mock.mockPty().stdinBuffer() == "\033[A\033[?997;1nx");
}

namespace
{
/// Answers status-line command runs from a script; local to this suite on purpose.
struct FakeCommandCapture final: vtpty::CommandCapture
{
    std::deque<std::expected<std::string, vtpty::CommandCaptureError>> answers;
    std::vector<std::string> commands;

    std::expected<std::string, vtpty::CommandCaptureError> run(std::string_view command,
                                                               std::chrono::milliseconds,
                                                               core::async::StopToken) override
    {
        commands.emplace_back(command);
        if (answers.empty())
            return std::unexpected(
                vtpty::CommandCaptureError { vtpty::CommandCaptureError::Kind::SpawnFailed,
                                             std::make_error_code(std::errc::invalid_argument) });
        auto answer = std::move(answers.front());
        answers.pop_front();
        return answer;
    }
};

/// A MockTerm that counts how often the terminal asked its host for a repaint.
struct CountingMockTerm: vtbackend::MockTerm<>
{
    using MockTerm::MockTerm;
    int renderBufferUpdates = 0;
    void renderBufferUpdated() override { ++renderBufferUpdates; }
};

constexpr auto CommandPageSize = vtbackend::PageSize { vtbackend::LineCount(5), vtbackend::ColumnCount(40) };
} // namespace

TEST_CASE("Terminal.statusLineCommandRunsOffThreadAndSchedulesWakeup", "[terminal][statusline]")
{
    using namespace vtbackend;
    using namespace std::chrono_literals;

    auto capture = FakeCommandCapture {};
    capture.answers.emplace_back("main");
    auto executor = core::async::testing::ManualExecutor {};
    auto mock =
        MockTerm { CommandPageSize, HistoryLimits {}, 1024, StatusLineCommandServices { capture, executor } };
    auto& terminal = mock.terminal;
    terminal.setStatusLineDefinition(
        parseStatusLineDefinition("", "", "{Command:Program=git branch,Interval=10}"));
    terminal.setStatusDisplay(StatusDisplayType::Indicator);

    auto const item = StatusLineDefinitions::Command { {}, "git branch", 10s };
    CHECK(terminal.statusLineCommandOutput(item).empty());
    CHECK(capture.commands.empty()); // nothing ran on the calling thread
    executor.drain();
    CHECK(terminal.statusLineCommandOutput(item) == "main");

    // The minute wakeup is never below a second, so 500 ms can only be the command coming due.
    terminal.tick(terminal.currentTime() + 9500ms);
    auto const wakeup = terminal.nextRender();
    REQUIRE(wakeup.has_value());
    CHECK(*wakeup == 500ms);
}

TEST_CASE("Terminal.statusLineCommandWithoutServicesRendersEmpty", "[terminal][statusline]")
{
    using namespace vtbackend;
    using namespace std::chrono_literals;

    auto mock = MockTerm { CommandPageSize };
    CHECK(
        mock.terminal.statusLineCommandOutput(StatusLineDefinitions::Command { {}, "echo hi", 10s }).empty());
}

TEST_CASE("Terminal.statusLineCommandOutputThatResetsKeepsTheIndicatorColors", "[terminal][statusline]")
{
    // A command's output is written as it is printed, SGR included, and a reset in it leaves the cells
    // after it in the default colours: those are the indicator's, not the main page's.
    using namespace vtbackend;

    auto capture = FakeCommandCapture {};
    capture.answers.emplace_back("\033[1ma\033[0mb\033[39;49mc");
    auto executor = core::async::testing::ManualExecutor {};
    auto mock =
        MockTerm { CommandPageSize, HistoryLimits {}, 1024, StatusLineCommandServices { capture, executor } };
    auto& terminal = mock.terminal;
    terminal.setStatusLineDefinition(parseStatusLineDefinition("{Command:Program=git branch}", "", ""));
    terminal.setStatusDisplay(StatusDisplayType::Indicator);
    executor.drain();
    terminal.refreshRenderBuffer();

    auto const& row = terminal.indicatorStatusLineDisplay();
    REQUIRE(row.grid().lineAt(LineOffset(0)).toUtf8Trimmed() == "abc");
    auto const colors = terminal.focused() ? terminal.colorPalette().indicatorStatusLineInsertMode
                                           : terminal.colorPalette().indicatorStatusLineInactive;
    for (auto const column: { 0, 1, 2, 10 })
    {
        INFO("column " << column);
        auto const cell = row.at(LineOffset(0), ColumnOffset(column));
        CHECK(cell.foregroundColor() == Color(colors.foreground));
        CHECK(cell.backgroundColor() == Color(colors.background));
    }
}

TEST_CASE("Terminal.statusLineCommandItemRendersThroughTheVisitor", "[terminal][statusline]")
{
    using namespace vtbackend;

    auto capture = FakeCommandCapture {};
    capture.answers.emplace_back("main");
    auto executor = core::async::testing::ManualExecutor {};
    auto mock =
        MockTerm { CommandPageSize, HistoryLimits {}, 1024, StatusLineCommandServices { capture, executor } };
    auto const segment = parseStatusLineSegment("{Command:Program=git branch}");
    REQUIRE(segment.size() == 1);
    REQUIRE(std::holds_alternative<StatusLineDefinitions::Command>(segment[0]));

    CHECK(serializeToVT(mock.terminal, segment, StatusLineStyling::Disabled).empty());
    executor.drain();
    CHECK(serializeToVT(mock.terminal, segment, StatusLineStyling::Disabled) == "main");
}

TEST_CASE("Terminal.statusLineCommandCompletionNotifiesTheHost", "[terminal][statusline]")
{
    using namespace vtbackend;
    using namespace std::chrono_literals;

    auto capture = FakeCommandCapture {};
    capture.answers.emplace_back("main");
    capture.answers.emplace_back("main"); // identical: the second run changes nothing
    auto executor = core::async::testing::ManualExecutor {};
    auto mock = CountingMockTerm {
        CommandPageSize, HistoryLimits {}, 1024, StatusLineCommandServices { capture, executor }
    };
    auto& terminal = mock.terminal;
    auto const item = StatusLineDefinitions::Command { {}, "git branch", 1s };

    (void) terminal.statusLineCommandOutput(item);
    terminal.ensureFreshRenderBuffer(); // settle the buffer so a later dirtying is observable
    REQUIRE(terminal.renderBufferState() == RenderBufferState::WaitingForRefresh);
    auto const before = mock.renderBufferUpdates;
    executor.drain();
    CHECK(mock.renderBufferUpdates == before + 1);
    CHECK(terminal.renderBufferState() == RenderBufferState::RefreshBuffersAndTrySwap); // Changed

    terminal.ensureFreshRenderBuffer();
    REQUIRE(terminal.renderBufferState() == RenderBufferState::WaitingForRefresh);
    terminal.tick(terminal.currentTime() + 2s);
    (void) terminal.statusLineCommandOutput(item); // due again: starts the second run
    auto const afterFirst = mock.renderBufferUpdates;
    executor.drain();
    CHECK(mock.renderBufferUpdates == afterFirst + 1);
    CHECK(terminal.renderBufferState() == RenderBufferState::WaitingForRefresh); // Unchanged: not dirtied
}

TEST_CASE("Terminal.suppressedRenderingDoesNotSpinOnAnOverdueCommand", "[terminal][statusline]")
{
    using namespace vtbackend;
    using namespace std::chrono_literals;

    auto capture = FakeCommandCapture {};
    capture.answers.emplace_back("main");
    auto executor = core::async::testing::ManualExecutor {};
    auto mock =
        MockTerm { CommandPageSize, HistoryLimits {}, 1024, StatusLineCommandServices { capture, executor } };
    auto& terminal = mock.terminal;
    terminal.setStatusLineDefinition(
        parseStatusLineDefinition("", "", "{Command:Program=git branch,Interval=1}"));
    terminal.setStatusDisplay(StatusDisplayType::Indicator);
    (void) serializeToVT(terminal,
                         parseStatusLineSegment("{Command:Program=git branch,Interval=1}"),
                         StatusLineStyling::Disabled);
    executor.drain();
    terminal.tick(terminal.currentTime() + 30s);
    REQUIRE(terminal.nextRender().value_or(1h) == 0ms); // overdue while rendering is live

    // Synchronized output suppresses rendering, so the status line is not rebuilt and the command
    // could never be refreshed: it must not ask for back-to-back immediate wakeups meanwhile.
    // A generous timeout keeps a scheduler stall from ending the suppression window mid-test.
    terminal.settings().synchronizedOutputTimeout = 1h;
    mock.writeToScreen("\033[?2026h");
    REQUIRE(terminal.isRenderingSuppressed());
    CHECK(terminal.nextRender().value_or(1h) > 0ms);
}

TEST_CASE("Terminal.replacingTheStatusLineDefinitionForgetsOverdueCommands", "[terminal][statusline]")
{
    using namespace vtbackend;
    using namespace std::chrono_literals;

    auto capture = FakeCommandCapture {};
    capture.answers.emplace_back("main");
    capture.answers.emplace_back("main");
    auto executor = core::async::testing::ManualExecutor {};
    auto mock =
        MockTerm { CommandPageSize, HistoryLimits {}, 1024, StatusLineCommandServices { capture, executor } };
    auto& terminal = mock.terminal;
    auto const withOverdueCommand = [&] {
        terminal.setStatusLineDefinition(
            parseStatusLineDefinition("", "", "{Command:Program=git branch,Interval=1}"));
        terminal.setStatusDisplay(StatusDisplayType::Indicator);
        // Through the visitor, as the render path does.
        (void) serializeToVT(terminal,
                             parseStatusLineSegment("{Command:Program=git branch,Interval=1}"),
                             StatusLineStyling::Disabled);
        executor.drain();
        // Long overdue: the command alone asks for an immediate wakeup.
        terminal.tick(terminal.currentTime() + 30s);
        REQUIRE(terminal.nextRender().value_or(1h) == 0ms);
    };

    SECTION("setStatusLineDefinition")
    {
        withOverdueCommand();
        terminal.setStatusLineDefinition(parseStatusLineDefinition("", "", "plain"));
        CHECK(terminal.nextRender().value_or(1h) > 0ms);
    }

    SECTION("resetStatusLineDefinition")
    {
        withOverdueCommand();
        terminal.resetStatusLineDefinition(); // back to the settings' default, which has no commands
        CHECK(terminal.nextRender().value_or(1h) > 0ms);
    }
}

TEST_CASE("Terminal.resetColorPalette.reportsOnlyAPaletteThatChanged", "[terminal][colors]")
{
    // A reload re-seeds the palette whatever else it changed. An application that subscribed to DEC mode 2031
    // answers each report by querying its colours again and redrawing, so a reload that changed no colour
    // must not send one.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    mock.writeToScreen("\033[?2031h");
    REQUIRE(mock.terminal.isModeEnabled(vtbackend::DECMode::ReportColorPaletteUpdated));
    auto const original = mock.terminal.colorPalette();

    mock.terminal.resetColorPalette(original);
    CHECK(e(mock.replyData()).empty());

    auto changed = original;
    changed.defaultForeground = vtbackend::RGBColor { 0x10, 0x20, 0x30 };
    mock.terminal.resetColorPalette(changed);
    CHECK(e(mock.replyData()) == e(std::string { "\033[?997;2n" }));
    mock.resetReplyData();

    mock.terminal.resetColorPalette(changed);
    CHECK(e(mock.replyData()).empty());

    // A colour a program set is part of what the application sees, so restoring the configured one is a
    // change it must hear about.
    mock.writeToScreen("\033]10;rgb:ff/ff/ff\033\\");
    mock.discardPendingReplies();
    mock.terminal.resetColorPalette(changed);
    CHECK(e(mock.replyData()) == e(std::string { "\033[?997;2n" }));
}

TEST_CASE(
    "Terminal.a reply raised outside the parser under CONTOUR_SYNC_PTY_OUTPUT waits for the lock's release",
    "[terminal]")
{
    // The knob flushes a reply as it is raised, which the parser can afford: it defers the local echo of what
    // it flushes until it has unwound. Outside the parser the caller may hold the state lock, as a frontend
    // selecting text does, and with SRM reset the flush would echo, which parses and takes that lock again.
    auto events = vtbackend::Terminal::NullEvents {};
    auto const pageSize = vtbackend::PageSize { vtbackend::LineCount(5), vtbackend::ColumnCount(20) };
    auto settings = vtbackend::Settings {};
    settings.pageSize = pageSize;
    auto const environment = core::testing::FakeEnvironment { { { "CONTOUR_SYNC_PTY_OUTPUT", "1" } } };
    auto terminal = vtbackend::Terminal { events,
                                          environment,
                                          core::platform::NativeFileSystem::instance(),
                                          std::make_unique<vtpty::MockPty>(pageSize),
                                          settings,
                                          std::nullopt,
                                          vtbackend::TerminalClocks::system(),
                                          vtbackend::randomSemanticBlockTokens() };
    auto& pty = static_cast<vtpty::MockPty&>(terminal.device());
    terminal.writeToScreen("\033[12l\033[?2030h");

    // Inside the parser, the reply to DA1 is on the PTY before the parse returns.
    terminal.writeToScreen("\033[c");
    CHECK(pty.stdinBuffer().starts_with("\033[?"));
    pty.stdinBuffer().clear();

    // Outside it, selecting reports the selection (DECSET 2030) under the lock, and the report waits for the
    // lock's release, where its echo can take the lock again.
    {
        auto const _ = std::scoped_lock { terminal };
        terminal.selectAll();
        CHECK(pty.stdinBuffer().empty());
    }
    CHECK(pty.stdinBuffer().starts_with("\033[>"));
}

TEST_CASE("Terminal.a report raised under a frontend's lock goes out once the lock is released", "[terminal]")
{
    // A frontend changes what the parser thread changes under the state lock, and a change can raise a
    // report: here a selection, with DECSET 2030 on. What the hold queued goes out as the lock is released,
    // whichever way the frontend took it.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mc.writeToScreen("\033[?2030h");
    mc.discardPendingReplies();
    {
        auto const _ = std::scoped_lock { mc.terminal };
        mc.terminal.selectAll();
        CHECK(mc.replyData().empty());
    }
    CHECK(mc.replyData().starts_with("\033[>"));
}

TEST_CASE("Terminal.what a frontend's lock held back goes out when an exception unwinds it", "[terminal]")
{
    // An exception thrown under the lock releases it on the way out, and what the hold kept back goes out
    // then: the report it raised, and the document it asked to open.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mc.writeToScreen("\033[?2030h");
    mc.discardPendingReplies();
    auto const holdAndThrow = [&] {
        auto const _ = std::scoped_lock { mc.terminal };
        mc.terminal.selectAll();
        mc.terminal.openDocument("file:///tmp/held-back");
        CHECK(mc.openedDocument.empty());
        throw std::runtime_error("unwound");
    };
    CHECK_THROWS_AS(holdAndThrow(), std::runtime_error);
    CHECK(mc.replyData().starts_with("\033[>"));
    CHECK(mc.openedDocument == "file:///tmp/held-back");
}

namespace
{
/// A PTY that, at every write, asks another thread to take its terminal's state lock, and counts the
/// writes for which that thread could not: a write made with the lock held, which parks every thread
/// waiting for the lock for as long as the application leaves its input unread.
class LockProbingPty final: public vtpty::MockPty
{
  public:
    using MockPty::MockPty;

    /// @param terminal The terminal whose lock is probed from now on.
    void probe(vtbackend::Terminal const& terminal) { _terminal = &terminal; }

    /// @return How many writes found the state lock held.
    [[nodiscard]] int writesUnderLock() const noexcept { return _writesUnderLock; }

    [[nodiscard]] int write(std::string_view data) override
    {
        if (_terminal != nullptr)
        {
            auto taken = std::async(std::launch::async, [terminal = _terminal] {
                auto const _ = std::scoped_lock { *terminal };
            });
            if (taken.wait_for(std::chrono::seconds { 1 }) != std::future_status::ready)
                ++_writesUnderLock;
            // Kept rather than dropped, as dropping it waits for the lock, which this write's caller holds.
            _probes.push_back(std::move(taken));
        }
        return MockPty::write(data);
    }

  private:
    vtbackend::Terminal const* _terminal = nullptr;
    int _writesUnderLock = 0;
    std::vector<std::future<void>> _probes;
};

/// A PTY whose next write, once armed, parks until released: an application that leaves its input unread.
class ParkingPty final: public vtpty::MockPty
{
  public:
    using MockPty::MockPty;

    /// Makes the next write park.
    void arm()
    {
        auto const _ = std::scoped_lock { _mutex };
        _armed = true;
    }

    /// Waits until a write is parked.
    void waitUntilParked()
    {
        auto lock = std::unique_lock { _mutex };
        _changed.wait(lock, [this] { return _parked; });
    }

    /// Lets the parked write through.
    void release()
    {
        {
            auto const _ = std::scoped_lock { _mutex };
            _armed = false;
        }
        _changed.notify_all();
    }

    [[nodiscard]] int write(std::string_view data) override
    {
        {
            auto lock = std::unique_lock { _mutex };
            _parked = _armed;
            _changed.notify_all();
            _changed.wait(lock, [this] { return !_armed; });
            _parked = false;
        }
        return MockPty::write(data);
    }

  private:
    std::mutex _mutex;
    std::condition_variable _changed;
    bool _armed = false;
    bool _parked = false;
};

/// A PTY whose reader waits for output that never comes -- a read parks until it is woken, or until its
/// timeout has passed -- and which has no room for the first write it is handed, as a PTY whose program
/// reads slowly.
class PushingBackPty final: public vtpty::MockPty
{
  public:
    using MockPty::MockPty;

    [[nodiscard]] std::optional<ReadResult> read(crispy::BufferObject<char>& /*storage*/,
                                                 std::optional<std::chrono::milliseconds> timeout,
                                                 size_t /*size*/) override
    {
        auto lock = std::unique_lock { _mutex };
        auto const woken = [this] {
            return _wakeups > 0;
        };
        if (timeout)
            std::ignore = _wake.wait_for(lock, *timeout, woken);
        else
            _wake.wait(lock, woken);
        if (_wakeups > 0)
            --_wakeups;
        errno = EAGAIN;
        return std::nullopt;
    }

    void wakeupReader() override
    {
        {
            auto const _ = std::scoped_lock { _mutex };
            ++_wakeups;
        }
        _wake.notify_all();
    }

    [[nodiscard]] int write(std::string_view data) override
    {
        auto const _ = std::scoped_lock { _mutex };
        if (!_pushedBack)
        {
            _pushedBack = true;
            errno = EAGAIN;
            return -1;
        }
        _written += data;
        return static_cast<int>(data.size());
    }

    /// @return What was written so far.
    [[nodiscard]] std::string written() const
    {
        auto const _ = std::scoped_lock { _mutex };
        return _written;
    }

  private:
    mutable std::mutex _mutex;
    std::condition_variable _wake;
    int _wakeups = 0;
    bool _pushedBack = false;
    std::string _written;
};
} // namespace

TEST_CASE("Terminal.a reply sent on the spot is written once the state lock is released",
          "[terminal][threading]")
{
    // Some sequences send their reply on the spot rather than with the next flush. Written inside the parse,
    // under the state lock, the reply parked the parsing thread once the application left its input unread,
    // and every thread waiting for the lock with it: a GUI's, a daemon's loop building a client's delta. The
    // parse's own hold owes the flush instead, and the same thread sends it once it has let go of the lock.
    struct OnTheSpot
    {
        std::string_view name;    ///< The sequence, for the section's name.
        std::string_view request; ///< What the application writes.
        std::string_view reply;   ///< What the reply contains.
    };
    auto constexpr Rows = std::array {
        OnTheSpot { .name = "DSR 996", .request = "\033[?996n", .reply = "\033[?997;" },
        OnTheSpot { .name = "GIP query", .request = "\033P!go=q\033\\", .reply = "\033P!gs=8," },
        OnTheSpot { .name = "DECRQLP", .request = "\033[0'|", .reply = "&w" },
        OnTheSpot { .name = "DECSET 2048", .request = "\033[?2048h", .reply = "\033[48;" },
    };
    for (auto const& row: Rows)
    {
        DYNAMIC_SECTION(row.name)
        {
            auto mc = MockTerm<LockProbingPty> { PageSize { LineCount(5), ColumnCount(20) } };
            mc.mockPty().probe(mc.terminal);
            mc.writeToScreen(row.request);
            CHECK(mc.mockPty().writesUnderLock() == 0);
            CHECK(mc.replyData().contains(row.reply));
        }
    }
}

TEST_CASE("Terminal.queueing a reply waits for no write in progress", "[terminal][threading]")
{
    // A flush keeps other flushes out across the PTY write, which parks while the application leaves its
    // input unread. A thread that only queues -- a daemon's loop thread reporting a resize -- waited for that
    // write as well, and every client of the daemon with it. Only another flush waits now.
    auto mc = MockTerm<ParkingPty> { PageSize { LineCount(5), ColumnCount(20) } };
    mc.terminal.reply("first");
    mc.mockPty().arm();
    auto flusher = std::jthread { [&mc] { mc.terminal.flushInput(); } };
    mc.mockPty().waitUntilParked();

    auto queued = std::async(std::launch::async, [&mc] { mc.terminal.reply("second"); });
    auto const status = queued.wait_for(std::chrono::seconds { 1 });
    mc.mockPty().release();
    flusher.join();
    queued.get();
    CHECK(status == std::future_status::ready);

    mc.terminal.flushInput();
    CHECK(mc.replyData() == "firstsecond");
}

TEST_CASE("Terminal.input the PTY pushed back on is written again by its reader", "[terminal][threading]")
{
    // Under InputWriter::ReaderThread only the PTY's reader writes, and a write the PTY had no room for
    // leaves the input queued. The reader writes it again after a short wait rather than after the next
    // output, which a program waiting for that input may never write.
    auto mc = MockTerm<PushingBackPty> { PageSize { LineCount(5), ColumnCount(20) } };
    mc.terminal.settings().inputWriter = vtbackend::InputWriter::ReaderThread;
    auto& pty = mc.mockPty();
    auto stop = std::atomic<bool> { false };
    auto reader = std::jthread { [&mc, &stop] {
        while (!stop.load() && mc.terminal.processInputOnce())
            mc.terminal.flushInput();
    } };
    mc.terminal.setReaderThread(reader.get_id());

    mc.terminal.sendRawInput("x");
    for ([[maybe_unused]] auto const tick: std::views::iota(0, 200))
    {
        if (pty.written() == "x")
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds { 10 });
    }
    // Read before the reader is stopped: the wakeup that stops it would let it write the byte as well.
    auto const written = pty.written();
    stop.store(true);
    pty.wakeupReader();
    reader.join();
    CHECK(written == "x");
}

namespace
{
/// A PTY whose program prints one full row Rows times, then falls silent: a read waits for its
/// timeout, as a real PTY's does, until the PTY closes.
class RowPrintingPty final: public vtpty::MockPty
{
  public:
    static constexpr auto Row = std::string_view { "PPPPPPPPPP\r\n" };
    static constexpr auto Rows = 1000;

    using MockPty::MockPty;

    [[nodiscard]] std::optional<ReadResult> read(crispy::BufferObject<char>& storage,
                                                 std::optional<std::chrono::milliseconds> timeout,
                                                 size_t /*size*/) override
    {
        auto lock = std::unique_lock { _mutex };
        if (_rowsLeft > 0)
        {
            --_rowsLeft;
            auto const _ = std::scoped_lock { storage };
            auto const pooled = storage.writeAtEnd(Row);
            return ReadResult { .data = std::string_view(pooled.data(), pooled.size()),
                                .fromStdoutFastPipe = false };
        }
        auto const closed = [this] {
            return _closed;
        };
        if (timeout)
            std::ignore = _wake.wait_for(lock, *timeout, closed);
        else
            _wake.wait(lock, closed);
        errno = EAGAIN;
        return std::nullopt;
    }

    void close() override
    {
        {
            auto const _ = std::scoped_lock { _mutex };
            _closed = true;
        }
        _wake.notify_all();
    }

    [[nodiscard]] bool isClosed() const noexcept override
    {
        auto const _ = std::scoped_lock { _mutex };
        return _closed;
    }

    [[nodiscard]] bool printedAll() const
    {
        auto const _ = std::scoped_lock { _mutex };
        return _rowsLeft == 0;
    }

  private:
    mutable std::mutex _mutex;
    std::condition_variable _wake;
    int _rowsLeft = Rows;
    bool _closed = false;
};
} // namespace

TEST_CASE("Terminal.writeToScreen from another thread keeps apart from what the reader reads",
          "[terminal][threading]")
{
    // The GUI writes to the screen (a notice, the WriteScreen action) while the session's reader reads the
    // PTY. Each must parse out of a buffer of its own: sharing the reader's, the GUI's text lands where the
    // reader's next read lands, and one overwrites the other's row.
    constexpr auto Rows = RowPrintingPty::Rows;
    constexpr auto WriterRow = std::string_view { "WWWWWWWWWW\r\n" };
    auto mc =
        MockTerm<RowPrintingPty> { PageSize { LineCount(5), ColumnCount(10) }, LineCount(2 * Rows), 1024 };
    auto& pty = mc.mockPty();
    auto reader = std::jthread { [&mc] {
        while (!mc.terminal.device().isClosed() && mc.terminal.processInputOnce())
            ;
    } };
    mc.terminal.setReaderThread(reader.get_id());

    for ([[maybe_unused]] auto const row: std::views::iota(0, Rows))
        mc.terminal.writeToScreen(WriterRow);
    for ([[maybe_unused]] auto const tick: std::views::iota(0, 500))
    {
        if (pty.printedAll())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds { 5 });
    }
    REQUIRE(pty.printedAll());
    pty.close();
    reader.join();

    auto const _ = std::scoped_lock { mc.terminal };
    auto const& grid = mc.terminal.primaryScreen().grid();
    auto const isWhole = [](std::string_view text) {
        return text == "PPPPPPPPPP" || text == "WWWWWWWWWW" || text == "          ";
    };
    for (auto const offset: std::views::iota(-grid.historyLineCount().as<int>(), 5))
    {
        auto const text = grid.lineText(LineOffset(offset));
        INFO(std::format("line {}: \"{}\"", offset, text));
        CHECK(isWhole(text));
    }
}

TEST_CASE("Terminal.DECSCUSR sets the cursor a frame is drawn with", "[terminal]")
{
    // What a program asks of the cursor reaches the render buffer, and the blink it asks for reaches the
    // terminal's display of the cursor, which the frame's blink follows.
    struct Row
    {
        std::string_view sequence;
        vtbackend::CursorShape shape;
        vtbackend::CursorDisplay display;
    };
    using vtbackend::CursorDisplay;
    using vtbackend::CursorShape;
    auto const rows = std::array {
        Row { .sequence = "\033[1 q", .shape = CursorShape::Block, .display = CursorDisplay::Blink },
        Row { .sequence = "\033[2 q", .shape = CursorShape::Block, .display = CursorDisplay::Steady },
        Row { .sequence = "\033[3 q", .shape = CursorShape::Underscore, .display = CursorDisplay::Blink },
        Row { .sequence = "\033[4 q", .shape = CursorShape::Underscore, .display = CursorDisplay::Steady },
        Row { .sequence = "\033[5 q", .shape = CursorShape::Bar, .display = CursorDisplay::Blink },
        Row { .sequence = "\033[6 q", .shape = CursorShape::Bar, .display = CursorDisplay::Steady },
    };
    for (auto const& row: rows)
    {
        INFO(core::escape(row.sequence));
        auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
        (void) mc.terminal.sendFocusInEvent();
        mc.writeToScreen(row.sequence);
        (void) mc.terminal.refreshRenderBuffer();
        auto const frame = mc.terminal.renderBuffer();
        REQUIRE(frame.get().cursor.has_value());
        CHECK(frame.get().cursor->shape == row.shape);
        CHECK(mc.terminal.cursorDisplay() == row.display);
    }
}

TEST_CASE("Terminal looks a printed path up in the filesystem it was given", "[terminal][path]")
{
    // Whether a path on the screen names a file is asked of the filesystem the terminal was constructed
    // with: the one file here exists only in memory, so only the injected filesystem can find it.
    auto const directory = std::filesystem::temp_directory_path() / "contour-in-memory-only" / "proj";
    auto const file = directory / "doc.txt";
    auto files = core::platform::testing::InMemoryFileSystem {};
    REQUIRE(files.createDirectories(directory).has_value());
    files.addFile(file, "x");
    REQUIRE_FALSE(std::filesystem::exists(file));

    auto events = vtbackend::Terminal::NullEvents {};
    auto const pageSize = PageSize { LineCount(2), ColumnCount(80) };
    auto settings = vtbackend::Settings {};
    settings.pageSize = pageSize;
    auto const environment = core::testing::FakeEnvironment {};
    auto terminal = vtbackend::Terminal { events,
                                          environment,
                                          files,
                                          std::make_unique<vtpty::MockPty>(pageSize),
                                          settings,
                                          std::nullopt,
                                          vtbackend::TerminalClocks::system(),
                                          vtbackend::randomSemanticBlockTokens() };

    terminal.setCurrentWorkingDirectory(fileUrlOf(directory));
    terminal.writeToScreen("open doc.txt now");

    // Hint mode looks paths up while it labels them, so it asks the filesystem itself.
    terminal.activateHintMode(
        vtbackend::HintModeRequest { .patterns = {}, .action = vtbackend::HintAction::Copy });
    auto const& matches = terminal.hintMatches();
    REQUIRE(matches.size() == 1);
    CHECK(matches[0].matchedText == core::pathToUtf8(file.lexically_normal()));
}
