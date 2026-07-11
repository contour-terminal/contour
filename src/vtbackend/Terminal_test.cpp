// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/CellUtil.h>
#include <vtbackend/HintModeHandler.h>
#include <vtbackend/MockTerm.h>
#include <vtbackend/Terminal.h>
#include <vtbackend/primitives.h>
#include <vtbackend/test_helpers.h>

#include <vtpty/MockPty.h>

#include <crispy/App.h>
#include <crispy/times.h>
#include <crispy/utils.h>

#include <libunicode/convert.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <string>
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
using vtbackend::SmoothScrollResult;

using namespace vtbackend::test;

// TODO: Test case posibilities:
//
// - [x] Synchronized output (?2026)
// - [x] Blinking cursor visiblity over time and on input events
// - [ ] double click word selection
// - [ ] triple click line selection
// - [ ] rectangular block selection
// - [ ] text selection with bypassing enabled application mouse protocol
// - [ ] extractLastMarkRange
// - [ ] scroll mark up
// - [ ] scroll mark down

// TODO: Writing text, leading to page-scroll properly updates viewport.
// TODO: Writing text, leading to page-scroll properly updates active selection.

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
    terminal.keyboardProtocol().enter(vtbackend::KeyboardEventFlag::ReportAllKeysAsEscapeCodes);

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
                              std::chrono::steady_clock::now());

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
                              std::chrono::steady_clock::now());

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
                                  std::chrono::steady_clock::now());

            CHECK(terminal.viewport().scrolled());
        }
    }
}

TEST_CASE("Terminal.localPathAtMousePosition", "[terminal]")
{
    namespace fs = std::filesystem;

    auto const tmpRoot =
        fs::temp_directory_path()
        / std::format("contour-local-path-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(tmpRoot / "nested");
    {
        auto file = std::ofstream(tmpRoot / "nested" / "file.txt");
        file << "test";
    }

    auto const cleanup = crispy::finally { [&]() { fs::remove_all(tmpRoot); } };
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

        auto const path = terminal.localPathAtMousePosition();
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

        auto const path = terminal.localPathAtMousePosition();
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
        fs::create_directories(shortDir);
        {
            auto file = std::ofstream(shortDir / "file.txt");
            file << "test";
        }
        auto const tildePath = (shortDir / "file.txt").generic_string();
        auto const expectedTildeFile = (shortDir / "file.txt").lexically_normal().string();

        auto mc = MockTerm { PageSize { LineCount(2), ColumnCount(240) } };
        auto& terminal = mc.terminal;
        mc.writeToScreen("open " + tildePath);

        terminal.sendMouseMoveEvent(Modifier::None,
                                    CellLocation { .line = LineOffset(0), .column = ColumnOffset(8) },
                                    PixelCoordinate,
                                    UiHandledHint);

        auto const path = terminal.localPathAtMousePosition();
        REQUIRE(path.has_value());
        CHECK(*path == expectedTildeFile);
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

        CHECK_FALSE(terminal.localPathAtMousePosition().has_value());
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

    SECTION("keypress always scrolls to bottom, even when autoScrollOnUpdate=false")
    {
        // User input must reveal the cursor regardless of the output-scroll setting: sending a key
        // snaps the viewport back to the bottom even though autoScrollOnUpdate (which governs
        // output-driven scrolling only) is disabled.
        terminal.settings().autoScrollOnUpdate = false;
        terminal.viewport().scrollUp(LineCount(2));
        REQUIRE(terminal.viewport().scrolled());

        terminal.sendKeyEvent(vtbackend::Key::Enter,
                              anyModifiers,
                              vtbackend::KeyboardEventType::Press,
                              std::chrono::steady_clock::now());

        CHECK(!terminal.viewport().scrolled());
    }

    SECTION("keypress honors autoScrollOnUpdate=true (default)")
    {
        REQUIRE(terminal.settings().autoScrollOnUpdate);
        terminal.viewport().scrollUp(LineCount(2));
        REQUIRE(terminal.viewport().scrolled());

        terminal.sendKeyEvent(vtbackend::Key::Enter,
                              anyModifiers,
                              vtbackend::KeyboardEventType::Press,
                              std::chrono::steady_clock::now());

        CHECK(!terminal.viewport().scrolled());
    }

    SECTION("char input always scrolls to bottom, even when autoScrollOnUpdate=false")
    {
        // Same input-independence as the keypress case above, exercised through the char path.
        terminal.settings().autoScrollOnUpdate = false;
        terminal.viewport().scrollUp(LineCount(2));
        REQUIRE(terminal.viewport().scrolled());

        terminal.sendCharEvent(
            U'a', 0, anyModifiers, vtbackend::KeyboardEventType::Press, std::chrono::steady_clock::now());

        CHECK(!terminal.viewport().scrolled());
    }

    SECTION("char input honors autoScrollOnUpdate=true")
    {
        REQUIRE(terminal.settings().autoScrollOnUpdate);
        terminal.viewport().scrollUp(LineCount(2));
        REQUIRE(terminal.viewport().scrolled());

        terminal.sendCharEvent(
            U'a', 0, anyModifiers, vtbackend::KeyboardEventType::Press, std::chrono::steady_clock::now());

        CHECK(!terminal.viewport().scrolled());
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
        terminal.sendKeyEvent(vtbackend::Key::UpArrow,
                              anyModifiers,
                              vtbackend::KeyboardEventType::Press,
                              std::chrono::steady_clock::now());
        REQUIRE(!terminal.viewport().scrolled());

        // The release generates input too, but must leave the viewport parked where the user put it.
        terminal.viewport().scrollUp(LineCount(2));
        REQUIRE(terminal.viewport().scrolled());
        auto const offsetBefore = terminal.viewport().scrollOffset();

        terminal.sendKeyEvent(vtbackend::Key::UpArrow,
                              anyModifiers,
                              vtbackend::KeyboardEventType::Release,
                              std::chrono::steady_clock::now());

        CHECK(terminal.viewport().scrolled());
        CHECK(terminal.viewport().scrollOffset() == offsetBefore);
    }

    SECTION("char release does not scroll to bottom even when it generates input")
    {
        terminal.setMode(vtbackend::DECMode::Win32InputMode, true);

        terminal.viewport().scrollUp(LineCount(2));
        REQUIRE(terminal.viewport().scrolled());
        auto const offsetBefore = terminal.viewport().scrollOffset();

        terminal.sendCharEvent(
            U'a', 0, anyModifiers, vtbackend::KeyboardEventType::Release, std::chrono::steady_clock::now());

        CHECK(terminal.viewport().scrolled());
        CHECK(terminal.viewport().scrollOffset() == offsetBefore);
    }

    SECTION("scrollbackBufferCleared (CSI 3 J) honors autoScrollOnUpdate=false")
    {
        terminal.settings().autoScrollOnUpdate = false;
        terminal.viewport().scrollUp(LineCount(2));
        REQUIRE(terminal.viewport().scrolled());
        auto const offsetBefore = terminal.viewport().scrollOffset();

        mc.writeToScreen("\x1b[3J");

        CHECK(terminal.viewport().scrollOffset() == offsetBefore);
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
        REQUIRE(terminal.settings().autoScrollOnUpdate);
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

    mock.writeToScreen(std::format("\033[>{};{}t", NoLogicalLines, NumberOfLinesToCapture));
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

TEST_CASE("Terminal.SynchronizedOutput", "[terminal]")
{
    constexpr auto BatchOn = "\033[?2026h"sv;
    constexpr auto BatchOff = "\033[?2026l"sv;

    auto const now = chrono::steady_clock::now();
    auto mc = MockTerm { ColumnCount(20), LineCount(1) };

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
                       Color foreground = DefaultColor(),
                       Color background = DefaultColor()) {
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

TEST_CASE("Terminal.UnderlineStyleClearing", "[terminal]")
{
    // Each subsequent underline style should clear the former if present.

    auto const now = chrono::steady_clock::now();
    auto mc = MockTerm { ColumnCount(20), LineCount(1) };

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

TEST_CASE("Terminal.CurlyUnderline", "[terminal]")
{
    auto const now = chrono::steady_clock::now();
    auto mc = MockTerm { ColumnCount(20), LineCount(1) };

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

TEST_CASE("Terminal.TextSelection", "[terminal]")
{
    // Create empty TE
    auto mock = MockTerm { ColumnCount(5), LineCount(5) };
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    mock.terminal.tick(ClockBase);
    mock.terminal.ensureFreshRenderBuffer();
    CHECK(trimmedTextScreenshot(mock).empty());

    // Fill main page with text
    mock.writeToScreen("12345\r\n"
                       "67890\r\n"
                       "ABCDE\r\n"
                       "abcde\r\n"
                       "fghij");

    mock.terminal.tick(ClockBase + chrono::seconds(1));
    mock.terminal.ensureFreshRenderBuffer();
    CHECK("12345\n67890\nABCDE\nabcde\nfghij" == trimmedTextScreenshot(mock));

    // Perform selection
    using namespace vtbackend;
    auto constexpr UiHandledHint = false;
    auto constexpr PixelCoordinate = vtbackend::PixelCoordinate {};

    mock.terminal.tick(1s);
    mock.terminal.sendMouseMoveEvent(
        Modifier::None, 1_lineOffset + 1_columnOffset, PixelCoordinate, UiHandledHint);

    mock.terminal.tick(1s);
    auto const appHandledMouse =
        mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, PixelCoordinate, UiHandledHint);

    // We want to ensure that this call is returning false if the app has not explicitly requested
    // to listen on mouse events (without passive mode being on).
    REQUIRE(appHandledMouse == Handled { false });

    CHECK(mock.terminal.selector()->state() == Selection::State::Waiting);

    // Mouse is pressed, but we did not start selecting (by moving the mouse) yet,
    // so any text extraction shall be empty.
    CHECK(mock.terminal.extractSelectionText().empty());

    mock.terminal.tick(1s);
    mock.terminal.sendMouseMoveEvent(
        Modifier::None, 2_lineOffset + 2_columnOffset, PixelCoordinate, UiHandledHint);
    CHECK(mock.terminal.extractSelectionText() == "7890\nABC");

    mock.terminal.tick(1s);
    mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoordinate, UiHandledHint);
    CHECK(mock.terminal.extractSelectionText() == "7890\nABC");

    // Clear selection by simply left-clicking.
    mock.terminal.tick(1s);
    mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, PixelCoordinate, UiHandledHint);
    mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoordinate, UiHandledHint);
    CHECK(mock.terminal.extractSelectionText().empty());
}

TEST_CASE("Terminal.TextSelection_wrapped_line", "[terminal]")
{
    // Create empty TE
    auto mock = MockTerm { ColumnCount(5), LineCount(2) };
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    mock.terminal.tick(ClockBase);
    mock.terminal.ensureFreshRenderBuffer();
    CHECK(trimmedTextScreenshot(mock).empty());

    // write one line with 10 a
    mock.writeToScreen(std::string(10, 'a'));

    mock.terminal.tick(ClockBase + chrono::seconds(1));
    mock.terminal.ensureFreshRenderBuffer();
    CHECK("aaaaa\naaaaa" == trimmedTextScreenshot(mock));

    // Perform selection
    using namespace vtbackend;
    auto constexpr UiHandledHint = false;
    auto constexpr PixelCoordinate = vtbackend::PixelCoordinate {};

    mock.terminal.tick(1s);
    mock.terminal.sendMouseMoveEvent(
        Modifier::None, 0_lineOffset + 1_columnOffset, PixelCoordinate, UiHandledHint);

    mock.terminal.tick(1s);
    auto const appHandledMouse =
        mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, PixelCoordinate, UiHandledHint);

    REQUIRE(appHandledMouse == Handled { false });

    CHECK(mock.terminal.selector()->state() == Selection::State::Waiting);

    CHECK(mock.terminal.extractSelectionText().empty());

    mock.terminal.tick(1s);
    mock.terminal.sendMouseMoveEvent(
        Modifier::None, 1_lineOffset + 1_columnOffset, PixelCoordinate, UiHandledHint);
    CHECK(mock.terminal.extractSelectionText() == "aaaaaa");

    mock.terminal.tick(1s);
    mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoordinate, UiHandledHint);
    CHECK(mock.terminal.extractSelectionText() == "aaaaaa");

    mock.terminal.tick(1s);
    mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, PixelCoordinate, UiHandledHint);
    mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoordinate, UiHandledHint);
    CHECK(mock.terminal.extractSelectionText().empty());
}

