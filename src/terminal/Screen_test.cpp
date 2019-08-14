#include <terminal/Screen.h>
#include <catch2/catch.hpp>

using namespace terminal;
using namespace std;

template <typename T>
void logScreen(T& test, Screen const& screen, string const& headline)
{
	// TODO
    // if (!headline.empty())
    //     test.logf("{}:", headline);
    // else
    //     test.log("screen dump:");
    // 
    // for (size_t row = 1; row <= screen.rowCount(); ++row)
    //     test.logf("[{}] \"{}\"", row, screen.renderTextLine(row));
}

TEST_CASE("Screen.AppendChar", "[screen]")
{
    auto screen = Screen{3, 1, {}, [&](auto const& msg) { logf("{}", msg); }, {}};
    REQUIRE("   ", screen.renderTextLine(1));

    screen.write("A");
    REQUIRE("A  ", screen.renderTextLine(1));

    screen.write("B");
    REQUIRE("AB ", screen.renderTextLine(1));

    screen.write("C");
    REQUIRE("ABC", screen.renderTextLine(1));

    screen.write("D");
    REQUIRE("ABD", screen.renderTextLine(1));
}

TEST_CASE("Screen.AppendChar_AutoWrap", "[screen]")
{
    auto screen = Screen{3, 2, {}, [&](auto const& msg) { logf("{}", msg); }, {}};
    screen(SetMode{Mode::AutoWrap, true});

    screen.write("ABC");
    REQUIRE("ABC", screen.renderTextLine(1));
    REQUIRE("   ", screen.renderTextLine(2));
    REQUIRE(1, screen.currentRow());
    REQUIRE(3, screen.currentColumn());

    screen.write("D");
    REQUIRE("ABC", screen.renderTextLine(1));
    REQUIRE("D  ", screen.renderTextLine(2));

    screen.write("EF");
    REQUIRE("ABC", screen.renderTextLine(1));
    REQUIRE("DEF", screen.renderTextLine(2));

    screen.write("G");
    REQUIRE("DEF", screen.renderTextLine(1));
    REQUIRE("G  ", screen.renderTextLine(2));
}

TEST_CASE("Screen.AppendChar_AutoWrap_LF", "[screen]")
{
    auto screen = Screen{3, 2, {}, [&](auto const& msg) { logf("{}", msg); }, {}};
    screen(SetMode{Mode::AutoWrap, true});

    screen.write("ABC");
    logScreen(*this, screen, "after writing ABC");
    REQUIRE("ABC", screen.renderTextLine(1));
    REQUIRE("   ", screen.renderTextLine(2));
    REQUIRE(1, screen.currentRow());
    REQUIRE(3, screen.currentColumn());

    screen.write("\n");
    logScreen(*this, screen, "after writing LF");
    REQUIRE(2, screen.currentRow());
    REQUIRE(1, screen.currentColumn());

    screen.write("D");
    logScreen(*this, screen, "after writing 'D'");
    REQUIRE("ABC", screen.renderTextLine(1));
    REQUIRE("D  ", screen.renderTextLine(2));
    REQUIRE(2, screen.currentRow());
    REQUIRE(2, screen.currentColumn());
}

