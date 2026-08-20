// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/input/InputGenerator.hpp>
#include <vtbackend/screen/Screen.hpp>
#include <vtbackend/screen/ScreenTestFixtures.hpp>
#include <vtbackend/screen/Viewport.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>
#include <vtbackend/vt/Charset.hpp>

#include <crispy/Escape.hpp>
#include <crispy/Utils.hpp>

#include <libunicode/convert.h>

#include <catch2/catch_test_macros.hpp>

#include <charconv>
#include <cstddef>
#include <ranges>
#include <set>
#include <string_view>
using crispy::escape;
using namespace vtbackend;
using namespace vtbackend::test;
using namespace std;
using namespace std::literals::chrono_literals;

// NOLINTBEGIN(misc-const-correctness,readability-function-cognitive-complexity)

TEST_CASE("ClearToEndOfScreen", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("ABC\r\nDEF\r\nGHI");

    REQUIRE("ABC" == screen.grid().lineText(LineOffset(0)));
    REQUIRE("DEF" == screen.grid().lineText(LineOffset(1)));
    REQUIRE("GHI" == screen.grid().lineText(LineOffset(2)));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(2), ColumnOffset(2) });

    logScreenText(screen);
    mock.writeToScreen(CUP(2, 2));
    mock.writeToScreen(ED());
    logScreenText(screen);

    CHECK("ABC" == screen.grid().lineText(LineOffset(0)));
    CHECK("D  " == screen.grid().lineText(LineOffset(1)));
    CHECK("   " == screen.grid().lineText(LineOffset(2)));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(1) });
}

TEST_CASE("ClearToBeginOfScreen", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("ABC\r\nDEF\r\nGHI");

    REQUIRE("ABC" == screen.grid().lineText(LineOffset(0)));
    REQUIRE("DEF" == screen.grid().lineText(LineOffset(1)));
    REQUIRE("GHI" == screen.grid().lineText(LineOffset(2)));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(2), ColumnOffset(2) });

    screen.moveCursorTo(LineOffset(1), ColumnOffset(1));
    screen.clearToBeginOfScreen();

    CHECK("   " == screen.grid().lineText(LineOffset(0)));
    CHECK("  F" == screen.grid().lineText(LineOffset(1)));
    CHECK("GHI" == screen.grid().lineText(LineOffset(2)));
    CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(1) });
}

TEST_CASE("ClearScreen", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(2) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("AB\r\nC");
    screen.clearScreen();
    CHECK("  " == screen.grid().lineText(LineOffset(0)));
    CHECK("  " == screen.grid().lineText(LineOffset(1)));
}

TEST_CASE("ClearToEndOfLine", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("ABC");
    REQUIRE("ABC" == screen.grid().lineText(LineOffset(0)));

    screen.moveCursorToColumn(ColumnOffset(1));
    screen.clearToEndOfLine();
    CHECK("A  " == screen.grid().lineText(LineOffset(0)));
}

TEST_CASE("ClearToBeginOfLine", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.terminal.setMode(DECMode::AutoWrap, false);
    mock.writeToScreen("ABC");
    REQUIRE("ABC" == screen.grid().lineText(LineOffset(0)));

    screen.moveCursorToColumn(ColumnOffset(1));
    screen.clearToBeginOfLine();
    CHECK("  C" == screen.grid().lineText(LineOffset(0)));
}

TEST_CASE("ClearLine", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.terminal.setMode(DECMode::AutoWrap, false);
    mock.writeToScreen("ABC");
    REQUIRE("ABC" == screen.grid().lineText(LineOffset(0)));

    screen.clearLine();
    CHECK("   " == screen.grid().lineText(LineOffset(0)));
}