TEST_CASE("Terminal.ParsingBuffer", "[terminal]")
{
    // Test that parsingBuffer() returns the correct buffer during parsing.
    // When _parsingBuffer is not set, it should fall back to currentPtyBuffer().

    auto mock = MockTerm { ColumnCount { 10 }, LineCount { 3 } };
    auto& terminal = mock.terminal;

    // Initially, parsingBuffer() should return currentPtyBuffer() since _parsingBuffer is not set
    CHECK(terminal.parsingBuffer() == terminal.currentPtyBuffer());

    // Write some text - this will exercise the parsing path
    mock.writeToScreen("Hello");

    // After parsing completes, parsingBuffer() should still return currentPtyBuffer()
    // because _parsingBuffer is reset after each parse
    CHECK(terminal.parsingBuffer() == terminal.currentPtyBuffer());
}

TEST_CASE("Terminal.TrivialLineBufferIntegrity", "[terminal]")
{
    // Test that TrivialLineBuffer correctly stores text when written through terminal.
    // This tests the fast path where text is stored directly in a buffer_fragment.

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
    CHECK(line0.find("\xE2\x94\x82") != std::string::npos); // │
    CHECK(line1.find("\xE2\x94\x9C") != std::string::npos); // ├
    CHECK(line0.find("file") != std::string::npos);
    CHECK(line1.find("dir") != std::string::npos);
}

TEST_CASE("Terminal.smoothScrollExtraLines.zero_when_no_offset", "[terminal]")
{
    auto mc = MockTerm { ColumnCount { 10 }, LineCount { 4 } };
    auto& terminal = mc.terminal;
    CHECK(*terminal.smoothScrollExtraLines() == 0);
}

TEST_CASE("Terminal.smoothScrollExtraLines.one_when_offset_nonzero", "[terminal]")
{
    auto mc = MockTerm { ColumnCount { 10 }, LineCount { 4 } };
    auto& terminal = mc.terminal;
    terminal.viewport().setPixelOffset(5.0f);
    CHECK(*terminal.smoothScrollExtraLines() == 1);
}

TEST_CASE("Terminal.screenTransitionProgress.no_transition_returns_1", "[terminal]")
{
    auto mc = MockTerm { ColumnCount { 10 }, LineCount { 4 } };
    auto& terminal = mc.terminal;
    // No transition active, should return 1.0 (complete).
    CHECK(terminal.screenTransitionProgress() == 1.0f);
    CHECK_FALSE(terminal.isScreenTransitionActive());
}

TEST_CASE("Terminal.cursorAnimationProgress.no_animation_returns_1", "[terminal]")
{
    auto mc = MockTerm { ColumnCount { 10 }, LineCount { 4 } };
    auto& terminal = mc.terminal;
    // With no animation, cursor at current position should return 1.0 (complete).
    auto const gridPos = terminal.currentScreen().cursor().position;
    CHECK(terminal.cursorAnimationProgress(gridPos) == 1.0f);
}

// {{{ applySmoothScrollPixelDelta tests

TEST_CASE("Terminal.applySmoothScrollPixelDelta.accumulates_subline_offset", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    // Write enough lines to generate history.
    for (auto i = 0; i < 14; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // A delta smaller than one cell height should only accumulate pixel offset.
    auto const result = terminal.applySmoothScrollPixelDelta(5.0f);
    CHECK(result == SmoothScrollResult::Applied);
    CHECK(terminal.smoothScrollPixelOffset() == 5.0f);
    CHECK(terminal.viewport().scrollOffset().value == 0);
}

TEST_CASE("Terminal.applySmoothScrollPixelDelta.converts_full_cell_to_scroll", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 14; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    auto const cellHeight = terminal.cellPixelSize().height.as<float>();
    auto const result = terminal.applySmoothScrollPixelDelta(cellHeight + 3.0f);
    CHECK(result == SmoothScrollResult::Applied);
    CHECK(terminal.viewport().scrollOffset().value == 1);
    CHECK(terminal.smoothScrollPixelOffset() == Catch::Approx(3.0f));
}

TEST_CASE("Terminal.applySmoothScrollPixelDelta.clamps_at_top_of_history", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 14; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Apply a delta much larger than all available history.
    auto const result = terminal.applySmoothScrollPixelDelta(100000.0f);
    CHECK(result == SmoothScrollResult::Applied);
    // Scroll offset should be clamped to max history.
    auto const maxOffset = terminal.primaryScreen().historyLineCount();
    CHECK(terminal.viewport().scrollOffset().value == maxOffset.as<int>());
    CHECK(terminal.smoothScrollPixelOffset() == 0.0f);
}

TEST_CASE("Terminal.applySmoothScrollPixelDelta.returns_disabled_on_alternate_screen", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Switch to alternate screen.
    mc.writeToScreen("\033[?1049h");
    CHECK(terminal.isAlternateScreen());

    auto const result = terminal.applySmoothScrollPixelDelta(10.0f);
    CHECK(result == SmoothScrollResult::Disabled);
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

// }}}
// {{{ momentum scroll tests

TEST_CASE("Terminal.momentumScroll.starts_on_end_with_velocity", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 8; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Simulate touchpad gesture: Begin, several Updates, then End.
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 30.0f, ClockBase + 10ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 30.0f, ClockBase + 20ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 30.0f, ClockBase + 30ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 40ms);

    CHECK(terminal.isMomentumScrollActive());
}

TEST_CASE("Terminal.momentumScroll.velocity_computation_is_correct", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 20 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 20; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // 3 Updates each 10ms apart, each with 20px delta.
    // Expected velocity = (20 + 20) / (30ms - 10ms) = 40 / 0.02 = 2000 px/s
    // (The oldest sample's delta is excluded from the sum.)
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 20.0f, ClockBase + 10ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 20.0f, ClockBase + 20ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 20.0f, ClockBase + 30ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 40ms);

    REQUIRE(terminal.isMomentumScrollActive());

    // Apply one tick at ~16ms after End and verify the scroll offset advanced.
    // With v=2000 px/s and dt=16ms: pixelDelta = 2000 * 0.016 = 32px.
    // With cellHeight=20, that's 1 full line + 12px remainder.
    auto const offsetBefore = terminal.viewport().scrollOffset().value;
    terminal.tick(ClockBase + 56ms);
    auto const offsetAfter = terminal.viewport().scrollOffset().value;
    CHECK(offsetAfter > offsetBefore);
}

TEST_CASE("Terminal.momentumScroll.no_start_below_threshold", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 8; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Very small, slow deltas — velocity below startThreshold (50 px/s).
    // velocity = 0.5 / 0.1 = 5 px/s
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 0.5f, ClockBase + 100ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 0.5f, ClockBase + 200ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 300ms);

    CHECK_FALSE(terminal.isMomentumScrollActive());
}

TEST_CASE("Terminal.momentumScroll.no_start_with_single_sample", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 8; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Only a single Update — VelocityTracker needs at least 2 samples.
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 100.0f, ClockBase + 10ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 20ms);

    CHECK_FALSE(terminal.isMomentumScrollActive());
}

