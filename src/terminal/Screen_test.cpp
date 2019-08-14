#define CATCH_CONFIG_MAIN
#include <terminal/Screen.h>
#include <catch2/catch.hpp>
#include <string_view>

using namespace terminal;
using namespace std;

void logScreenText(Screen const& screen, string const& headline = "")
{
    if (headline.empty())
        UNSCOPED_INFO("dump:");
    else
        UNSCOPED_INFO(headline + ":");

    for (size_t row = 1; row <= screen.rowCount(); ++row)
        UNSCOPED_INFO(fmt::format("[{}] \"{}\"", row, screen.renderTextLine(row)));
}

TEST_CASE("AppendChar", "[screen]")
{
    auto screen = Screen{3, 1, {}, [&](auto const& msg) { INFO(msg); }, {}};
    REQUIRE("   " == screen.renderTextLine(1));

    screen.write("A");
    REQUIRE("A  " == screen.renderTextLine(1));

    screen.write("B");
    REQUIRE("AB " == screen.renderTextLine(1));

    screen.write("C");
    REQUIRE("ABC" == screen.renderTextLine(1));

    screen.write("D");
    REQUIRE("ABD" == screen.renderTextLine(1));
}

TEST_CASE("AppendChar_AutoWrap", "[screen]")
{
    auto screen = Screen{3, 2, {}, [&](auto const& msg) { INFO(msg); }, {}};
    screen(SetMode{Mode::AutoWrap, true});

    screen.write("ABC");
    REQUIRE("ABC" == screen.renderTextLine(1));
    REQUIRE("   " == screen.renderTextLine(2));
    REQUIRE(1 == screen.currentRow());
    REQUIRE(3 == screen.currentColumn());

    screen.write("D");
    REQUIRE("ABC" == screen.renderTextLine(1));
    REQUIRE("D  " == screen.renderTextLine(2));

    screen.write("EF");
    REQUIRE("ABC" == screen.renderTextLine(1));
    REQUIRE("DEF" == screen.renderTextLine(2));

    screen.write("G");
    REQUIRE("DEF" == screen.renderTextLine(1));
    REQUIRE("G  " == screen.renderTextLine(2));
}

TEST_CASE("AppendChar_AutoWrap_LF", "[screen]")
{
    auto screen = Screen{3, 2, {}, [&](auto const& msg) { INFO(msg); }, {}};
    screen(SetMode{Mode::AutoWrap, true});
    printf("inside test above sections!\n");

    INFO("write ABC");
    screen.write("ABC");
    logScreenText(screen);
    REQUIRE("ABC" == screen.renderTextLine(1));
    REQUIRE("   " == screen.renderTextLine(2));
    REQUIRE(1 == screen.currentRow());
    REQUIRE(3 == screen.currentColumn());

    INFO("write LF");
    screen.write("\n");
    logScreenText(screen, "after writing LF");
    REQUIRE(2 == screen.currentRow());
    REQUIRE(1 == screen.currentColumn());

    INFO("write 'D'");
    screen.write("D");
    logScreenText(screen);
    REQUIRE("ABC" == screen.renderTextLine(1));
    REQUIRE("D  " == screen.renderTextLine(2));
    REQUIRE(2 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());
}

TEST_CASE("Backspace", "[screen]")
{
    auto screen = Screen{3, 2, {}, [&](auto const& msg) { INFO(msg); }, {}};
    REQUIRE(1 == screen.currentRow());
    REQUIRE(1 == screen.currentColumn());

    screen.write("12");
    CHECK("12 " == screen.renderTextLine(1));;
    REQUIRE(1 == screen.currentRow());
    REQUIRE(3 == screen.currentColumn());

    screen.write("\b");
    CHECK("12 " == screen.renderTextLine(1));;
    REQUIRE(1 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());

    screen.write("\b");
    CHECK("12 " == screen.renderTextLine(1));;
    REQUIRE(1 == screen.currentRow());
    REQUIRE(1 == screen.currentColumn());

    screen.write("\b");
    CHECK("12 " == screen.renderTextLine(1));;
    REQUIRE(1 == screen.currentRow());
    REQUIRE(1 == screen.currentColumn());
}