TEST_CASE("DECSEL-0", "[screen]")
{
    // Erasing from the cursor position forwards to the end of the current line.
    for (auto const param: { "0"sv, ""sv })
    {
        INFO(std::format("param: \"{}\"", param));
        auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(6) } };
        auto& screen = mock.terminal.primaryScreen();
        mock.writeToScreen(std::format("AB{}CDE{}F", "\033[1\"q", "\033[2\"q"));
        REQUIRE("ABCDEF" == screen.grid().lineText(LineOffset(0)));
        mock.writeToScreen("\033[1;2H");
        mock.writeToScreen(std::format("\033[?{}K", param));
        REQUIRE("A CDE " == screen.grid().lineText(LineOffset(0)));
    }
}

TEST_CASE("DECSEL-1", "[screen]")
{
    // Erasing from the cursor position backwards to the beginning of the current line.
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(6) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen(std::format("A{}BCD{}EF", "\033[1\"q", "\033[2\"q"));
    REQUIRE("ABCDEF" == screen.grid().lineText(LineOffset(0)));

    mock.writeToScreen("\033[1;5H");
    mock.writeToScreen("\033[?1K");
    REQUIRE(" BCD F" == screen.grid().lineText(LineOffset(0)));
}

TEST_CASE("DECSEL-2", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(4) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("ABCD");
    REQUIRE("ABCD" == screen.grid().lineText(LineOffset(0)));

    mock.writeToScreen(std::format("\ra{}bc{}d\r", "\033[1\"q", "\033[2\"q"));
    REQUIRE("abcd" == screen.grid().lineText(LineOffset(0)));

    mock.writeToScreen("\033[?2K");
    REQUIRE(" bc " == screen.grid().lineText(LineOffset(0)));

    mock.writeToScreen(std::format("\r{0}A{1}BC{0}D", "\033[1\"q", "\033[2\"q")); // DECSCA 2
    REQUIRE("ABCD" == screen.grid().lineText(LineOffset(0)));
    mock.writeToScreen("\033[?2K");
    REQUIRE("A  D" == screen.grid().lineText(LineOffset(0)));
}

TEST_CASE("DECSED-0", "[screen]")
{
    for (auto const param: { "0"sv, ""sv })
    {
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(3) } };
        auto& screen = mock.terminal.primaryScreen();

        mock.writeToScreen(std::format("{0}A{1}B{0}C{1}\r\n"
                                       "D{0}E{1}F\r\n"
                                       "{0}G{1}H{0}I{1}",
                                       "\033[1\"q",
                                       "\033[2\"q"));

        REQUIRE(e(mainPageText(screen)) == "ABC\\nDEF\\nGHI\\n");

        mock.writeToScreen("\033[2;2H");
        mock.writeToScreen(std::format("\033[?{}J", param));
        REQUIRE(e(mainPageText(screen)) == "ABC\\nDE \\nG I\\n");
    }
}

TEST_CASE("DECSED-1", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen(std::format("{0}A{1}B{0}C{1}\r\n"
                                   "D{0}E{1}F\r\n"
                                   "{0}G{1}H{0}I{1}",
                                   "\033[1\"q",
                                   "\033[2\"q"));

    REQUIRE(e(mainPageText(screen)) == "ABC\\nDEF\\nGHI\\n");

    mock.writeToScreen("\033[2;2H");
    mock.writeToScreen("\033[?1J");
    REQUIRE(e(mainPageText(screen)) == "A C\\n EF\\nGHI\\n");
}

TEST_CASE("DECSED-2", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen(std::format("{0}A{1}B{0}C{1}\r\n"
                                   "D{0}E{1}F\r\n"
                                   "{0}G{1}H{0}I{1}",
                                   "\033[1\"q",
                                   "\033[2\"q"));

    REQUIRE(e(mainPageText(screen)) == "ABC\\nDEF\\nGHI\\n");

    mock.writeToScreen("\033[2;2H");
    mock.writeToScreen("\033[?2J");
    REQUIRE(e(mainPageText(screen)) == "A C\\n E \\nG I\\n");
}