TEST_CASE("Terminal.momentumScroll.decelerates_over_ticks", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 20 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 20; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Start momentum with fast gesture.
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 10ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 20ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 30ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 40ms);

    REQUIRE(terminal.isMomentumScrollActive());

    // Track scroll offset + pixel offset through several frames.
    auto const offsetAfterStart = terminal.viewport().scrollOffset().value;
    auto const pixelOffsetAfterStart = terminal.smoothScrollPixelOffset();

    // First frame of momentum.
    terminal.tick(ClockBase + 56ms);
    auto const offset1 = terminal.viewport().scrollOffset().value;
    auto const pixel1 = terminal.smoothScrollPixelOffset();

    // Second frame.
    terminal.tick(ClockBase + 72ms);
    auto const offset2 = terminal.viewport().scrollOffset().value;
    auto const pixel2 = terminal.smoothScrollPixelOffset();

    // Third frame.
    terminal.tick(ClockBase + 88ms);
    auto const offset3 = terminal.viewport().scrollOffset().value;

    // Scroll offset should advance (or pixel accumulate) over time.
    auto const totalScroll1 = static_cast<float>(offset1) * 20.0f + pixel1;
    auto const totalScroll0 = static_cast<float>(offsetAfterStart) * 20.0f + pixelOffsetAfterStart;
    CHECK(totalScroll1 > totalScroll0);

    // Overall scroll should only increase (deceleration, not reversal).
    auto const totalScroll2 = static_cast<float>(offset2) * 20.0f + pixel2;
    CHECK(totalScroll2 >= totalScroll1);

    // Scroll offset should have advanced by at least one line after several ticks.
    CHECK(offset3 >= offsetAfterStart);
}

TEST_CASE("Terminal.momentumScroll.stops_at_min_velocity", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 8; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 30.0f, ClockBase + 10ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 30.0f, ClockBase + 20ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 30ms);

    REQUIRE(terminal.isMomentumScrollActive());

    // Advance time far enough for velocity to decay below threshold.
    // With FrictionDecayPerSecond=0.05 and velocity ~3000 px/s, after ~2.5s velocity ≈ 3.7 px/s < 10.
    terminal.tick(ClockBase + 3030ms);

    CHECK_FALSE(terminal.isMomentumScrollActive());
}

TEST_CASE("Terminal.momentumScroll.cancelled_by_begin", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 8; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Start momentum.
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 10ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 20ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 30ms);

    REQUIRE(terminal.isMomentumScrollActive());

    // Let momentum run for one tick.
    terminal.tick(ClockBase + 46ms);
    REQUIRE(terminal.isMomentumScrollActive());

    // New gesture begins — should cancel momentum.
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase + 50ms);

    CHECK_FALSE(terminal.isMomentumScrollActive());
}

// {{{ Wheel-glide (smooth mouse-wheel scrolling)

namespace
{
/// Total scroll displacement in pixels: whole-line offset plus the sub-cell pixel remainder.
[[nodiscard]] float totalScrollPixels(vtbackend::Terminal const& terminal, float cellHeight) noexcept
{
    return static_cast<float>(terminal.viewport().scrollOffset().value) * cellHeight
           + terminal.smoothScrollPixelOffset();
}
} // namespace

TEST_CASE("Terminal.wheelGlide.single_notch_glides_over_frames", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 40 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 30; ++i)
        mc.writeToScreen("line\r\n");

    auto constexpr CellHeight = 20.0f;
    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // One notch worth of pixels: 9 lines * 20px = 180px (matches helper.cpp's angle-to-pixel math).
    auto constexpr NotchPixels = 180.0f;
    terminal.injectWheelMomentum(NotchPixels, ClockBase);
    REQUIRE(terminal.isMomentumScrollActive());

    // Frame 1: the glide must NOT jump the full notch distance instantly.
    terminal.tick(ClockBase + 16ms);
    auto const afterFrame1 = totalScrollPixels(terminal, CellHeight);
    CHECK(afterFrame1 > 0.0f);
    CHECK(afterFrame1 < NotchPixels);

    // Subsequent frames advance monotonically toward the target.
    terminal.tick(ClockBase + 33ms);
    auto const afterFrame2 = totalScrollPixels(terminal, CellHeight);
    CHECK(afterFrame2 >= afterFrame1);

    // After enough time the glide settles near the intended distance and stops.
    for (auto t = 50; t <= 600; t += 16)
        terminal.tick(ClockBase + chrono::milliseconds { t });

    auto const settled = totalScrollPixels(terminal, CellHeight);
    CHECK_FALSE(terminal.isMomentumScrollActive());
    // Lands within one line of the intended distance (tuned impulse, not floaty).
    CHECK(settled == Catch::Approx(NotchPixels).margin(CellHeight));
}

TEST_CASE("Terminal.wheelGlide.rapid_notches_accumulate", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 60 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 50; ++i)
        mc.writeToScreen("line\r\n");

    auto constexpr CellHeight = 20.0f;
    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Two notches in quick succession accumulate into one longer glide.
    terminal.injectWheelMomentum(180.0f, ClockBase);
    terminal.injectWheelMomentum(180.0f, ClockBase + 8ms);
    REQUIRE(terminal.isMomentumScrollActive());

    for (auto t = 16; t <= 800; t += 16)
        terminal.tick(ClockBase + chrono::milliseconds { t });

    auto const settled = totalScrollPixels(terminal, CellHeight);
    CHECK_FALSE(terminal.isMomentumScrollActive());
    // Both notches contribute; total lands near their sum (within a line).
    CHECK(settled == Catch::Approx(360.0f).margin(CellHeight));
}

TEST_CASE("Terminal.wheelGlide.direction_reversal", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 60 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 50; ++i)
        mc.writeToScreen("line\r\n");

    auto constexpr CellHeight = 20.0f;
    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Scroll up, then reverse before the first glide settles: velocity is signed-accumulated.
    terminal.injectWheelMomentum(180.0f, ClockBase);
    terminal.tick(ClockBase + 8ms);
    terminal.injectWheelMomentum(-120.0f, ClockBase + 12ms);

    for (auto t = 20; t <= 800; t += 16)
        terminal.tick(ClockBase + chrono::milliseconds { t });

    auto const settled = totalScrollPixels(terminal, CellHeight);
    CHECK_FALSE(terminal.isMomentumScrollActive());
    // Net displacement trends toward +60px (up), never overshooting far past it.
    CHECK(settled > 0.0f);
    CHECK(settled < 180.0f);
}

TEST_CASE("Terminal.wheelGlide.clamps_at_history_top", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 20 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 12; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    auto const historyLines = terminal.primaryScreen().historyLineCount().as<int>();
    REQUIRE(historyLines > 0);

    // Inject far more than the scrollable distance; the glide must stop at the wall, not spin.
    terminal.injectWheelMomentum(static_cast<float>(historyLines + 20) * 20.0f, ClockBase);

    for (auto t = 16; t <= 800; t += 16)
        terminal.tick(ClockBase + chrono::milliseconds { t });

    CHECK(terminal.viewport().scrollOffset().value == historyLines);
    CHECK(terminal.smoothScrollPixelOffset() == 0.0f);
    CHECK_FALSE(terminal.isMomentumScrollActive());
}

TEST_CASE("Terminal.wheelGlide.inactive_on_alt_screen", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 20 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Switch to alternate screen; the wheel glide must not arm there.
    mc.writeToScreen("\033[?1049h");
    REQUIRE(terminal.isAlternateScreen());

    auto const offsetBefore = terminal.viewport().scrollOffset().value;
    terminal.injectWheelMomentum(180.0f, ClockBase);

    CHECK_FALSE(terminal.isMomentumScrollActive());
    CHECK(terminal.viewport().scrollOffset().value == offsetBefore);
}

TEST_CASE("Terminal.wheelGlide.cancelled_by_reset", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 20 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 12; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    terminal.injectWheelMomentum(180.0f, ClockBase);
    REQUIRE(terminal.isMomentumScrollActive());

    // resetSmoothScroll (called on resize / page switch / scroll-to-bottom / new output) cancels it.
    terminal.resetSmoothScroll();
    CHECK_FALSE(terminal.isMomentumScrollActive());
}

TEST_CASE("Terminal.wheelGlide.gated_on_smoothScrolling_only", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 20 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 12; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Independent of momentumScrolling: with momentum off but smooth on, the wheel still glides.
    terminal.settings().smoothScrolling = true;
    terminal.settings().momentumScrolling = false;
    terminal.injectWheelMomentum(180.0f, ClockBase);
    CHECK(terminal.isMomentumScrollActive());
    terminal.cancelMomentumScroll();

    // With smooth scrolling off, the wheel glide does not arm (legacy line path handles it).
    terminal.settings().smoothScrolling = false;
    terminal.injectWheelMomentum(180.0f, ClockBase);
    CHECK_FALSE(terminal.isMomentumScrollActive());
}

TEST_CASE("Terminal.wheelGlide.nextRender_schedules_while_active", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 40 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 30; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    terminal.injectWheelMomentum(180.0f, ClockBase);
    terminal.tick(ClockBase + 16ms);
    REQUIRE(terminal.isMomentumScrollActive());

    // While a glide is active, nextRender() schedules a frame at (or below) the refresh interval.
    auto const wakeup = terminal.nextRender();
    REQUIRE(wakeup.has_value());
    CHECK(wakeup->count() > 0);
}

TEST_CASE("Terminal.wheelGlide.opposing_notches_do_not_spin_forever", "[terminal]")
{
    // Regression: two exactly-opposing notches delivered before a frame tick accumulate to a net
    // velocity of exactly 0.0f. Arming at velocity 0 would anchor the fraction-of-seed stop
    // threshold to 0, so shouldStop() could never fire — the glide would stay active forever and
    // keep waking the render loop (continuous CPU/battery drain). It must instead refuse to arm.
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 40 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 30; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // First notch arms a glide; the exactly-opposing second notch (same magnitude, same time point,
    // no tick() in between) drives the accumulated velocity to precisely 0.
    REQUIRE(terminal.injectWheelMomentum(180.0f, ClockBase) == SmoothScrollResult::Applied);
    REQUIRE(terminal.isMomentumScrollActive());
    auto const result = terminal.injectWheelMomentum(-180.0f, ClockBase);

    // The degenerate notch neither arms nor leaves the previous glide running; the caller is told to
    // fall through to the legacy line path.
    CHECK(result == SmoothScrollResult::InvalidCellSize);
    CHECK_FALSE(terminal.isMomentumScrollActive());

    // Advance well past any plausible glide duration: still inactive, and nextRender() no longer
    // requests wake-ups on account of momentum.
    for (auto t = 16; t <= 2000; t += 16)
        terminal.tick(ClockBase + chrono::milliseconds { t });
    CHECK_FALSE(terminal.isMomentumScrollActive());
}

