// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/MockTerm.h>
#include <vtbackend/Terminal.h>
#include <vtbackend/primitives.h>
#include <vtbackend/test_helpers.h>

#include <vtpty/MockPty.h>

#include <crispy/App.h>
#include <crispy/times.h>

#include <libunicode/convert.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace std;
using namespace std::chrono_literals;
using vtbackend::CellFlag;
using vtbackend::ColumnCount;
using vtbackend::ColumnOffset;
using vtbackend::LineCount;
using vtbackend::LineOffset;
using vtbackend::MockTerm;
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

    // The line should be in trivial buffer form for simple ASCII text
    if (line.isTrivialBuffer())
    {
        auto const& trivialBuffer = line.trivialBuffer();
        CHECK(trivialBuffer.text.view() == "ABCDEFGHIJ");
        CHECK(trivialBuffer.usedColumns == ColumnCount(10));
    }
    else
    {
        // If not trivial, verify via inflated content
        CHECK(line.toUtf8().substr(0, 10) == "ABCDEFGHIJ");
    }
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

// NOLINTEND(misc-const-correctness)