TEST_CASE("DECSED-2: lines without protected characters are erased correctly", "[screen]")
{
    // Regression test: selectiveEraseLine must erase the correct line even when
    // the line has no protected characters and is not the cursor's current line.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();

    // Line 0: "ABC" — all unprotected
    // Line 1: "DEF" — all unprotected
    // Line 2: protected "G", unprotected "H", protected "I"
    mock.writeToScreen("ABC\r\nDEF\r\n");
    mock.writeToScreen(std::format("{0}G{1}H{0}I{1}", "\033[1\"q", "\033[2\"q"));

    REQUIRE(e(mainPageText(screen)) == "ABC\\nDEF\\nGHI\\n");

    // Move cursor to line 1, col 1 and perform DECSED-2 (erase entire display selectively).
    // Lines 0 and 1 have NO protected characters, so they should be fully erased.
    // Line 2 should keep 'G' and 'I' (protected) but erase 'H'.
    mock.writeToScreen("\033[2;2H");
    mock.writeToScreen("\033[?2J");
    REQUIRE(e(mainPageText(screen)) == "   \\n   \\nG I\\n");
}

TEST_CASE("DECSERA-all-defaults", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen(std::format("{0}A{1}B{0}C{1}\r\n"
                                   "D{0}E{1}F\r\n"
                                   "{0}G{1}H{0}I{1}",
                                   "\033[1\"q",
                                   "\033[2\"q"));

    REQUIRE(e(mainPageText(screen)) == "ABC\\nDEF\\nGHI\\n");

    mock.writeToScreen("\033[${");
    REQUIRE(e(mainPageText(screen)) == "A C\\n E \\nG I\\n");
}

TEST_CASE("DECSERA", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen(std::format("{0}A{1}B{0}C{1}\r\n"
                                   "D{0}E{1}F\r\n"
                                   "{0}G{1}H{0}I{1}",
                                   "\033[1\"q",
                                   "\033[2\"q"));

    REQUIRE(e(mainPageText(screen)) == "ABC\\nDEF\\nGHI\\n");

    mock.writeToScreen("\033[2;2;3;3${");
    REQUIRE(e(mainPageText(screen)) == "ABC\\nDE \\nG I\\n");
}

TEST_CASE("DECFRA", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");

    mock.writeToScreen("\033[46;2;2;4;4$x");
    CHECK(escape(mainPageText(screen)) == "12345\\n6...0\\nA...E\\nF...J\\nKLMNO\\n");
}

TEST_CASE("DECFRA.Vertical", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");

    mock.writeToScreen("\033[46;3;1;3;5$x");
    CHECK(escape(mainPageText(screen)) == "12345\\n67890\\n.....\\nFGHIJ\\nKLMNO\\n");
}

TEST_CASE("DECFRA.Horizontal", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");

    mock.writeToScreen("\033[46;1;3;5;3$x");
    CHECK(escape(mainPageText(screen)) == "12.45\\n67.90\\nAB.DE\\nFG.IJ\\nKL.NO\\n");
}

TEST_CASE("DECFRA.Invalid", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");

    mock.writeToScreen("\033[46;0;0;5;5$x");
    CHECK(escape(mainPageText(screen)) == ".....\\n.....\\n.....\\n.....\\n.....\\n");
}

TEST_CASE("DECFRA.Default", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");

    mock.writeToScreen("\033[46$x");
    CHECK(escape(mainPageText(screen)) == ".....\\n.....\\n.....\\n.....\\n.....\\n");
}

TEST_CASE("DECFRA.Full", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");

    mock.writeToScreen("\033[46;1;1;5;5$x");
    CHECK(escape(mainPageText(screen)) == ".....\\n.....\\n.....\\n.....\\n.....\\n");
}