TEST_CASE("Terminal.wheelGlide.reports_result_for_caller_fallthrough", "[terminal]")
{
    // The SmoothScrollResult return value is the seam helper.cpp uses to decide whether to consume
    // the wheel notch (Applied) or fall through to the legacy line-based scroll (anything else).
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 20 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 12; ++i)
        mc.writeToScreen("line\r\n");

    SECTION("unknown cell size (not laid out yet) -> InvalidCellSize, no arm")
    {
        // No setCellPixelSize(): the display has not laid out, so cell height is still 0.
        REQUIRE(terminal.cellPixelSize().height.as<int>() == 0);
        CHECK(terminal.injectWheelMomentum(180.0f, ClockBase) == SmoothScrollResult::InvalidCellSize);
        CHECK_FALSE(terminal.isMomentumScrollActive());
    }

    SECTION("smooth scrolling disabled -> Disabled, no arm")
    {
        terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });
        terminal.settings().smoothScrolling = false;
        CHECK(terminal.injectWheelMomentum(180.0f, ClockBase) == SmoothScrollResult::Disabled);
        CHECK_FALSE(terminal.isMomentumScrollActive());
    }

    SECTION("alternate screen -> Disabled, no arm")
    {
        terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });
        mc.writeToScreen("\033[?1049h");
        REQUIRE(terminal.isAlternateScreen());
        CHECK(terminal.injectWheelMomentum(180.0f, ClockBase) == SmoothScrollResult::Disabled);
        CHECK_FALSE(terminal.isMomentumScrollActive());
    }

    SECTION("healthy notch -> Applied, arms a glide")
    {
        terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });
        CHECK(terminal.injectWheelMomentum(180.0f, ClockBase) == SmoothScrollResult::Applied);
        CHECK(terminal.isMomentumScrollActive());
    }
}

TEST_CASE("Terminal.momentumScroll.stray_update_cancels_active_glide", "[terminal]")
{
    // Regression: a touchpad Update phase arriving while a wheel glide is still in flight, without a
    // preceding Begin (the only phase that used to cancel momentum), would let BOTH the immediate
    // apply of the Update and the live glide move the viewport -> double-scroll. The first sample of
    // a fresh gesture (tracker still empty) must cancel any active momentum first.
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 40 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 30; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Arm a wheel glide, let it run for a frame so it is genuinely mid-flight.
    REQUIRE(terminal.injectWheelMomentum(180.0f, ClockBase) == SmoothScrollResult::Applied);
    terminal.tick(ClockBase + 16ms);
    REQUIRE(terminal.isMomentumScrollActive());

    // A stray Update (no Begin) is the first sample of a new gesture -> it must cancel the glide.
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 40.0f, ClockBase + 24ms);
    CHECK_FALSE(terminal.isMomentumScrollActive());
}

// }}} Wheel-glide (smooth mouse-wheel scrolling)

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

TEST_CASE("Terminal.momentumScroll.cancelled_by_resize", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 8; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 10ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 20ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 30ms);

    REQUIRE(terminal.isMomentumScrollActive());

    // Resize cancels momentum via resetSmoothScroll().
    terminal.resizeScreen(PageSize { LineCount { 5 }, ColumnCount { 10 } });

    CHECK_FALSE(terminal.isMomentumScrollActive());
}

TEST_CASE("Terminal.momentumScroll.disabled_when_setting_off", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 8; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Disable momentum scrolling.
    terminal.settings().momentumScrolling = false;

    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 10ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 20ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 30ms);

    CHECK_FALSE(terminal.isMomentumScrollActive());
}

TEST_CASE("Terminal.momentumScroll.disabled_when_smooth_scrolling_off", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 8; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Momentum requires smooth scrolling to be enabled.
    terminal.settings().smoothScrolling = false;

    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 10ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 20ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 30ms);

    CHECK_FALSE(terminal.isMomentumScrollActive());
}

TEST_CASE("Terminal.momentumScroll.noPhase_never_triggers_momentum", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 8; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Mouse wheel events have NoPhase — should never trigger momentum.
    terminal.handleScrollPhase(vtbackend::ScrollPhase::NoPhase, 100.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::NoPhase, 100.0f, ClockBase + 10ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::NoPhase, 100.0f, ClockBase + 20ms);

    CHECK_FALSE(terminal.isMomentumScrollActive());
}

TEST_CASE("Terminal.momentumScroll.nextRender_schedules_during_active", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 8; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 10ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 20ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 30ms);

    REQUIRE(terminal.isMomentumScrollActive());

    // nextRender should return a value during active momentum.
    auto const next = terminal.nextRender();
    CHECK(next.has_value());
}

TEST_CASE("Terminal.momentumScroll.repeated_gestures_work_independently", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 30 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 30; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // --- First gesture: scroll up into history ---
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 40.0f, ClockBase + 10ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 40.0f, ClockBase + 20ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 40.0f, ClockBase + 30ms);
    // Apply the scroll deltas.
    for (auto i = 0; i < 3; ++i)
        terminal.applySmoothScrollPixelDelta(40.0f);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 40ms);

    REQUIRE(terminal.isMomentumScrollActive());
    auto const offsetAfterFirstEnd = terminal.viewport().scrollOffset().value;

    // Let momentum run for a bit.
    terminal.tick(ClockBase + 56ms);
    terminal.tick(ClockBase + 72ms);
    auto const offsetAfterFirstMomentum = terminal.viewport().scrollOffset().value;
    CHECK(offsetAfterFirstMomentum >= offsetAfterFirstEnd);
    REQUIRE(terminal.isMomentumScrollActive());

    // --- Second gesture: Begin cancels first momentum ---
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase + 100ms);
    CHECK_FALSE(terminal.isMomentumScrollActive());

    auto const offsetBeforeSecond = terminal.viewport().scrollOffset().value;

    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 60.0f, ClockBase + 110ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 60.0f, ClockBase + 120ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 60.0f, ClockBase + 130ms);
    for (auto i = 0; i < 3; ++i)
        terminal.applySmoothScrollPixelDelta(60.0f);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 140ms);

    // Second gesture should start its own momentum.
    REQUIRE(terminal.isMomentumScrollActive());

    // Momentum from second gesture should still scroll further.
    terminal.tick(ClockBase + 156ms);
    auto const offsetAfterSecondMomentum = terminal.viewport().scrollOffset().value;
    CHECK(offsetAfterSecondMomentum >= offsetBeforeSecond);
}

TEST_CASE("Terminal.momentumScroll.rapid_repeated_gestures", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 50 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 50; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Simulate 3 rapid gestures in quick succession without letting momentum settle.
    auto t = ClockBase;
    for (auto gesture = 0; gesture < 3; ++gesture)
    {
        terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, t);
        if (gesture > 0)
            CHECK_FALSE(terminal.isMomentumScrollActive()); // Begin cancels previous momentum.

        terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 30.0f, t + 8ms);
        terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 30.0f, t + 16ms);
        terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 30.0f, t + 24ms);
        for (auto i = 0; i < 3; ++i)
            terminal.applySmoothScrollPixelDelta(30.0f);
        terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, t + 32ms);

        CHECK(terminal.isMomentumScrollActive());

        // Let one tick of momentum run before next gesture.
        t += 50ms;
        terminal.tick(t);
    }

    // After all three gestures, momentum from the last one should still be active.
    CHECK(terminal.isMomentumScrollActive());

    // Scroll should have advanced into history.
    CHECK(terminal.viewport().scrollOffset().value > 0);
}

TEST_CASE("Terminal.momentumScroll.scroll_position_advances_correctly", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 40 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 40; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Known velocity: 3 Updates, 40px each, 10ms apart.
    // velocity = (40 + 40) / 0.02 = 4000 px/s
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 40.0f, ClockBase + 10ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 40.0f, ClockBase + 20ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 40.0f, ClockBase + 30ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 40ms);

    REQUIRE(terminal.isMomentumScrollActive());

    // Run momentum frames at 16ms intervals until it stops.
    auto lastActive = true;
    auto frameCount = 0;
    for (auto t = ClockBase + 56ms; lastActive && frameCount < 200; t += 16ms, ++frameCount)
    {
        terminal.tick(t);
        lastActive = terminal.isMomentumScrollActive();
    }

    // Momentum should have eventually stopped.
    CHECK_FALSE(terminal.isMomentumScrollActive());

    // Should have scrolled at least several lines (4000 px/s is a brisk swipe).
    CHECK(terminal.viewport().scrollOffset().value > 0);
}

TEST_CASE("Terminal.momentumScroll.cancelled_by_alternate_screen", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 8; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 10ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 20ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 30ms);

    REQUIRE(terminal.isMomentumScrollActive());

    // Switch to alternate screen (e.g. vim) — cancels momentum via resetSmoothScroll.
    mc.writeToScreen("\033[?1049h");
    CHECK(terminal.isAlternateScreen());
    CHECK_FALSE(terminal.isMomentumScrollActive());
}

// }}}
// {{{ cursor motion animation tests