TEST_CASE("Screen.Backspace", "[screen]")
{
    auto screen = Screen{3, 2, {}, [&](auto const& msg) { logf("{}", msg); }, {}};
    REQUIRE(1, screen.currentRow());
    REQUIRE(1, screen.currentColumn());

    screen.write("12");
    CHECK("12 " == screen.renderTextLine(1));;
    REQUIRE(1, screen.currentRow());
    REQUIRE(3, screen.currentColumn());

    screen.write("\b");
    CHECK("12 " == screen.renderTextLine(1));;
    REQUIRE(1, screen.currentRow());
    REQUIRE(2, screen.currentColumn());

    screen.write("\b");
    CHECK("12 " == screen.renderTextLine(1));;
    REQUIRE(1, screen.currentRow());
    REQUIRE(1, screen.currentColumn());

    screen.write("\b");
    CHECK("12 " == screen.renderTextLine(1));;
    REQUIRE(1, screen.currentRow());
    REQUIRE(1, screen.currentColumn());
}
#if 0
TEST_CASE("Screen.Linefeed_with_scrollup", "[screen]")
{
    auto screen = Screen{2, 2, {}, [&](auto const& msg) { logf("{}", msg); }, {}};

    log("init:");
    logf("  line 1: '{}'", screen.renderTextLine(1));
    logf("  line 2: '{}'", screen.renderTextLine(2));

    screen.write("1\n2");

    log("after writing '1\\n2':");
    logf("  line 1: '{}'", screen.renderTextLine(1));
    logf("  line 2: '{}'", screen.renderTextLine(2));

    REQUIRE("1 ", screen.renderTextLine(1));
    REQUIRE("2 ", screen.renderTextLine(2));

    screen.write("\n3"); // line 3

    log("After writing '\\n3':");
    logf("  line 1: '{}'", screen.renderTextLine(1));
    logf("  line 2: '{}'", screen.renderTextLine(2));

    REQUIRE("2 ", screen.renderTextLine(1));
    REQUIRE("3 ", screen.renderTextLine(2));
}

TEST_CASE("Screen.ClearToEndOfScreen", "[screen]")
{
    auto screen = Screen{2, 2, {}, [&](auto const& msg) { logf("{}", msg); }, {}};

    screen.write("AB\nC");
    CHECK("AB", screen.renderTextLine(1));
    CHECK("C ", screen.renderTextLine(2));
    screen(ClearToEndOfScreen{});

    CHECK("AB", screen.renderTextLine(1));
    CHECK("  ", screen.renderTextLine(2));
}

TEST_CASE("Screen.ClearToBeginOfScreen", "[screen]")
{
    Screen screen{2, 3, {}, [&](auto const& msg) { logf("{}", msg); }, {}};
    screen.write("AB\nCD\nE");

    REQUIRE("AB", screen.renderTextLine(1));
    REQUIRE("CD", screen.renderTextLine(2));
    REQUIRE("E ", screen.renderTextLine(3));
    REQUIRE(3, screen.currentRow());
    REQUIRE(2, screen.currentColumn());

    screen(MoveCursorUp{1});
    screen(ClearToBeginOfScreen{});

    CHECK("  ", screen.renderTextLine(1));
    CHECK("  ", screen.renderTextLine(2));
    CHECK("E ", screen.renderTextLine(3));
    REQUIRE(2, screen.currentRow());
    REQUIRE(2, screen.currentColumn());
}

TEST_CASE("Screen.ClearScreen", "[screen]")
{
    Screen screen{2, 2, {}, [&](auto const& msg) { logf("{}", msg); }, {}};
    screen.write("AB\nC");
    screen(ClearScreen{});
    CHECK("  ", screen.renderTextLine(1));
    CHECK("  ", screen.renderTextLine(2));
}

TEST_CASE("Screen.ClearToEndOfLine", "[screen]")
{
    Screen screen{3, 1};
    screen.write("ABC");
    REQUIRE("ABC", screen.renderTextLine(1));

    screen(MoveCursorToColumn{2});
    screen(ClearToEndOfLine{});
    CHECK("A  ", screen.renderTextLine(1));
}

TEST_CASE("Screen.ClearToBeginOfLine", "[screen]")
{
    Screen screen{3, 1};
    screen(SetMode{Mode::AutoWrap, false});
    screen.write("ABC");
    REQUIRE("ABC", screen.renderTextLine(1));

    screen(MoveCursorToColumn{2});
    screen(ClearToBeginOfLine{});
    CHECK("  C", screen.renderTextLine(1));
}