TEST_CASE("EraseCharacters", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO\033[H");
    logScreenText(screen, "AFTER POPULATE");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });

    SECTION("ECH-0 equals ECH-1")
    {
        screen.eraseCharacters(ColumnCount(0));
        REQUIRE(" 2345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
    }

    SECTION("ECH-1")
    {
        screen.eraseCharacters(ColumnCount(1));
        REQUIRE(" 2345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
    }

    SECTION("ECH-2")
    {
        screen.eraseCharacters(ColumnCount(2));
        REQUIRE("  345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
    }

    SECTION("ECH-2@2.2")
    {
        screen.moveCursorTo(LineOffset(1), ColumnOffset(1));
        screen.eraseCharacters(ColumnCount(2));
        REQUIRE("12345\n6  90\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
    }

    SECTION("ECH-4")
    {
        screen.eraseCharacters(ColumnCount(4));
        REQUIRE("    5\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
    }

    SECTION("ECH-5")
    {
        screen.eraseCharacters(ColumnCount(5));
        REQUIRE("     \n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
    }

    SECTION("ECH-6-clamped")
    {
        screen.eraseCharacters(ColumnCount(6));
        REQUIRE("     \n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
    }
}

TEST_CASE("ED.2_ignoresScrollRegion", "[screen]")
{
    // ED 2 erases the whole screen regardless of a DECSTBM scrolling region (the region is ignored).
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("\033[Haaa\r\nbbb\r\nccc");         // fill rows 1..3
    mock.writeToScreen("\033[2;2r");                       // DECSTBM(2,2): margin is row 2 only
    mock.writeToScreen("\033[2J");                         // ED 2
    mock.writeToScreen("\033[r");                          // reset margin
    CHECK(screen.grid().lineText(LineOffset(0)) == "   "); // row 1 cleared (outside region)
    CHECK(screen.grid().lineText(LineOffset(2)) == "   "); // row 3 cleared (outside region)
}

TEST_CASE("DECRQCRA.honors_origin_mode", "[screen]")
{
    // In origin mode (DECOM) a rectangular-area request is measured from the scroll region's top-left,
    // not the page's, so DECRQCRA(1,1) reads the origin cell rather than the absolute top-left.
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(10) } };

    mock.writeToScreen("\033[5;5HX"); // CUP(5,5) + 'X'
    mock.writeToScreen("\033[5;7r");  // DECSTBM 5;7
    mock.writeToScreen("\033[?69h");  // DECSET DECLRMM
    mock.writeToScreen("\033[5;7s");  // DECSLRM 5;7 -> origin at (5,5)
    mock.writeToScreen("\033[?6h");   // DECSET DECOM

    mock.resetReplyData();
    mock.writeToScreen("\033[1;1;1;1;1;1*y"); // DECRQCRA rect (1,1,1,1), origin-relative -> cell (5,5)='X'
    mock.terminal.flushInput();
    auto const originReply = mock.replyData();
    CHECK_FALSE(originReply.empty());

    // The identical request outside origin mode addresses absolute (1,1), a blank cell -> other checksum.
    mock.writeToScreen("\033[?6l"); // DECRESET DECOM
    mock.resetReplyData();
    mock.writeToScreen("\033[1;1;1;1;1;1*y");
    mock.terminal.flushInput();
    CHECK(originReply != mock.replyData());
}

TEST_CASE("ScreenAlignmentPattern", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    mock.terminal.setTopBottomMargin(LineOffset { 1 }, LineOffset { 3 });
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());

    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(4), ColumnOffset(4) });

    REQUIRE(1 == *screen.margin().vertical.from);
    REQUIRE(3 == *screen.margin().vertical.to);

    SECTION("test")
    {
        screen.screenAlignmentPattern();
        REQUIRE("EEEEE\nEEEEE\nEEEEE\nEEEEE\nEEEEE\n" == screen.renderMainPageText());

        REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });

        REQUIRE(0 == *screen.margin().horizontal.from);
        REQUIRE(4 == *screen.margin().horizontal.to);
        REQUIRE(0 == *screen.margin().vertical.from);
        REQUIRE(4 == *screen.margin().vertical.to);
    }
}