TEST_CASE("Terminal.cursorMotionAnimation.starts_on_position_change", "[terminal]")
{
    auto mc = MockTerm { ColumnCount { 20 }, LineCount { 4 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();

    // Ensure animation is enabled (default is 80ms).
    REQUIRE(terminal.settings().cursorMotionAnimationDuration.count() > 0);

    // Tick far enough from epoch so the refresh interval (41ms) is satisfied.
    terminal.tick(ClockBase + 100ms);
    terminal.ensureFreshRenderBuffer();

    // Move cursor by writing a character.
    mc.writeToScreen("A");
    terminal.tick(ClockBase + 200ms);
    terminal.ensureFreshRenderBuffer();

    auto const renderBuffer = terminal.renderBuffer();
    REQUIRE(renderBuffer.get().cursor.has_value());
    auto const& cursor = *renderBuffer.get().cursor;
    CHECK(cursor.animateFrom.has_value());
    CHECK(cursor.animationProgress < 1.0f);
}

TEST_CASE("Terminal.cursorMotionAnimation.chains_midanimation", "[terminal]")
{
    auto mc = MockTerm { ColumnCount { 20 }, LineCount { 4 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();

    REQUIRE(terminal.settings().cursorMotionAnimationDuration.count() > 0);

    // Tick far enough from epoch so the refresh interval is satisfied.
    terminal.tick(ClockBase + 100ms);
    terminal.ensureFreshRenderBuffer();

    // Move cursor (start first animation).
    mc.writeToScreen("A");
    terminal.tick(ClockBase + 200ms);
    terminal.ensureFreshRenderBuffer();

    auto fromAfterFirst = std::optional<vtbackend::CellLocation> {};
    {
        auto const buf1 = terminal.renderBuffer();
        REQUIRE(buf1.get().cursor.has_value());
        fromAfterFirst = buf1.get().cursor->animateFrom;
        REQUIRE(fromAfterFirst.has_value());
    } // Release RenderBufferRef lock before next render cycle.

    // Tick partway through animation (40ms into default 80ms).
    terminal.tick(ClockBase + 240ms);

    // Chain: move cursor again while animation is still in progress.
    mc.writeToScreen("B");
    terminal.tick(ClockBase + 300ms);
    terminal.ensureFreshRenderBuffer();

    auto const buf2 = terminal.renderBuffer();
    REQUIRE(buf2.get().cursor.has_value());
    auto const& cursor = *buf2.get().cursor;

    // The new animateFrom should be an interpolated position (not the original from-position).
    CHECK(cursor.animateFrom.has_value());
    CHECK(cursor.animationProgress < 1.0f);
    // The chained from-position should differ from the first animation's from-position,
    // because it was computed from the interpolated mid-animation point.
    CHECK(cursor.animateFrom != fromAfterFirst);
}

// }}}
// {{{ screen transition fade tests

TEST_CASE("Terminal.screenTransition.activates_on_screen_switch", "[terminal]")
{
    auto mc = MockTerm { ColumnCount { 10 }, LineCount { 4 } };
    auto& terminal = mc.terminal;

    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    // Ensure fade transition is configured.
    terminal.settings().screenTransitionStyle = vtbackend::ScreenTransitionStyle::Fade;
    terminal.settings().screenTransitionDuration = 200ms;

    // Write some text to the primary screen.
    mc.writeToScreen("Hello");
    terminal.tick(ClockBase + 100ms);
    terminal.ensureFreshRenderBuffer();

    // Switch to alternate screen.
    mc.writeToScreen("\033[?1049h");

    CHECK(terminal.isScreenTransitionActive());
}

TEST_CASE("Terminal.screenTransition.fades_out_blends_to_background", "[terminal]")
{
    auto mc = MockTerm { ColumnCount { 10 }, LineCount { 4 } };
    auto& terminal = mc.terminal;

    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    terminal.settings().screenTransitionStyle = vtbackend::ScreenTransitionStyle::Fade;
    terminal.settings().screenTransitionDuration = 200ms;

    // Write text so there are non-trivial cells to blend.
    mc.writeToScreen("Hello");
    terminal.tick(ClockBase + 100ms);
    terminal.ensureFreshRenderBuffer();

    // Switch to alternate screen, starting the transition.
    // setScreen() records _currentTime (ClockBase + 100ms) as startTime.
    mc.writeToScreen("\033[?1049h");
    REQUIRE(terminal.isScreenTransitionActive());

    // Tick to 50ms past startTime (ClockBase + 150ms), i.e. 25% of the 200ms duration.
    terminal.tick(ClockBase + 150ms);
    terminal.ensureFreshRenderBuffer();

    // The transition is still active and in fade-out phase.
    auto const progress = terminal.screenTransitionProgress();
    CHECK(progress > 0.0f);
    CHECK(progress < 0.5f);
}

TEST_CASE("Terminal.screenTransition.fadeout_cell_colors_blend_toward_background", "[terminal]")
{
    auto mc = MockTerm { ColumnCount { 10 }, LineCount { 4 } };
    auto& terminal = mc.terminal;

    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    terminal.settings().screenTransitionStyle = vtbackend::ScreenTransitionStyle::Fade;
    terminal.settings().screenTransitionDuration = 200ms;

    // Set a known foreground color via SGR so snapshot cells have non-default foreground.
    // ESC[38;2;255;0;0m sets foreground to bright red.
    mc.writeToScreen("\033[38;2;255;0;0mHello");
    terminal.tick(ClockBase + 100ms);
    terminal.ensureFreshRenderBuffer();

    // Capture the pre-transition foreground color of the first rendered cell.
    auto preFg = vtbackend::RGBColor {};
    {
        auto const buf = terminal.renderBuffer();
        REQUIRE(!buf.get().cells.empty());
        preFg = buf.get().cells.front().attributes.foregroundColor;
    }
    // The foreground should be close to red (255, 0, 0).
    REQUIRE(preFg.red > 200);

    auto const defaultBg = terminal.colorPalette().defaultBackground;

    // Switch to alternate screen, starting the fade transition.
    mc.writeToScreen("\033[?1049h");
    REQUIRE(terminal.isScreenTransitionActive());

    // Tick to 25% of the 200ms duration (fade-out phase: progress < 0.5).
    // At 25% overall, the fade-out factor is 0.5 (progress * 2).
    terminal.tick(ClockBase + 150ms);
    terminal.ensureFreshRenderBuffer();

    auto const progress = terminal.screenTransitionProgress();
    REQUIRE(progress > 0.0f);
    REQUIRE(progress < 0.5f);

    auto const buf = terminal.renderBuffer();
    REQUIRE(!buf.get().cells.empty());

    auto const& blendedFg = buf.get().cells.front().attributes.foregroundColor;

    // During fade-out, the foreground should be blended toward defaultBg.
    // The red channel should have decreased from the original value toward defaultBg.red.
    // The green/blue channels should have moved toward defaultBg.green/blue.
    if (preFg.red > defaultBg.red)
        CHECK(blendedFg.red < preFg.red);
    else
        CHECK(blendedFg.red > preFg.red);

    // Verify that the blended color is between the original and the default background.
    auto const isRedBetween = (blendedFg.red >= std::min(preFg.red, defaultBg.red))
                              && (blendedFg.red <= std::max(preFg.red, defaultBg.red));
    auto const isGreenBetween = (blendedFg.green >= std::min(preFg.green, defaultBg.green))
                                && (blendedFg.green <= std::max(preFg.green, defaultBg.green));
    auto const isBlueBetween = (blendedFg.blue >= std::min(preFg.blue, defaultBg.blue))
                               && (blendedFg.blue <= std::max(preFg.blue, defaultBg.blue));
    CHECK(isRedBetween);
    CHECK(isGreenBetween);
    CHECK(isBlueBetween);
}

TEST_CASE("Terminal.screenTransition.finalizes_after_duration", "[terminal]")
{
    auto mc = MockTerm { ColumnCount { 10 }, LineCount { 4 } };
    auto& terminal = mc.terminal;

    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    terminal.settings().screenTransitionStyle = vtbackend::ScreenTransitionStyle::Fade;
    terminal.settings().screenTransitionDuration = 200ms;

    mc.writeToScreen("Hello");
    terminal.tick(ClockBase + 100ms);
    terminal.ensureFreshRenderBuffer();

    // setScreen() records _currentTime (ClockBase + 100ms) as startTime.
    mc.writeToScreen("\033[?1049h");
    REQUIRE(terminal.isScreenTransitionActive());

    // Tick past the full duration (startTime + 200ms = ClockBase + 300ms).
    terminal.tick(ClockBase + 400ms);
    CHECK_FALSE(terminal.isScreenTransitionActive());
}

TEST_CASE("Terminal.screenTransition.reaches_fade_in_phase", "[terminal]")
{
    auto mc = MockTerm { ColumnCount { 10 }, LineCount { 4 } };
    auto& terminal = mc.terminal;

    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    terminal.settings().screenTransitionStyle = vtbackend::ScreenTransitionStyle::Fade;
    terminal.settings().screenTransitionDuration = 200ms;

    mc.writeToScreen("Hello");
    terminal.tick(ClockBase + 100ms);
    terminal.ensureFreshRenderBuffer();

    // Switch to alternate screen, starting the transition at _currentTime = ClockBase + 100ms.
    mc.writeToScreen("\033[?1049h");
    REQUIRE(terminal.isScreenTransitionActive());

    // Tick to 60% of the 200ms duration (120ms past startTime = ClockBase + 220ms).
    terminal.tick(ClockBase + 220ms);
    terminal.ensureFreshRenderBuffer();

    auto const progress = terminal.screenTransitionProgress();
    CHECK(progress > 0.5f);
    CHECK(terminal.isScreenTransitionActive());
}

// }}}

// {{{ CancelSelection precondition tests

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
    terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, PixelCoordinate, UiHandledHint);
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
    terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, PixelCoordinate, UiHandledHint);
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

// }}}

// {{{ Selection: Shift+Click extend and auto-scroll

TEST_CASE("Terminal.ShiftClickExtendSelection", "[terminal]")
{
    // Create TE with some content.
    auto mock = MockTerm { ColumnCount(5), LineCount(5) };
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    mock.terminal.tick(ClockBase);
    mock.writeToScreen("12345\r\n"
                       "67890\r\n"
                       "ABCDE\r\n"
                       "abcde\r\n"
                       "fghij");

    using namespace vtbackend;
    auto constexpr UiHandledHint = false;
    auto constexpr PixelCoord = vtbackend::PixelCoordinate {};

    SECTION("extends completed selection forward")
    {
        // Select "7890\nABC" (row 1 col 1 → row 2 col 2)
        mock.terminal.tick(1s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 1_lineOffset + 1_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(2s);
        mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        mock.terminal.tick(3s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 2_lineOffset + 2_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(4s);
        mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        CHECK(mock.terminal.extractSelectionText() == "7890\nABC");
        CHECK(mock.terminal.selector()->state() == Selection::State::Complete);

        // Shift+Click at row 3, col 3 to extend selection.
        mock.terminal.tick(6s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 3_lineOffset + 3_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(7s);
        mock.terminal.sendMousePressEvent(Modifier::Shift, MouseButton::Left, PixelCoord, UiHandledHint);

        // Selection should now span from original start to new click position.
        CHECK(mock.terminal.extractSelectionText() == "7890\nABCDE\nabcd");
    }

    SECTION("extends completed selection backward")
    {
        // Select "BCDE\nabcd" (row 2 col 1 → row 3 col 3)
        mock.terminal.tick(1s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 2_lineOffset + 1_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(2s);
        mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        mock.terminal.tick(3s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 3_lineOffset + 3_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(4s);
        mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        CHECK(mock.terminal.extractSelectionText() == "BCDE\nabcd");
        CHECK(mock.terminal.selector()->state() == Selection::State::Complete);

        // Shift+Click at row 0 col 0 to extend backward.
        // Anchor moves to selEnd (3,3), extend to (0,0).
        // Selection covers (0,0) to (3,3): "12345\n67890\nABCDE\nabcd"
        mock.terminal.tick(6s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(7s);
        mock.terminal.sendMousePressEvent(Modifier::Shift, MouseButton::Left, PixelCoord, UiHandledHint);

        CHECK(mock.terminal.extractSelectionText() == "12345\n67890\nABCDE\nabcd");
    }

    SECTION("no selection starts new selection on Shift+Click")
    {
        // No prior selection exists. Shift+Click should start a new selection.
        mock.terminal.tick(1s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 1_lineOffset + 1_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(2s);
        mock.terminal.sendMousePressEvent(Modifier::Shift, MouseButton::Left, PixelCoord, UiHandledHint);

        // Should create a new selection (Waiting state), not crash.
        REQUIRE(mock.terminal.selectionAvailable());
        CHECK(mock.terminal.selector()->state() == Selection::State::Waiting);
    }

    SECTION("click to deselect after Shift+Click extend")
    {
        // Regression test: a normal click shortly after Shift+Click extend
        // must deselect rather than trigger a word-wise selection.

        // 1. Create and complete a selection: "7890\nABC" (row 1 col 1 → row 2 col 2)
        mock.terminal.tick(1s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 1_lineOffset + 1_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(2s);
        mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        mock.terminal.tick(3s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 2_lineOffset + 2_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(4s);
        mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        CHECK(mock.terminal.extractSelectionText() == "7890\nABC");

        // 2. Shift+Click to extend the selection.
        mock.terminal.tick(4s + 300ms);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 3_lineOffset + 3_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(4s + 500ms);
        mock.terminal.sendMousePressEvent(Modifier::Shift, MouseButton::Left, PixelCoord, UiHandledHint);
        mock.terminal.sendMouseReleaseEvent(Modifier::Shift, MouseButton::Left, PixelCoord, UiHandledHint);
        CHECK_FALSE(mock.terminal.extractSelectionText().empty());

        // 3. Normal click shortly after (within 1s) to deselect — must clear selection.
        mock.terminal.tick(4s + 800ms);
        mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        CHECK(mock.terminal.extractSelectionText().empty());
    }

    SECTION("extends upward then downward re-anchors correctly")
    {
        // Select "890\nABC" (row 1 col 2 → row 2 col 2)
        mock.terminal.tick(1s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 1_lineOffset + 2_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(2s);
        mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        mock.terminal.tick(3s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 2_lineOffset + 2_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(4s);
        mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        CHECK(mock.terminal.extractSelectionText() == "890\nABC");

        // Shift+Click upward at (0,0): anchor moves to selEnd (2,2), extend to (0,0).
        mock.terminal.tick(6s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(7s);
        mock.terminal.sendMousePressEvent(Modifier::Shift, MouseButton::Left, PixelCoord, UiHandledHint);
        mock.terminal.sendMouseReleaseEvent(Modifier::Shift, MouseButton::Left, PixelCoord, UiHandledHint);
        CHECK(mock.terminal.extractSelectionText() == "12345\n67890\nABC");

        // Shift+Click downward at (4,4): anchor moves to selStart (0,0), extend to (4,4).
        mock.terminal.tick(9s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 4_lineOffset + 4_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(10s);
        mock.terminal.sendMousePressEvent(Modifier::Shift, MouseButton::Left, PixelCoord, UiHandledHint);
        CHECK(mock.terminal.extractSelectionText() == "12345\n67890\nABCDE\nabcde\nfghij");
    }

    SECTION("extends into selection interior shrinks to nearer anchor")
    {
        // Select full content (0,0)→(4,4)
        mock.terminal.tick(1s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(2s);
        mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        mock.terminal.tick(3s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 4_lineOffset + 4_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(4s);
        mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        CHECK(mock.terminal.extractSelectionText() == "12345\n67890\nABCDE\nabcde\nfghij");

        // Shift+Click inside at (2,2): click >= selStart, so anchor = selStart (0,0), extend to (2,2).
        mock.terminal.tick(6s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 2_lineOffset + 2_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(7s);
        mock.terminal.sendMousePressEvent(Modifier::Shift, MouseButton::Left, PixelCoord, UiHandledHint);
        CHECK(mock.terminal.extractSelectionText() == "12345\n67890\nABC");
    }
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
        mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
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
        mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
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
        mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
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
        mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
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
        mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        mock.terminal.tick(3s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);

        // Try to scroll way past history.
        mock.terminal.performAutoScroll(-1, LineCount(100));
        // Should be capped at the available history.
        CHECK(mock.terminal.viewport().scrollOffset() <= ScrollOffset(3));
    }
}

// }}}

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
        mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
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
        auto const handled =
            mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);

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
        CHECK(reply.find("\033[<") != std::string::npos);
    }

    SECTION("click-to-deselect works with passive tracking")
    {
        selectHello();

        // Now click to deselect
        mock.terminal.tick(5s);
        mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        CHECK(mock.terminal.extractSelectionText().empty());
    }
}

TEST_CASE("Terminal.KittyKeyRelease.sendKeyEvent", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount(24), ColumnCount(80) } };
    auto& terminal = mc.terminal;

    terminal.keyboardProtocol().enter(vtbackend::KeyboardEventFlag::DisambiguateEscapeCodes);
    terminal.keyboardProtocol().flags().enable(vtbackend::KeyboardEventFlag::ReportEventTypes);

    auto constexpr Now = std::chrono::steady_clock::time_point {};

    mc.resetReplyData();
    terminal.sendKeyEvent(
        vtbackend::Key::UpArrow, vtbackend::Modifier::None, vtbackend::KeyboardEventType::Press, Now);
    CHECK(e(mc.replyData()) == e("\033[A"s));

    mc.resetReplyData();
    terminal.sendKeyEvent(
        vtbackend::Key::UpArrow, vtbackend::Modifier::None, vtbackend::KeyboardEventType::Release, Now);
    CHECK(!mc.replyData().empty());
    CHECK(e(mc.replyData()) == e("\033[1;1:3A"s));
}

TEST_CASE("Terminal.KittyKeyRelease.sendCharEvent", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount(24), ColumnCount(80) } };
    auto& terminal = mc.terminal;

    terminal.keyboardProtocol().enter(vtbackend::KeyboardEventFlag::DisambiguateEscapeCodes);
    terminal.keyboardProtocol().flags().enable(vtbackend::KeyboardEventFlag::ReportEventTypes);

    auto constexpr Now = std::chrono::steady_clock::time_point {};

    mc.resetReplyData();
    terminal.sendCharEvent('a', 'a', vtbackend::Modifier::Control, vtbackend::KeyboardEventType::Press, Now);
    CHECK(e(mc.replyData()) == e("\033[97;5u"s));

    mc.resetReplyData();
    terminal.sendCharEvent(
        'a', 'a', vtbackend::Modifier::Control, vtbackend::KeyboardEventType::Release, Now);
    CHECK(!mc.replyData().empty());
    CHECK(e(mc.replyData()) == e("\033[97;5:3u"s));
}

TEST_CASE("Terminal.KittyKeyRelease.NoOutputWithoutFlag", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount(24), ColumnCount(80) } };
    auto& terminal = mc.terminal;

    terminal.keyboardProtocol().enter(vtbackend::KeyboardEventFlag::DisambiguateEscapeCodes);

    auto constexpr Now = std::chrono::steady_clock::time_point {};

    mc.resetReplyData();
    terminal.sendCharEvent('a', 'a', vtbackend::Modifier::Control, vtbackend::KeyboardEventType::Press, Now);
    CHECK(!mc.replyData().empty());

    mc.resetReplyData();
    terminal.sendCharEvent(
        'a', 'a', vtbackend::Modifier::Control, vtbackend::KeyboardEventType::Release, Now);
    CHECK(mc.replyData().empty());
}

TEST_CASE("Terminal.KittyKeyRelease.RepeatStillWorks", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount(24), ColumnCount(80) } };
    auto& terminal = mc.terminal;

    terminal.keyboardProtocol().enter(vtbackend::KeyboardEventFlag::DisambiguateEscapeCodes);
    terminal.keyboardProtocol().flags().enable(vtbackend::KeyboardEventFlag::ReportEventTypes);

    auto constexpr Now = std::chrono::steady_clock::time_point {};

    mc.resetReplyData();
    terminal.sendKeyEvent(
        vtbackend::Key::UpArrow, vtbackend::Modifier::None, vtbackend::KeyboardEventType::Repeat, Now);
    CHECK(e(mc.replyData()) == e("\033[1;1:2A"s));
}

// {{{ Regression tests for top-anchored partial scroll regions (PR #1946)
TEST_CASE("Terminal.TopAnchoredRegion.PartialScrollKeepsViewportFixed", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(8) }, LineCount(20) };
    auto& terminal = mc.terminal;

    // Generate scrollback so the viewport can be scrolled up.
    mc.writeToScreen("h1\r\nh2\r\nh3\r\nh4\r\nh5\r\nh6\r\nh7\r\nh8\r\n");
    terminal.viewport().scrollUp(LineCount(3));
    REQUIRE(terminal.viewport().scrolled());
    auto const scrollOffsetBefore = terminal.viewport().scrollOffset();

    // Top-anchored partial region (rows 1..3), cursor at the region bottom.
    mc.writeToScreen("\033[1;3r");
    mc.writeToScreen("\033[3;1H");

    // CSI S (SU) scrolls the region up; the viewport the user scrolled to must
    // not jump as a side effect.
    mc.writeToScreen("\033[S");

    CHECK(terminal.viewport().scrollOffset() == scrollOffsetBefore);
}

TEST_CASE("Terminal.TopAnchoredRegion.PartialScrollDoesNotMoveNormalModeCursor", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(8) }, LineCount(20) };
    auto& terminal = mc.terminal;

    mc.writeToScreen("r1\r\nr2\r\nr3\r\nr4\r\nr5\r\nr6");
    terminal.inputHandler().setMode(vtbackend::ViMode::Normal);
    auto const cursorLineBefore = terminal.normalModeCursorPosition().line;

    // Top-anchored partial region (rows 1..3), cursor at region bottom, then IND.
    mc.writeToScreen("\033[1;3r");
    mc.writeToScreen("\033[3;1H");
    mc.writeToScreen("\033D");

    CHECK(terminal.normalModeCursorPosition().line == cursorLineBefore);
}

TEST_CASE("Terminal.TopAnchoredRegion.ScrollCountMatchesScrolledLines", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(8) }, LineCount(2) };
    auto& terminal = mc.terminal;

    // Fill history to capacity (2 lines) so further scrolls have no headroom.
    mc.writeToScreen("a\r\nb\r\nc\r\nd\r\ne\r\nf\r\n");
    terminal.viewport().scrollUp(LineCount(1));
    REQUIRE(terminal.viewport().scrolled());
    auto const scrollOffsetBefore = terminal.viewport().scrollOffset();

    // Top-anchored partial region, cursor at region bottom, scroll it.
    mc.writeToScreen("\033[1;2r");
    mc.writeToScreen("\033[2;1H");
    mc.writeToScreen("\033D");

    // The viewport must not drift by the history/scroll-count mismatch.
    CHECK(terminal.viewport().scrollOffset() == scrollOffsetBefore);
}
// }}}