TEST_CASE("Screen.ClearLine", "[screen]")
{
    Screen screen{3, 1};
    screen(SetMode{Mode::AutoWrap, false});
    screen.write("ABC");
    REQUIRE("ABC", screen.renderTextLine(1));

    screen(ClearLine{});
    CHECK("   ", screen.renderTextLine(1));
}

TEST_CASE("Screen.InsertLines", "[screen]")
{
    Screen screen{2, 3, {}, [&](auto const& msg) { logf("{}", msg); }, {}};

    screen.write("AB\nCD");
    REQUIRE("AB", screen.renderTextLine(1));
    REQUIRE("CD", screen.renderTextLine(2));
    REQUIRE("  ", screen.renderTextLine(3));

    screen(InsertLines{1});
    CHECK("AB", screen.renderTextLine(1));
    CHECK("  ", screen.renderTextLine(2));
    CHECK("CD", screen.renderTextLine(3));

    screen(MoveCursorTo{1, 1});
    screen(InsertLines{1});
    CHECK("  ", screen.renderTextLine(1));
    CHECK("AB", screen.renderTextLine(2));
    CHECK("  ", screen.renderTextLine(3));

    // TODO: test with (top/bottom and left/right) margins enabled
}

TEST_CASE(Screen, DISABLED_DeleteLines) // TODO: Screen's implementation
{
    Screen screen{2, 3, {}, [&](auto const& msg) { logf("{}", msg); }, {}};
    screen.write("AB\nCD\nEF");

    screen(MoveCursorTo{2, 1});

    // no-op
    screen(DeleteLines{0});
    REQUIRE("AB", screen.renderTextLine(1));
    REQUIRE("CD", screen.renderTextLine(2));
    REQUIRE("EF", screen.renderTextLine(3));

    // in-range
    screen(DeleteLines{1});
    REQUIRE("AB", screen.renderTextLine(1));
    REQUIRE("EF", screen.renderTextLine(2));
    REQUIRE("  ", screen.renderTextLine(3));

    // fill screen again, for next test
    screen(MoveCursorTo{3, 1});
    screen.write("GH");
    REQUIRE("AB", screen.renderTextLine(1));
    REQUIRE("EF", screen.renderTextLine(2));
    REQUIRE("GH", screen.renderTextLine(3));

    // clamped
    screen(MoveCursorTo{3, 2});
    screen(DeleteLines{5});
    REQUIRE("AB", screen.renderTextLine(1));
    REQUIRE("  ", screen.renderTextLine(2));
    REQUIRE("  ", screen.renderTextLine(3));
}

// TODO: DeleteCharacters

// TODO: ClearScrollbackBuffer
// TODO: ScrollUp
// TODO: ScrollDown

TEST_CASE("Screen.MoveCursorUp", "[screen]")
{
    Screen screen{2, 3, {}, [&](auto const& msg) { logf("{}", msg); }, {}};
    screen.write("AB\nCD\nEF");
    REQUIRE(3, screen.currentCursor().row);
    REQUIRE(2, screen.currentCursor().column);

    // no-op
    screen(MoveCursorUp{0});
    REQUIRE(3, screen.currentCursor().row);
    REQUIRE(2, screen.currentCursor().column);

    // in-range
    screen(MoveCursorUp{1});
    REQUIRE(2, screen.currentCursor().row);
    REQUIRE(2, screen.currentCursor().column);

    // overflow
    screen(MoveCursorUp{5});
    REQUIRE(1, screen.currentCursor().row);
    REQUIRE(2, screen.currentCursor().column);
}

TEST_CASE("Screen.MoveCursorDown", "[screen]")
{
    Screen screen{2, 3, {}, [&](auto const& msg) { logf("{}", msg); }, {}};
    screen.write("A");
    REQUIRE(1, screen.currentCursor().row);
    REQUIRE(2, screen.currentCursor().column);

    // no-op
    screen(MoveCursorDown{0});
    REQUIRE(1, screen.currentCursor().row);
    REQUIRE(2, screen.currentCursor().column);

    // in-range
    screen(MoveCursorDown{1});
    REQUIRE(2, screen.currentCursor().row);
    REQUIRE(2, screen.currentCursor().column);

    // overflow
    screen(MoveCursorDown{5});
    REQUIRE(3, screen.currentCursor().row);
    REQUIRE(2, screen.currentCursor().column);
}