TEST_CASE("DECCRA.DownLeft.intersecting", "[screen]")
{
    auto mock = screenForDECRA();
    auto& screen = mock.terminal.primaryScreen();
    auto const* const initialText = "ABCDEF\n"
                                    "abcdef\n"
                                    "123456\n"
                                    "GHIJKL\n"
                                    "ghijkl\n";
    CHECK(screen.renderMainPageText() == initialText);

    auto constexpr Page = 0;

    auto constexpr STop = 4;
    auto constexpr SLeft = 3;

    auto constexpr SBottom = 5;
    auto constexpr SRight = 6;

    auto constexpr TTop = 3;
    auto constexpr TLeft = 2;

    auto const* const expectedText = "ABCDEF\n"
                                     "abcdef\n" // .3456.
                                     "1IJKL6\n" // .IJKL.
                                     "GijklL\n"
                                     "ghijkl\n";

    // copy up by one line (4 to 3), 2 lines
    // copy left by one column (3 to 2), 2 columns

    auto const deccraSeq =
        std::format("\033[{};{};{};{};{};{};{};{}$v", STop, SLeft, SBottom, SRight, Page, TTop, TLeft, Page);
    mock.writeToScreen(deccraSeq);

    auto const resultText = screen.renderMainPageText();
    CHECK(resultText == expectedText);
}

TEST_CASE("DECCRA.trailing semicolon", "[screen]")
{
    // The form vttest actually sends: esc.c:732 is `"%d;%d;%d;%d;%d;%d;%d;%d;$v"` -- eight values and
    // then a trailing `;`. ECMA-48 5.4.1 makes that a ninth, empty parameter taking its default, and an
    // omitted parameter is counted here, so DECCRA arrived with nine and matched nothing at all: the
    // whole copy was silently dropped as an unknown sequence. A terminal must ignore parameters it does
    // not use. Every other test in this file writes the eight-parameter form, which is why none caught
    // it -- and vttest's chapter 11.3.6 could not, because its `*` walked straight past the test.
    auto mock = screenForDECRA();
    auto& screen = mock.terminal.primaryScreen();
    REQUIRE(screen.renderMainPageText()
            == "ABCDEF\n"
               "abcdef\n"
               "123456\n"
               "GHIJKL\n"
               "ghijkl\n");

    mock.writeToScreen("\033[4;3;5;6;0;3;2;0;$v"); // note the trailing ';'

    CHECK(screen.renderMainPageText()
          == "ABCDEF\n"
             "abcdef\n"
             "1IJKL6\n"
             "GijklL\n"
             "ghijkl\n");
}

TEST_CASE("DECCRA.Right.intersecting", "[screen]")
{
    // Moves a rectangular area by one column to the right.
    auto mock = screenForDECRA();
    auto& screen = mock.terminal.primaryScreen();

    auto const* initialText = "ABCDEF\n"
                              "abcdef\n"
                              "123456\n"
                              "GHIJKL\n"
                              "ghijkl\n";
    REQUIRE(screen.renderMainPageText() == initialText);
    auto const* expectedText = "ABCDEF\n"
                               "abbcdf\n"
                               "122346\n"
                               "GHHIJL\n"
                               "ghijkl\n";

    auto constexpr Page = 0;
    auto constexpr STopLeft = CellLocation { .line = LineOffset(1), .column = ColumnOffset(1) };
    auto constexpr SBottomRight = CellLocation { .line = LineOffset(3), .column = ColumnOffset(3) };
    auto constexpr TTopLeft = CellLocation { .line = LineOffset(1), .column = ColumnOffset(2) };

    auto const deccraSeq = std::format("\033[{};{};{};{};{};{};{};{}$v",
                                       STopLeft.line + 1,
                                       STopLeft.column + 1,
                                       SBottomRight.line + 1,
                                       SBottomRight.column + 1,
                                       Page,
                                       TTopLeft.line + 1,
                                       TTopLeft.column + 1,
                                       Page);
    mock.writeToScreen(deccraSeq);

    auto const resultText = screen.renderMainPageText();
    CHECK(resultText == expectedText);
}