// {{{ Normal-mode cursorline & yank-highlight on trivial vs inflated lines
//
// Regression coverage for the AoS→SoA grid migration: a plain-text line with uniform SGR stays
// "trivial" even when the vi/normal-mode cursor is on it, so it takes RenderBufferBuilder's
// trivial fast path. That path used to hard-code the cursorline off (the old invariant "a line
// with a cursor is always inflated"), dropping the current-line highlight on plain-text lines.
// The same fast path also skipped the vi yank/motion highlight. These tests pin both down.
namespace
{

/// Background color of screen @p line at a column away from column 0, in a freshly built render
/// buffer. Sampling off column 0 avoids the block-cursor cell (which inverts fg/bg) when the
/// cursor sits at the line start. A line is emitted either as per-cell RenderCells (the
/// per-cell/fallback path) or as one batched RenderLine (the trivial simple path); check both.
vtbackend::RGBColor screenLineBackground(vtbackend::RenderBufferRef const& buf,
                                         vtbackend::LineOffset line) noexcept
{
    for (auto const& cell: buf.get().cells)
        if (cell.position.line == line && cell.position.column >= vtbackend::ColumnOffset(2))
            return cell.attributes.backgroundColor;
    for (auto const& renderLine: buf.get().lines)
        if (renderLine.lineOffset == line)
            // The batched RenderLine covers text (0..usedColumns) and fill (usedColumns..end)
            // with one attribute set each; the cursorline tint is applied uniformly to both, so
            // either represents the line background — prefer the fill (always present).
            return renderLine.fillAttributes.backgroundColor;
    return vtbackend::RGBColor {};
}

/// Screen line the cursor is rendered on, per the render buffer itself — the single source of
/// truth for which line should carry the cursorline highlight (the render loop and the highlight
/// decision share this coordinate). Returns nullopt when the buffer carries no cursor.
std::optional<vtbackend::LineOffset> renderedCursorLine(vtbackend::RenderBufferRef const& buf) noexcept
{
    if (!buf.get().cursor.has_value())
        return std::nullopt;
    return buf.get().cursor->position.line;
}

} // namespace

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