TEST_CASE("Linefeed", "[screen]")
{
    auto screen = Screen{2, 2, {}, [&](auto const& msg) { INFO(msg); }, {}};
    SECTION("with scroll-up") {
        INFO("init:");
        INFO(fmt::format("  line 1: '{}'", screen.renderTextLine(1)));
        INFO(fmt::format("  line 2: '{}'", screen.renderTextLine(2)));

        screen.write("1\n2");

        INFO("after writing '1\\n2':");
        INFO(fmt::format("  line 1: '{}'", screen.renderTextLine(1)));
        INFO(fmt::format("  line 2: '{}'", screen.renderTextLine(2)));

        REQUIRE("1 " == screen.renderTextLine(1));
        REQUIRE("2 " == screen.renderTextLine(2));

        screen.write("\n3"); // line 3

        INFO("After writing '\\n3':");
        INFO(fmt::format("  line 1: '{}'", screen.renderTextLine(1)));
        INFO(fmt::format("  line 2: '{}'", screen.renderTextLine(2)));

        REQUIRE("2 " == screen.renderTextLine(1));
        REQUIRE("3 " == screen.renderTextLine(2));
    }
}

TEST_CASE("ClearToEndOfScreen", "[screen]")
{
    auto screen = Screen{2, 2, {}, [&](auto const& msg) { INFO(msg); }, {}};

    screen.write("AB\nC");
    CHECK("AB" == screen.renderTextLine(1));
    CHECK("C " == screen.renderTextLine(2));
    screen(ClearToEndOfScreen{});

    CHECK("AB" == screen.renderTextLine(1));
    CHECK("  " == screen.renderTextLine(2));
}

TEST_CASE("ClearToBeginOfScreen", "[screen]")
{
    Screen screen{2, 3, {}, [&](auto const& msg) { INFO(msg); }, {}};
    screen.write("AB\nCD\nE");

    REQUIRE("AB" == screen.renderTextLine(1));
    REQUIRE("CD" == screen.renderTextLine(2));
    REQUIRE("E " == screen.renderTextLine(3));
    REQUIRE(3 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());

    screen(MoveCursorUp{1});
    screen(ClearToBeginOfScreen{});

    CHECK("  " == screen.renderTextLine(1));
    CHECK("  " == screen.renderTextLine(2));
    CHECK("E " == screen.renderTextLine(3));
    REQUIRE(2 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());
}

TEST_CASE("ClearScreen", "[screen]")
{
    Screen screen{2, 2, {}, [&](auto const& msg) { INFO(msg); }, {}};
    screen.write("AB\nC");
    screen(ClearScreen{});
    CHECK("  " == screen.renderTextLine(1));
    CHECK("  " == screen.renderTextLine(2));
}

TEST_CASE("ClearToEndOfLine", "[screen]")
{
    Screen screen{3, 1, {}, [&](auto const& msg) { INFO(msg); }, {}};
    screen.write("ABC");
    REQUIRE("ABC" == screen.renderTextLine(1));

    screen(MoveCursorToColumn{2});
    screen(ClearToEndOfLine{});
    CHECK("A  " == screen.renderTextLine(1));
}

TEST_CASE("ClearToBeginOfLine", "[screen]")
{
    Screen screen{3, 1, {}, [&](auto const& msg) { INFO(msg); }, {}};
    screen(SetMode{Mode::AutoWrap, false});
    screen.write("ABC");
    REQUIRE("ABC" == screen.renderTextLine(1));

    screen(MoveCursorToColumn{2});
    screen(ClearToBeginOfLine{});
    CHECK("  C" == screen.renderTextLine(1));
}

TEST_CASE("ClearLine", "[screen]")
{
    Screen screen{3, 1, {}, [&](auto const& msg) { INFO(msg); }, {}};
    screen(SetMode{Mode::AutoWrap, false});
    screen.write("ABC");
    REQUIRE("ABC" == screen.renderTextLine(1));

    screen(ClearLine{});
    CHECK("   " == screen.renderTextLine(1));
}