TEST_CASE("Screen.MoveCursorForward", "[screen]")
{
    Screen screen{3, 3, {}, [&](auto const& msg) { logf("{}", msg); }, {}};
    REQUIRE(1, screen.currentRow());
    REQUIRE(1, screen.currentColumn());

    // no-op
    screen(MoveCursorForward{0});
    REQUIRE(1, screen.currentRow());
    REQUIRE(1, screen.currentColumn());

    // in-range
    screen(MoveCursorForward{1});
    REQUIRE(1, screen.currentRow());
    REQUIRE(2, screen.currentColumn());

    // overflow
    screen(MoveCursorForward{5});
    REQUIRE(1, screen.currentRow());
    REQUIRE(3, screen.currentColumn());
}

TEST_CASE("Screen.MoveCursorBackward", "[screen]")
{
    Screen screen{3, 3, {}, [&](auto const& msg) { logf("{}", msg); }, {}};
    screen.write("ABC");
    REQUIRE(1, screen.currentRow());
    REQUIRE(3, screen.currentColumn());

    // no-op
    screen(MoveCursorBackward{0});
    REQUIRE(1, screen.currentRow());
    REQUIRE(3, screen.currentColumn());

    // in-range
    screen(MoveCursorBackward{1});
    REQUIRE(1, screen.currentRow());
    REQUIRE(2, screen.currentColumn());

    // overflow
    screen(MoveCursorBackward{5});
    REQUIRE(1, screen.currentRow());
    REQUIRE(1, screen.currentColumn());
}

TEST_CASE("Screen.MoveCursorToColumn", "[screen]")
{
    Screen screen{3, 3, {}, [&](auto const& msg) { logf("{}", msg); }, {}};
    REQUIRE(1, screen.currentRow());
    REQUIRE(1, screen.currentColumn());

    // no-op
    screen(MoveCursorToColumn{1});
    REQUIRE(1, screen.currentRow());
    REQUIRE(1, screen.currentColumn());

    // in-range
    screen(MoveCursorToColumn{3});
    REQUIRE(1, screen.currentRow());
    REQUIRE(3, screen.currentColumn());

    screen(MoveCursorToColumn{2});
    REQUIRE(1, screen.currentRow());
    REQUIRE(2, screen.currentColumn());

    // overflow
    screen(MoveCursorToColumn{5});
    REQUIRE(1, screen.currentRow());
    REQUIRE(3, screen.currentColumn()); // clamped
}

TEST_CASE("Screen.MoveCursorToBeginOfLine", "[screen]")
{
    Screen screen{3, 3, {}, [&](auto const& msg) { logf("{}", msg); }, {}};

    screen.write("\nAB");
    REQUIRE(2, screen.currentRow());
    REQUIRE(3, screen.currentColumn());

    screen(MoveCursorToBeginOfLine{});
    REQUIRE(2, screen.currentRow());
    REQUIRE(1, screen.currentColumn());
}

TEST_CASE("Screen.MoveCursorTo", "[screen]")
{
    Screen screen{3, 3, {}, [&](auto const& msg) { logf("{}", msg); }, {}};

    // in-range
    screen(MoveCursorTo{3, 2});
    REQUIRE(3, screen.currentRow());
    REQUIRE(2, screen.currentColumn());

    // origin
    screen(MoveCursorTo{1, 1});
    REQUIRE(1, screen.currentRow());
    REQUIRE(1, screen.currentColumn());

    // overflow
    screen(MoveCursorTo{5, 7});
    REQUIRE(3, screen.currentRow());
    REQUIRE(3, screen.currentColumn());
}