TEST_CASE("DECCRA.Left.intersecting", "[screen]")
{
    // Moves a rectangular area by one column to the left.
    auto mock = screenForDECRA();
    auto& screen = mock.terminal.primaryScreen();
    auto const* const initialText = "ABCDEF\n"
                                    "abcdef\n"
                                    "123456\n"
                                    "GHIJKL\n"
                                    "ghijkl\n";
    CHECK(screen.renderMainPageText() == initialText);

    auto const* const expectedText = "ABCDEF\n"
                                     "abdeff\n"
                                     "124566\n"
                                     "GHIJKL\n"
                                     "ghijkl\n";

    auto constexpr Page = 0;
    auto constexpr STopLeft = CellLocation { .line = LineOffset(1), .column = ColumnOffset(3) };
    auto constexpr SBottomRight = CellLocation { .line = LineOffset(2), .column = ColumnOffset(5) };
    auto constexpr TTopLeft = CellLocation { .line = LineOffset(1), .column = ColumnOffset(2) };

    auto const deccraSeq = std::format("\033[{};{};{};{};{};{};{};{}$v",
                                       STopLeft.line + 1,
                                       STopLeft.column + 1,
                                       SBottomRight.line + 1,
                                       SBottomRight.column + 1,
                                       Page,
                                       TTopLeft.line + 1,
                                       TTopLeft.column + 1,
                                       Page);
    mock.writeToScreen(deccraSeq);

    auto const resultText = screen.renderMainPageText();
    CHECK(resultText == expectedText);
}

TEST_CASE("DECRQCRA answers regardless of the operating level", "[screen]")
{
    // DECRQCRA merely reports a checksum, so xterm answers it at any operating level; conformance tools
    // rely on that to read the screen back even after DECSCL drops to VT100. @see SupportedSequences::reset.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    mock.writeToScreen("\033[61\"p"); // DECSCL(61): VT100 operating level
    mock.resetReplyData();
    mock.writeToScreen("\033[1;1;1;1;1;1*y"); // DECRQCRA over the top-left cell
    mock.terminal.flushInput();
    CHECK_FALSE(mock.replyData().empty()); // answered, not gated into silence
}

TEST_CASE("DECALN: fills the page with E's", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(4) } };
    mock.writeToScreen("\033#8");

    CHECK(mock.terminal.primaryScreen().renderMainPageText() == "EEEE\nEEEE\nEEEE\n");
}

TEST_CASE("DECALN: page that wraps the history ring is still filled in bounds", "[screen]")
{
    // Grid::_lines is a ring whose rotation is an INDEX move, not a data move. Once enough lines
    // have scrolled into history, the logical main page straddles the ring's physical end. Anything
    // that walks the page as a contiguous block therefore runs off the underlying vector -- which is
    // what DECALN used to do, and what ASan catches here.
    //
    // Found by driving vttest through the conformance harness; there was no DECALN test at all.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(4) }, LineCount(2) };

    // Scroll past the history capacity so the ring's zero-index no longer sits at physical zero.
    for ([[maybe_unused]] auto const _: std::views::iota(0, 6))
        mock.writeToScreen("x\r\n");

    REQUIRE(mock.terminal.primaryScreen().historyLineCount() == LineCount(2));

    mock.writeToScreen("\033#8");

    CHECK(mock.terminal.primaryScreen().renderMainPageText() == "EEEE\nEEEE\nEEEE\n");
}

