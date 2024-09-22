// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/MockTerm.h>
#include <vtbackend/Terminal.h>
#include <vtbackend/primitives.h>
#include <vtbackend/test_helpers.h>

#include <vtpty/MockPty.h>

#include <crispy/App.h>
#include <crispy/times.h>

#include <libunicode/convert.h>

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

using namespace vtbackend::test;

// TODO: Test case posibilities:
//
// - [x] Synchronized output (?2026)
// - [x] Blinking cursor visiblity over time and on input events
// - [ ] double click word selection
// - [ ] tripple click line selection
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

// NOLINTEND(misc-const-correctness)