TEST_CASE("Screen.MoveCursorToNextTab", "[screen]")
{
    auto constexpr TabWidth = 8;
    Screen screen{20, 3, {}, [&](auto const& msg) { logf("{}", msg); }, {}};
    screen(MoveCursorToNextTab{});
    REQUIRE(1, screen.currentRow());
    REQUIRE(1 * TabWidth + 1, screen.currentColumn());

    screen(MoveCursorToNextTab{});
    REQUIRE(1, screen.currentRow());
    REQUIRE(2 * TabWidth + 1, screen.currentColumn());

    screen(MoveCursorToNextTab{});
    REQUIRE(1, screen.currentRow());
    REQUIRE(20, screen.currentColumn());

    screen(SetMode{Mode::AutoWrap, true});
    screen.write("A"); // 'A' is being written at the right margin
    screen.write("B"); // force wrap to next line, writing 'B' at the beginning of the line

    screen(MoveCursorToNextTab{});
    REQUIRE(2, screen.currentRow());
    REQUIRE(9, screen.currentColumn());
}

// TODO: HideCursor
// TODO: ShowCursor

// TODO: SaveCursor
// TODO: RestoreCursor

TEST_CASE("Screen.Index_outside_margin", "[screen]")
{
    Screen screen{4, 6, {}, [&](auto const& msg) { logf("{}", msg); }, {}};
    screen.write("1234\n5678\nABCD\nEFGH\nIJKL\nMNOP");
    logScreen(*this, screen, "initial");
    REQUIRE("1234\n5678\nABCD\nEFGH\nIJKL\nMNOP\n", screen.renderText());
    screen(SetTopBottomMargin{2, 4});

    // with cursor above top margin
    screen(MoveCursorTo{1, 3});
    screen(Index{});
    REQUIRE("1234\n5678\nABCD\nEFGH\nIJKL\nMNOP\n", screen.renderText());
    REQUIRE(2, screen.currentRow());
    REQUIRE(3, screen.currentColumn());

    // with cursor below bottom margin and above bottom screen (=> only moves cursor one down)
    screen(MoveCursorTo{5, 3});
    screen(Index{});
    REQUIRE("1234\n5678\nABCD\nEFGH\nIJKL\nMNOP\n", screen.renderText());
    REQUIRE(6, screen.currentRow());
    REQUIRE(3, screen.currentColumn());

    // with cursor below bottom margin and at bottom screen (=> no-op)
    screen(MoveCursorTo{6, 3});
    screen(Index{});
    REQUIRE("1234\n5678\nABCD\nEFGH\nIJKL\nMNOP\n", screen.renderText());
    REQUIRE(6, screen.currentRow());
    REQUIRE(3, screen.currentColumn());
}

TEST_CASE("Screen.Index_inside_margin", "[screen]")
{
    Screen screen{2, 6, {}, [&](auto const& msg) { logf("{}", msg); }, {}};
    screen.write("11\n22\n33\n44\n55\n66");
    logScreen(*this, screen, "initial setup");

    // test IND when cursor is within margin range (=> move cursor down)
    screen(SetTopBottomMargin{2, 4});
    screen(MoveCursorTo{3, 2});
    screen(Index{});
    logScreen(*this, screen, "IND while cursor at line 3");
    REQUIRE(4, screen.currentRow());
    REQUIRE(2, screen.currentColumn());
    REQUIRE("11\n22\n33\n44\n55\n66\n", screen.renderText());
}