TEST_CASE("DECRQCRA: reports the checksum of a rectangular area", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) } };

    auto const request = [&](std::string_view sequence) -> std::string {
        mock.mockPty().stdinBuffer().clear();
        mock.writeToScreen(sequence);
        mock.terminal.flushInput();
        return mock.replyData();
    };

    mock.writeToScreen("ab");

    SECTION("the final byte is `* y`, not `$ y`")
    {
        // Regression. DECRQCRA was registered with a '$' intermediate -- which is DECRPM's, a reply
        // form no terminal ever parses -- so the implementation was unreachable: every application
        // asking for a checksum, esctest included, waited for an answer that could not come.
        CHECK(request("\033[1;1;1;1;1;1*y") == "\033P1!~FF9F\033\\");

        // And the old spelling is not DECRQCRA, so it must draw no reply at all.
        CHECK(request("\033[1;1;1;1;1;1$y").empty());
    }

    SECTION("the request id is echoed back, so answers can be correlated")
    {
        CHECK(request("\033[42;1;1;1;1;1*y") == "\033P42!~FF9F\033\\");
    }

    SECTION("a rectangle spanning several cells sums them")
    {
        CHECK(request("\033[1;1;1;1;1;2*y") == "\033P1!~FF3D\033\\"); // -( 'a' + 'b' )
    }

    SECTION("an omitted rectangle covers the whole page")
    {
        // The two written cells count; the rest of the page was never written to and drops out.
        CHECK(request("\033[1*y") == "\033P1!~FF3D\033\\");
    }

    SECTION("cells never written to contribute nothing")
    {
        CHECK(request("\033[1;1;3;1;3;5*y") == "\033P1!~0000\033\\");
    }

    SECTION("a written space is not an empty cell")
    {
        mock.writeToScreen("\033[2;1H "); // an explicit space on row 2
        CHECK(request("\033[1;1;2;1;2;1*y") == "\033P1!~FFE0\033\\");
    }

    SECTION("video attributes are folded into the value")
    {
        mock.writeToScreen("\033[2;1H\033[1ma");                      // bold 'a'
        CHECK(request("\033[1;1;2;1;2;1*y") == "\033P1!~FF1F\033\\"); // -( 'a' + 0x80 )
    }
}

TEST_CASE("XTCHECKSUM: selects how DECRQCRA computes its checksum", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) } };

    auto const request = [&](std::string_view sequence) -> std::string {
        mock.mockPty().stdinBuffer().clear();
        mock.writeToScreen(sequence);
        mock.terminal.flushInput();
        return mock.replyData();
    };
    auto const checksumOfFirstCell = [&] {
        return request("\033[1;1;1;1;1;1*y");
    };

    mock.writeToScreen("\033[1ma"); // bold 'a' at 1,1

    SECTION("the default is DEC-compatible: negated, with attributes folded in")
    {
        REQUIRE(mock.terminal.checksumExtension() == vtbackend::ChecksumFlags {});
        CHECK(checksumOfFirstCell() == "\033P1!~FF1F\033\\");
    }

    SECTION("bit 0 reports the plain sum")
    {
        mock.writeToScreen("\033[1#y");
        CHECK(checksumOfFirstCell() == "\033P1!~00E1\033\\");
    }

    SECTION("bit 1 leaves the video attributes out")
    {
        mock.writeToScreen("\033[2#y");
        CHECK(checksumOfFirstCell() == "\033P1!~FF9F\033\\");
    }

    SECTION("bit 3 counts cells that were never written to")
    {
        mock.writeToScreen("\033[8#y");
        CHECK(request("\033[1;1;3;1;3;1*y") == "\033P1!~FFE0\033\\"); // an untouched cell reads blank
    }

    SECTION("bits combine")
    {
        // The combination the conformance suites need: undrawn cells read as blanks, and a cell's
        // attributes stay out of its value.
        mock.writeToScreen("\033[10#y");
        CHECK(checksumOfFirstCell() == "\033P1!~FF9F\033\\");
        CHECK(request("\033[1;1;3;1;3;1*y") == "\033P1!~FFE0\033\\");
    }

    SECTION("an omitted parameter selects the DEC-compatible default")
    {
        mock.writeToScreen("\033[1#y");
        REQUIRE(mock.terminal.checksumExtension() != vtbackend::ChecksumFlags {});
        mock.writeToScreen("\033[#y");
        CHECK(mock.terminal.checksumExtension() == vtbackend::ChecksumFlags {});
    }
}