TEST_CASE("Terminal.YankHighlight.trivialLineIsHighlighted", "[terminal][vi]")
{
    // Regression: a vi yank/motion highlight over a plain-text (trivial) line must recolor it.
    // Tall page so content lines stay clear of the bottom indicator status line.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(10) }, LineCount(0) };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);
    mc.writeToScreen("plainA\r\n");
    mc.writeToScreen("plainB\r\n");
    mc.writeToScreen("plainC");

    // Highlight grid line 1 (a plain-text trivial line) while still in insert mode so no
    // status-line resize shifts coordinates; the highlight alone must recolor the trivial line.
    terminal.setHighlightRange(vtbackend::LinearHighlight {
        .from = vtbackend::CellLocation { .line = LineOffset(1), .column = ColumnOffset(0) },
        .to = vtbackend::CellLocation { .line = LineOffset(1), .column = ColumnOffset(5) } });
    terminal.tick(ClockBase + chrono::seconds(1));
    terminal.refreshRenderBuffer();
    auto const buf = terminal.renderBuffer();
    auto const defaultBg = terminal.colorPalette().defaultBackground;

    CHECK(screenLineBackground(buf, LineOffset(1)) != defaultBg);
    // A non-highlighted plain-text line keeps the default background.
    CHECK(screenLineBackground(buf, LineOffset(2)) == defaultBg);
}
// }}}

// {{{ mouse wheel alternate-scroll (GitHub #1951)

TEST_CASE("Terminal.Wheel.AltScreen.NoProtocol.emits_cursor_keys", "[terminal]")
{
    // #1951: on the alt screen (less/most/man) with no mouse protocol, a wheel notch must
    // translate into cursor keys so the pager scrolls. Before the fix, replyData() stayed empty.
    using namespace vtbackend;
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(10) };
    auto constexpr NoPixel = PixelCoordinate {};
    auto constexpr UiHandledHint = false;
    mock.terminal.tick(1s);

    // Enter the alternate screen (DECSET ?1049).
    mock.writeToScreen("\033[?1049h");
    mock.terminal.tick(1s);
    REQUIRE(mock.terminal.isAlternateScreen());

    mock.resetReplyData();
    auto const handledDown =
        mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::WheelDown, NoPixel, UiHandledHint);
    CHECK(handledDown == Handled { true });
    CHECK(e(mock.replyData()) == e("\033[B")); // default multiplier is 1 in MockTerm

    mock.resetReplyData();
    auto const handledUp =
        mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::WheelUp, NoPixel, UiHandledHint);
    CHECK(handledUp == Handled { true });
    CHECK(e(mock.replyData()) == e("\033[A"));
}

TEST_CASE("Terminal.Wheel.AltScreen.AppCursorKeys.emits_SS3", "[terminal]")
{
    // DECCKM (?1h) on the alternate screen selects application cursor keys (SS3 form).
    using namespace vtbackend;
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(10) };
    auto constexpr NoPixel = PixelCoordinate {};
    mock.terminal.tick(1s);

    mock.writeToScreen("\033[?1049h"); // alt screen
    mock.writeToScreen("\033[?1h");    // DECCKM: application cursor keys
    mock.terminal.tick(1s);
    REQUIRE(mock.terminal.isAlternateScreen());

    mock.resetReplyData();
    CHECK(mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::WheelDown, NoPixel, false)
          == Handled { true });
    CHECK(e(mock.replyData()) == e("\033OB"));
}

TEST_CASE("Terminal.Wheel.AltScreen.DECSET1007.emits_cursor_keys", "[terminal]")
{
    // DECSET ?1007 (alternate-scroll) was previously a no-op because of the mouse-protocol
    // gate; it must now produce cursor keys on the alternate screen.
    using namespace vtbackend;
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(10) };
    auto constexpr NoPixel = PixelCoordinate {};
    mock.terminal.tick(1s);

    mock.writeToScreen("\033[?1049h"); // alt screen
    mock.writeToScreen("\033[?1007h"); // alternate-scroll -> application cursor keys
    mock.terminal.tick(1s);

    mock.resetReplyData();
    CHECK(mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::WheelDown, NoPixel, false)
          == Handled { true });
    CHECK(e(mock.replyData()) == e("\033OB"));
}

TEST_CASE("Terminal.Wheel.AltScreen.AppTracking.passes_through_as_SGR", "[terminal]")
{
    // With an app-enabled protocol (`less --mouse`, `ov`: ?1000h/?1002h + SGR ?1006h), the wheel
    // must be reported to the app, NOT translated to cursor keys. Guards the passthrough case.
    using namespace vtbackend;
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(10) };
    auto constexpr NoPixel = PixelCoordinate {};
    mock.terminal.tick(1s);

    mock.writeToScreen("\033[?1049h");                       // alt screen
    mock.writeToScreen("\033[?1000h\033[?1002h\033[?1006h"); // less --mouse style
    mock.terminal.tick(1s);

    mock.resetReplyData();
    CHECK(mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::WheelDown, NoPixel, false)
          == Handled { true });
    // SGR mouse report, not a cursor key.
    CHECK(e(mock.replyData()).starts_with(e("\033[<")));
    CHECK(e(mock.replyData()).find(e("\033[B")) == std::string::npos);
}