TEST_CASE("InsertLines", "[screen]")
{
    Screen screen{2, 3, {}, [&](auto const& msg) { INFO(msg); }, {}};

    screen.write("AB\nCD");
    REQUIRE("AB" == screen.renderTextLine(1));
    REQUIRE("CD" == screen.renderTextLine(2));
    REQUIRE("  " == screen.renderTextLine(3));

    screen(InsertLines{1});
    CHECK("AB" == screen.renderTextLine(1));
    CHECK("  " == screen.renderTextLine(2));
    CHECK("CD" == screen.renderTextLine(3));

    screen(MoveCursorTo{1, 1});
    screen(InsertLines{1});
    CHECK("  " == screen.renderTextLine(1));
    CHECK("AB" == screen.renderTextLine(2));
    CHECK("  " == screen.renderTextLine(3));

    // TODO: test with (top/bottom and left/right) margins enabled
}

TEST_CASE("DeleteLines", "[screen]")
{
    Screen screen{2, 3, {}, [&](auto const& msg) { INFO(msg); }, {}};

    screen.write("AB\nCD\nEF");
    logScreenText(screen, "initial");
    REQUIRE("AB" == screen.renderTextLine(1));
    REQUIRE("CD" == screen.renderTextLine(2));
    REQUIRE("EF" == screen.renderTextLine(3));

    screen(MoveCursorTo{2, 1});
    REQUIRE(screen.currentRow() == 2);
    REQUIRE(screen.currentColumn() == 1);

    SECTION("no-op") {
        screen(DeleteLines{0});
        REQUIRE("AB" == screen.renderTextLine(1));
        REQUIRE("CD" == screen.renderTextLine(2));
        REQUIRE("EF" == screen.renderTextLine(3));
    }

    SECTION("in-range") {
        screen(DeleteLines{1});
        logScreenText(screen, "After EL(1)");
        REQUIRE("AB" == screen.renderTextLine(1));
        REQUIRE("EF" == screen.renderTextLine(2));
        REQUIRE("  " == screen.renderTextLine(3));
    }

    SECTION("clamped") {
        screen(MoveCursorTo{2, 2});
        screen(DeleteLines{5});
        logScreenText(screen, "After clamped EL(5)");
        REQUIRE("AB" == screen.renderTextLine(1));
        REQUIRE("  " == screen.renderTextLine(2));
        REQUIRE("  " == screen.renderTextLine(3));
    }
}

// TODO: DeleteCharacters

// TODO: ClearScrollbackBuffer
// TODO: ScrollUp
// TODO: ScrollDown

TEST_CASE("MoveCursorUp", "[screen]")
{
    Screen screen{2, 3, {}, [&](auto const& msg) { INFO(msg); }, {}};
    screen.write("AB\nCD\nEF");
    REQUIRE(3 == screen.currentCursor().row);
    REQUIRE(2 == screen.currentCursor().column);

    // no-op
    screen(MoveCursorUp{0});
    REQUIRE(3 == screen.currentCursor().row);
    REQUIRE(2 == screen.currentCursor().column);

    // in-range
    screen(MoveCursorUp{1});
    REQUIRE(2 == screen.currentCursor().row);
    REQUIRE(2 == screen.currentCursor().column);

    // overflow
    screen(MoveCursorUp{5});
    REQUIRE(1 == screen.currentCursor().row);
    REQUIRE(2 == screen.currentCursor().column);
}

TEST_CASE("MoveCursorDown", "[screen]")
{
    Screen screen{2, 3, {}, [&](auto const& msg) { UNSCOPED_INFO(msg); }, {}};
    screen.write("A");
    REQUIRE(1 == screen.currentCursor().row);
    REQUIRE(2 == screen.currentCursor().column);

    // no-op
    screen(MoveCursorDown{0});
    REQUIRE(1 == screen.currentCursor().row);
    REQUIRE(2 == screen.currentCursor().column);

    // in-range
    screen(MoveCursorDown{1});
    REQUIRE(2 == screen.currentCursor().row);
    REQUIRE(2 == screen.currentCursor().column);

    // overflow
    screen(MoveCursorDown{5});
    REQUIRE(3 == screen.currentCursor().row);
    REQUIRE(2 == screen.currentCursor().column);
}