TEST_CASE("Screen.Index_at_bottom_margin", "[screen]")
{
    Screen screen{5, 5, {}, [&](auto const& msg) { logf("{}", msg); }, {}};
    screen.write("12345\n67890\nABCDE\nFGHIJ\nKLMNO");
    logScreen(*this, screen, "initial");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n", screen.renderText());

    screen(SetTopBottomMargin{2, 4});

    // test IND with cursor at bottom margin and full horizontal margins
    screen(MoveCursorTo{4, 2});
    screen(Index{});
    logScreen(*this, screen, "IND while cursor at bottom margin");
    REQUIRE(4, screen.currentRow());
    REQUIRE(2, screen.currentColumn());
    REQUIRE("12345\nABCDE\nFGHIJ\n     \nKLMNO\n", screen.renderText());

    // (reset screen buffer)
    screen(MoveCursorTo{1, 1});
    screen.write("12345\n67890\nABCDE\nFGHIJ\nKLMNO");

    // test IND with cursor at bottom margin and NOT full horizontal margins
    screen(SetTopBottomMargin{2, 4});
    screen(SetLeftRightMargin{2, 4});
    screen(MoveCursorTo{4, 2});
    screen(Index{});
    REQUIRE("12345\n6BCD0\nAGHIE\nF   J\nKLMNO\n", screen.renderText());
    REQUIRE(4, screen.currentRow());
    REQUIRE(2, screen.currentColumn());
}

TEST_CASE("Screen.ReverseIndex_without_custom_margins", "[screen]")
{
    Screen screen{5, 5, {}, [&](auto const& msg) { logf("{}", msg); }, {}};
    screen.write("12345\n67890\nABCDE\nFGHIJ\nKLMNO");
    logScreen(*this, screen, "initial");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n", screen.renderText());

    // at bottom screen
    screen(MoveCursorTo{5, 2});
    screen(ReverseIndex{});
    REQUIRE(4, screen.currentRow());
    REQUIRE(2, screen.currentColumn());

    screen(ReverseIndex{});
    REQUIRE(3, screen.currentRow());
    REQUIRE(2, screen.currentColumn());

    screen(ReverseIndex{});
    REQUIRE(2, screen.currentRow());
    REQUIRE(2, screen.currentColumn());

    screen(ReverseIndex{});
    REQUIRE(1, screen.currentRow());
    REQUIRE(2, screen.currentColumn());

    screen(ReverseIndex{});
    logScreen(*this, screen, "RI at top screen");
    REQUIRE("     \n12345\n67890\nABCDE\nFGHIJ\n", screen.renderText());
    REQUIRE(1, screen.currentRow());
    REQUIRE(2, screen.currentColumn());

    screen(ReverseIndex{});
    logScreen(*this, screen, "RI at top screen");
    REQUIRE("     \n     \n12345\n67890\nABCDE\n", screen.renderText());
    REQUIRE(1, screen.currentRow());
    REQUIRE(2, screen.currentColumn());
}

TEST_CASE("Screen.ReverseIndex_with_vertical_margin", "[screen]")
{
    Screen screen{5, 5, {}, [&](auto const& msg) { logf("{}", msg); }, {}};
    screen.write("12345\n67890\nABCDE\nFGHIJ\nKLMNO");
    logScreen(*this, screen, "initial");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n", screen.renderText());

    screen(SetTopBottomMargin{2, 4});

    // below bottom margin
    screen(MoveCursorTo{5, 2});
    screen(ReverseIndex{});
    logScreen(*this, screen, "RI below bottom margin");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n", screen.renderText());
    REQUIRE(4, screen.currentRow());
    REQUIRE(2, screen.currentColumn());

    // at bottom margin
    screen(ReverseIndex{});
    logScreen(*this, screen, "RI at bottom margin");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n", screen.renderText());
    REQUIRE(3, screen.currentRow());
    REQUIRE(2, screen.currentColumn());

    screen(ReverseIndex{});
    logScreen(*this, screen, "RI middle margin");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n", screen.renderText());
    REQUIRE(2, screen.currentRow());
    REQUIRE(2, screen.currentColumn());

    // at top margin
    screen(ReverseIndex{});
    logScreen(*this, screen, "RI at top margin #1");
    REQUIRE("12345\n     \n67890\nABCDE\nKLMNO\n", screen.renderText());
    REQUIRE(2, screen.currentRow());
    REQUIRE(2, screen.currentColumn());

    // at top margin (again)
    screen(ReverseIndex{});
    logScreen(*this, screen, "RI at top margin #2");
    REQUIRE("12345\n     \n     \n67890\nKLMNO\n", screen.renderText());
    REQUIRE(2, screen.currentRow());
    REQUIRE(2, screen.currentColumn());

    // above top margin
    screen(MoveCursorTo{1, 2});
    screen(ReverseIndex{});
    logScreen(*this, screen, "RI above top margin");
    REQUIRE("12345\n     \n     \n67890\nKLMNO\n", screen.renderText());
    REQUIRE(1, screen.currentRow());
    REQUIRE(2, screen.currentColumn());

    // above top margin (top screen) => no-op
    screen(ReverseIndex{});
    logScreen(*this, screen, "RI above top margin (top-screen)");
    REQUIRE("12345\n     \n     \n67890\nKLMNO\n", screen.renderText());
    REQUIRE(1, screen.currentRow());
    REQUIRE(2, screen.currentColumn());
}