TEST_CASE("Terminal.Wheel.PrimaryScreen.NoProtocol.local_scroll", "[terminal]")
{
    // Primary screen, no protocol: wheel mode is Default, so the backend does not handle it
    // (the frontend scrolls scrollback). No bytes are sent and the event is reported unhandled.
    using namespace vtbackend;
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(10) };
    auto constexpr NoPixel = PixelCoordinate {};
    mock.terminal.tick(1s);
    REQUIRE(mock.terminal.isPrimaryScreen());

    mock.resetReplyData();
    CHECK(mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::WheelDown, NoPixel, false)
          == Handled { false });
    CHECK(mock.replyData().empty());
}

TEST_CASE("Terminal.Wheel.AltScreen.ShiftBypass.not_handled", "[terminal]")
{
    // Holding the bypass modifier (Shift by default) must let the wheel event fall through to
    // the frontend's Shift+Wheel binding (page-scroll), so the backend reports it unhandled
    // and emits nothing.
    using namespace vtbackend;
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(10) };
    auto constexpr NoPixel = PixelCoordinate {};
    mock.terminal.tick(1s);

    mock.writeToScreen("\033[?1049h");
    mock.terminal.tick(1s);

    mock.resetReplyData();
    CHECK(mock.terminal.sendMousePressEvent(Modifier::Shift, MouseButton::WheelDown, NoPixel, false)
          == Handled { false });
    CHECK(mock.replyData().empty());
}

TEST_CASE("Terminal.Wheel.AltScreen.ViNormalMode.no_cursor_keys", "[terminal]")
{
    // In Vi normal mode the wheel must not inject cursor keys into the app.
    using namespace vtbackend;
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(10) };
    auto constexpr NoPixel = PixelCoordinate {};
    mock.terminal.tick(1s);

    mock.writeToScreen("\033[?1049h");
    mock.terminal.tick(1s);
    mock.terminal.inputHandler().setMode(ViMode::Normal);

    mock.resetReplyData();
    CHECK(mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::WheelDown, NoPixel, false)
          == Handled { false });
    CHECK(mock.replyData().empty());

    mock.terminal.inputHandler().setMode(ViMode::Insert);
}

TEST_CASE("Terminal.Wheel.AltScreen.ScrollMultiplier.repeats_cursor_keys", "[terminal]")
{
    // With a scroll multiplier of 3, one alt-screen wheel notch emits 3 cursor keys, matching
    // the primary-screen scrollback feel.
    using namespace vtbackend;
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(10) };
    auto constexpr NoPixel = PixelCoordinate {};
    mock.terminal.tick(1s);

    mock.writeToScreen("\033[?1049h");
    mock.terminal.tick(1s);
    mock.terminal.setMouseWheelScrollMultiplier(LineCount(3));

    mock.resetReplyData();
    CHECK(mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::WheelDown, NoPixel, false)
          == Handled { true });
    CHECK(e(mock.replyData()) == e("\033[B\033[B\033[B"));
}

// }}}

// {{{ #1954: lock modifiers must not reach the terminal's UI decisions

// Hint-mode label characters were gated on `modifiers.none()`, so with a lock key latched they
// fell through to the input generator and were typed into the running application instead.
TEST_CASE("Terminal.hint_mode_accepts_labels_while_lock_keys_are_latched", "[terminal][locks]")
{
    for (auto const locks: LockCombinations)
    {
        INFO(std::format("lock modifiers {}", locks));
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(40) } };
        mock.writeToScreen("visit https://example.com now");

        mock.terminal.activateHintMode(vtbackend::HintModeHandler::builtinPatterns(),
                                       vtbackend::HintAction::Copy);
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

// DECUDK lookup was gated on `modifiers.none()`, so a latched lock key silently disabled every
// user-defined key.
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

// The load-bearing counterpart: lock state must still reach the input generator, which reports it
// to the application on purpose. selectNumpad() emits the digit only when it can see NumLock;
// without it, application-keypad mode would emit CSI E instead.
TEST_CASE("Terminal.numpad_digit_keeps_NumLock_for_the_input_generator", "[terminal][locks]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.terminal.setApplicationkeypadMode(true);
    mock.resetReplyData();

    mock.sendKeyEvent(vtbackend::Key::Numpad_5, NumLockOnly);

    CHECK(e(mock.replyData()) == e("5"));
}

// The recurrence firewall.
//
// In the default configuration -- legacy keyboard protocol, no Win32 input mode, modifyOtherKeys
// off, numeric keypad -- a latched lock key is invisible to the application. Nothing a user can
// type may encode differently just because CapsLock or NumLock happens to be on.
//
// This sweep fails the moment any consumer, present or future and wherever it lives, lets a lock
// bit reach chord logic. It would have caught every bug in the #1884 / #1901 / #1954 family.
//
// The three places where lock state legitimately *does* change the encoding are covered by their
// own tests, so they are deliberately outside this invariant:
//   - the numpad under application-keypad mode (see the test directly above, and InputGenerator.DECNKM)
//   - the Kitty keyboard protocol (see the test directly below)
//   - Win32 input mode (see buildWin32ControlKeyState)
TEST_CASE("Terminal.no_key_encoding_depends_on_lock_modifiers", "[terminal][locks]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };

    auto const encodeKey = [&](vtbackend::Key key, vtbackend::KeyboardModifiers modifiers) {
        mock.resetReplyData();
        mock.sendKeyEvent(key, modifiers);
        return std::string { mock.replyData() };
    };

    auto const encodeChar = [&](char32_t ch, vtbackend::KeyboardModifiers modifiers) {
        mock.resetReplyData();
        mock.sendCharEvent(ch, modifiers);
        return std::string { mock.replyData() };
    };

    SECTION("every key")
    {
        // The Key enumerators run contiguously from F1 to Numpad_9.
        for (auto const rawKey: std::views::iota(static_cast<int>(vtbackend::Key::F1),
                                                 static_cast<int>(vtbackend::Key::Numpad_9) + 1))
        {
            auto const key = static_cast<vtbackend::Key>(rawKey);
            auto const baseline = encodeKey(key, {});
            for (auto const locks: LockCombinations)
            {
                INFO(std::format("key {} with lock keys {}", key, locks));
                CHECK(e(encodeKey(key, locks)) == e(baseline));
            }
        }
    }

    SECTION("every printable character")
    {
        for (auto const codepoint: std::views::iota(0x20, 0x7F))
        {
            auto const ch = static_cast<char32_t>(codepoint);
            auto const baseline = encodeChar(ch, {});
            for (auto const locks: LockCombinations)
            {
                INFO(std::format("character U+{:04X} with lock keys {}", codepoint, locks));
                CHECK(e(encodeChar(ch, locks)) == e(baseline));
            }
        }
    }

    SECTION("every printable character under modifyOtherKeys mode 2")
    {
        mock.terminal.setModifyOtherKeys(2);
        for (auto const codepoint: std::views::iota(0x20, 0x7F))
        {
            auto const ch = static_cast<char32_t>(codepoint);
            auto const baseline = encodeChar(ch, {});
            for (auto const locks: LockCombinations)
            {
                INFO(std::format("character U+{:04X} with lock keys {}", codepoint, locks));
                CHECK(e(encodeChar(ch, locks)) == e(baseline));
            }
        }
    }
}

// The positive counterpart to the sweep above: under the Kitty keyboard protocol the lock state is
// reported to the application on purpose, so it must survive all the way to the encoder.
TEST_CASE("Terminal.kitty_keyboard_protocol_reports_lock_modifiers", "[terminal][locks]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };

    // CSI > 9 u: DisambiguateEscapeCodes (1) | ReportAllKeysAsEscapeCodes (8).
    mock.writeToScreen("\033[>9u");
    mock.terminal.flushInput();
    mock.resetReplyData();

    // Key code 97 (lowercase a), modifier 65 == 1 + LockKey::CapsLock.
    mock.terminal.sendCharEvent(
        U'a', U'a', CapsLockOnly, vtbackend::KeyboardEventType::Press, std::chrono::steady_clock::now());

    CHECK(e(mock.replyData()) == e("\033[97;65u"));
}

// The positive counterpart for Win32 input mode (DEC private mode 9001, which ConPTY enables on
// Windows). ConPTY forwards the record's Unicode-char field verbatim and cannot reconstruct it from
// the virtual-key code, so character-bearing keys that lack a receiver-side VK fallback -- Escape
// and the numpad keys -- must carry their character or applications such as neovim never see them.
// Regression test for "Escape and numpad keys dead in neovim on Windows".
TEST_CASE("Terminal.win32_input_mode_reports_unicode_for_escape_and_numpad", "[terminal][locks]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };

    // CSI ? 9001 h enables Win32 input mode, exactly as ConPTY does on Windows.
    mock.writeToScreen("\033[?9001h");
    mock.terminal.flushInput();

    auto const now = std::chrono::steady_clock::now();

    SECTION("Escape carries its Unicode char 0x1B")
    {
        mock.resetReplyData();
        mock.terminal.sendKeyEvent(vtbackend::Key::Escape, {}, vtbackend::KeyboardEventType::Press, now);
        // CSI Vk ; Sc ; Uc ; Kd ; Cs ; Rc _  -- VK_ESCAPE=27, Uc=ESC=27.
        CHECK(e(mock.replyData()) == e("\033[27;0;27;1;0;1_"));
    }

    SECTION("numpad digit carries its Unicode char while NumLock is latched")
    {
        mock.resetReplyData();
        mock.terminal.sendKeyEvent(
            vtbackend::Key::Numpad_5, NumLockOnly, vtbackend::KeyboardEventType::Press, now);
        // VK_NUMPAD5=101, Uc='5'=53, CS=NUMLOCK_ON=32.
        CHECK(e(mock.replyData()) == e("\033[101;0;53;1;32;1_"));
    }
}

// }}}

// NOLINTEND(misc-const-correctness)