TEST_CASE("MoveCursorForward", "[screen]")
{
    Screen screen{3, 3, {}, [&](auto const& msg) { UNSCOPED_INFO(msg); }, {}};
    REQUIRE(1 == screen.currentRow());
    REQUIRE(1 == screen.currentColumn());

    // no-op
    screen(MoveCursorForward{0});
    REQUIRE(1 == screen.currentRow());
    REQUIRE(1 == screen.currentColumn());

    // in-range
    screen(MoveCursorForward{1});
    REQUIRE(1 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());

    // overflow
    screen(MoveCursorForward{5});
    REQUIRE(1 == screen.currentRow());
    REQUIRE(3 == screen.currentColumn());
}

TEST_CASE("MoveCursorBackward", "[screen]")
{
    Screen screen{3, 3, {}, [&](auto const& msg) { UNSCOPED_INFO(msg); }, {}};
    screen.write("ABC");
    REQUIRE(1 == screen.currentRow());
    REQUIRE(3 == screen.currentColumn());

    // no-op
    screen(MoveCursorBackward{0});
    REQUIRE(1 == screen.currentRow());
    REQUIRE(3 == screen.currentColumn());

    // in-range
    screen(MoveCursorBackward{1});
    REQUIRE(1 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());

    // overflow
    screen(MoveCursorBackward{5});
    REQUIRE(1 == screen.currentRow());
    REQUIRE(1 == screen.currentColumn());
}

TEST_CASE("MoveCursorToColumn", "[screen]")
{
    Screen screen{3, 3, {}, [&](auto const& msg) { UNSCOPED_INFO(msg); }, {}};
    REQUIRE(1 == screen.currentRow());
    REQUIRE(1 == screen.currentColumn());

    // no-op
    screen(MoveCursorToColumn{1});
    REQUIRE(1 == screen.currentRow());
    REQUIRE(1 == screen.currentColumn());

    // in-range
    screen(MoveCursorToColumn{3});
    REQUIRE(1 == screen.currentRow());
    REQUIRE(3 == screen.currentColumn());

    screen(MoveCursorToColumn{2});
    REQUIRE(1 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());

    // overflow
    screen(MoveCursorToColumn{5});
    REQUIRE(1 == screen.currentRow());
    REQUIRE(3 == screen.currentColumn()); // clamped
}

TEST_CASE("MoveCursorToBeginOfLine", "[screen]")
{
    Screen screen{3, 3, {}, [&](auto const& msg) { UNSCOPED_INFO(msg); }, {}};

    screen.write("\nAB");
    REQUIRE(2 == screen.currentRow());
    REQUIRE(3 == screen.currentColumn());

    screen(MoveCursorToBeginOfLine{});
    REQUIRE(2 == screen.currentRow());
    REQUIRE(1 == screen.currentColumn());
}

TEST_CASE("MoveCursorTo", "[screen]")
{
    Screen screen{3, 3, {}, [&](auto const& msg) { UNSCOPED_INFO(msg); }, {}};

    // in-range
    screen(MoveCursorTo{3, 2});
    REQUIRE(3 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());

    // origin
    screen(MoveCursorTo{1, 1});
    REQUIRE(1 == screen.currentRow());
    REQUIRE(1 == screen.currentColumn());

    // overflow
    screen(MoveCursorTo{5, 7});
    REQUIRE(3 == screen.currentRow());
    REQUIRE(3 == screen.currentColumn());
}

TEST_CASE("MoveCursorToNextTab", "[screen]")
{
    auto constexpr TabWidth = 8;
    Screen screen{20, 3, {}, [&](auto const& msg) { UNSCOPED_INFO(msg); }, {}};
    screen(MoveCursorToNextTab{});
    REQUIRE(1 == screen.currentRow());
    REQUIRE(1 * TabWidth + 1 == screen.currentColumn());

    screen(MoveCursorToNextTab{});
    REQUIRE(1 == screen.currentRow());
    REQUIRE(2 * TabWidth + 1 == screen.currentColumn());

    screen(MoveCursorToNextTab{});
    REQUIRE(1 == screen.currentRow());
    REQUIRE(20 == screen.currentColumn());

    screen(SetMode{Mode::AutoWrap, true});
    screen.write("A"); // 'A' is being written at the right margin
    screen.write("B"); // force wrap to next line, writing 'B' at the beginning of the line

    screen(MoveCursorToNextTab{});
    REQUIRE(2 == screen.currentRow());
    REQUIRE(9 == screen.currentColumn());
}