TEST_CASE("XTCHECKSUM: a reset restores the configured extension, not zero", "[screen]")
{
    // xterm restores its `checksumExtension` resource on reset rather than clearing it, and Contour
    // mirrors that via Settings. It matters: esctest sends DECSTR before every single test, so a
    // reset-to-zero would throw the harness's configuration away on the first one.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) } };
    auto const configured = vtbackend::ChecksumFlags { vtbackend::ChecksumFlag::NoAttributes }
                            | vtbackend::ChecksumFlag::IncludeUndrawn;
    mock.terminal.settings().checksumExtension = configured;

    mock.writeToScreen("\033[1#y"); // the application selects something else
    REQUIRE(mock.terminal.checksumExtension()
            == vtbackend::ChecksumFlags { vtbackend::ChecksumFlag::Positive });

    SECTION("DECSTR (soft reset) restores it")
    {
        mock.writeToScreen("\033[!p");
        CHECK(mock.terminal.checksumExtension() == configured);
    }

    SECTION("RIS (hard reset) restores it")
    {
        mock.writeToScreen("\033c");
        CHECK(mock.terminal.checksumExtension() == configured);
    }
}

TEST_CASE("DECCRA with a defaulted source corner", "[screen]")
{
    // The sequence esctest sends: it names no source top-left corner at all. Contour read the omitted
    // parameters as the value zero, computed `0 - 1`, and copied from column -1 -- aborting the engine
    // on a precondition, and reading out of bounds in a release build where that precondition is gone.
    auto mock = MockTerm { PageSize { LineCount(8), ColumnCount(8) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033[1;1H");
    mock.writeToScreen("abcdefgh\r\nijklmnop\r\nqrstuvwx\r\nyz012345\r\n"
                       "ABCDEFGH\r\nIJKLMNOP\r\nQRSTUVWX\r\nYZ6789!@");

    // Copy the 2x2 area at the page's top-left corner -- named only by its bottom-right corner -- to
    // row 5, column 5.
    mock.writeToScreen("\033[;;2;2;;5;5;1$v");

    CHECK(screen.grid().lineText(LineOffset(4)) == "ABCDabGH");
    CHECK(screen.grid().lineText(LineOffset(5)) == "IJKLijOP");

    // Everything else is untouched.
    CHECK(screen.grid().lineText(LineOffset(0)) == "abcdefgh");
    CHECK(screen.grid().lineText(LineOffset(6)) == "QRSTUVWX");
}

TEST_CASE("DECCRA truncates a copy at the page's edge", "[screen]")
{
    // An area that would not fit copies only the part that does. Copying every cell the source named
    // ran the write past the end of a line -- the engine asserted, and a release build would have
    // corrupted memory.
    auto mock = MockTerm { PageSize { LineCount(8), ColumnCount(8) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033[1;1H");
    mock.writeToScreen("abcdefgh\r\nijklmnop\r\nqrstuvwx\r\nyz012345\r\n"
                       "ABCDEFGH\r\nIJKLMNOP\r\nQRSTUVWX\r\nYZ6789!@");

    // Copy the 3x3 area at 2,2 to 7,7 -- where only its top-left 2x2 corner still fits on the page.
    mock.writeToScreen("\033[2;2;4;4;1;7;7;1$v");

    CHECK(screen.grid().lineText(LineOffset(6)) == "QRSTUVjk");
    CHECK(screen.grid().lineText(LineOffset(7)) == "YZ6789rs");
}

// NOLINTEND(misc-const-correctness,readability-function-cognitive-complexity)