TEST_CASE("Screen.ReverseIndex_with_vertical_and_horizontal_margin", "[screen]")
{
    Screen screen{5, 5, {}, [&](auto const& msg) { logf("{}", msg); }, {}};
    screen.write("12345\n67890\nABCDE\nFGHIJ\nKLMNO");
    logScreen(*this, screen, "initial");
    REQUIRE("12345\n"
              "67890\n"
              "ABCDE\n"
              "FGHIJ\n"
              "KLMNO\n",
              screen.renderText());

    screen(SetTopBottomMargin{2, 4});
    screen(SetLeftRightMargin{2, 4});

    // below bottom margin
    screen(MoveCursorTo{5, 2});
    screen(ReverseIndex{});
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n", screen.renderText());
    REQUIRE(4, screen.currentRow());
    REQUIRE(2, screen.currentColumn());

    // at bottom margin
    screen(ReverseIndex{});
    logScreen(*this, screen, "after RI at bottom margin");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n", screen.renderText());
    REQUIRE(3, screen.currentRow());
    REQUIRE(2, screen.currentColumn());

    screen(ReverseIndex{});
    logScreen(*this, screen, "after RI at bottom margin (again)");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n", screen.renderText());
    REQUIRE(2, screen.currentRow());
    REQUIRE(2, screen.currentColumn());

    // at top margin
    screen(ReverseIndex{});
    logScreen(*this, screen, "after RI at top margin");
    REQUIRE(2, screen.currentRow());
    REQUIRE(2, screen.currentColumn());
    REQUIRE("12345\n6   0\nA789E\nFBCDJ\nKLMNO\n", screen.renderText());

    // at top margin (again)
    screen(ReverseIndex{});
    logScreen(*this, screen, "after RI at top margin (again)");
    REQUIRE("12345\n6   0\nA   E\nF789J\nKLMNO\n", screen.renderText());
    REQUIRE(2, screen.currentRow());
    REQUIRE(2, screen.currentColumn());

    // above top margin
    screen(MoveCursorTo{1, 2});
    screen(ReverseIndex{});
    REQUIRE("12345\n6   0\nA   E\nF789J\nKLMNO\n", screen.renderText());
    REQUIRE(1, screen.currentRow());
    REQUIRE(2, screen.currentColumn());
}

// TODO: SetForegroundColor
// TODO: SetBackgroundColor
// TODO: SetGraphicsRendition
// TODO: SetScrollingRegion

// TODO: SetMode
// TODO: SendMouseEvents
// TODO: AlternateKeypadMode

// TODO: DesignateCharset
// TODO: SingleShiftSelect

// TODO: ChangeWindowTitle
// TODO: ChangeIconName

// TODO: Bell
// TODO: FullReset

// TODO: DeviceStatusReport
// TODO: ReportCursorPosition
// TODO: SendDeviceAttributes
// TODO: SendTerminalId
#endif