// TODO: HideCursor
// TODO: ShowCursor

// TODO: SaveCursor
// TODO: RestoreCursor

TEST_CASE("Index_outside_margin", "[screen]")
{
    Screen screen{4, 6, {}, [&](auto const& msg) { UNSCOPED_INFO(msg); }, {}};
    screen.write("1234\n5678\nABCD\nEFGH\nIJKL\nMNOP");
    logScreenText(screen, "initial");
    REQUIRE("1234\n5678\nABCD\nEFGH\nIJKL\nMNOP\n" == screen.renderText());
    screen(SetTopBottomMargin{2, 4});

    // with cursor above top margin
    screen(MoveCursorTo{1, 3});
    screen(Index{});
    REQUIRE("1234\n5678\nABCD\nEFGH\nIJKL\nMNOP\n" == screen.renderText());
    REQUIRE(2 == screen.currentRow());
    REQUIRE(3 == screen.currentColumn());

    // with cursor below bottom margin and above bottom screen (=> only moves cursor one down)
    screen(MoveCursorTo{5, 3});
    screen(Index{});
    REQUIRE("1234\n5678\nABCD\nEFGH\nIJKL\nMNOP\n" == screen.renderText());
    REQUIRE(6 == screen.currentRow());
    REQUIRE(3 == screen.currentColumn());

    // with cursor below bottom margin and at bottom screen (=> no-op)
    screen(MoveCursorTo{6, 3});
    screen(Index{});
    REQUIRE("1234\n5678\nABCD\nEFGH\nIJKL\nMNOP\n" == screen.renderText());
    REQUIRE(6 == screen.currentRow());
    REQUIRE(3 == screen.currentColumn());
}

TEST_CASE("Index_inside_margin", "[screen]")
{
    Screen screen{2, 6, {}, [&](auto const& msg) { UNSCOPED_INFO(msg); }, {}};
    screen.write("11\n22\n33\n44\n55\n66");
    logScreenText(screen, "initial setup");

    // test IND when cursor is within margin range (=> move cursor down)
    screen(SetTopBottomMargin{2, 4});
    screen(MoveCursorTo{3, 2});
    screen(Index{});
    logScreenText(screen, "IND while cursor at line 3");
    REQUIRE(4 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());
    REQUIRE("11\n22\n33\n44\n55\n66\n" == screen.renderText());
}

TEST_CASE("Index_at_bottom_margin", "[screen]")
{
    Screen screen{5, 5, {}, [&](auto const& msg) { UNSCOPED_INFO(msg); }, {}};
    screen.write("12345\n67890\nABCDE\nFGHIJ\nKLMNO");
    logScreenText(screen, "initial setup");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());

    screen(SetTopBottomMargin{2, 4});

    // test IND with cursor at bottom margin and full horizontal margins
    screen(MoveCursorTo{4, 2});
    screen(Index{});
    logScreenText(screen, "IND while cursor at bottom margin");
    REQUIRE(4 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());
    REQUIRE("12345\nABCDE\nFGHIJ\n     \nKLMNO\n" == screen.renderText());

    // (reset screen buffer)
    screen(MoveCursorTo{1, 1});
    screen.write("12345\n67890\nABCDE\nFGHIJ\nKLMNO");

    // test IND with cursor at bottom margin and NOT full horizontal margins
    screen(SetTopBottomMargin{2, 4});
    screen(SetLeftRightMargin{2, 4});
    screen(MoveCursorTo{4, 2});
    screen(Index{});
    REQUIRE("12345\n6BCD0\nAGHIE\nF   J\nKLMNO\n" == screen.renderText());
    REQUIRE(4 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());
}

TEST_CASE("ReverseIndex_without_custom_margins", "[screen]")
{
    Screen screen{5, 5, {}, [&](auto const& msg) { UNSCOPED_INFO(msg); }, {}};
    screen.write("12345\n67890\nABCDE\nFGHIJ\nKLMNO");
    logScreenText(screen, "initial");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());

    // at bottom screen
    screen(MoveCursorTo{5, 2});
    screen(ReverseIndex{});
    REQUIRE(4 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());

    screen(ReverseIndex{});
    REQUIRE(3 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());

    screen(ReverseIndex{});
    REQUIRE(2 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());

    screen(ReverseIndex{});
    REQUIRE(1 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());

    screen(ReverseIndex{});
    logScreenText(screen, "RI at top screen");
    REQUIRE("     \n12345\n67890\nABCDE\nFGHIJ\n" == screen.renderText());
    REQUIRE(1 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());

    screen(ReverseIndex{});
    logScreenText(screen, "RI at top screen");
    REQUIRE("     \n     \n12345\n67890\nABCDE\n" == screen.renderText());
    REQUIRE(1 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());
}

TEST_CASE("ReverseIndex_with_vertical_margin", "[screen]")
{
    Screen screen{5, 5, {}, [&](auto const& msg) { UNSCOPED_INFO(msg); }, {}};
    screen.write("12345\n67890\nABCDE\nFGHIJ\nKLMNO");
    logScreenText(screen, "initial");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());

    screen(SetTopBottomMargin{2, 4});

    // below bottom margin
    screen(MoveCursorTo{5, 2});
    screen(ReverseIndex{});
    logScreenText(screen, "RI below bottom margin");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(4 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());

    // at bottom margin
    screen(ReverseIndex{});
    logScreenText(screen, "RI at bottom margin");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(3 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());

    screen(ReverseIndex{});
    logScreenText(screen, "RI middle margin");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(2 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());

    // at top margin
    screen(ReverseIndex{});
    logScreenText(screen, "RI at top margin #1");
    REQUIRE("12345\n     \n67890\nABCDE\nKLMNO\n" == screen.renderText());
    REQUIRE(2 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());

    // at top margin (again)
    screen(ReverseIndex{});
    logScreenText(screen, "RI at top margin #2");
    REQUIRE("12345\n     \n     \n67890\nKLMNO\n" == screen.renderText());
    REQUIRE(2 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());

    // above top margin
    screen(MoveCursorTo{1, 2});
    screen(ReverseIndex{});
    logScreenText(screen, "RI above top margin");
    REQUIRE("12345\n     \n     \n67890\nKLMNO\n" == screen.renderText());
    REQUIRE(1 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());

    // above top margin (top screen) => no-op
    screen(ReverseIndex{});
    logScreenText(screen, "RI above top margin (top-screen)");
    REQUIRE("12345\n     \n     \n67890\nKLMNO\n" == screen.renderText());
    REQUIRE(1 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());
}

TEST_CASE("ReverseIndex_with_vertical_and_horizontal_margin", "[screen]")
{
    Screen screen{5, 5, {}, [&](auto const& msg) { UNSCOPED_INFO(msg); }, {}};
    screen.write("12345\n67890\nABCDE\nFGHIJ\nKLMNO");
    logScreenText(screen, "initial");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());

    screen(SetTopBottomMargin{2, 4});
    screen(SetLeftRightMargin{2, 4});

    // below bottom margin
    screen(MoveCursorTo{5, 2});
    screen(ReverseIndex{});
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(4 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());

    // at bottom margin
    screen(ReverseIndex{});
    logScreenText(screen, "after RI at bottom margin");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(3 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());

    screen(ReverseIndex{});
    logScreenText(screen, "after RI at bottom margin (again)");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(2 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());

    // at top margin
    screen(ReverseIndex{});
    logScreenText(screen, "after RI at top margin");
    REQUIRE(2 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());
    REQUIRE("12345\n6   0\nA789E\nFBCDJ\nKLMNO\n" == screen.renderText());

    // at top margin (again)
    screen(ReverseIndex{});
    logScreenText(screen, "after RI at top margin (again)");
    REQUIRE("12345\n6   0\nA   E\nF789J\nKLMNO\n" == screen.renderText());
    REQUIRE(2 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());

    // above top margin
    screen(MoveCursorTo{1, 2});
    screen(ReverseIndex{});
    REQUIRE("12345\n6   0\nA   E\nF789J\nKLMNO\n" == screen.renderText());
    REQUIRE(1 == screen.currentRow());
    REQUIRE(2 == screen.currentColumn());
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
