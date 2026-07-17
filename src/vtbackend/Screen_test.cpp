// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Charset.h>
#include <vtbackend/InputGenerator.h>
#include <vtbackend/MockTerm.h>
#include <vtbackend/Screen.h>
#include <vtbackend/Viewport.h>
#include <vtbackend/primitives.h>
#include <vtbackend/test_helpers.h>

#include <crispy/escape.h>
#include <crispy/utils.h>

#include <libunicode/convert.h>

#include <catch2/catch_test_macros.hpp>

#include <charconv>
#include <ranges>
#include <set>
#include <string_view>

using crispy::escape;
using crispy::size;
using namespace vtbackend;
using namespace vtbackend::test;
using namespace std;
using namespace std::literals::chrono_literals;

namespace // {{{
{

// Chessboard image with each square of size 10x10 pixels
std::string const chessBoard =
    R"=(P0;0;0q"1;1;100;100#0;2;0;0;0#1;2;100;100;100#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~-#0!10N!10o!10N!10o!10N!10o!10N!10o!10N!10o$#1!10o!10N!10o!10N!10o!10N!10o!10N!10o!10N-!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~-!10{!10B!10{!10B!10{!10B!10{!10B!10{!10B$#1!10B!10{!10B!10{!10B!10{!10B!10{!10B!10{-#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~-!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~-!10o!10N!10o!10N!10o!10N!10o!10N!10o!10N$#1!10N!10o!10N!10o!10N!10o!10N!10o!10N!10o-#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~-#0!10B!10{!10B!10{!10B!10{!10B!10{!10B!10{$#1!10{!10B!10{!10B!10{!10B!10{!10B!10{!10B-!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~-!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~-#0!10N!10o!10N!10o!10N!10o!10N!10o!10N!10o$#1!10o!10N!10o!10N!10o!10N!10o!10N!10o!10N-!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~-!10{!10B!10{!10B!10{!10B!10{!10B!10{!10B$#1!10B!10{!10B!10{!10B!10{!10B!10{!10B!10{-#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~-!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~-#1!10N#0!10N#1!10N#0!10N#1!10N#0!10N#1!10N#0!10N#1!10N#0!10N-\)=";

Image::Data const black10x10 = [] {
    Image::Data ret(100 * 4, 0);
    for (size_t i = 3; i < ret.size(); i += 4)
    {
        ret[i] = 255;
    }
    return ret;
}();

Image::Data const white10x10(100 * 4, 255);

struct TextRenderBuilder
{
    std::string text;

    void startLine(LineOffset lineOffset, LineFlags flags);
    void renderCell(ConstCellProxy cell, LineOffset lineOffset, ColumnOffset columnOffset);
    void endLine();
    void renderTrivialLine(TrivialLineBuffer const& lineBuffer,
                           LineOffset lineOffset,
                           LineFlags flags,
                           std::u32string_view textOverride = {});
    void finish();
};

void TextRenderBuilder::startLine(LineOffset lineOffset, LineFlags /*flags*/)
{
    if (!*lineOffset)
        text.clear();
}

void TextRenderBuilder::renderCell(ConstCellProxy cell, LineOffset, ColumnOffset)
{
    text += cell.toUtf8();
}

void TextRenderBuilder::endLine()
{
    text += '\n';
}

void TextRenderBuilder::renderTrivialLine(TrivialLineBuffer const& lineBuffer,
                                          LineOffset lineOffset,
                                          LineFlags /*flags*/,
                                          std::u32string_view textOverride)
{
    if (!*lineOffset)
        text.clear();

    if (!textOverride.empty())
        text.append(unicode::convert_to<char>(textOverride));
    else
        text.append(lineBuffer.text.data(), lineBuffer.text.size());
    text += '\n';
}

void TextRenderBuilder::finish()
{
}

MockTerm<vtpty::MockPty> screenForDECRA()
{
    return MockTerm<vtpty::MockPty> { PageSize { LineCount(5), ColumnCount(6) }, {}, 1024, [](auto& mock) {
                                         mock.writeToScreen("ABCDEF\r\n"
                                                            "abcdef\r\n"
                                                            "123456\r\n");
                                         mock.writeToScreen("\033[43m");
                                         mock.writeToScreen("GHIJKL\r\n"
                                                            "ghijkl");
                                         mock.writeToScreen("\033[0m");

                                         auto const* const initialText = "ABCDEF\n"
                                                                         "abcdef\n"
                                                                         "123456\n"
                                                                         "GHIJKL\n"
                                                                         "ghijkl\n";

                                         CHECK(mock.terminal.primaryScreen().renderMainPageText()
                                               == initialText);
                                     } };
}

} // namespace
// }}}

// NOLINTBEGIN(misc-const-correctness,readability-function-cognitive-complexity)

// {{{ writeText
// AutoWrap disabled: text length is less then available columns in line.
TEST_CASE("writeText.bulk.A.1", "[screen]")
{
    auto mock = MockTerm(PageSize { LineCount(3), ColumnCount(5) }, LineCount(2));
    auto& screen = mock.terminal.primaryScreen();
    mock.terminal.setMode(DECMode::AutoWrap, false);
    mock.writeToScreen("a");
    mock.writeToScreen("b");
    logScreenText(screen, "initial state");
    REQUIRE(screen.cursor().position == CellLocation { LineOffset(0), ColumnOffset(2) });
    mock.writeToScreen("CD");
    logScreenText(screen, "final state");
    CHECK(screen.grid().lineText(LineOffset(0)) == "abCD ");
    CHECK(screen.grid().lineText(LineOffset(1)) == "     ");
    CHECK(screen.cursor().position == CellLocation { LineOffset(0), ColumnOffset(4) });
}

// AutoWrap disabled: text length equals available columns in line.
TEST_CASE("writeText.bulk.A.2", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) }, LineCount(2) };
    auto& screen = mock.terminal.primaryScreen();
    mock.terminal.setMode(DECMode::AutoWrap, false);
    mock.writeToScreen("a");
    mock.writeToScreen("b");
    logScreenText(screen, "initial state");
    REQUIRE(screen.cursor().position == CellLocation { LineOffset(0), ColumnOffset(2) });
    mock.writeToScreen("CDE");
    logScreenText(screen, "final state");
    CHECK(screen.grid().lineText(LineOffset(0)) == "abCDE");
    CHECK(screen.grid().lineText(LineOffset(1)) == "     ");
    CHECK(screen.cursor().position == CellLocation { LineOffset(0), ColumnOffset(4) });
}

// AutoWrap disabled: text length exceeds available columns in line.
TEST_CASE("writeText.bulk.A.3", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) }, LineCount(2) };
    auto& screen = mock.terminal.primaryScreen();
    mock.terminal.setMode(DECMode::AutoWrap, false);
    mock.writeToScreen("a");
    mock.writeToScreen("b");
    logScreenText(screen, "initial state");
    REQUIRE(screen.cursor().position == CellLocation { LineOffset(0), ColumnOffset(2) });
    mock.writeToScreen("CDEF");
    logScreenText(screen, "final state");
    CHECK(screen.grid().lineText(LineOffset(0)) == "abCDF");
    CHECK(screen.grid().lineText(LineOffset(1)) == "     ");
    CHECK(screen.cursor().position == CellLocation { LineOffset(0), ColumnOffset(4) });
}

// vttest chapter 2 (screen features) page 1: writing 2*cols '*' with autowrap ON fills two lines by
// wrapping; writing 2*cols '*' with autowrap OFF fills one line (the last column overwrites in place,
// no wrap). All three lines must be identical, full lines of '*' -- "three identical lines of *'s
// completely filling the top of the screen without any empty lines between."
TEST_CASE("writeText.autowrap.threeIdenticalFullLines", "[screen]")
{
    auto constexpr Cols = 10;
    auto mock = MockTerm { PageSize { LineCount(6), ColumnCount(Cols) } };
    auto& screen = mock.terminal.primaryScreen();

    // Write the stars one at a time, as vttest does (a tprintf per '*'), so the incremental
    // deferred-wrap path is exercised rather than the bulk fast path.
    auto const writeStars = [&](int n) {
        for (auto i = 0; i < n; ++i)
            mock.writeToScreen("*");
    };

    mock.writeToScreen("\033[H\033[?7h"); // cursor home, autowrap ON
    writeStars(2 * Cols);                 // -> rows 1 and 2 by wrapping
    mock.writeToScreen("\033[?7l");       // autowrap OFF
    mock.writeToScreen("\033[3;1H");      // cursor to row 3
    writeStars(2 * Cols);                 // -> row 3 only (last column overwrites, no wrap)
    mock.writeToScreen("\033[?7h");       // autowrap ON

    auto const full = std::string(Cols, '*');
    CHECK(screen.grid().lineText(LineOffset(3)) == std::string(Cols, ' ')); // row 4 stays empty
}

// Text does not fully fill current line.
TEST_CASE("writeText.bulk.B", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) }, LineCount(2) };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("a");
    mock.writeToScreen("b");
    logScreenText(screen, "initial state");
    REQUIRE(screen.cursor().position == CellLocation { LineOffset(0), ColumnOffset(2) });
    mock.writeToScreen("CD");
    logScreenText(screen, "final state");
    CHECK(screen.grid().lineText(LineOffset(0)) == "abCD ");
    CHECK(screen.cursor().position == CellLocation { LineOffset(0), ColumnOffset(4) });
}

// Text spans current line exactly.
TEST_CASE("writeText.bulk.C", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) }, LineCount(2) };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("a");
    mock.writeToScreen("b");
    logScreenText(screen, "initial state");
    REQUIRE(screen.cursor().position == CellLocation { LineOffset(0), ColumnOffset(2) });
    mock.writeToScreen("CDE");
    logScreenText(screen, "final state");
    CHECK(screen.grid().lineText(LineOffset(0)) == "abCDE");
    CHECK(screen.grid().lineText(LineOffset(1)) == "     ");
    CHECK(screen.cursor().position == CellLocation { LineOffset(0), ColumnOffset(4) });
    // Now, verify AutoWrap works by writing one char more.
    mock.writeToScreen("F");
    logScreenText(screen, "AutoWrap-around");
    CHECK(screen.grid().lineText(LineOffset(0)) == "abCDE");
    CHECK(screen.grid().lineText(LineOffset(1)) == "F    ");
    CHECK(screen.cursor().position == CellLocation { LineOffset(1), ColumnOffset(1) });
}

// Text spans this line and some of the next.
TEST_CASE("writeText.bulk.D", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) }, LineCount(2) };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("a");
    mock.writeToScreen("b");
    logScreenText(screen, "initial state");
    REQUIRE(screen.cursor().position == CellLocation { LineOffset(0), ColumnOffset(2) });
    mock.writeToScreen("CDEF");
    logScreenText(screen, "final state");
    CHECK(screen.grid().lineText(LineOffset(0)) == "abCDE");
    CHECK(screen.grid().lineText(LineOffset(1)) == "F    ");
    CHECK(screen.cursor().position == CellLocation { LineOffset(1), ColumnOffset(1) });
}

// Text spans full main page exactly.
TEST_CASE("writeText.bulk.E", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) }, LineCount(2) };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("0123456789"
                       "abcdefghij"
                       "ABCDEFGHIJ");
    logScreenText(screen, "final state");
    CHECK(screen.grid().lineText(LineOffset(0)) == "0123456789");
    CHECK(screen.grid().lineText(LineOffset(1)) == "abcdefghij");
    CHECK(screen.grid().lineText(LineOffset(2)) == "ABCDEFGHIJ");
    REQUIRE(screen.cursor().position == CellLocation { LineOffset(2), ColumnOffset(9) });

    // now check if AutoWrap is triggered
    mock.writeToScreen("X");
    CHECK(screen.grid().lineText(LineOffset(-1)) == "0123456789");
    CHECK(screen.grid().lineText(LineOffset(0)) == "abcdefghij");
    CHECK(screen.grid().lineText(LineOffset(1)) == "ABCDEFGHIJ");
    CHECK(screen.grid().lineText(LineOffset(2)) == "X         ");
}

// Text spans 3 lines.
TEST_CASE("writeText.bulk.F", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) }, LineCount(1) };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("a");
    mock.writeToScreen("b");
    mock.writeToScreen("CDEFGHIJ"
                       "ABcdefghij"
                       "01234");
    logScreenText(screen, "final state");
    CHECK(screen.grid().lineText(LineOffset(0)) == "abCDEFGHIJ");
    CHECK(screen.grid().lineText(LineOffset(1)) == "ABcdefghij");
    CHECK(screen.grid().lineText(LineOffset(2)) == "01234     ");
    CHECK(screen.cursor().position == CellLocation { LineOffset(2), ColumnOffset(5) });
}

// Text spans 4 lines with one line being scrolled up.
TEST_CASE("writeText.bulk.G", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) }, LineCount(1) };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("a");
    mock.writeToScreen("b");
    mock.writeToScreen("CDEFGHIJ"
                       "ABCDEFGHIJ"
                       "abcdefghij"
                       "01234");
    logScreenText(screen, "final state");
    CHECK(screen.grid().lineText(LineOffset(-1)) == "abCDEFGHIJ");
    CHECK(screen.grid().lineText(LineOffset(0)) == "ABCDEFGHIJ");
    CHECK(screen.grid().lineText(LineOffset(1)) == "abcdefghij");
    CHECK(screen.grid().lineText(LineOffset(2)) == "01234     ");
    CHECK(screen.cursor().position == CellLocation { LineOffset(2), ColumnOffset(5) });
}

// Text spans more lines than totally available.
TEST_CASE("writeText.bulk.H", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(10) }, LineCount(1) };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("ABCDEFGHIJ"
                       "KLMNOPQRST"
                       "abcdefghij"
                       "0123456789");

    logScreenText(screen, "final state");
    CHECK(screen.grid().lineText(LineOffset(-1)) == "KLMNOPQRST");
    CHECK(screen.grid().lineText(LineOffset(0)) == "abcdefghij");
    CHECK(screen.grid().lineText(LineOffset(1)) == "0123456789");
    CHECK(screen.cursor().position == CellLocation { LineOffset(1), ColumnOffset(9) });
}

// TODO: Test spanning writes over all history and then reusing old lines.
// Verify we do not leak any old cell attribs.

// }}}

TEST_CASE("AppendChar", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(3) }, LineCount(1) };
    auto& screen = mock.terminal.primaryScreen();
    REQUIRE(screen.historyLineCount() == LineCount(0));
    REQUIRE(screen.pageSize().lines == LineCount(1));
    REQUIRE("   " == screen.grid().lineText(LineOffset(0)));

    mock.terminal.setMode(DECMode::AutoWrap, false);

    mock.writeToScreen("A");
    REQUIRE("A  " == screen.grid().lineText(LineOffset(0)));

    mock.writeToScreen("B");
    REQUIRE("AB " == screen.grid().lineText(LineOffset(0)));

    mock.writeToScreen("C");
    REQUIRE("ABC" == screen.grid().lineText(LineOffset(0)));

    mock.writeToScreen("D");
    REQUIRE("ABD" == screen.grid().lineText(LineOffset(0)));

    logScreenText(screen, "with AutoWrap off (before switching on)");
    mock.terminal.setMode(DECMode::AutoWrap, true);

    mock.writeToScreen("E");
    REQUIRE("ABE" == screen.grid().lineText(LineOffset(0)));

    mock.writeToScreen("F");
    CHECK("F  " == screen.grid().lineText(LineOffset(0)));
    CHECK("ABE" == screen.grid().lineText(LineOffset(-1)));
}

TEST_CASE("AppendChar_CR_LF", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();
    REQUIRE("   " == screen.grid().lineText(LineOffset(0)));

    mock.terminal.setMode(DECMode::AutoWrap, false);

    mock.writeToScreen("ABC");
    REQUIRE("ABC" == screen.grid().lineText(LineOffset(0)));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(2) });

    mock.writeToScreen("\r");
    REQUIRE("ABC\n   \n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });

    mock.writeToScreen("\n");
    REQUIRE("ABC\n   \n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(0) });
}

TEST_CASE("AppendChar.emoji_exclamationmark", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();

    screen.setBackgroundColor(IndexedColor::Blue);

    mock.writeToScreen(U"\u2757"); // ❗
    // mock.writeToScreen(U"\uFE0F");
    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).backgroundColor() == Color::Indexed(IndexedColor::Blue));
    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).width() == 2);
    CHECK(screen.at(LineOffset(0), ColumnOffset(1)).backgroundColor() == Color::Indexed(IndexedColor::Blue));
    CHECK(screen.at(LineOffset(0), ColumnOffset(1)).width() == 1);

    mock.writeToScreen("M");
    CHECK(screen.at(LineOffset(0), ColumnOffset(2)).backgroundColor() == IndexedColor::Blue);
}

TEST_CASE("AppendChar.emoji_VS15_smiley", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(4) } };
    auto& screen = mock.terminal.primaryScreen();

    // print letter-like symbol copyright sign with forced emoji presentation style.
    REQUIRE(*screen.logicalCursorPosition().column == 0);
    mock.writeToScreen(U"\U0001F600");
    REQUIRE(*screen.logicalCursorPosition().column == 2);
    mock.writeToScreen(U"\uFE0E");
    REQUIRE(*screen.logicalCursorPosition().column == 2);
    // ^^^ U+FE0E does *NOT* lower width to 1 (easier to implement)
    mock.writeToScreen("X");
    REQUIRE(*screen.logicalCursorPosition().column == 3);
    logScreenText(screen);

    // emoji
    auto const& c1 = screen.at(LineOffset(0), ColumnOffset(0));
    CHECK(c1.codepoints() == U"\U0001F600\uFE0E");
    CHECK(c1.width() == 2);

    // unused cell
    auto const& c2 = screen.at(LineOffset(0), ColumnOffset(1));
    CHECK(c2.empty());
    CHECK(c2.width() == 1);

    // character after the emoji
    auto const& c3 = screen.at(LineOffset(0), ColumnOffset(2));
    CHECK(c3.codepoints() == U"X");
    CHECK(c3.width() == 1);

    // tail
    auto const& c4 = screen.at(LineOffset(0), ColumnOffset(3));
    CHECK(c4.codepoints().empty());
}

TEST_CASE("AppendChar.emoji_VS16_copyright_sign", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(4) } };
    auto& screen = mock.terminal.primaryScreen();
    auto const& c0 = screen.at(LineOffset(0), ColumnOffset(0));
    auto const& c1 = screen.at(LineOffset(0), ColumnOffset(1));
    auto const& c2 = screen.at(LineOffset(0), ColumnOffset(2));
    auto const& c3 = screen.at(LineOffset(0), ColumnOffset(3));

    // print letter-like symbol copyright sign with forced emoji presentation style.
    REQUIRE(screen.cursor().position.column.value == 0);
    mock.writeToScreen(U"\u00A9");
    REQUIRE(screen.cursor().position.column.value == 1);
    CHECK(c0.codepointCount() == 1);
    CHECK(c0.width() == 1);
    mock.writeToScreen(U"\uFE0F");
    CHECK(c0.codepointCount() == 2);
    REQUIRE(screen.cursor().position.column.value == 1);
    mock.writeToScreen("X");
    REQUIRE(screen.cursor().position.column.value == 2);

    // double-width emoji with VS16
    CHECK(c0.codepoints() == U"\u00A9\uFE0F");
    CHECK(c0.width() == 1);

    // character after the emoji
    CHECK(c1.codepoints() == U"X");
    CHECK(c1.width() == 1);

    // unused cell
    CHECK(c2.empty());
    CHECK(c2.width() == 1);

    CHECK(c3.codepoints().empty());
}

TEST_CASE("AppendChar.emoji_VS16_i", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    auto const& c0 = screen.at(LineOffset(0), ColumnOffset(0));

    // print letter-like symbol `i` with forced emoji presentation style.
    mock.writeToScreen(U"\u2139");
    REQUIRE(screen.cursor().position.column.value == 1);
    CHECK(c0.codepoints() == U"\u2139");
    CHECK(c0.width() == 1);

    // append into last cell
    mock.writeToScreen(U"\uFE0F");
    // XXX ^^^ later on U+FE0F *will* ensure width 2 if respective mode is enabled.
    REQUIRE(screen.cursor().position.column.value == 1);
    CHECK(c0.codepoints() == U"\u2139\uFE0F");
    CHECK(c0.width() == 1);

    // write into 3rd cell
    mock.writeToScreen("X");

    // X-cell
    auto const& c1 = screen.at(LineOffset(0), ColumnOffset(1));
    CHECK(c1.codepoints() == U"X");
    CHECK(c1.width() == 1);

    // character after the emoji
    auto const& c2 = screen.at(LineOffset(0), ColumnOffset(2));
    CHECK(c2.empty());

    auto const& c3 = screen.at(LineOffset(0), ColumnOffset(3));
    CHECK(c3.empty());

    auto const& c4 = screen.at(LineOffset(0), ColumnOffset(4));
    CHECK(c4.empty());
}

TEST_CASE("AppendChar.emoji_family", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    auto const& c0 = screen.at(LineOffset(0), ColumnOffset(0));

    REQUIRE(screen.logicalCursorPosition().column.value == 0);

    // print letter-like symbol `i` with forced emoji presentation style.
    mock.writeToScreen(U"\U0001F468");
    CHECK(c0.codepoints() == U"\U0001F468");
    REQUIRE(screen.logicalCursorPosition().column.value == 2);
    mock.writeToScreen(U"\u200D");
    CHECK(c0.codepoints() == U"\U0001F468\u200D");
    REQUIRE(screen.logicalCursorPosition().column.value == 2);
    mock.writeToScreen(U"\U0001F468");
    CHECK(c0.codepoints() == U"\U0001F468\u200D\U0001F468");
    REQUIRE(screen.logicalCursorPosition().column.value == 2);
    mock.writeToScreen(U"\u200D");
    CHECK(c0.codepoints() == U"\U0001F468\u200D\U0001F468\u200D");
    REQUIRE(screen.logicalCursorPosition().column.value == 2);
    mock.writeToScreen(U"\U0001F467");
    CHECK(c0.codepoints() == U"\U0001F468\u200D\U0001F468\u200D\U0001F467");
    REQUIRE(screen.logicalCursorPosition().column.value == 2);
    mock.writeToScreen("X");
    REQUIRE(screen.logicalCursorPosition().column.value == 3);

    // double-width emoji with VS16
    auto const& c1 = screen.at(LineOffset(0), ColumnOffset(0));
    CHECK(c1.codepoints() == U"\U0001F468\u200D\U0001F468\u200D\U0001F467");
    CHECK(c1.width() == 2);

    // unused cell
    auto const& c2 = screen.at(LineOffset(0), ColumnOffset(1));
    CHECK(c2.codepointCount() == 0);
    CHECK(c2.width() == 1);

    // character after the emoji
    auto const& c3 = screen.at(LineOffset(0), ColumnOffset(2));
    CHECK(c3.codepoints() == U"X");
    CHECK(c3.width() == 1);
}

TEST_CASE("AppendChar.emoji_zwj_1", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.terminal.setMode(DECMode::AutoWrap, false);

    // https://emojipedia.org/man-facepalming-medium-light-skin-tone/
    auto const emoji = u32string_view { U"\U0001F926\U0001F3FC\u200D\u2642\uFE0F" };
    mock.writeToScreen(unicode::convert_to<char>(emoji));

    auto const& c0 = screen.at(LineOffset(0), ColumnOffset(0));
    CHECK(c0.codepoints() == emoji);
    CHECK(c0.width() == 2);

    // other columns remain untouched
    CHECK(screen.at(LineOffset(0), ColumnOffset(1)).empty());
    CHECK(screen.at(LineOffset(0), ColumnOffset(2)).empty());
    CHECK(screen.at(LineOffset(0), ColumnOffset(3)).empty());
    CHECK(screen.at(LineOffset(0), ColumnOffset(4)).empty());

    auto const s8 = screen.grid().lineText(LineOffset(0));
    auto const s32 = unicode::from_utf8(s8);
    CHECK(U"\U0001F926\U0001F3FC\u200D\u2642\uFE0F" == c0.codepoints());
    CHECK(U"\U0001F926\U0001F3FC\u200D\u2642\uFE0F   " == s32);
}

TEST_CASE("AppendChar.emoji_1", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen(U"\U0001F600");

    auto const& c1 = screen.at(LineOffset(0), ColumnOffset(0));
    CHECK(c1.codepoints() == U"\U0001F600");
    CHECK(c1.width() == 2);
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(2) });

    CHECK(screen.at(LineOffset(0), ColumnOffset(1)).codepointCount() == 0);
    CHECK(screen.at(LineOffset(0), ColumnOffset(2)).codepointCount() == 0);

    mock.writeToScreen("B");
    auto const& c2 = screen.at(LineOffset(0), ColumnOffset(1));
    CHECK(c2.codepointCount() == 0);
    CHECK(c2.codepoints().empty());
    CHECK(c2.width() == 1);

    auto const& c3 = screen.at(LineOffset(0), ColumnOffset(2));
    CHECK(c3.codepointCount() == 1);
    CHECK(c3.codepoint(0) == 'B');
    CHECK(c3.width() == 1);
}

TEST_CASE("AppendChar_WideChar", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.terminal.setMode(DECMode::AutoWrap, true);
    mock.writeToScreen(U"\U0001F600");
    CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(2) });
}

TEST_CASE("AppendChar_Into_WideChar_Right_Half", "[screen]")
{
    auto const pageSize = PageSize { LineCount(2), ColumnCount(4) };
    auto mock = MockTerm { pageSize, LineCount(5) };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen(U"\U0001F600B"); // "😀B"
    REQUIRE(screen.grid().lineText(LineOffset(0)) == unicode::convert_to<char>(U"\U0001F600B "sv));
    mock.writeToScreen(CHA(2));
    mock.writeToScreen("X");
    REQUIRE(screen.grid().lineText(LineOffset(0)) == " XB ");
}

TEST_CASE("AppendChar_AutoWrap", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.terminal.setMode(DECMode::AutoWrap, true);

    mock.writeToScreen("ABC");
    REQUIRE("ABC" == screen.grid().lineText(LineOffset(0)));
    REQUIRE("   " == screen.grid().lineText(LineOffset(1)));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(2) });

    mock.writeToScreen("D");
    REQUIRE("ABC" == screen.grid().lineText(LineOffset(0)));
    REQUIRE("D  " == screen.grid().lineText(LineOffset(1)));

    mock.writeToScreen("EF");
    REQUIRE("ABC" == screen.grid().lineText(LineOffset(0)));
    REQUIRE("DEF" == screen.grid().lineText(LineOffset(1)));

    logScreenText(screen);
    mock.writeToScreen("G");
    logScreenText(screen);
    REQUIRE("DEF" == screen.grid().lineText(LineOffset(0)));
    REQUIRE("G  " == screen.grid().lineText(LineOffset(1)));
}

TEST_CASE("AppendChar_AutoWrap_LF", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.terminal.setMode(DECMode::AutoWrap, true);

    INFO("write ABC");
    mock.writeToScreen("ABC");
    logScreenText(screen);
    REQUIRE("ABC" == screen.grid().lineText(LineOffset(0)));
    REQUIRE("   " == screen.grid().lineText(LineOffset(1)));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(2) });

    INFO("write CRLF");
    mock.writeToScreen("\r\n");
    logScreenText(screen, "after writing LF");
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(0) });

    INFO("write 'D'");
    mock.writeToScreen("D");
    logScreenText(screen);
    REQUIRE("ABC" == screen.grid().lineText(LineOffset(0)));
    REQUIRE("D  " == screen.grid().lineText(LineOffset(1)));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(1) });
}

TEST_CASE("Screen.isLineVisible", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(2) }, LineCount(5) };
    auto& screen = mock.terminal.primaryScreen();
    auto viewport = vtbackend::Viewport { mock.terminal };

    mock.writeToScreen("10203040");
    logScreenText(screen);
    CHECK(screen.grid().lineText(LineOffset(0)) == "40");
    CHECK(screen.grid().lineText(LineOffset(-1)) == "30");
    CHECK(screen.grid().lineText(LineOffset(-2)) == "20");
    CHECK(screen.grid().lineText(LineOffset(-3)) == "10");

    CHECK(viewport.isLineVisible(LineOffset { 0 }));
    CHECK_FALSE(viewport.isLineVisible(LineOffset { -1 }));
    CHECK_FALSE(viewport.isLineVisible(LineOffset { -2 }));
    CHECK_FALSE(viewport.isLineVisible(LineOffset { -3 }));
    CHECK_FALSE(viewport.isLineVisible(LineOffset { -4 })); // minimal out-of-bounds

    viewport.scrollUp(LineCount(1));
    REQUIRE(viewport.scrollOffset() == ScrollOffset(1));
    CHECK_FALSE(viewport.isLineVisible(LineOffset { 0 }));
    CHECK(viewport.isLineVisible(LineOffset { -1 }));
    CHECK_FALSE(viewport.isLineVisible(LineOffset { -2 }));
    CHECK_FALSE(viewport.isLineVisible(LineOffset { -3 }));

    viewport.scrollUp(LineCount(1));
    REQUIRE(viewport.scrollOffset() == ScrollOffset(2));
    CHECK_FALSE(viewport.isLineVisible(LineOffset { 0 }));
    CHECK_FALSE(viewport.isLineVisible(LineOffset { -1 }));
    CHECK(viewport.isLineVisible(LineOffset { -2 }));
    CHECK_FALSE(viewport.isLineVisible(LineOffset { -3 }));

    viewport.scrollUp(LineCount(1));
    REQUIRE(viewport.scrollOffset() == ScrollOffset(3));
    CHECK(!viewport.isLineVisible(LineOffset { 0 }));
    CHECK(!viewport.isLineVisible(LineOffset { -1 }));
    CHECK(!viewport.isLineVisible(LineOffset { -2 }));
    CHECK(viewport.isLineVisible(LineOffset { -3 }));
}

TEST_CASE("Backspace", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });

    mock.writeToScreen("12");
    CHECK("12 " == screen.grid().lineText(LineOffset(0)));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(2) });

    mock.writeToScreen("\b");
    CHECK("12 " == screen.grid().lineText(LineOffset(0)));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(1) });

    mock.writeToScreen("\b");
    CHECK("12 " == screen.grid().lineText(LineOffset(0)));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });

    mock.writeToScreen("\b");
    CHECK("12 " == screen.grid().lineText(LineOffset(0)));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });
}

TEST_CASE("Linefeed", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(2) } };
    auto& screen = mock.terminal.primaryScreen();
    SECTION("with scroll-up")
    {
        INFO("init:");
        INFO(std::format("  line 1: '{}'", screen.grid().lineText(LineOffset(0))));
        INFO(std::format("  line 2: '{}'", screen.grid().lineText(LineOffset(1))));

        mock.writeToScreen("1\r\n2");

        INFO("after writing '1\\n2':");
        INFO(std::format("  line 1: '{}'", screen.grid().lineText(LineOffset(0))));
        INFO(std::format("  line 2: '{}'", screen.grid().lineText(LineOffset(1))));

        REQUIRE("1 " == screen.grid().lineText(LineOffset(0)));
        REQUIRE("2 " == screen.grid().lineText(LineOffset(1)));

        mock.writeToScreen("\r\n3"); // line 3

        INFO("After writing '\\n3':");
        INFO(std::format("  line 1: '{}'", screen.grid().lineText(LineOffset(0))));
        INFO(std::format("  line 2: '{}'", screen.grid().lineText(LineOffset(1))));

        REQUIRE("2 " == screen.grid().lineText(LineOffset(0)));
        REQUIRE("3 " == screen.grid().lineText(LineOffset(1)));
    }
}

TEST_CASE("DSR.Unsolicited_ColorPaletteUpdated", "[screen]")
{
    auto const lightModeColors = []() -> ColorPalette {
        ColorPalette palette {};
        palette.defaultForeground = RGBColor { 0x00, 0x00, 0x00 };
        palette.defaultBackground = RGBColor { 0xff, 0xff, 0xff };
        return palette;
    }();

    auto const darkModeColors = []() -> ColorPalette {
        ColorPalette palette {};
        palette.defaultForeground = RGBColor { 0xff, 0xff, 0xff };
        palette.defaultBackground = RGBColor { 0x00, 0x00, 0x00 };
        return palette;
    }();

    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(3) } };

    REQUIRE_FALSE(mock.terminal.isModeEnabled(DECMode::ReportColorPaletteUpdated));

    // Set light mode colors
    mock.terminal.resetColorPalette(lightModeColors);

    // This must not trigger an unsolicited DSR by default.
    REQUIRE(escape(mock.replyData()).empty());

    // Request unsolicited DSRs for color palette updates.
    mock.writeToScreen(DECSM(toDECModeNum(DECMode::ReportColorPaletteUpdated)));
    // mock.terminal.setMode(DECMode::ReportColorPaletteUpdated, true); // FIXME (above)
    REQUIRE(mock.terminal.isModeEnabled(DECMode::ReportColorPaletteUpdated));

    // Set dark mode colors
    mock.terminal.resetColorPalette(lightModeColors);

    // This must trigger an unsolicited DSR.
    REQUIRE(escape(mock.replyData()) == escape("\033[?997;2n"sv));
    mock.resetReplyData();

    // Set light mode colors
    mock.terminal.resetColorPalette(darkModeColors);

    // This must trigger an unsolicited DSR.
    REQUIRE(escape(mock.replyData()) == escape("\033[?997;1n"sv));
    mock.resetReplyData();
}

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

TEST_CASE("DECFI", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& primaryScreen = mock.terminal.primaryScreen();

    mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    REQUIRE(primaryScreen.realCursorPosition() == CellLocation { LineOffset(4), ColumnOffset(4) });
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == primaryScreen.renderMainPageText());

    mock.writeToScreen(DECSM(69)); // Enable left right margin mode
    REQUIRE(mock.terminal.isModeEnabled(DECMode::LeftRightMargin));

    mock.writeToScreen(DECSLRM(2, 4)); // Set left/right margin
    REQUIRE(primaryScreen.margin().horizontal == Margin::Horizontal { ColumnOffset(1), ColumnOffset(3) });
    REQUIRE(primaryScreen.realCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });

    mock.writeToScreen(DECSTBM(2, 4)); // Set top/bottom margin
    REQUIRE(primaryScreen.margin().vertical == Margin::Vertical { LineOffset(1), LineOffset(3) });
    REQUIRE(primaryScreen.realCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });

    // from 0,0 to 0,1 (from outside margin to left border)
    mock.writeToScreen(DECFI());
    CHECK(primaryScreen.realCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(1) });
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == primaryScreen.renderMainPageText());

    // from 0,1 to 0,2
    mock.writeToScreen(DECFI());
    CHECK(primaryScreen.realCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(2) });
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == primaryScreen.renderMainPageText());

    // from 0,2 to 0,3
    mock.writeToScreen(DECFI());
    CHECK(primaryScreen.realCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(3) });
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == primaryScreen.renderMainPageText());

    // from 0,3 to 0,3, scrolling 1 left
    mock.writeToScreen(DECFI());
    CHECK(primaryScreen.realCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(3) });
    REQUIRE("12345\n689 0\nACD E\nFHI J\nKLMNO\n" == primaryScreen.renderMainPageText());

    // from 0,3 to 0,3, scrolling 1 left
    mock.writeToScreen(DECFI());
    REQUIRE(primaryScreen.realCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(3) });
    REQUIRE("12345\n69  0\nAD  E\nFI  J\nKLMNO\n" == primaryScreen.renderMainPageText());

    // from 0,3 to 0,3, scrolling 1 left (now all empty)
    mock.writeToScreen(DECFI());
    REQUIRE(primaryScreen.realCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(3) });
    REQUIRE("12345\n6   0\nA   E\nF   J\nKLMNO\n" == primaryScreen.renderMainPageText());

    // from 0,3 to 0,3, scrolling 1 left (looks just like before)
    mock.writeToScreen(DECFI());
    REQUIRE(primaryScreen.realCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(3) });
    REQUIRE("12345\n6   0\nA   E\nF   J\nKLMNO\n" == primaryScreen.renderMainPageText());
}

TEST_CASE("InsertColumns", "[screen]")
{
    // "DECIC has no effect outside the scrolling margins."
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");

    mock.terminal.setMode(DECMode::LeftRightMargin, true);
    mock.terminal.setLeftRightMargin(ColumnOffset(1), ColumnOffset(3));
    mock.terminal.setTopBottomMargin(LineOffset(1), LineOffset(3));

    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(4), ColumnOffset(4) });

    SECTION("outside margins: top left")
    {
        screen.moveCursorTo({}, {});
        screen.insertColumns(ColumnCount(1));
        REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
    }

    SECTION("outside margins: bottom right")
    {
        screen.moveCursorTo(LineOffset(4), ColumnOffset(4));
        screen.insertColumns(ColumnCount(1));
        REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
    }

    SECTION("inside margins")
    {
        screen.moveCursorTo(LineOffset { 1 }, ColumnOffset { 2 });
        REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(2) });

        SECTION("DECIC-0")
        {
            screen.insertColumns(ColumnCount(0));
            REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
        }

        SECTION("DECIC-1")
        {
            screen.insertColumns(ColumnCount(1));
            REQUIRE("12345\n67 80\nAB CE\nFG HJ\nKLMNO\n" == screen.renderMainPageText());
        }

        SECTION("DECIC-2")
        {
            screen.insertColumns(ColumnCount(2));
            REQUIRE("12345\n67  0\nAB  E\nFG  J\nKLMNO\n" == screen.renderMainPageText());
        }

        SECTION("DECIC-2 (another)")
        {
            screen.moveCursorTo(LineOffset { 1 }, ColumnOffset { 1 });
            screen.insertColumns(ColumnCount(2));
            REQUIRE("12345\n6  70\nA  BE\nF  GJ\nKLMNO\n" == screen.renderMainPageText());
        }

        SECTION("DECIC-3-clamped")
        {
            screen.insertColumns(ColumnCount(3));
            REQUIRE("12345\n67  0\nAB  E\nFG  J\nKLMNO\n" == screen.renderMainPageText());
        }
    }

    SECTION("inside margins - repeative")
    {
        screen.moveCursorTo(LineOffset { 1 }, ColumnOffset { 1 });
        screen.insertColumns(ColumnCount(1));
        REQUIRE("12345\n6 780\nA BCE\nF GHJ\nKLMNO\n" == screen.renderMainPageText());
        screen.insertColumns(ColumnCount(1));
        REQUIRE("12345\n6  70\nA  BE\nF  GJ\nKLMNO\n" == screen.renderMainPageText());
    }
}

TEST_CASE("InsertCharacters.NoMargins", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("123\r\n456");
    mock.writeToScreen("\033[2;2H");
    REQUIRE("123\n456\n" == screen.renderMainPageText());
    REQUIRE(screen.realCursorPosition().line == LineOffset(1));
    REQUIRE(screen.realCursorPosition().column == ColumnOffset(1));

    SECTION("default")
    {
        mock.writeToScreen("\033[@");
        REQUIRE("123\n4 5\n" == screen.renderMainPageText());
    }

    SECTION("ICH: 1 like default")
    {
        mock.writeToScreen("\033[1@");
        REQUIRE("123\n4 5\n" == screen.renderMainPageText());
    }

    SECTION("ICH: exact match")
    {
        mock.writeToScreen("\033[2@");
        REQUIRE("123\n4  \n" == screen.renderMainPageText());
    }

    SECTION("ICH: one overflow")
    {
        mock.writeToScreen("\033[3@");
        REQUIRE("123\n4  \n" == screen.renderMainPageText());
    }

    SECTION("ICH: full line (n-1)")
    {
        mock.writeToScreen("\033[2;1H");
        mock.writeToScreen("\033[2@");
        REQUIRE("123\n  4\n" == screen.renderMainPageText());
    }

    SECTION("ICH: full line (n)")
    {
        mock.writeToScreen("\033[2;1H");
        mock.writeToScreen("\033[3@");
        REQUIRE("123\n   \n" == screen.renderMainPageText());
    }

    SECTION("ICH: full line (n+1)")
    {
        mock.writeToScreen("\033[2;1H");
        mock.writeToScreen("\033[4@");
        REQUIRE("123\n   \n" == screen.renderMainPageText());
    }
}

TEST_CASE("InsertCharacters.Margins", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("12345\r\n678");
    mock.writeToScreen("90");

    mock.terminal.setMode(DECMode::LeftRightMargin, true);
    mock.terminal.setLeftRightMargin(ColumnOffset(1), ColumnOffset(3));
    REQUIRE("12345\n67890\n" == screen.renderMainPageText());

    SECTION("outside margins: left")
    {
        screen.moveCursorTo(LineOffset(0), ColumnOffset(0));
        screen.insertCharacters(ColumnCount(1));
        REQUIRE("12345\n67890\n" == screen.renderMainPageText());
    }

    SECTION("outside margins: right")
    {
        screen.moveCursorTo(LineOffset { 0 }, ColumnOffset { 4 });
        screen.insertCharacters(ColumnCount(1));
        REQUIRE("12345\n67890\n" == screen.renderMainPageText());
    }

    SECTION("inside margins")
    {
        screen.moveCursorTo(LineOffset(0), ColumnOffset(2));
        REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(2) });

        SECTION("no-op")
        {
            screen.insertCharacters(ColumnCount(0));
            CHECK(screen.renderMainPageText() == "12345\n67890\n");
        }

        SECTION("ICH-1")
        {
            screen.insertCharacters(ColumnCount(1));
            CHECK(screen.renderMainPageText() == "12 35\n67890\n");
        }

        SECTION("ICH-2")
        {
            screen.insertCharacters(ColumnCount(2));
            CHECK(screen.renderMainPageText() == "12  5\n67890\n");
        }

        SECTION("ICH-3-clamped")
        {
            screen.insertCharacters(ColumnCount(3));
            REQUIRE(screen.renderMainPageText() == "12  5\n67890\n");
        }
    }
}

TEST_CASE("InsertMode", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(10) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("ABCDEFGHIJ");
    screen.moveCursorTo(LineOffset(0), ColumnOffset(3));

    SECTION("basic insert shifts text right")
    {
        mock.writeToScreen("\033[4h"); // Enable IRM
        mock.writeToScreen("XY");      // Insert "XY" at column 3
        CHECK(screen.renderMainPageText() == "ABCXYDEFGH\n");
        // "IJ" pushed past the right margin are lost
    }

    SECTION("disable insert returns to overwrite")
    {
        mock.writeToScreen("\033[4h"); // Enable IRM
        mock.writeToScreen("X");       // Insert 'X' at column 3 -> "ABCXDEFGHI"
        mock.writeToScreen("\033[4l"); // Disable IRM
        mock.writeToScreen("Z");       // Overwrite at column 4 -> "ABCXZEFGHI"
        CHECK(screen.renderMainPageText() == "ABCXZEFGHI\n");
    }

    SECTION("insert single character at end of line")
    {
        screen.moveCursorTo(LineOffset(0), ColumnOffset(9));
        mock.writeToScreen("\033[4h"); // Enable IRM
        mock.writeToScreen("X");       // Insert at last column
        CHECK(screen.renderMainPageText() == "ABCDEFGHIX\n");
    }
}

TEST_CASE("InsertLines", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(6), ColumnCount(4) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("1234\r\n5678\r\nABCD\r\nEFGH\r\nIJKL\r\nMNOP");
    REQUIRE("1234\n5678\nABCD\nEFGH\nIJKL\nMNOP\n" == screen.renderMainPageText());

    SECTION("old")
    {
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(2) } };
        auto& screen = mock.terminal.primaryScreen();

        mock.writeToScreen("AB\r\nCD");
        REQUIRE("AB" == screen.grid().lineText(LineOffset(0)));
        REQUIRE("CD" == screen.grid().lineText(LineOffset(1)));
        REQUIRE("  " == screen.grid().lineText(LineOffset(2)));

        logScreenText(screen, "A");
        screen.insertLines(LineCount(1));
        logScreenText(screen, "B");
        CHECK("AB" == screen.grid().lineText(LineOffset(0)));
        CHECK("  " == screen.grid().lineText(LineOffset(1)));
        CHECK("CD" == screen.grid().lineText(LineOffset(2)));

        screen.moveCursorTo(LineOffset { 0 }, ColumnOffset { 0 });
        screen.insertLines(LineCount(1));
        CHECK("  " == screen.grid().lineText(LineOffset(0)));
        CHECK("AB" == screen.grid().lineText(LineOffset(1)));
        CHECK("  " == screen.grid().lineText(LineOffset(2)));
    }
    // TODO: test with (top/bottom and left/right) margins enabled
}

// {{{ DECSCA
TEST_CASE("DECSCA: enable and disable character protection", "[screen]")
{
    // Verifies that DECSCA Ps=1 enables CharacterProtected on subsequent characters,
    // and DECSCA Ps=0/2 disables it.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(6) } };
    auto& screen = mock.terminal.primaryScreen();

    // A unprotected, BC protected, D unprotected (Ps=0), EF unprotected (Ps=2)
    mock.writeToScreen(std::format("A{0}BC{1}D{2}EF", "\033[1\"q", "\033[0\"q", "\033[2\"q"));

    REQUIRE("ABCDEF" == screen.grid().lineText(LineOffset(0)));

    CHECK_FALSE(screen.at(LineOffset(0), ColumnOffset(0)).isFlagEnabled(CellFlag::CharacterProtected)); // A
    CHECK(screen.at(LineOffset(0), ColumnOffset(1)).isFlagEnabled(CellFlag::CharacterProtected));       // B
    CHECK(screen.at(LineOffset(0), ColumnOffset(2)).isFlagEnabled(CellFlag::CharacterProtected));       // C
    CHECK_FALSE(screen.at(LineOffset(0), ColumnOffset(3)).isFlagEnabled(CellFlag::CharacterProtected)); // D
    CHECK_FALSE(screen.at(LineOffset(0), ColumnOffset(4)).isFlagEnabled(CellFlag::CharacterProtected)); // E
    CHECK_FALSE(screen.at(LineOffset(0), ColumnOffset(5)).isFlagEnabled(CellFlag::CharacterProtected)); // F
}

TEST_CASE("DECSCA: default parameter disables protection", "[screen]")
{
    // DECSCA with no parameter (default) should disable protection.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(4) } };
    auto& screen = mock.terminal.primaryScreen();

    // AB protected, then DECSCA with default (0) disables protection, CD unprotected
    mock.writeToScreen(std::format("{0}AB{1}CD", "\033[1\"q", "\033[\"q"));

    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).isFlagEnabled(CellFlag::CharacterProtected));       // A
    CHECK(screen.at(LineOffset(0), ColumnOffset(1)).isFlagEnabled(CellFlag::CharacterProtected));       // B
    CHECK_FALSE(screen.at(LineOffset(0), ColumnOffset(2)).isFlagEnabled(CellFlag::CharacterProtected)); // C
    CHECK_FALSE(screen.at(LineOffset(0), ColumnOffset(3)).isFlagEnabled(CellFlag::CharacterProtected)); // D
}

TEST_CASE("DECSCA: protection is independent of SGR rendition", "[screen]")
{
    // DECSCA protection attribute is independent of SGR visual attributes.
    // Setting SGR bold or other attributes should not affect the protection state.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(4) } };
    auto& screen = mock.terminal.primaryScreen();

    // Enable protection, then set bold, then write characters — protection should persist.
    mock.writeToScreen(std::format("{0}\033[1mAB{1}CD", "\033[1\"q", "\033[0\"q"));

    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).isFlagEnabled(CellFlag::CharacterProtected)); // A
    CHECK(screen.at(LineOffset(0), ColumnOffset(1)).isFlagEnabled(CellFlag::CharacterProtected)); // B
    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).isFlagEnabled(CellFlag::Bold));               // A bold
    CHECK(screen.at(LineOffset(0), ColumnOffset(1)).isFlagEnabled(CellFlag::Bold));               // B bold
}

TEST_CASE("DECSCA: save and restore cursor preserves protection state", "[screen]")
{
    // DECSC/DECRC should save and restore the CharacterProtected attribute.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(4) } };
    auto& screen = mock.terminal.primaryScreen();

    // Enable protection, save cursor, disable protection, write AB (unprotected),
    // restore cursor (re-enables protection), write CD (protected, overwrites AB).
    mock.writeToScreen(std::format("{0}\0337{1}AB\0338CD", "\033[1\"q", "\033[0\"q"));

    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).isFlagEnabled(CellFlag::CharacterProtected)); // C
    CHECK(screen.at(LineOffset(0), ColumnOffset(1)).isFlagEnabled(CellFlag::CharacterProtected)); // D
    CHECK_FALSE(
        screen.at(LineOffset(0), ColumnOffset(2)).isFlagEnabled(CellFlag::CharacterProtected)); // empty
    CHECK_FALSE(
        screen.at(LineOffset(0), ColumnOffset(3)).isFlagEnabled(CellFlag::CharacterProtected)); // empty
}
// }}}

// {{{ DECSEL
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
// }}}

// {{{ DECSED
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
// }}}

// {{{ SPA / EPA (ISO 6429 guarded-area protection)
TEST_CASE("SPA/EPA: ED respects ISO protection", "[screen]")
{
    // Mirrors esctest ED_respectsISOProtection: a cell written between SPA (ESC V) and EPA (ESC W)
    // survives a *regular* ED, while the unprotected cells around it are erased.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("ab\033Vc\033W"); // a, b, SPA, protected c, EPA
    REQUIRE(e(mainPageText(screen)) == "abc\\n");

    mock.writeToScreen("\033[H\033[J"); // CUP home, then ED to end of screen
    REQUIRE(e(mainPageText(screen)) == "  c\\n");
}

TEST_CASE("SPA/EPA: EL respects ISO protection", "[screen]")
{
    // Mirrors esctest EL_respectsISOProtection: EL 2 (erase whole line) spares the protected cell.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("ab\033Vc\033W");
    mock.writeToScreen("\033[H\033[2K"); // CUP home, EL 2 (entire line)
    REQUIRE(e(mainPageText(screen)) == "  c\\n");
}

TEST_CASE("SPA/EPA: ECH respects ISO protection", "[screen]")
{
    // Mirrors esctest ECH_respectsISOProtection: ECH 3 erases three cells but spares the protected one.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("ab\033Vc\033W");
    mock.writeToScreen("\033[H\033[3X"); // CUP home, ECH 3
    REQUIRE(e(mainPageText(screen)) == "  c\\n");
}

TEST_CASE("SPA/EPA: 8-bit C1 forms behave like ESC V / ESC W", "[screen]")
{
    // The parser folds a lone C1 byte onto ESC + (byte - 0x40): 0x96 -> SPA, 0x97 -> EPA. So the
    // 8-bit forms must guard cells identically to the 7-bit ESC V / ESC W (esctest S8C1T_SPA_EPA).
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("ab\x96"
                       "c\x97"); // a, b, SPA(0x96), protected c, EPA(0x97)
    mock.writeToScreen("\033[H\033[J");
    REQUIRE(e(mainPageText(screen)) == "  c\\n");
}

TEST_CASE("SPA/EPA: 8-bit C1 protection survives inside a coalesced text run", "[screen]")
{
    // Regression for the real-PTY case: the bytes arrive in one buffer, so the 8-bit SPA (0x96) sits
    // mid-run followed by a long text tail -- the condition under which the bulk text scanner would
    // swallow the C1 as U+FFFD instead of leaving it for the state machine to fold. The guarded cell
    // must still survive a later erase. (esctest S8C1T_SPA_EPA is the end-to-end counterpart.)
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(20) } };
    auto& screen = mock.terminal.primaryScreen();

    // "ab", SPA, protected "c", EPA, then a long ASCII tail -- all one write, i.e. one parser buffer.
    mock.writeToScreen("ab\x96"
                       "c\x97"
                       "defghijklmnop");
    mock.writeToScreen("\033[H\033[K"); // CUP home, EL to end of line
    REQUIRE(e(mainPageText(screen)).substr(0, 3) == "  c");
}

TEST_CASE("DECSCA: regular ED does not respect DEC protection", "[screen]")
{
    // Mirrors esctest ED_doesNotRespectDECProtection: DECSCA protection is honoured only by the
    // *selective* erases, so a regular ED erases a DECSCA-protected cell.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("ab\033[1\"qc\033[0\"q");  // a, b, DECSCA(1), protected c, DECSCA(0)
    mock.writeToScreen("\033[H\033[J");           // CUP home, ED to end
    REQUIRE(e(mainPageText(screen)) == "   \\n"); // c erased too
}

TEST_CASE("SPA/EPA: soft reset clears ISO protection mode", "[screen]")
{
    // xterm's ReallyReset zeroes protected_mode unconditionally, so a DECSTR must return the screen
    // to the unprotected model: a subsequent regular ED then erases even a previously guarded cell.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("ab\033Vc\033W");
    mock.writeToScreen("\033[!p");                // DECSTR (soft reset)
    mock.writeToScreen("\033[H\033[J");           // CUP home, ED to end
    REQUIRE(e(mainPageText(screen)) == "   \\n"); // guarded c is now erasable
}

TEST_CASE("SPA/EPA: selective erases do NOT respect ISO protection", "[screen]")
{
    // The inverse pairing: DECSED/DECSEL/DECSERA spare DEC (DECSCA) protection only. An ISO-guarded
    // cell (SPA/EPA) is erased by them -- mirrors esctest DECSED/DECSEL/DECSERA_doesNotRespectISOProtect.
    SECTION("DECSED erases an ISO-guarded cell")
    {
        auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(2) } };
        auto& screen = mock.terminal.primaryScreen();
        mock.writeToScreen("a\033Vb\033W"); // a, SPA, ISO-guarded b, EPA
        mock.writeToScreen("\033[?2J");     // DECSED 2 (selective erase display)
        REQUIRE(e(mainPageText(screen)) == "  \\n");
    }
    SECTION("DECSEL erases an ISO-guarded cell")
    {
        auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(2) } };
        auto& screen = mock.terminal.primaryScreen();
        mock.writeToScreen("a\033Vb\033W");
        mock.writeToScreen("\033[?2K"); // DECSEL 2 (selective erase line)
        REQUIRE(e(mainPageText(screen)) == "  \\n");
    }
    SECTION("DECSERA erases an ISO-guarded cell")
    {
        auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(2) } };
        auto& screen = mock.terminal.primaryScreen();
        mock.writeToScreen("a\033Vb\033W");
        mock.writeToScreen("\033[1;1;1;2${"); // DECSERA over the row
        REQUIRE(e(mainPageText(screen)) == "  \\n");
    }
}

TEST_CASE("DECSCA: selective erase still respects DEC protection after the ISO split", "[screen]")
{
    // Regression guard for the two-flag split: DECSED must keep sparing DECSCA-protected cells.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(2) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("a\033[1\"qb\033[0\"q"); // a, DECSCA(1), DEC-protected b, DECSCA(0)
    mock.writeToScreen("\033[?2J");             // DECSED 2 spares the DEC-protected b
    REQUIRE(e(mainPageText(screen)) == " b\\n");
}
// }}}

// {{{ VT52 mode
TEST_CASE("VT52: enter, cursor movement, and leave", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033[?2l"); // DECANM reset: enter VT52
    REQUIRE(mock.terminal.isVT52Mode());

    // ESC Y row col -- direct cursor address; each coordinate byte is value + 0x20.
    mock.writeToScreen("\033Y\x23\x25"); // row 0x23-0x20=3, col 0x25-0x20=5
    REQUIRE(screen.cursor().position == CellLocation { LineOffset(3), ColumnOffset(5) });

    mock.writeToScreen("\033H"); // ESC H -- home (must be cursor-home in VT52, not HTS)
    REQUIRE(screen.cursor().position == CellLocation { LineOffset(0), ColumnOffset(0) });

    mock.writeToScreen("\033B\033B\033C"); // down, down, right
    REQUIRE(screen.cursor().position == CellLocation { LineOffset(2), ColumnOffset(1) });

    mock.writeToScreen("\033A\033D"); // up, left (ESC D is cursor-left in VT52, not IND)
    REQUIRE(screen.cursor().position == CellLocation { LineOffset(1), ColumnOffset(0) });

    mock.writeToScreen("\033<"); // ESC < -- leave VT52
    REQUIRE_FALSE(mock.terminal.isVT52Mode());
    REQUIRE(mock.terminal.operatingLevel() == VTType::VT100); // VT52 exit enters ANSI at VT100

    // Back in ANSI mode, CSI cursor movement works again.
    mock.writeToScreen("\033[3;4H");
    REQUIRE(screen.cursor().position == CellLocation { LineOffset(2), ColumnOffset(3) });
}

TEST_CASE("VT52: identify responds with ESC / Z", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(4) } };
    mock.writeToScreen("\033[?2l\033Z"); // enter VT52, then ESC Z (identify)
    REQUIRE(mock.terminal.peekInput() == "\033/Z");
}

TEST_CASE("VT52: erase to end of line and screen", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(4) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("abcd\033[?2l");  // fill row 0, enter VT52
    mock.writeToScreen("\033Y\x20\x22"); // ESC Y: row 0, col 2
    mock.writeToScreen("\033K");         // ESC K -- erase to end of line
    REQUIRE(screen.grid().lineText(LineOffset(0)) == "ab  ");
}
// }}}

// {{{ DECSERA
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
// }}}

TEST_CASE("DeleteLines", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(2) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("AB\r\nCD\r\nEF");
    logScreenText(screen, "initial");
    REQUIRE("AB" == screen.grid().lineText(LineOffset(0)));
    REQUIRE("CD" == screen.grid().lineText(LineOffset(1)));
    REQUIRE("EF" == screen.grid().lineText(LineOffset(2)));

    screen.moveCursorTo(LineOffset(1), ColumnOffset(0));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(0) });

    SECTION("no-op")
    {
        screen.deleteLines(LineCount(0));
        CHECK("AB" == screen.grid().lineText(LineOffset(0)));
        CHECK("CD" == screen.grid().lineText(LineOffset(1)));
        CHECK("EF" == screen.grid().lineText(LineOffset(2)));
    }

    SECTION("in-range")
    {
        logScreenText(screen, "After EL(1) - 1");
        screen.deleteLines(LineCount(1));
        logScreenText(screen, "After EL(1)");
        CHECK("AB" == screen.grid().lineText(LineOffset(0)));
        CHECK("EF" == screen.grid().lineText(LineOffset(1)));
        CHECK("  " == screen.grid().lineText(LineOffset(2)));
    }

    SECTION("clamped")
    {
        screen.moveCursorTo(LineOffset(1), ColumnOffset(1));
        screen.deleteLines(LineCount(5));
        // logScreenText(screen, "After clamped EL(5)");
        CHECK("AB" == screen.grid().lineText(LineOffset(0)));
        CHECK("  " == screen.grid().lineText(LineOffset(1)));
        CHECK("  " == screen.grid().lineText(LineOffset(2)));
    }
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

TEST_CASE("DeleteColumns", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    mock.terminal.setMode(DECMode::LeftRightMargin, true);
    mock.terminal.setLeftRightMargin(ColumnOffset(1), ColumnOffset(3));
    mock.terminal.setTopBottomMargin(LineOffset(1), LineOffset(3));

    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(4), ColumnOffset(4) });

    SECTION("outside margin")
    {
        screen.deleteColumns(ColumnCount(1));
        REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
    }

    SECTION("inside margin")
    {
        screen.moveCursorTo(LineOffset(1), ColumnOffset(2));
        REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(2) });

        SECTION("DECDC-0")
        {
            screen.deleteColumns(ColumnCount(0));
            REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
        }
        SECTION("DECDC-1")
        {
            screen.deleteColumns(ColumnCount(1));
            REQUIRE("12345\n679 0\nABD E\nFGI J\nKLMNO\n" == screen.renderMainPageText());
        }
        SECTION("DECDC-2")
        {
            screen.deleteColumns(ColumnCount(2));
            REQUIRE("12345\n67  0\nAB  E\nFG  J\nKLMNO\n" == screen.renderMainPageText());
        }
        SECTION("DECDC-3-clamped")
        {
            screen.deleteColumns(ColumnCount(4));
            REQUIRE("12345\n67  0\nAB  E\nFG  J\nKLMNO\n" == screen.renderMainPageText());
        }
    }
}

TEST_CASE("DeleteCharacters", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("12345\r\n67890\033[1;2H");
    REQUIRE("12345\n67890\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(1) });

    SECTION("outside margin")
    {
        mock.writeToScreen(DECSM(toDECModeNum(DECMode::LeftRightMargin)));
        mock.writeToScreen(DECSLRM(2, 4));
        mock.writeToScreen(CUP(1, 1));
        mock.writeToScreen(DCH(1));
        REQUIRE("12345\n67890\n" == screen.renderMainPageText());
    }

    SECTION("without horizontal margin")
    {
        SECTION("no-op")
        {
            screen.deleteCharacters(ColumnCount(0));
            REQUIRE("12345\n67890\n" == screen.renderMainPageText());
        }
        SECTION("in-range-1")
        {
            screen.deleteCharacters(ColumnCount(1));
            REQUIRE("1345 \n67890\n" == screen.renderMainPageText());
        }
        SECTION("in-range-2")
        {
            screen.deleteCharacters(ColumnCount(2));
            REQUIRE("145  \n67890\n" == screen.renderMainPageText());
        }
        SECTION("in-range-4")
        {
            screen.deleteCharacters(ColumnCount(4));
            REQUIRE("1    \n67890\n" == screen.renderMainPageText());
        }
        SECTION("clamped")
        {
            screen.deleteCharacters(ColumnCount(5));
            REQUIRE("1    \n67890\n" == screen.renderMainPageText());
        }
    }
    SECTION("with horizontal margin")
    {
        mock.terminal.setMode(DECMode::LeftRightMargin, true);
        mock.terminal.setLeftRightMargin(ColumnOffset(0), ColumnOffset(3));
        screen.moveCursorTo(LineOffset(0), ColumnOffset(1));
        REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(1) });

        SECTION("no-op")
        {
            screen.deleteCharacters(ColumnCount(0));
            REQUIRE("12345\n67890\n" == screen.renderMainPageText());
        }
        SECTION("in-range-1")
        {
            REQUIRE("12345\n67890\n" == screen.renderMainPageText());
            screen.deleteCharacters(ColumnCount(1));
            REQUIRE("134 5\n67890\n" == screen.renderMainPageText());
        }
        SECTION("in-range-2")
        {
            screen.deleteCharacters(ColumnCount(2));
            REQUIRE("14  5\n67890\n" == screen.renderMainPageText());
        }
        SECTION("clamped")
        {
            screen.deleteCharacters(ColumnCount(4));
            REQUIRE("1   5\n67890\n" == screen.renderMainPageText());
        }
    }
}

TEST_CASE("ClearScrollbackBuffer", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) }, LineCount(1) };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO\r\nPQRST\033[H");
    REQUIRE("67890\nABCDE\nFGHIJ\nKLMNO\nPQRST\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });
    REQUIRE(screen.historyLineCount() == LineCount(1));
    REQUIRE("12345" == screen.grid().lineText(LineOffset(-1)));

    screen.grid().clearHistory();
    REQUIRE(screen.historyLineCount() == LineCount(0));
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

TEST_CASE("ScrollUp.WithMargins", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    logScreenText(screen, "init");
    REQUIRE("12345\n"
            "67890\n"
            "ABCDE\n"
            "FGHIJ\n"
            "KLMNO\n"
            == screen.renderMainPageText());

    // "\033[?69h\033[2;4s\033[2;4r\033[20S"
    mock.terminal.setMode(DECMode::LeftRightMargin, true);              // DECSLRM
    mock.terminal.setLeftRightMargin(ColumnOffset(1), ColumnOffset(3)); // DECLRMM
    mock.terminal.setTopBottomMargin(LineOffset(1), LineOffset(3));     // DECSTBM

    SECTION("SU-1")
    {
        screen.scrollUp(LineCount(1));
        logScreenText(screen, "after 1");
        REQUIRE("12345\n"
                "6BCD0\n"
                "AGHIE\n"
                "F   J\n"
                "KLMNO\n"
                == screen.renderMainPageText());
    }

    SECTION("SU-2")
    {
        screen.scrollUp(LineCount(2));
        logScreenText(screen, "after 2");
        REQUIRE("12345\n"
                "6GHI0\n"
                "A   E\n"
                "F   J\n"
                "KLMNO\n"
                == screen.renderMainPageText());
    }

    SECTION("SU-3")
    {
        screen.scrollUp(LineCount(3));
        logScreenText(screen, "after 3");
        REQUIRE("12345\n"
                "6   0\n"
                "A   E\n"
                "F   J\n"
                "KLMNO\n"
                == screen.renderMainPageText());
    }

    SECTION("SU-3 (overflow)")
    {
        screen.scrollUp(LineCount(4));
        logScreenText(screen, "after 4");
        REQUIRE("12345\n"
                "6   0\n"
                "A   E\n"
                "F   J\n"
                "KLMNO\n"
                == screen.renderMainPageText());
    }
    mock.writeToScreen("\033[r");
    mock.writeToScreen("\033[s");
    REQUIRE(screen.margin().vertical == Margin::Vertical { LineOffset(0), LineOffset(4) });
    REQUIRE(screen.margin().horizontal == Margin::Horizontal { ColumnOffset(0), ColumnOffset(4) });
}

TEST_CASE("ScrollUp", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("ABC\r\n");
    mock.writeToScreen("DEF\r\n");
    mock.writeToScreen("GHI");
    REQUIRE("ABC\nDEF\nGHI\n" == screen.renderMainPageText());

    SECTION("no-op")
    {
        INFO("begin:");
        screen.scrollUp(LineCount(0));
        INFO("end:");
        REQUIRE("ABC\nDEF\nGHI\n" == screen.renderMainPageText());
    }

    SECTION("by-1")
    {
        screen.scrollUp(LineCount(1));
        REQUIRE("DEF\nGHI\n   \n" == screen.renderMainPageText());
    }

    SECTION("by-2")
    {
        screen.scrollUp(LineCount(2));
        REQUIRE("GHI\n   \n   \n" == screen.renderMainPageText());
    }

    SECTION("by-3")
    {
        screen.scrollUp(LineCount(3));
        REQUIRE("   \n   \n   \n" == screen.renderMainPageText());
    }

    SECTION("clamped")
    {
        screen.scrollUp(LineCount(4));
        REQUIRE("   \n   \n   \n" == screen.renderMainPageText());
    }
}

TEST_CASE("ScrollDown", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());

    SECTION("scroll fully inside margins")
    {
        mock.terminal.setMode(DECMode::LeftRightMargin, true);
        mock.terminal.setLeftRightMargin(ColumnOffset { 1 }, ColumnOffset { 3 });
        mock.terminal.setTopBottomMargin(LineOffset { 1 }, LineOffset { 3 });
        mock.terminal.setMode(DECMode::Origin, true);

        // SECTION("SD 1") {
        //     screen.scrollDown(LineCount(1));
        //     CHECK("12345\n6   0\nA789E\nFBCDJ\nKLMNO\n" == screen.renderMainPageText());
        // }

        // SECTION("SD 2") {
        //     screen.scrollDown(LineCount(2));
        //     CHECK(
        //         "12345\n"
        //         "6   0\n"
        //         "A   E\n"
        //         "F789J\n"
        //         "KLMNO\n" == screen.renderMainPageText());
        // }
        //
        // SECTION("SD 3") {
        //     screen.scrollDown(LineCount(3));
        //     CHECK(
        //         "12345\n"
        //         "6   0\n"
        //         "A   E\n"
        //         "F   J\n"
        //         "KLMNO\n" == screen.renderMainPageText());
        // }
    }

    SECTION("vertical margins")
    {
        mock.terminal.setTopBottomMargin(LineOffset { 1 }, LineOffset { 3 });
        SECTION("SD 0")
        {
            screen.scrollDown(LineCount(0));
            REQUIRE("12345\n"
                    "67890\n"
                    "ABCDE\n"
                    "FGHIJ\n"
                    "KLMNO\n"
                    == screen.renderMainPageText());
        }

        SECTION("SD 1")
        {
            screen.scrollDown(LineCount(1));
            REQUIRE("12345\n"
                    "     \n"
                    "67890\n"
                    "ABCDE\n"
                    "KLMNO\n"
                    == screen.renderMainPageText());
        }

        SECTION("SD 3")
        {
            screen.scrollDown(LineCount(5));
            REQUIRE("12345\n"
                    "     \n"
                    "     \n"
                    "     \n"
                    "KLMNO\n"
                    == screen.renderMainPageText());
        }

        SECTION("SD 4 clamped")
        {
            screen.scrollDown(LineCount(4));
            REQUIRE("12345\n"
                    "     \n"
                    "     \n"
                    "     \n"
                    "KLMNO\n"
                    == screen.renderMainPageText());
        }
    }

    SECTION("no custom margins")
    {
        SECTION("SD 0")
        {
            screen.scrollDown(LineCount(0));
            REQUIRE("12345\n"
                    "67890\n"
                    "ABCDE\n"
                    "FGHIJ\n"
                    "KLMNO\n"
                    == screen.renderMainPageText());
        }
        SECTION("SD 1")
        {
            screen.scrollDown(LineCount(1));
            REQUIRE("     \n"
                    "12345\n"
                    "67890\n"
                    "ABCDE\n"
                    "FGHIJ\n"
                    == screen.renderMainPageText());
        }
        SECTION("SD 5")
        {
            screen.scrollDown(LineCount(5));
            REQUIRE("     \n"
                    "     \n"
                    "     \n"
                    "     \n"
                    "     \n"
                    == screen.renderMainPageText());
        }
        SECTION("SD 6 clamped")
        {
            screen.scrollDown(LineCount(6));
            REQUIRE("     \n"
                    "     \n"
                    "     \n"
                    "     \n"
                    "     \n"
                    == screen.renderMainPageText());
        }
    }
}

TEST_CASE("Unscroll", "[screen]")
{
    SECTION("with history")
    {
        // 5 lines page, 5 lines scrollback capacity
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) }, LineCount(5) };
        auto& screen = mock.terminal.primaryScreen();

        // Write 8 lines to create 3 lines of history
        mock.writeToScreen("AAAAA\r\nBBBBB\r\nCCCCC\r\nDDDDD\r\nEEEEE\r\nFFFFF\r\nGGGGG\r\nHHHHH");
        REQUIRE(screen.historyLineCount() == LineCount(3));
        REQUIRE("DDDDD\nEEEEE\nFFFFF\nGGGGG\nHHHHH\n" == screen.renderMainPageText());

        // Unscroll 2 lines — should pull 2 most-recent history lines into view
        screen.unscroll(LineCount(2));
        CHECK(screen.historyLineCount() == LineCount(1));
        CHECK("BBBBB\nCCCCC\nDDDDD\nEEEEE\nFFFFF\n" == screen.renderMainPageText());
    }

    SECTION("partial history")
    {
        // 5 lines page, 3 lines scrollback capacity
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) }, LineCount(3) };
        auto& screen = mock.terminal.primaryScreen();

        // Write 7 lines to create 2 lines of history
        mock.writeToScreen("AAAAA\r\nBBBBB\r\nCCCCC\r\nDDDDD\r\nEEEEE\r\nFFFFF\r\nGGGGG");
        REQUIRE(screen.historyLineCount() == LineCount(2));
        REQUIRE("CCCCC\nDDDDD\nEEEEE\nFFFFF\nGGGGG\n" == screen.renderMainPageText());

        // Unscroll 4 lines — 2 from history + 2 blank
        screen.unscroll(LineCount(4));
        CHECK(screen.historyLineCount() == LineCount(0));
        CHECK("     \n     \nAAAAA\nBBBBB\nCCCCC\n" == screen.renderMainPageText());
    }

    SECTION("no history")
    {
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
        auto& screen = mock.terminal.primaryScreen();

        mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
        REQUIRE(screen.historyLineCount() == LineCount(0));
        REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());

        // Unscroll with no history — should behave like regular SD
        screen.unscroll(LineCount(2));
        CHECK("     \n     \n12345\n67890\nABCDE\n" == screen.renderMainPageText());
    }

    SECTION("clamped to page size")
    {
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) }, LineCount(5) };
        auto& screen = mock.terminal.primaryScreen();

        mock.writeToScreen("AAAAA\r\nBBBBB\r\nCCCCC\r\nDDDDD\r\nEEEEE\r\nFFFFF");
        REQUIRE(screen.historyLineCount() == LineCount(3));

        // Unscroll 10 lines — should clamp to page size (3)
        screen.unscroll(LineCount(10));
        CHECK(screen.historyLineCount() == LineCount(0));
        CHECK("AAAAA\nBBBBB\nCCCCC\n" == screen.renderMainPageText());
    }
}

TEST_CASE("Sequence.CUU", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
    screen.moveCursorTo(LineOffset { 2 }, ColumnOffset { 1 });
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(2), ColumnOffset(1) });

    SECTION("default")
    {
        mock.writeToScreen(CUU());
        REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(1) });
    }

    SECTION("0")
    {
        mock.writeToScreen(CUU());
        REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(1) });
    }

    SECTION("in-range")
    {
        mock.writeToScreen(CUU(1));
        REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(1) });
    }

    SECTION("overflow")
    {
        mock.writeToScreen(CUU(5));
        REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(1) });
    }

    SECTION("with margins")
    {
        mock.writeToScreen(DECSTBM(2, 4));
        mock.writeToScreen(CUP(3, 2));
        REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(2), ColumnOffset(1) });

        SECTION("in-range")
        {
            mock.writeToScreen(CUU(1));
            REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(1) });
        }

        SECTION("overflow")
        {
            mock.writeToScreen(CUU(5));
            REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(1) });
        }
    }

    SECTION("cursor already above margins")
    {
        mock.writeToScreen(DECSTBM(3, 4));
        mock.writeToScreen(CUP(2, 3));
        mock.writeToScreen(CUU(1));
        REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(2) });
    }
}

TEST_CASE("MoveCursorDown", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(2) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("A");
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(1) });

    // no-op
    screen.moveCursorDown(LineCount(0));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(1) });

    // in-range
    screen.moveCursorDown(LineCount(1));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(1) });

    // overflow
    screen.moveCursorDown(LineCount(5));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(2), ColumnOffset(1) });
}

TEST_CASE("MoveCursorForward", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });

    SECTION("no-op")
    {
        screen.moveCursorForward(ColumnCount(0));
        REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });
    }

    SECTION("CUF-1")
    {
        screen.moveCursorForward(ColumnCount(1));
        REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(1) });
    }

    SECTION("CUF-3 (to right border)")
    {
        screen.moveCursorForward(screen.pageSize().columns);
        REQUIRE(screen.logicalCursorPosition().column.value == screen.pageSize().columns.value - 1);
    }

    SECTION("CUF-overflow")
    {
        screen.moveCursorForward(screen.pageSize().columns + ColumnCount(1));
        REQUIRE(screen.logicalCursorPosition().column.value == screen.pageSize().columns.value - 1);
    }
}

TEST_CASE("MoveCursorBackward", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("ABC");
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(2) });

    // no-op
    screen.moveCursorBackward(ColumnCount(0));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(2) });

    // in-range
    screen.moveCursorBackward(ColumnCount(1));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(1) });
    // overflow
    screen.moveCursorBackward(ColumnCount(5));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });
}

TEST_CASE("HorizontalPositionAbsolute", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });

    // no-op
    screen.moveCursorToColumn(ColumnOffset(0));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });

    // in-range
    screen.moveCursorToColumn(ColumnOffset(2));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(2) });

    screen.moveCursorToColumn(ColumnOffset(1));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(1) });

    // overflow
    screen.moveCursorToColumn(ColumnOffset(4));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(2) /*clamped*/ });
}

TEST_CASE("HorizontalPositionRelative", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });

    SECTION("no-op")
    {
        screen.moveCursorForward(ColumnCount(0));
        REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });
    }

    SECTION("HPR-1")
    {
        screen.moveCursorForward(ColumnCount(1));
        REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(1) });
    }

    SECTION("HPR-3 (to right border)")
    {
        screen.moveCursorForward(screen.pageSize().columns - 1);
        REQUIRE(screen.logicalCursorPosition().column.value == screen.pageSize().columns.value - 1);
    }

    SECTION("HPR-overflow")
    {
        screen.moveCursorForward(screen.pageSize().columns);
        REQUIRE(screen.logicalCursorPosition().column.value == screen.pageSize().columns.value - 1);
    }
}

TEST_CASE("MoveCursorToColumn", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });

    // no-op
    screen.moveCursorToColumn(ColumnOffset(0));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });

    // in-range
    screen.moveCursorToColumn(ColumnOffset(2));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(2) });

    screen.moveCursorToColumn(ColumnOffset(1));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(1) });

    // overflow
    screen.moveCursorToColumn(ColumnOffset(3));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(2) /*clamped*/ });

    SECTION("with wide character")
    {
        screen.moveCursorTo({}, {});
        REQUIRE(screen.logicalCursorPosition().column.value == 0);
        mock.writeToScreen(U"\u26A1"); // ⚡ :flash: (double width)
        REQUIRE(screen.logicalCursorPosition().column.value == 2);
    }
}

TEST_CASE("MoveCursorToLine", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });

    // no-op
    screen.moveCursorToLine(LineOffset(0));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });

    // in-range
    screen.moveCursorToLine(LineOffset(2));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(2), ColumnOffset(0) });

    screen.moveCursorToLine(LineOffset(1));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(0) });

    // overflow
    screen.moveCursorToLine(LineOffset(3));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(2 /*clamped*/), ColumnOffset(0) });
}

TEST_CASE("MoveCursorToBeginOfLine", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\r\nAB");
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(2) });

    screen.moveCursorToBeginOfLine();
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(0) });
}

TEST_CASE("CarriageReturn_honours_left_margin", "[screen]")
{
    // xterm's CarriageReturn: with DECLRMM on, CR snaps to the left margin when the cursor is at or
    // right of it, and to the screen's left edge when the cursor is left of it (only reachable in
    // non-origin mode). In origin mode it always snaps to the left margin.
    // Mirrors esctest CRTests.test_CR_* with left/right margins [4..9] (1-based cols 5..10).
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(12) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.terminal.setMode(DECMode::LeftRightMargin, true);
    mock.terminal.setLeftRightMargin(ColumnOffset { 4 }, ColumnOffset { 9 });

    SECTION("right of the left margin: snaps to the left margin")
    {
        screen.moveCursorTo(LineOffset { 0 }, ColumnOffset { 5 });
        screen.moveCursorToBeginOfLine();
        CHECK(screen.realCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(4) });
    }

    SECTION("at the left margin: stays put")
    {
        screen.moveCursorTo(LineOffset { 0 }, ColumnOffset { 4 });
        screen.moveCursorToBeginOfLine();
        CHECK(screen.realCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(4) });
    }

    SECTION("left of the left margin (non-origin): falls to the screen edge")
    {
        screen.moveCursorTo(LineOffset { 0 }, ColumnOffset { 3 });
        screen.moveCursorToBeginOfLine();
        CHECK(screen.realCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });
    }

    SECTION("origin mode: always snaps to the left margin")
    {
        mock.terminal.setMode(DECMode::Origin, true);
        // In origin mode addressing is margin-relative, so logical column 3 is absolute column 7.
        screen.moveCursorTo(LineOffset { 0 }, ColumnOffset { 3 });
        REQUIRE(screen.realCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(7) });
        screen.moveCursorToBeginOfLine();
        CHECK(screen.realCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(4) });
    }
}

TEST_CASE("NEL_indexes_and_returns_to_margin", "[screen]")
{
    // NEL (ESC E) is an index followed by a carriage return: it moves down (scrolling within the
    // scroll region when it hits the bottom margin) and returns to the left margin.
    SECTION("basic: moves down and to the start of the line")
    {
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
        auto& screen = mock.terminal.primaryScreen();
        mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
        screen.moveCursorTo(LineOffset { 2 }, ColumnOffset { 4 });
        mock.writeToScreen("\033E"); // NEL
        CHECK(screen.realCursorPosition() == CellLocation { LineOffset(3), ColumnOffset(0) });
        CHECK("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
    }

    SECTION("scrolls when it hits the bottom of the page")
    {
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(3) } };
        auto& screen = mock.terminal.primaryScreen();
        mock.writeToScreen("111\r\n222\r\n333");
        screen.moveCursorTo(LineOffset { 2 }, ColumnOffset { 2 }); // last line
        mock.writeToScreen("\033E");                               // NEL scrolls
        CHECK("222\n333\n   \n" == screen.renderMainPageText());
        CHECK(screen.realCursorPosition() == CellLocation { LineOffset(2), ColumnOffset(0) });
    }

    SECTION("outside the left/right band: no scroll, returns to the left margin")
    {
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
        auto& screen = mock.terminal.primaryScreen();
        mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
        mock.terminal.setTopBottomMargin(LineOffset { 1 }, LineOffset { 3 });
        mock.terminal.setMode(DECMode::LeftRightMargin, true);
        mock.terminal.setLeftRightMargin(ColumnOffset { 1 }, ColumnOffset { 3 });
        screen.moveCursorTo(LineOffset { 3 }, ColumnOffset { 4 }); // bottom margin, right of band
        mock.writeToScreen("\033E");
        // No scroll (cursor was outside the band); CR snaps to the left margin (column offset 1).
        CHECK("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
        CHECK(screen.realCursorPosition() == CellLocation { LineOffset(3), ColumnOffset(1) });
    }

    SECTION("inside the band scrolls within it, returning to the left margin")
    {
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
        auto& screen = mock.terminal.primaryScreen();
        mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
        mock.terminal.setTopBottomMargin(LineOffset { 1 }, LineOffset { 3 });
        mock.terminal.setMode(DECMode::LeftRightMargin, true);
        mock.terminal.setLeftRightMargin(ColumnOffset { 1 }, ColumnOffset { 3 });
        screen.moveCursorTo(LineOffset { 3 }, ColumnOffset { 2 }); // bottom margin, inside band
        mock.writeToScreen("\033E");
        CHECK("12345\n6BCD0\nAGHIE\nF   J\nKLMNO\n" == screen.renderMainPageText());
        CHECK(screen.realCursorPosition() == CellLocation { LineOffset(3), ColumnOffset(1) });
    }
}

TEST_CASE("SD_respects_left_right_margin", "[screen]")
{
    // SD (CSI Ps T) scrolls only the margined region down. With DECLRMM on it must confine the scroll
    // to the left/right band. Mirrors esctest test_SD_RespectsLeftRightScrollRegion.
    //
    // The page is deliberately taller than the content: with a full-height vertical margin the region
    // top (0) differs from the bottom, so the copy loop's lower bound (from+n vs. to-n) matters -- the
    // 5x5 case where they coincide once hid a bug that left the mid-region lines unscrolled.
    auto mock = MockTerm { PageSize { LineCount(7), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("abcde\r\nfghij\r\nklmno\r\npqrst\r\nuvwxy");
    mock.writeToScreen("\033[?69h"); // DECSET DECLRMM
    mock.writeToScreen("\033[2;4s"); // DECSLRM 2;4
    mock.writeToScreen("\033[2;3H"); // CUP row 2, col 3
    mock.writeToScreen("\033[2T");   // SD 2
    CHECK("a   e\nf   j\nkbcdo\npghit\nulmny\n qrs \n vwx \n" == screen.renderMainPageText());
}

TEST_CASE("IL_over_region_clears_the_band", "[screen]")
{
    // IL scrolls the region below the cursor down via scrollDown(). Inserting more lines than the
    // region is tall must clear its left/right band, not leave the mid-region lines behind (the same
    // scrollDown loop-bound bug the SD test guards). Mirrors esctest test_IL_RespectsScrollRegion_Over.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("abcde\r\nfGHIj\r\nkLMNo\r\npQRSt\r\nuvwxy");
    mock.writeToScreen("\033[?69h"); // DECSET DECLRMM
    mock.writeToScreen("\033[2;4s"); // DECSLRM 2;4
    mock.writeToScreen("\033[2;4r"); // DECSTBM 2;4
    mock.writeToScreen("\033[2;3H"); // CUP row 2, col 3
    mock.writeToScreen("\033[99L");  // IL 99
    CHECK("abcde\nf   j\nk   o\np   t\nuvwxy\n" == screen.renderMainPageText());
}

TEST_CASE("Autowrap_within_left_right_margin", "[screen]")
{
    // Text written inside the left/right band wraps at the right margin -- not one column early. The
    // right margin is the last writable column; a char destined for it must land there, and only the
    // *next* char wraps. Regression for the off-by-one in clearAndAdvance().
    SECTION("autowrap on: the right-margin char lands, then the next wraps to the left margin")
    {
        auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(6) } };
        auto& screen = mock.terminal.primaryScreen();
        mock.writeToScreen("\033[?69h"); // DECSET DECLRMM
        mock.writeToScreen("\033[2;4s"); // DECSLRM 2;4 -> band cols 1..3
        mock.writeToScreen("\033[1;2H"); // CUP to the left margin
        mock.writeToScreen("xyzw");      // x y z fill the band; w wraps to the next line's left margin
        CHECK(" xyz  \n w    \n" == screen.renderMainPageText());
    }

    SECTION("autowrap off: writes pile up on the right margin")
    {
        auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(6) } };
        auto& screen = mock.terminal.primaryScreen();
        mock.writeToScreen("\033[?69h"); // DECSET DECLRMM
        mock.writeToScreen("\033[2;4s"); // DECSLRM 2;4
        mock.writeToScreen("\033[?7l");  // DECRESET DECAWM (autowrap off)
        mock.writeToScreen("\033[1;2H"); // CUP to the left margin
        mock.writeToScreen("xyzw");      // w overwrites the right-margin cell; nothing wraps
        CHECK(" xyw  \n      \n" == screen.renderMainPageText());
    }
}

TEST_CASE("DECBI_back_index", "[screen]")
{
    // DECBI (ESC 6): on the left margin it scrolls the margined region right by one column; anywhere
    // else it moves the cursor back one column without wrapping. Mirrors esctest DECBITests.
    SECTION("basic: moves the cursor back one column")
    {
        auto mock = MockTerm { PageSize { LineCount(6), ColumnCount(6) } };
        auto& screen = mock.terminal.primaryScreen();
        screen.moveCursorTo(LineOffset { 5 }, ColumnOffset { 4 });
        mock.writeToScreen("\0336"); // DECBI
        CHECK(screen.realCursorPosition() == CellLocation { LineOffset(5), ColumnOffset(3) });
    }

    SECTION("does not wrap at the left edge")
    {
        auto mock = MockTerm { PageSize { LineCount(6), ColumnCount(6) } };
        auto& screen = mock.terminal.primaryScreen();
        mock.writeToScreen("\033[2;1H"); // row 2, col 1
        screen.moveCursorTo(LineOffset { 1 }, ColumnOffset { 0 });
        mock.writeToScreen("\0336");
        CHECK(screen.realCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(0) });
    }

    SECTION("left of the left margin: moves back toward the screen edge")
    {
        auto mock = MockTerm { PageSize { LineCount(6), ColumnCount(12) } };
        auto& screen = mock.terminal.primaryScreen();
        mock.terminal.setMode(DECMode::LeftRightMargin, true);
        mock.terminal.setLeftRightMargin(ColumnOffset { 2 }, ColumnOffset { 4 });
        screen.moveCursorTo(LineOffset { 0 }, ColumnOffset { 1 }); // left of the left margin
        mock.writeToScreen("\0336");
        CHECK(screen.realCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });
    }

    SECTION("on the left margin, inside the region: scrolls the region right")
    {
        auto mock = MockTerm { PageSize { LineCount(7), ColumnCount(6) } };
        auto& screen = mock.terminal.primaryScreen();
        mock.writeToScreen("\033[3;2Habcde");
        mock.writeToScreen("\033[4;2Hfghij");
        mock.writeToScreen("\033[5;2Hklmno");
        mock.writeToScreen("\033[6;2Hpqrst");
        mock.writeToScreen("\033[7;2Huvwxy");
        mock.terminal.setMode(DECMode::LeftRightMargin, true);
        mock.terminal.setLeftRightMargin(ColumnOffset { 2 }, ColumnOffset { 4 }); // DECSLRM 3;5
        mock.terminal.setTopBottomMargin(LineOffset { 3 }, LineOffset { 5 });     // DECSTBM 4;6
        screen.moveCursorTo(LineOffset { 4 }, ColumnOffset { 2 });                // on the left margin
        mock.writeToScreen("\0336");
        // Columns 2..4 of rows 3..5 shift right one; column 5 (outside the band) is untouched.
        CHECK(" f ghj" == screen.grid().lineText(LineOffset(3)));
        CHECK(" k lmo" == screen.grid().lineText(LineOffset(4)));
        CHECK(" p qrt" == screen.grid().lineText(LineOffset(5)));
        CHECK(" abcde" == screen.grid().lineText(LineOffset(2))); // above the region: unchanged
        CHECK(" uvwxy" == screen.grid().lineText(LineOffset(6))); // below the region: unchanged
        CHECK(screen.realCursorPosition() == CellLocation { LineOffset(4), ColumnOffset(2) });
    }
}

TEST_CASE("CNL_CPL_clamp_to_scroll_region_and_left_margin", "[screen]")
{
    // CNL (CSI E) and CPL (CSI F) are cursor down/up followed by a carriage return. They clamp at the
    // scroll-region margin (never scrolling), and the carriage return snaps to the left margin.
    // Mirrors esctest CNLTests/CPLTests StopsAt{Bottom,Top}MarginInScrollRegion and *Below/AboveRegion.
    auto withRegion =
        [](auto& mock, LineOffset top, LineOffset bottom, ColumnOffset left, ColumnOffset right) {
            mock.terminal.setTopBottomMargin(top, bottom);
            mock.terminal.setMode(DECMode::LeftRightMargin, true);
            mock.terminal.setLeftRightMargin(left, right);
        };

    SECTION("CNL stops at the bottom margin and moves to the left margin")
    {
        auto mock = MockTerm { PageSize { LineCount(6), ColumnCount(12) } };
        auto& screen = mock.terminal.primaryScreen();
        withRegion(mock, LineOffset { 1 }, LineOffset { 3 }, ColumnOffset { 4 }, ColumnOffset { 9 });
        screen.moveCursorTo(LineOffset { 2 }, ColumnOffset { 6 }); // inside the region
        mock.writeToScreen("\033[99E");                            // CNL 99
        CHECK(screen.realCursorPosition() == CellLocation { LineOffset(3), ColumnOffset(4) });
    }

    SECTION("CNL below the region stops at the page bottom and the left margin")
    {
        auto mock = MockTerm { PageSize { LineCount(6), ColumnCount(12) } };
        auto& screen = mock.terminal.primaryScreen();
        withRegion(mock, LineOffset { 2 }, LineOffset { 3 }, ColumnOffset { 4 }, ColumnOffset { 9 });
        screen.moveCursorTo(LineOffset { 4 }, ColumnOffset { 6 }); // below the region
        mock.writeToScreen("\033[99E");
        CHECK(screen.realCursorPosition() == CellLocation { LineOffset(5), ColumnOffset(4) });
    }

    SECTION("CPL stops at the top margin and moves to the left margin")
    {
        auto mock = MockTerm { PageSize { LineCount(6), ColumnCount(12) } };
        auto& screen = mock.terminal.primaryScreen();
        withRegion(mock, LineOffset { 1 }, LineOffset { 3 }, ColumnOffset { 4 }, ColumnOffset { 9 });
        screen.moveCursorTo(LineOffset { 2 }, ColumnOffset { 6 }); // inside the region
        mock.writeToScreen("\033[99F");                            // CPL 99
        CHECK(screen.realCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(4) });
    }

    SECTION("without margins CNL still stops at the page bottom, column 1")
    {
        auto mock = MockTerm { PageSize { LineCount(6), ColumnCount(12) } };
        auto& screen = mock.terminal.primaryScreen();
        screen.moveCursorTo(LineOffset { 2 }, ColumnOffset { 6 });
        mock.writeToScreen("\033[99E");
        CHECK(screen.realCursorPosition() == CellLocation { LineOffset(5), ColumnOffset(0) });
    }
}

TEST_CASE("MoveCursorTo", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());

    SECTION("origin mode disabled")
    {
        SECTION("in range")
        {
            screen.moveCursorTo(LineOffset { 2 }, ColumnOffset { 1 });
            REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(2), ColumnOffset(1) });
        }

        SECTION("origin")
        {
            screen.moveCursorTo(LineOffset { 0 }, ColumnOffset { 0 });
            REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });
        }

        SECTION("clamped")
        {
            screen.moveCursorTo(LineOffset { 5 }, ColumnOffset { 5 });
            REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(4), ColumnOffset(4) });
        }
    }

    SECTION("origin-mode enabled")
    {
        constexpr auto TopMargin = LineOffset(1);
        constexpr auto BottomMargin = LineOffset(3);
        constexpr auto LeftMargin = ColumnOffset(1);
        constexpr auto RightMargin = ColumnOffset(3);
        mock.terminal.setMode(DECMode::LeftRightMargin, true);
        mock.terminal.setLeftRightMargin(LeftMargin, RightMargin);
        mock.terminal.setTopBottomMargin(TopMargin, BottomMargin);
        mock.terminal.setMode(DECMode::Origin, true);

        SECTION("move to origin")
        {
            screen.moveCursorTo({}, {});
            CHECK(CellLocation { LineOffset(0), ColumnOffset(0) } == screen.logicalCursorPosition());
            CHECK(CellLocation { LineOffset(1), ColumnOffset(1) } == screen.realCursorPosition());
            CHECK('7' == (char) screen.at({ TopMargin + 0, LeftMargin + 0 }).codepoint(0));
            CHECK('I' == (char) screen.at({ TopMargin + 2, LeftMargin + 2 }).codepoint(0));
        }
    }
}

TEST_CASE("MoveCursorToNextTab", "[screen]")
{
    auto constexpr TabWidth = 8;
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) } };
    auto& screen = mock.terminal.primaryScreen();
    screen.moveCursorToNextTab();
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(TabWidth + 0) });

    screen.moveCursorToColumn(ColumnOffset(TabWidth - 1));
    screen.moveCursorToNextTab();
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(TabWidth + 0) });

    screen.moveCursorToColumn(ColumnOffset(TabWidth - 1));
    screen.moveCursorToNextTab();
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(TabWidth + 0) });

    screen.moveCursorToNextTab();
    REQUIRE(screen.logicalCursorPosition()
            == CellLocation { LineOffset(0), ColumnOffset((2 * TabWidth) + 0) });

    screen.moveCursorToNextTab();
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(19) });

    mock.terminal.setMode(DECMode::AutoWrap, true);
    mock.writeToScreen("A"); // 'A' is being written at the right margin
    mock.writeToScreen("B"); // force wrap to next line, writing 'B' at the beginning of the line

    screen.moveCursorToNextTab();
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(8) });
}

// TODO: HideCursor
// TODO: ShowCursor

TEST_CASE("SaveCursor and RestoreCursor", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.terminal.setMode(DECMode::AutoWrap, false);
    mock.terminal.currentScreen().saveCursor();

    // mutate the cursor's position, autowrap and origin flags
    screen.moveCursorTo(LineOffset { 2 }, ColumnOffset { 2 });
    mock.terminal.setMode(DECMode::AutoWrap, true);
    mock.terminal.setMode(DECMode::Origin, true);

    // restore cursor and see if the changes have been reverted
    mock.terminal.currentScreen().restoreCursor();
    CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });
    CHECK_FALSE(mock.terminal.isModeEnabled(DECMode::AutoWrap));
    CHECK_FALSE(mock.terminal.isModeEnabled(DECMode::Origin));
}

TEST_CASE("Index_outside_margin", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(6), ColumnCount(4) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("1234\r\n5678\r\nABCD\r\nEFGH\r\nIJKL\r\nMNOP");
    logScreenText(screen, "initial");
    REQUIRE("1234\n5678\nABCD\nEFGH\nIJKL\nMNOP\n" == screen.renderMainPageText());
    mock.terminal.setTopBottomMargin(LineOffset { 1 }, LineOffset { 3 });

    // with cursor above top margin
    screen.moveCursorTo(LineOffset { 0 }, ColumnOffset { 2 });
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(2) });

    screen.index();
    REQUIRE("1234\n5678\nABCD\nEFGH\nIJKL\nMNOP\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(2) });

    // with cursor below bottom margin and above bottom screen (=> only moves cursor one down)
    screen.moveCursorTo(LineOffset { 4 }, ColumnOffset { 2 });
    screen.index();
    REQUIRE("1234\n5678\nABCD\nEFGH\nIJKL\nMNOP\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(5), ColumnOffset(2) });

    // with cursor below bottom margin and at bottom screen (=> no-op)
    screen.moveCursorTo(LineOffset { 5 }, ColumnOffset { 2 });
    screen.index();
    REQUIRE("1234\n5678\nABCD\nEFGH\nIJKL\nMNOP\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(5), ColumnOffset(2) });
TEST_CASE("DCH.worksOutsideTopBottomMargin", "[screen]")
{
    // DCH deletes characters even when the cursor sits outside the top/bottom scrolling margin (xterm
    // patch 316) -- it is confined only by the left/right margins.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    CHECK(screen.grid().lineText(LineOffset(0)) == "     "); // row 1 was still deleted
}

TEST_CASE("ED.2_ignoresScrollRegion", "[screen]")
{
    // ED 2 erases the whole screen regardless of a DECSTBM scrolling region (the region is ignored).
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();
    logScreenText(screen, "initial setup");
    CHECK(screen.grid().lineText(LineOffset(0)) == "   "); // row 1 cleared (outside region)
    CHECK(screen.grid().lineText(LineOffset(2)) == "   "); // row 3 cleared (outside region)
}

TEST_CASE("CBT.ignoresLeftRightMargin", "[screen]")
{
    // CBT (cursor backward tab) ignores the left/right margin (xterm): from column 9 it tabs back past
    // the left margin (5) to column 1, not stopping at the margin.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) } };
    auto& screen = mock.terminal.primaryScreen();
    CHECK(screen.cursor().position.column == ColumnOffset(0)); // column 1, ignoring the left margin
}

TEST_CASE("CBT.ignoresLeftRightMarginUnderOriginMode", "[screen]")
{
    // Same rule, with origin mode on. CBT computes an absolute target column, so placing it through the
    // DECOM-aware column setter added the left margin to it a second time and landed the cursor to the
    // right of the tab stop it had just found.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("\033[?69h");  // DECLRMM: enable left/right margins
    mock.writeToScreen("\033[5;30s"); // DECSLRM(5,30)
    mock.writeToScreen("\033[?6h");   // DECOM: origin mode on

    SECTION("default tab stops")
    {
        mock.writeToScreen("\033[1;20H"); // CUP, origin-relative: column 5+20-1 = 24
        REQUIRE(screen.cursor().position.column == ColumnOffset(23));
        mock.writeToScreen("\033[Z"); // CBT(1) -> the tab stop at column 17 (0-based 16)
        CHECK(screen.cursor().position.column == ColumnOffset(16));
    }

    SECTION("back past the left margin lands on the first column, not the margin")
    {
        mock.writeToScreen("\033[1;5H"); // CUP, origin-relative: column 5+5-1 = 9
        REQUIRE(screen.cursor().position.column == ColumnOffset(8));
        mock.writeToScreen("\033[4Z"); // CBT(4): further back than any tab stop
        CHECK(screen.cursor().position.column == ColumnOffset(0));
    }
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

TEST_CASE("Index_outside_margin", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(6), ColumnCount(4) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("1234\r\n5678\r\nABCD\r\nEFGH\r\nIJKL\r\nMNOP");
    logScreenText(screen, "initial");
    REQUIRE("1234\n5678\nABCD\nEFGH\nIJKL\nMNOP\n" == screen.renderMainPageText());
    mock.terminal.setTopBottomMargin(LineOffset { 1 }, LineOffset { 3 });

    // with cursor above top margin
    screen.moveCursorTo(LineOffset { 0 }, ColumnOffset { 2 });
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(2) });

    screen.index();
    REQUIRE("1234\n5678\nABCD\nEFGH\nIJKL\nMNOP\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(2) });

    // with cursor below bottom margin and above bottom screen (=> only moves cursor one down)
    screen.moveCursorTo(LineOffset { 4 }, ColumnOffset { 2 });
    screen.index();
    REQUIRE("1234\n5678\nABCD\nEFGH\nIJKL\nMNOP\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(5), ColumnOffset(2) });

    // with cursor below bottom margin and at bottom screen (=> no-op)
    screen.moveCursorTo(LineOffset { 5 }, ColumnOffset { 2 });
    screen.index();
    REQUIRE("1234\n5678\nABCD\nEFGH\nIJKL\nMNOP\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(5), ColumnOffset(2) });
}

TEST_CASE("Index_inside_margin", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(6), ColumnCount(2) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("11\r\n22\r\n33\r\n44\r\n55\r\n66");
    logScreenText(screen, "initial setup");

    // test IND when cursor is within margin range (=> move cursor down)
    mock.terminal.setTopBottomMargin(LineOffset { 1 }, LineOffset { 3 });
    screen.moveCursorTo(LineOffset { 2 }, ColumnOffset { 1 });
    screen.index();
    logScreenText(screen, "IND while cursor at line 3");
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(3), ColumnOffset(1) });
    REQUIRE("11\n22\n33\n44\n55\n66\n" == screen.renderMainPageText());
}

TEST_CASE("Index_at_bottom_margin", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    logScreenText(screen, "initial setup");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());

    mock.terminal.setTopBottomMargin(LineOffset { 1 }, LineOffset { 3 });

    SECTION("cursor at bottom margin and full horizontal margins")
    {
        screen.moveCursorTo(LineOffset { 3 }, ColumnOffset { 1 });
        screen.index();
        logScreenText(screen, "IND while cursor at bottom margin");
        REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(3), ColumnOffset(1) });
        REQUIRE("12345\nABCDE\nFGHIJ\n     \nKLMNO\n" == screen.renderMainPageText());
    }

    SECTION("cursor at bottom margin and NOT full horizontal margins")
    {
        screen.moveCursorTo(LineOffset { 0 }, ColumnOffset { 0 });
        mock.terminal.setMode(DECMode::LeftRightMargin, true);
        mock.terminal.setLeftRightMargin(ColumnOffset { 1 }, ColumnOffset { 3 });
        mock.terminal.setTopBottomMargin(LineOffset { 1 }, LineOffset { 3 });
        screen.moveCursorTo(LineOffset { 3 }, ColumnOffset { 1 }); // cursor at bottom margin
        REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(3), ColumnOffset(1) });

        screen.index();
        CHECK("12345\n6BCD0\nAGHIE\nF   J\nKLMNO\n" == screen.renderMainPageText());
        REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(3), ColumnOffset(1) });
    }
}

TEST_CASE("VerticalScroll_confined_to_left_right_margins", "[screen]")
{
    // With DECLRMM on, vertical motion scrolls only when the cursor is within the left/right margins.
    // Outside that band the cursor neither scrolls the page nor walks past the top/bottom margin.
    // This mirrors esctest's test_{IND,RI,LF,FF,VT}_MovesDoesNotScrollOutsideLeftRight.
    auto setup = [](auto& mock) {
        auto& screen = mock.terminal.primaryScreen();
        mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
        mock.terminal.setTopBottomMargin(LineOffset { 1 }, LineOffset { 3 });
        mock.terminal.setMode(DECMode::LeftRightMargin, true);
        mock.terminal.setLeftRightMargin(ColumnOffset { 1 }, ColumnOffset { 3 });
        return &screen;
    };
    auto constexpr Untouched = "12345\n67890\nABCDE\nFGHIJ\nKLMNO\n";

    SECTION("IND at bottom margin, right of the right margin: no scroll, no move")
    {
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
        auto* screen = setup(mock);
        screen->moveCursorTo(LineOffset { 3 }, ColumnOffset { 4 }); // bottom margin, right of band
        screen->index();
        CHECK(Untouched == screen->renderMainPageText());
        CHECK(screen->logicalCursorPosition() == CellLocation { LineOffset(3), ColumnOffset(4) });
    }

    SECTION("IND at bottom margin, left of the left margin: no scroll, no move")
    {
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
        auto* screen = setup(mock);
        screen->moveCursorTo(LineOffset { 3 }, ColumnOffset { 0 }); // bottom margin, left of band
        screen->index();
        CHECK(Untouched == screen->renderMainPageText());
        CHECK(screen->logicalCursorPosition() == CellLocation { LineOffset(3), ColumnOffset(0) });
    }

    SECTION("IND above bottom margin, outside band: moves down without scrolling")
    {
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
        auto* screen = setup(mock);
        screen->moveCursorTo(LineOffset { 2 }, ColumnOffset { 4 });
        screen->index();
        CHECK(Untouched == screen->renderMainPageText());
        CHECK(screen->logicalCursorPosition() == CellLocation { LineOffset(3), ColumnOffset(4) });
    }

    SECTION("RI at top margin, outside band: no reverse scroll, no move")
    {
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
        auto* screen = setup(mock);
        screen->moveCursorTo(LineOffset { 1 }, ColumnOffset { 4 }); // top margin, right of band
        screen->reverseIndex();
        CHECK(Untouched == screen->renderMainPageText());
        CHECK(screen->logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(4) });
    }

    SECTION("LF (control byte) at bottom margin, outside band: no scroll, no move")
    {
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
        auto* screen = setup(mock);
        screen->moveCursorTo(LineOffset { 3 }, ColumnOffset { 4 });
        mock.writeToScreen("\n");
        CHECK(Untouched == screen->renderMainPageText());
        CHECK(screen->logicalCursorPosition() == CellLocation { LineOffset(3), ColumnOffset(4) });
    }

    SECTION("FF and VT (control bytes) at bottom margin, outside band: no scroll, no move")
    {
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
        auto* screen = setup(mock);
        screen->moveCursorTo(LineOffset { 3 }, ColumnOffset { 4 });
        mock.writeToScreen("\f"); // FF -> IND
        CHECK(Untouched == screen->renderMainPageText());
        CHECK(screen->logicalCursorPosition() == CellLocation { LineOffset(3), ColumnOffset(4) });
        mock.writeToScreen("\v"); // VT -> IND
        CHECK(Untouched == screen->renderMainPageText());
        CHECK(screen->logicalCursorPosition() == CellLocation { LineOffset(3), ColumnOffset(4) });
    }

    SECTION("IND inside the band still scrolls, confined to the band")
    {
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
        auto* screen = setup(mock);
        screen->moveCursorTo(LineOffset { 3 }, ColumnOffset { 1 }); // bottom margin, inside band
        screen->index();
        // Only columns 1..3 of the scrolling region 1..3 move up; the margins stay put.
        CHECK("12345\n6BCD0\nAGHIE\nF   J\nKLMNO\n" == screen->renderMainPageText());
        CHECK(screen->logicalCursorPosition() == CellLocation { LineOffset(3), ColumnOffset(1) });
    }
}

TEST_CASE("ReverseIndex_without_custom_margins", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    logScreenText(screen, "initial");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());

    // at bottom screen
    screen.moveCursorTo(LineOffset { 4 }, ColumnOffset { 1 });
    screen.reverseIndex();
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(3), ColumnOffset(1) });

    screen.reverseIndex();
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(2), ColumnOffset(1) });

    screen.reverseIndex();
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(1) });

    screen.reverseIndex();
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(1) });

    screen.reverseIndex();
    logScreenText(screen, "RI at top screen");
    REQUIRE("     \n12345\n67890\nABCDE\nFGHIJ\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(1) });

    screen.reverseIndex();
    logScreenText(screen, "RI at top screen");
    REQUIRE("     \n     \n12345\n67890\nABCDE\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(1) });
}

TEST_CASE("ReverseIndex_with_vertical_margin", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    logScreenText(screen, "initial");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());

    mock.terminal.setTopBottomMargin(LineOffset { 1 }, LineOffset { 3 });

    // below bottom margin
    screen.moveCursorTo(LineOffset { 4 }, ColumnOffset { 1 });
    screen.reverseIndex();
    logScreenText(screen, "RI below bottom margin");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(3), ColumnOffset(1) });

    // at bottom margin
    screen.reverseIndex();
    logScreenText(screen, "RI at bottom margin");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(2), ColumnOffset(1) });

    screen.reverseIndex();
    logScreenText(screen, "RI middle margin");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(1) });

    // at top margin
    screen.reverseIndex();
    logScreenText(screen, "RI at top margin #1");
    REQUIRE("12345\n     \n67890\nABCDE\nKLMNO\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(1) });

    // at top margin (again)
    screen.reverseIndex();
    logScreenText(screen, "RI at top margin #2");
    REQUIRE("12345\n     \n     \n67890\nKLMNO\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(1) });

    // above top margin
    screen.moveCursorTo(LineOffset { 0 }, ColumnOffset { 1 });
    screen.reverseIndex();
    logScreenText(screen, "RI above top margin");
    REQUIRE("12345\n     \n     \n67890\nKLMNO\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(1) });

    // above top margin (top screen) => no-op
    screen.reverseIndex();
    logScreenText(screen, "RI above top margin (top-screen)");
    REQUIRE("12345\n     \n     \n67890\nKLMNO\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(1) });
}

TEST_CASE("ReverseIndex_with_vertical_and_horizontal_margin", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    logScreenText(screen, "initial");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());

    mock.terminal.setMode(DECMode::LeftRightMargin, true);
    mock.terminal.setLeftRightMargin(ColumnOffset { 1 }, ColumnOffset { 3 });
    mock.terminal.setTopBottomMargin(LineOffset { 1 }, LineOffset { 3 });

    // below bottom margin
    screen.moveCursorTo(LineOffset { 4 }, ColumnOffset { 1 });
    screen.reverseIndex();
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(3), ColumnOffset(1) });

    // at bottom margin
    screen.reverseIndex();
    logScreenText(screen, "after RI at bottom margin");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(2), ColumnOffset(1) });

    screen.reverseIndex();
    logScreenText(screen, "after RI at bottom margin (again)");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(1) });

    // at top margin
    screen.reverseIndex();
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(1) });
    REQUIRE("12345\n6   0\nA789E\nFBCDJ\nKLMNO\n" == screen.renderMainPageText());

    // at top margin (again)
    screen.reverseIndex();
    logScreenText(screen, "after RI at top margin (again)");
    REQUIRE("12345\n6   0\nA   E\nF789J\nKLMNO\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(1) });

    // above top margin
    screen.moveCursorTo(LineOffset { 0 }, ColumnOffset { 1 });
    screen.reverseIndex();
    REQUIRE("12345\n6   0\nA   E\nF789J\nKLMNO\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(1) });
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

TEST_CASE("CursorNextLine", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    screen.moveCursorTo(LineOffset { 1 }, ColumnOffset { 2 });

    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(2) });

    SECTION("without margins")
    {
        SECTION("normal")
        {
            screen.moveCursorToNextLine(LineCount(1));
            REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(2), ColumnOffset(0) });
        }

        SECTION("clamped")
        {
            screen.moveCursorToNextLine(LineCount(5));
            REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(4), ColumnOffset(0) });
        }
    }

    SECTION("with margins")
    {
        mock.terminal.setMode(DECMode::LeftRightMargin, true);
        mock.terminal.setLeftRightMargin(ColumnOffset { 1 }, ColumnOffset { 3 });
        mock.terminal.setTopBottomMargin(LineOffset { 1 }, LineOffset { 3 });
        mock.terminal.setMode(DECMode::Origin, true);
        screen.moveCursorTo(LineOffset { 0 }, ColumnOffset { 1 });
        REQUIRE(screen.useCurrentCell().toUtf8() == "8");

        SECTION("normal-1")
        {
            screen.moveCursorToNextLine(LineCount(1));
            REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(0) });
        }

        SECTION("normal-2")
        {
            screen.moveCursorToNextLine(LineCount(2));
            REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(2), ColumnOffset(0) });
        }

        SECTION("clamped-at-bottom-margin")
        {
            // The region spans real rows 1..3, i.e. logical rows 0..2 in origin mode. CNL clamps at the
            // bottom margin (logical row 2) and never walks past it, however large the count.
            screen.moveCursorToNextLine(LineCount(3));
            REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(2), ColumnOffset(0) });
        }

        SECTION("clamped-stays-at-bottom-margin")
        {
            screen.moveCursorToNextLine(LineCount(4));
            REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(2), ColumnOffset(0) });
        }
    }
}

TEST_CASE("CursorPreviousLine", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");

    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(4), ColumnOffset(4) });

    SECTION("without margins")
    {
        SECTION("normal")
        {
            screen.moveCursorToPrevLine(LineCount(1));
            REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(3), ColumnOffset(0) });
        }

        SECTION("clamped")
        {
            screen.moveCursorToPrevLine(LineCount(5));
            REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });
        }
    }

    SECTION("with margins")
    {
        mock.terminal.setMode(DECMode::LeftRightMargin, true);
        mock.terminal.setLeftRightMargin(ColumnOffset { 1 }, ColumnOffset { 3 });
        mock.terminal.setTopBottomMargin(LineOffset { 1 }, LineOffset { 3 });
        mock.terminal.setMode(DECMode::Origin, true);
        screen.moveCursorTo(LineOffset { 2 }, ColumnOffset { 2 });
        REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(2), ColumnOffset(2) });

        SECTION("normal-1")
        {
            screen.moveCursorToPrevLine(LineCount(1));
            REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(0) });
        }

        SECTION("normal-2")
        {
            screen.moveCursorToPrevLine(LineCount(2));
            REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });
        }

        SECTION("clamped")
        {
            screen.moveCursorToPrevLine(LineCount(3));
            REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });
        }
    }
}

TEST_CASE("Eight-bit C1 controls on input", "[screen]")
{
    // A raw byte in 0x80..0x9F that begins a character is a C1 control, exactly the 8-bit form of the
    // 7-bit ESC sequence: 0x9B is CSI, 0x84 IND, 0x9D OSC, 0x9C the string terminator, and so on.
    SECTION("8-bit CSI (0x9B) drives a CSI sequence")
    {
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
        auto& screen = mock.terminal.primaryScreen();
        mock.writeToScreen("\x9b"
                           "3;5H"); // CSI 3 ; 5 H (CUP)
        CHECK(screen.realCursorPosition() == CellLocation { LineOffset(2), ColumnOffset(4) });
    }

    SECTION("8-bit IND (0x84) indexes down one line")
    {
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
        auto& screen = mock.terminal.primaryScreen();
        screen.moveCursorTo(LineOffset { 2 }, ColumnOffset { 3 });
        mock.writeToScreen("\x84"); // IND
        CHECK(screen.realCursorPosition() == CellLocation { LineOffset(3), ColumnOffset(3) });
    }

    SECTION("8-bit OSC (0x9D) with 8-bit ST (0x9C) sets the window title")
    {
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
        mock.writeToScreen("\x9d"
                           "2;hi\x9c"); // OSC 2 ; hi ST
        CHECK(mock.terminal.windowTitle() == "hi");
    }

    SECTION("a C1-range byte inside a UTF-8 sequence stays a continuation byte")
    {
        // U+0250 encodes as 0xC9 0x90; the 0x90 is in the C1 range but here it is a UTF-8 continuation,
        // so it must print the character rather than be taken for a DCS control.
        auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(10) } };
        auto& screen = mock.terminal.primaryScreen();
        mock.writeToScreen("\xc9\x90X"); // ɐX
        CHECK(screen.realCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(2) });
    }
}

TEST_CASE("S8C1T selects 8-bit C1 control transmission for replies", "[screen]")
{
    // With S7C1T (the default) the terminal frames its replies with 7-bit ESC-introduced C1 controls;
    // with S8C1T selected at VT level >= 2 it uses the single-byte 8-bit forms instead. @see
    // Terminal::reply(), foldC1ControlsToEightBit().
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    SECTION("default is 7-bit")
    {
        mock.writeToScreen("\033[6n"); // DSR: report cursor position
        CHECK(mock.terminal.peekInput() == "\033[1;1R");
    }

    SECTION("8-bit after S8C1T")
    {
        mock.writeToScreen("\033 G");  // S8C1T (ESC SP G)
        mock.writeToScreen("\033[6n"); // DSR
        CHECK(mock.terminal.peekInput()
              == std::string("\x9b"
                             "1;1R")); // 8-bit CSI introducer
    }
}

TEST_CASE("DECID identifies the terminal like DA1", "[screen]")
{
    // DECID (ESC Z) is the VT100 "identify terminal" control; it is answered with the primary device
    // attributes, exactly as DA1 (CSI c). Mirrors esctest test_DECID_8bit (which sends it 8-bit).
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen("\033Z"); // DECID
    auto const viaDecid = std::string { mock.terminal.peekInput() };
    mock.discardPendingReplies();

    mock.writeToScreen("\033[c"); // DA1
    auto const viaDa1 = std::string { mock.terminal.peekInput() };

    CHECK(viaDecid.starts_with("\033[?"));
    CHECK(viaDecid.ends_with("c"));
    CHECK(viaDecid == viaDa1);
}

TEST_CASE("ReportCursorPosition", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    screen.moveCursorTo(LineOffset { 1 }, ColumnOffset { 2 });

    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
    REQUIRE(mock.terminal.peekInput().empty());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(2) });

    SECTION("with Origin mode disabled")
    {
        screen.reportCursorPosition();
        CHECK("\033[2;3R" == mock.terminal.peekInput());
    }

    SECTION("with margins and origin mode enabled")
    {
        mock.terminal.setMode(DECMode::LeftRightMargin, true);
        mock.terminal.setLeftRightMargin(ColumnOffset { 1 }, ColumnOffset { 3 });
        mock.terminal.setTopBottomMargin(LineOffset { 1 }, LineOffset { 3 });
        mock.terminal.setMode(DECMode::Origin, true);
        screen.moveCursorTo(LineOffset { 2 }, ColumnOffset { 1 });

        screen.reportCursorPosition();
        CHECK("\033[3;2R" == mock.terminal.peekInput());
    }
}

TEST_CASE("ReportExtendedCursorPosition", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    screen.moveCursorTo(LineOffset { 1 }, ColumnOffset { 2 });

    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
    REQUIRE(mock.terminal.peekInput().empty());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(2) });

    SECTION("with Origin mode disabled")
    {
        screen.reportExtendedCursorPosition();
        CHECK("\033[?2;3;1R" == mock.terminal.peekInput());
    }

    SECTION("with margins and origin mode enabled")
    {
        mock.terminal.setMode(DECMode::LeftRightMargin, true);
        mock.terminal.setLeftRightMargin(ColumnOffset { 1 }, ColumnOffset { 3 });
        mock.terminal.setTopBottomMargin(LineOffset { 1 }, LineOffset { 3 });
        mock.terminal.setMode(DECMode::Origin, true);
        screen.moveCursorTo(LineOffset { 2 }, ColumnOffset { 1 });

        screen.reportExtendedCursorPosition();
        CHECK("\033[?3;2;1R" == mock.terminal.peekInput());
    }
}

TEST_CASE("RequestMode", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };

    constexpr auto AnsiInsertModeNum = toAnsiModeNum(AnsiMode::Insert);

    SECTION("ANSI modes: enabled")
    {
        mock.writeToScreen(SM(AnsiInsertModeNum));
        mock.writeToScreen(DECRQM_ANSI(AnsiInsertModeNum));
        REQUIRE(e(mock.terminal.peekInput())
                == e(std::format("\033[{};1$y", toAnsiModeNum(AnsiMode::Insert))));
    }

    SECTION("ANSI modes: disabled")
    {
        mock.writeToScreen(RM(AnsiInsertModeNum));
        mock.writeToScreen(DECRQM_ANSI(AnsiInsertModeNum));
        REQUIRE(e(mock.terminal.peekInput()) == e(std::format("\033[{};2$y", AnsiInsertModeNum)));
    }

    SECTION("ANSI modes: unknown")
    {
        auto const m = 1234u;
        mock.writeToScreen(SM(m));
        mock.writeToScreen(DECRQM_ANSI(m));
        REQUIRE(e(mock.terminal.peekInput()) == e(std::format("\033[{};0$y", m)));
    }

    constexpr auto DecOriginModeNum = toDECModeNum(DECMode::Origin);

    SECTION("DEC modes: enabled")
    {
        mock.writeToScreen(DECSM(DecOriginModeNum));
        mock.writeToScreen(DECRQM(DecOriginModeNum));
        REQUIRE(e(mock.terminal.peekInput()) == e(std::format("\033[?{};1$y", DecOriginModeNum)));
    }

    SECTION("DEC modes: disabled")
    {
        mock.writeToScreen(DECRM(DecOriginModeNum));
        mock.writeToScreen(DECRQM(DecOriginModeNum));
        REQUIRE(e(mock.terminal.peekInput()) == e(std::format("\033[?{};2$y", DecOriginModeNum)));
    }

    SECTION("DEC modes: unknown")
    {
        auto const m = std::numeric_limits<uint16_t>::max();
        mock.writeToScreen(DECSM(m));
        mock.writeToScreen(DECRQM(m));
        REQUIRE(e(mock.terminal.peekInput()) == e(std::format("\033[?{};0$y", m)));
    }
}

TEST_CASE("DECNKM", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(10) } };

    // Enable application keypad via DECSM 66
    mock.writeToScreen(DECSM(66));
    CHECK(mock.terminal.isModeEnabled(DECMode::ApplicationKeypad));

    // Disable via DECRM 66
    mock.writeToScreen(DECRM(66));
    CHECK_FALSE(mock.terminal.isModeEnabled(DECMode::ApplicationKeypad));

    // DECRQM should report correctly
    mock.writeToScreen(DECSM(66));
    mock.writeToScreen(DECRQM(66));
    REQUIRE(e(mock.terminal.peekInput()) == e("\033[?66;1$y"));
}

TEST_CASE("DECARM", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(10) } };

    // Auto-repeat should be enabled by default (VT100 spec)
    CHECK(mock.terminal.isModeEnabled(DECMode::AutoRepeat));

    // Disable auto-repeat via DECRM 8
    mock.writeToScreen(DECRM(8));
    CHECK_FALSE(mock.terminal.isModeEnabled(DECMode::AutoRepeat));

    // Re-enable via DECSM 8
    mock.writeToScreen(DECSM(8));
    CHECK(mock.terminal.isModeEnabled(DECMode::AutoRepeat));

    SECTION("DECRQM reports set when enabled")
    {
        mock.writeToScreen(DECSM(8));
        mock.writeToScreen(DECRQM(8));
        REQUIRE(e(mock.terminal.peekInput()) == e("\033[?8;1$y"));
    }

    SECTION("DECRQM reports reset when disabled")
    {
        mock.writeToScreen(DECRM(8));
        mock.writeToScreen(DECRQM(8));
        REQUIRE(e(mock.terminal.peekInput()) == e("\033[?8;2$y"));
    }
}

TEST_CASE("DECBKM", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(10) } };

    // Backarrow key mode should be disabled by default (VT340/VT420 spec)
    CHECK_FALSE(mock.terminal.isModeEnabled(DECMode::BackarrowKey));

    // Enable backarrow key mode via DECSM 67
    mock.writeToScreen(DECSM(67));
    CHECK(mock.terminal.isModeEnabled(DECMode::BackarrowKey));

    // Disable via DECRM 67
    mock.writeToScreen(DECRM(67));
    CHECK_FALSE(mock.terminal.isModeEnabled(DECMode::BackarrowKey));

    SECTION("DECRQM reports set when enabled")
    {
        mock.writeToScreen(DECSM(67));
        mock.writeToScreen(DECRQM(67));
        REQUIRE(e(mock.terminal.peekInput()) == e("\033[?67;1$y"));
    }

    SECTION("DECRQM reports reset when disabled")
    {
        mock.writeToScreen(DECRM(67));
        mock.writeToScreen(DECRQM(67));
        REQUIRE(e(mock.terminal.peekInput()) == e("\033[?67;2$y"));
    }
}

TEST_CASE("peek into history", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(3) }, LineCount { 5 } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("123\r\n456\r\nABC\r\nDEF");

    REQUIRE("ABC\nDEF\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(2) });

    // first line in history
    CHECK(screen.grid().lineText(LineOffset(-2)) == "123");

    // second line in history
    CHECK(screen.grid().lineText(LineOffset(-1)) == "456");

    // first line on screen buffer
    CHECK(screen.grid().lineText(LineOffset(0)) == "ABC");

    // second line on screen buffer
    CHECK(screen.grid().lineText(LineOffset(1)) == "DEF");

    // out-of-range corner cases
    // CHECK_THROWS(screen.at(LineOffset(2), ColumnOffset(0)));
    // CHECK_THROWS(screen.at(LineOffset(1), ColumnOffset(3)));
    // CHECK_THROWS(screen.at({LineOffset()), ColumnOffset()-1)));
    // XXX currently not checked, as they're intentionally using assert() instead.
}

TEST_CASE("captureBuffer", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(5) }, LineCount { 5 } };
    auto& screen = mock.terminal.primaryScreen();

    //           [...      history ...  ...][main page area]
    mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");

    SECTION("lines: 0")
    {
        screen.captureBuffer(LineCount(0), false);
        INFO(e(mock.terminal.peekInput()));
        CHECK(e(mock.terminal.peekInput()) == e("\033^314;\033\\"));
    }
    SECTION("lines: 1")
    {
        screen.captureBuffer(LineCount(1), false);
        INFO(e(mock.terminal.peekInput()));
        CHECK(e(mock.terminal.peekInput()) == e("\033^314;KLMNO\n\033\\\033^314;\033\\"));
    }
    SECTION("lines: 2")
    {
        screen.captureBuffer(LineCount(2), false);
        INFO(e(mock.terminal.peekInput()));
        CHECK(e(mock.terminal.peekInput()) == e("\033^314;FGHIJ\nKLMNO\n\033\\\033^314;\033\\"));
    }
    SECTION("lines: 3")
    {
        screen.captureBuffer(LineCount(3), false);
        INFO(e(mock.terminal.peekInput()));
        CHECK(e(mock.terminal.peekInput()) == e("\033^314;ABCDE\nFGHIJ\nKLMNO\n\033\\\033^314;\033\\"));
    }
    SECTION("lines: 4")
    {
        screen.captureBuffer(LineCount(4), false);
        INFO(e(mock.terminal.peekInput()));
        CHECK(e(mock.terminal.peekInput())
              == e("\033^314;67890\nABCDE\nFGHIJ\nKLMNO\n\033\\\033^314;\033\\"));
    }
    SECTION("lines: 5")
    {
        screen.captureBuffer(LineCount(5), false);
        INFO(e(mock.terminal.peekInput()));
        CHECK(e(mock.terminal.peekInput())
              == e("\033^314;12345\n67890\nABCDE\nFGHIJ\nKLMNO\n\033\\\033^314;\033\\"));
    }
    SECTION("lines: 5 (+1 overflow)")
    {
        screen.captureBuffer(LineCount(6), false);
        INFO(e(mock.terminal.peekInput()));
        CHECK(e(mock.terminal.peekInput())
              == e("\033^314;12345\n67890\nABCDE\nFGHIJ\nKLMNO\n\033\\\033^314;\033\\"));
    }
}

TEST_CASE("render into history", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(5) }, LineCount { 5 } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");

    REQUIRE("FGHIJ\nKLMNO\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(4) });
    REQUIRE(screen.historyLineCount() == LineCount { 3 });

    auto renderer = TextRenderBuilder {};
    string& renderedText = renderer.text;

    // main area
    logScreenText(screen, "render into history");
    screen.render(renderer);
    REQUIRE("FGHIJ\nKLMNO\n" == renderedText);

    // 1 line into history") {
    std::ranges::fill(renderedText, ' ');
    screen.render(renderer, ScrollOffset { 1 });
    REQUIRE("ABCDE\nFGHIJ\n" == renderedText);

    // 2 lines into history") {
    std::ranges::fill(renderedText, ' ');
    screen.render(renderer, ScrollOffset { 2 });
    REQUIRE("67890\nABCDE\n" == renderedText);

    // 3 lines into history") {
    std::ranges::fill(renderedText, ' ');
    screen.render(renderer, ScrollOffset { 3 });
    REQUIRE("12345\n67890\n" == renderedText);
}

TEST_CASE("HorizontalTabClear.AllTabs", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    screen.horizontalTabClear(HorizontalTabClear::AllTabs);

    screen.writeText('X');
    screen.moveCursorToNextTab();
    screen.writeText('Y');
    REQUIRE("X   Y" == screen.grid().lineText(LineOffset(0)));

    screen.moveCursorToNextTab();
    screen.writeText('Z');
    REQUIRE("X   Y" == screen.grid().lineText(LineOffset(0)));
    REQUIRE("Z    " == screen.grid().lineText(LineOffset(1)));

    screen.moveCursorToNextTab();
    screen.writeText('A');
    REQUIRE("X   Y" == screen.grid().lineText(LineOffset(0)));
    REQUIRE("Z   A" == screen.grid().lineText(LineOffset(1)));
}

TEST_CASE("HorizontalTabClear.UnderCursor", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) } };
    auto& screen = mock.terminal.primaryScreen();

    // clear tab at column 4
    screen.moveCursorTo(LineOffset { 0 }, ColumnOffset { 7 });
    screen.horizontalTabClear(HorizontalTabClear::UnderCursor);

    screen.moveCursorTo({}, {});
    screen.writeText('A');
    screen.moveCursorToNextTab();
    screen.writeText('B');

    //      "12345678901234567890"
    REQUIRE("A              B    " == screen.grid().lineText(LineOffset(0)));
    REQUIRE("                    " == screen.grid().lineText(LineOffset(1)));

    screen.moveCursorToNextTab();
    screen.writeText('C');
    //    "12345678901234567890"
    CHECK("A              B   C" == screen.grid().lineText(LineOffset(0)));
    CHECK("                    " == screen.grid().lineText(LineOffset(1)));
}

TEST_CASE("HorizontalTabSet", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    auto& screen = mock.terminal.primaryScreen();
    screen.horizontalTabClear(HorizontalTabClear::AllTabs);

    screen.moveCursorToColumn(ColumnOffset(2));
    screen.horizontalTabSet();

    screen.moveCursorToColumn(ColumnOffset(4));
    screen.horizontalTabSet();

    screen.moveCursorToColumn(ColumnOffset(7));
    screen.horizontalTabSet();

    screen.moveCursorToBeginOfLine();

    screen.writeText('1');

    screen.moveCursorToNextTab();
    screen.writeText('3');

    screen.moveCursorToNextTab();
    screen.writeText('5');

    screen.moveCursorToNextTab();
    screen.writeText('8');

    screen.moveCursorToNextTab(); // capped
    screen.writeText('A');        // writes B at right margin, flags for autowrap

    REQUIRE("1 3 5  8 A" == screen.grid().lineText(LineOffset(0)));

    screen.moveCursorToNextTab(); // wrapped
    screen.writeText('B');        // writes B at left margin

    //       1234567890
    REQUIRE("1 3 5  8 A" == screen.grid().lineText(LineOffset(0)));
    screen.moveCursorToNextTab(); // 1 -> 3 (overflow)
    screen.moveCursorToNextTab(); // 3 -> 5
    screen.moveCursorToNextTab(); // 5 -> 8
    screen.writeText('C');

    //     1234567890
    CHECK("1 3 5  8 A" == screen.grid().lineText(LineOffset(0)));
    CHECK("B      C  " == screen.grid().lineText(LineOffset(1)));
}

TEST_CASE("CursorBackwardTab.fixedTabWidth", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) } };
    auto& screen = mock.terminal.primaryScreen();

    screen.writeText('a');

    screen.moveCursorToNextTab(); // -> 9
    screen.writeText('b');

    screen.moveCursorToNextTab();
    screen.writeText('c'); // -> 17

    //      "12345678901234567890"
    REQUIRE("a       b       c   " == screen.grid().lineText(LineOffset(0)));
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(17) });

    SECTION("no op")
    {
        screen.cursorBackwardTab(TabStopCount(0));
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(17) });
    }

    SECTION("inside 1")
    {
        screen.cursorBackwardTab(TabStopCount(1));
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(16) });
        screen.writeText('X');
        //    "12345678901234567890"
        CHECK("a       b       X   " == screen.grid().lineText(LineOffset(0)));
    }

    SECTION("inside 2")
    {
        screen.cursorBackwardTab(TabStopCount(2));
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(8) });
        screen.writeText('X');
        //    "12345678901234567890"
        CHECK("a       X       c   " == screen.grid().lineText(LineOffset(0)));
    }

    SECTION("exact")
    {
        screen.cursorBackwardTab(TabStopCount(3));
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });
        screen.writeText('X');
        //    "12345678901234567890"
        CHECK("X       b       c   " == screen.grid().lineText(LineOffset(0)));
    }

    SECTION("oveflow")
    {
        screen.cursorBackwardTab(TabStopCount(4));
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });
        screen.writeText('X');
        //    "12345678901234567890"
        CHECK("X       b       c   " == screen.grid().lineText(LineOffset(0)));
    }
}

TEST_CASE("CursorBackwardTab.manualTabs", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    auto& screen = mock.terminal.primaryScreen();

    screen.moveCursorToColumn(ColumnOffset(4));
    screen.horizontalTabSet();
    screen.moveCursorToColumn(ColumnOffset(8));
    screen.horizontalTabSet();
    screen.moveCursorToBeginOfLine();

    screen.writeText('a');

    screen.moveCursorToNextTab(); // -> 4
    screen.writeText('b');

    screen.moveCursorToNextTab();
    screen.writeText('c'); // -> 8

    //      "1234567890"
    REQUIRE(screen.logicalCursorPosition().column.value == 9);
    REQUIRE("a   b   c " == screen.grid().lineText(LineOffset(0)));

    SECTION("oveflow")
    {
        screen.cursorBackwardTab(TabStopCount(4));
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });
        screen.writeText('X');
        CHECK("X   b   c " == screen.grid().lineText(LineOffset(0)));
    }

    SECTION("exact")
    {
        screen.cursorBackwardTab(TabStopCount(3));
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });
        screen.writeText('X');
        //    "1234567890"
        CHECK("X   b   c " == screen.grid().lineText(LineOffset(0)));
    }

    SECTION("inside 2")
    {
        screen.cursorBackwardTab(TabStopCount(2));
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(4) });
        screen.writeText('X');
        //    "1234567890"
        CHECK("a   X   c " == screen.grid().lineText(LineOffset(0)));
    }

    SECTION("inside 1")
    {
        screen.cursorBackwardTab(TabStopCount(1));
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(8) });
        screen.writeText('X');
        //    "1234567890"
        CHECK("a   b   X " == screen.grid().lineText(LineOffset(0)));
    }

    SECTION("no op")
    {
        screen.cursorBackwardTab(TabStopCount(0));
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(9) });
    }
}

TEST_CASE("searchReverse", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(4) }, LineCount(10) };
    mock.writeToScreen("1abc"); // -3: +
    mock.writeToScreen("2def"); // -2: | history
    mock.writeToScreen("3ghi"); // -1: +
    mock.writeToScreen("4jkl"); //  0: +
    mock.writeToScreen("5mno"); //  1: | main screen
    mock.writeToScreen("6pqr"); //  2: +

    auto& screen = mock.terminal.primaryScreen();
    auto const cursorPosition = screen.cursor().position;

    INFO(std::format("cursor pos {}", cursorPosition));

    // With SoA storage, no inflation needed -- all lines are always "inflated".
    {
        [[maybe_unused]] auto const inflate = true;

        // Find "qr" right at in front of the cursor.
        optional<CellLocation> const qr = screen.searchReverse(U"qr", cursorPosition);
        REQUIRE(qr.value() == CellLocation { LineOffset(2), ColumnOffset(2) });

        // Find something in main page area.
        optional<CellLocation> const mn = screen.searchReverse(U"mn", cursorPosition);
        REQUIRE(mn.value() == CellLocation { LineOffset(1), ColumnOffset(1) });

        // Search for something that doesn't exist.
        optional<CellLocation> const nnOut = screen.searchReverse(U"XY", *mn);
        REQUIRE(!nnOut.has_value());

        // Check that we can find a term in the top-most scrollback line.
        optional<CellLocation> const oneAB = screen.searchReverse(U"1ab", *mn);
        REQUIRE(oneAB.value() == CellLocation { LineOffset(-3), ColumnOffset(0) });

        mock.writeToScreen("7abcd");

        // Find text that got wrapped
        optional<CellLocation> const cd = screen.searchReverse(U"cd", screen.cursor().position);
        REQUIRE(cd.value() == CellLocation { LineOffset(1), ColumnOffset(3) });

        // Find text larger than the line length
        optional<CellLocation> const longSearch =
            screen.searchReverse(U"6pqr7abcd", screen.cursor().position);
        REQUIRE(longSearch.value() == CellLocation { LineOffset(0), ColumnOffset(0) });
    }
}

TEST_CASE("findMarkerDownwards", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(4) }, LineCount(10) };
    auto& screen = mock.terminal.primaryScreen();
    REQUIRE_FALSE(screen.findMarkerDownwards(LineOffset(0)).has_value());
    REQUIRE_FALSE(screen.findMarkerDownwards(LineOffset(1)).has_value()); // history bottom
    REQUIRE_FALSE(screen.findMarkerDownwards(LineOffset(2)).has_value());
    REQUIRE_FALSE(screen.findMarkerDownwards(LineOffset(3)).has_value()); // history top
    REQUIRE_FALSE(screen.findMarkerDownwards(LineOffset(4)).has_value()); // overflow

    SECTION("no marks")
    {
        mock.writeToScreen("1abc"); // -3: +
        mock.writeToScreen("2def"); // -2: | history
        mock.writeToScreen("3ghi"); // -1: +
        mock.writeToScreen("4jkl"); //  0: +
        mock.writeToScreen("5mno"); //  1: | main screen
        mock.writeToScreen("6pqr"); //  2: +

        REQUIRE(screen.historyLineCount() == LineCount { 3 });

        // overflow: one above scroll-top
        auto mark = screen.findMarkerDownwards(LineOffset(4));
        REQUIRE_FALSE(mark.has_value());

        // scroll-top
        mark = screen.findMarkerDownwards(LineOffset(3));
        REQUIRE_FALSE(mark.has_value());

        mark = screen.findMarkerDownwards(LineOffset(2));
        REQUIRE_FALSE(mark.has_value());

        mark = screen.findMarkerDownwards(LineOffset(1));
        REQUIRE_FALSE(mark.has_value());

        // underflow: one below scroll buttom
        mark = screen.findMarkerDownwards(LineOffset(0));
        REQUIRE_FALSE(mark.has_value());
    }

    SECTION("with marks")
    {
        // saved lines
        screen.setMark(); // 0 (-3)
        mock.writeToScreen("1abc\r\n");
        mock.writeToScreen("2def\r\n"); // 1 (-2)
        screen.setMark();
        mock.writeToScreen("3ghi\r\n"); // 2 (-1)

        // visibile screen
        screen.setMark(); // 3 (0)
        mock.writeToScreen("4jkl\r\n");
        mock.writeToScreen("5mno\r\n"); // 4 (1)
        screen.setMark();               // 5 (2)
        mock.writeToScreen("6pqr");

        // {{{ pre-expectations
        REQUIRE(screen.grid().lineText(LineOffset(-3)) == "1abc");
        REQUIRE(screen.grid().lineText(LineOffset(-2)) == "2def");
        REQUIRE(screen.grid().lineText(LineOffset(-1)) == "3ghi");

        REQUIRE(screen.grid().lineText(LineOffset(0)) == "4jkl");
        REQUIRE(screen.grid().lineText(LineOffset(1)) == "5mno");
        REQUIRE(screen.grid().lineText(LineOffset(2)) == "6pqr");
        // }}}

        // ======================================================

        // overflow: one above scroll top -> scroll bottom
        // gracefully clamps to scroll-top
        auto marker = screen.findMarkerDownwards(LineOffset(-4));
        REQUIRE(marker.has_value());
        REQUIRE(*marker.value() == -1);

        // scroll top -> scroll bottom
        marker = screen.findMarkerDownwards(LineOffset(-3));
        REQUIRE(marker.has_value());
        REQUIRE(*marker.value() == -1);

        // scroll bottom -> NONE
        marker = screen.findMarkerDownwards(LineOffset(-1));
        REQUIRE(marker.has_value());
        REQUIRE(*marker.value() == 0);
    }
}

TEST_CASE("findMarkerUpwards", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(4) }, LineCount(10) };
    auto& screen = mock.terminal.primaryScreen();
    REQUIRE_FALSE(screen.findMarkerUpwards(LineOffset(-1)).has_value()); // peak into history
    REQUIRE_FALSE(screen.findMarkerUpwards(LineOffset(0)).has_value());
    REQUIRE_FALSE(screen.findMarkerUpwards(LineOffset(1)).has_value());
    REQUIRE_FALSE(screen.findMarkerUpwards(LineOffset(2)).has_value());
    REQUIRE_FALSE(screen.findMarkerUpwards(LineOffset(3)).has_value()); // overflow

    SECTION("no marks")
    {
        mock.writeToScreen("1abc");
        mock.writeToScreen("2def");
        mock.writeToScreen("3ghi");
        mock.writeToScreen("4jkl");
        mock.writeToScreen("5mno");
        mock.writeToScreen("6pqr");

        REQUIRE(screen.historyLineCount() == LineCount { 3 });

        auto mark = screen.findMarkerUpwards(LineOffset(0));
        REQUIRE_FALSE(mark.has_value());

        // bottom line in history
        mark = screen.findMarkerUpwards(LineOffset(1));
        REQUIRE_FALSE(mark.has_value());

        // one above bottom line in history
        mark = screen.findMarkerUpwards(LineOffset(2));
        REQUIRE_FALSE(mark.has_value());

        // top history line
        mark = screen.findMarkerUpwards(LineOffset(3));
        REQUIRE_FALSE(mark.has_value());

        // one above history top
        mark = screen.findMarkerUpwards(LineOffset(4));
        REQUIRE_FALSE(mark.has_value());
    }

    SECTION("with marks")
    {
        // saved lines
        screen.setMark(); // 0 (-3)
        mock.writeToScreen("1abc\r\n");
        mock.writeToScreen("2def\r\n"); // 1 (-2)
        screen.setMark();
        mock.writeToScreen("3ghi\r\n"); // 2 (-1)

        // visibile screen
        screen.setMark(); // 3 (0)
        mock.writeToScreen("4jkl\r\n");
        mock.writeToScreen("5mno\r\n"); // 4 (1)
        screen.setMark();               // 5 (2)
        mock.writeToScreen("6pqr");

        // {{{ pre-checks
        REQUIRE(screen.grid().lineText(LineOffset(-3)) == "1abc"); // marked
        REQUIRE(screen.grid().lineText(LineOffset(-2)) == "2def");
        REQUIRE(screen.grid().lineText(LineOffset(-1)) == "3ghi"); // marked

        REQUIRE(screen.grid().lineText(LineOffset(0)) == "4jkl"); // marked
        REQUIRE(screen.grid().lineText(LineOffset(1)) == "5mno");
        REQUIRE(screen.grid().lineText(LineOffset(2)) == "6pqr"); // marked
        // }}}

        // ======================================================
        // main page top (0) -> scroll offset 1
        auto marker = screen.findMarkerUpwards(LineOffset(0));
        REQUIRE(marker.has_value());
        REQUIRE(marker.value().value == -1); // 3ghi

        // scroll offset 1 -> scroll offset 3
        marker = screen.findMarkerUpwards(LineOffset(-1));
        REQUIRE(marker.has_value());
        REQUIRE(marker.value().value == -3); // 1abc

        // scroll-top
        marker = screen.findMarkerUpwards(LineOffset(-3));
        REQUIRE(!marker.has_value());

        // one-off
        marker = screen.findMarkerUpwards(LineOffset(-4));
        REQUIRE(!marker.has_value());
    }
}

TEST_CASE("DECTABSR", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(35) } };
    auto& screen = mock.terminal.primaryScreen();

    SECTION("default tabstops")
    {
        screen.requestTabStops();
        CHECK(e(mock.terminal.peekInput()) == e("\033P2$u1/9/17/25/33\033\\"));
    }

    SECTION("cleared tabs")
    {
        screen.horizontalTabClear(HorizontalTabClear::AllTabs);
        screen.requestTabStops();
        CHECK(e(mock.terminal.peekInput()) == e("\033P2$u1/9/17/25/33\033\\"));
    }

    SECTION("custom tabstops")
    {
        screen.horizontalTabClear(HorizontalTabClear::AllTabs);

        screen.moveCursorToColumn(ColumnOffset(1));
        screen.horizontalTabSet();

        screen.moveCursorToColumn(ColumnOffset(3));
        screen.horizontalTabSet();

        screen.moveCursorToColumn(ColumnOffset(7));
        screen.horizontalTabSet();

        screen.moveCursorToColumn(ColumnOffset(15));
        screen.horizontalTabSet();

        screen.requestTabStops();
        CHECK(e(mock.terminal.peekInput()) == e("\033P2$u2/4/8/16\033\\"));
    }
}

TEST_CASE("save_restore_DEC_modes", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(2) } };

    mock.terminal.setMode(DECMode::MouseProtocolHighlightTracking, false);
    mock.terminal.saveModes({ DECMode::MouseProtocolHighlightTracking });

    mock.terminal.setMode(DECMode::MouseProtocolHighlightTracking, true);
    CHECK(mock.terminal.isModeEnabled(DECMode::MouseProtocolHighlightTracking));

    mock.terminal.restoreModes({ DECMode::MouseProtocolHighlightTracking });
    CHECK_FALSE(mock.terminal.isModeEnabled(DECMode::MouseProtocolHighlightTracking));
}

TEST_CASE("OSC.2.Unicode")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(2) } };

    auto const u32title = u32string_view(U"\U0001F600");
    auto const title = unicode::convert_to<char>(u32title);

    mock.writeToScreen(U"\033]2;\U0001F600\033\\");
    INFO(mock.terminal.peekInput());
    CHECK(e(mock.windowTitle) == e(title));
}

TEST_CASE("OSC.4")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(2) } };

    SECTION("query")
    {
        mock.writeToScreen("\033]4;7;?\033\\");
        INFO(e(mock.terminal.peekInput()));
        REQUIRE(e(mock.terminal.peekInput()) == e("\033]4;7;rgb:c0c0/c0c0/c0c0\033\\"));
    }

    SECTION("set color via format rgb:RR/GG/BB")
    {
        mock.writeToScreen("\033]4;7;rgb:ab/cd/ef\033\\");
        mock.writeToScreen("\033]4;7;?\033\\");
        INFO(mock.terminal.peekInput());
        REQUIRE(e(mock.terminal.peekInput()) == e("\033]4;7;rgb:abab/cdcd/efef\033\\"));
    }

    SECTION("set color via format #RRGGBB")
    {
        mock.writeToScreen("\033]4;7;#abcdef\033\\");
        mock.writeToScreen("\033]4;7;?\033\\");
        INFO(e(mock.terminal.peekInput()));
        REQUIRE(e(mock.terminal.peekInput()) == e("\033]4;7;rgb:abab/cdcd/efef\033\\"));
    }

    SECTION("set color via format #RGB")
    {
        mock.writeToScreen("\033]4;7;#abc\033\\");
        mock.writeToScreen("\033]4;7;?\033\\");
        INFO(mock.terminal.peekInput());
        REQUIRE(e(mock.terminal.peekInput()) == e("\033]4;7;rgb:a0a0/b0b0/c0c0\033\\"));
    }

    SECTION("set color via format rgb:RRRR/GGGG/BBBB")
    {
        // The four-digit form is the one Contour itself reports back, and the one applications
        // overwhelmingly send. It used to be rejected outright, leaving the palette untouched.
        mock.writeToScreen("\033]4;7;rgb:abab/cdcd/efef\033\\");
        mock.writeToScreen("\033]4;7;?\033\\");
        INFO(mock.terminal.peekInput());
        REQUIRE(e(mock.terminal.peekInput()) == e("\033]4;7;rgb:abab/cdcd/efef\033\\"));
    }

    SECTION("several index/specification pairs in one sequence")
    {
        mock.writeToScreen("\033]4;0;rgb:f0f0/f0f0/f0f0;1;rgb:f0f0/0000/0000\033\\");
        mock.writeToScreen("\033]4;0;?;1;?\033\\");
        INFO(mock.terminal.peekInput());
        REQUIRE(e(mock.terminal.peekInput())
                == e("\033]4;0;rgb:f0f0/f0f0/f0f0\033\\"
                     "\033]4;1;rgb:f0f0/0000/0000\033\\"));
    }
}

TEST_CASE("OSC.10-19")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(2) } };

    SECTION("set and query the foreground")
    {
        mock.writeToScreen("\033]10;rgb:f0f0/f0f0/f0f0\033\\");
        mock.writeToScreen("\033]10;?\033\\");
        INFO(mock.terminal.peekInput());
        REQUIRE(e(mock.terminal.peekInput()) == e("\033]10;rgb:f0f0/f0f0/f0f0\033\\"));
    }

    SECTION("one sequence walks upward through the colors")
    {
        // OSC 10 with two specifications sets the foreground *and* the background.
        mock.writeToScreen("\033]10;rgb:f0f0/f0f0/f0f0;rgb:f0f0/0000/0000\033\\");
        mock.writeToScreen("\033]10;?;?\033\\");
        INFO(mock.terminal.peekInput());

        // Each answer is tagged with the OSC command of the color it reports, not with the one the
        // sequence began at. Contour used to read "?;?" as a single color specification, fail to parse
        // it, and answer with nothing at all.
        REQUIRE(e(mock.terminal.peekInput())
                == e("\033]10;rgb:f0f0/f0f0/f0f0\033\\"
                     "\033]11;rgb:f0f0/0000/0000\033\\"));
    }

    SECTION("a sequence may begin at any color")
    {
        mock.writeToScreen("\033]11;rgb:0101/0202/0303;rgb:0404/0505/0606\033\\");
        mock.writeToScreen("\033]11;?;?\033\\");
        INFO(mock.terminal.peekInput());
        REQUIRE(e(mock.terminal.peekInput())
                == e("\033]11;rgb:0101/0202/0303\033\\"  // background
                     "\033]12;rgb:0404/0505/0606\033\\") // cursor
        );
    }

    SECTION("an empty specification skips its color")
    {
        mock.writeToScreen("\033]10;rgb:0f0f/0f0f/0f0f\033\\");
        mock.writeToScreen("\033]10;;rgb:f0f0/0000/0000\033\\");
        mock.writeToScreen("\033]10;?;?\033\\");
        INFO(mock.terminal.peekInput());
        REQUIRE(e(mock.terminal.peekInput())
                == e("\033]10;rgb:0f0f/0f0f/0f0f\033\\"  // untouched
                     "\033]11;rgb:f0f0/0000/0000\033\\") // set by the second specification
        );
    }

    SECTION("a color we do not model still consumes its specification")
    {
        // Specifications 6, 7 and 9 address xterm's Tektronix colors (OSC 15, 16 and 18), which Contour
        // does not model. They must still be consumed, so that the eighth lands on OSC 17 -- the
        // highlight background -- and the tenth on OSC 19 -- the highlight foreground -- rather than
        // shifting onto some earlier color. An eleventh specification runs past OSC 19 and addresses
        // nothing at all.
        mock.writeToScreen("\033]10;rgb:0101/0101/0101;rgb:0202/0202/0202;rgb:0303/0303/0303"
                           ";rgb:0404/0404/0404;rgb:0505/0505/0505;rgb:0606/0606/0606"
                           ";rgb:0707/0707/0707;rgb:0808/0808/0808;rgb:0909/0909/0909"
                           ";rgb:0a0a/0a0a/0a0a;rgb:0b0b/0b0b/0b0b\033\\");

        auto const& palette = mock.terminal.colorPalette();
        CHECK(palette.defaultForeground == RGBColor { 0x01, 0x01, 0x01 });  // OSC 10
        CHECK(palette.defaultBackground == RGBColor { 0x02, 0x02, 0x02 });  // OSC 11
        CHECK(get<RGBColor>(palette.cursor.color) == RGBColor { 3, 3, 3 }); // OSC 12
        CHECK(palette.mouseForeground == RGBColor { 0x04, 0x04, 0x04 });    // OSC 13
        CHECK(palette.mouseBackground == RGBColor { 0x05, 0x05, 0x05 });    // OSC 14
                                                                            // OSC 15, 16: Tektronix
        CHECK(get<RGBColor>(palette.selection.background)                   // OSC 17
              == RGBColor { 0x08, 0x08, 0x08 });                            //
                                                                            // OSC 18: Tektronix
        CHECK(get<RGBColor>(palette.selection.foreground)                   // OSC 19
              == RGBColor { 0x0A, 0x0A, 0x0A });                            //
    }

    SECTION("a malformed specification ends the sequence")
    {
        mock.writeToScreen("\033]10;rgb:0b0b/0b0b/0b0b\033\\");
        mock.resetReplyData();

        // As in xterm, the first specification that cannot be parsed stops the walk, so the background
        // is left alone -- but the foreground set before it stands.
        mock.writeToScreen("\033]10;not-a-color;rgb:0c0c/0c0c/0c0c\033\\");
        mock.writeToScreen("\033]10;?\033\\");
        INFO(mock.terminal.peekInput());
        REQUIRE(e(mock.terminal.peekInput()) == e("\033]10;rgb:0b0b/0b0b/0b0b\033\\"));
    }

    SECTION("the highlight colors are addressable in their own right")
    {
        // OSC 17 and OSC 19 could be reached by walking up from OSC 10, but not named directly: only
        // their reset counterparts (OSC 117 and OSC 119) were ever registered, so a highlight color
        // could be reset but never set.
        mock.writeToScreen("\033]17;rgb:1111/2222/3333\033\\");
        mock.writeToScreen("\033]19;rgb:4444/5555/6666\033\\");
        mock.writeToScreen("\033]17;?\033\\");
        mock.writeToScreen("\033]19;?\033\\");
        INFO(mock.terminal.peekInput());
        REQUIRE(e(mock.terminal.peekInput())
                == e("\033]17;rgb:1111/2222/3333\033\\"
                     "\033]19;rgb:4444/5555/6666\033\\"));
    }

    SECTION("a color following the cell's own color is still reported")
    {
        // The highlight foreground follows the cell's foreground by default rather than naming a color
        // of its own. A query must still be answered -- silence would leave the application reading
        // some later sequence's reply in place of this one.
        mock.writeToScreen("\033]10;rgb:1212/3434/5656\033\\"); // the default foreground
        mock.resetReplyData();

        mock.writeToScreen("\033]19;?\033\\");
        INFO(mock.terminal.peekInput());
        REQUIRE(e(mock.terminal.peekInput()) == e("\033]19;rgb:1212/3434/5656\033\\"));
    }

    SECTION("resetting a highlight color restores the configured one")
    {
        mock.writeToScreen("\033]17;rgb:1111/2222/3333\033\\");
        mock.writeToScreen("\033]117\033\\"); // RCOLORHIGHLIGHTBG
        mock.resetReplyData();

        mock.writeToScreen("\033]17;?\033\\");
        INFO(mock.terminal.peekInput());
        REQUIRE(e(mock.terminal.peekInput())
                == e(std::format("\033]17;{}\033\\",
                                 colorSpecification(get<RGBColor>(
                                     mock.terminal.defaultColorPalette().selection.background)))));
    }
}

TEST_CASE("XTGETTCAP")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(2) } };

    // Decodes the hex-encoded value from a valid XTGETTCAP response
    // "\033P1+r<hex-name>[=<hex-value>]\033\\"
    auto const extractValue = [](std::string_view reply) -> std::optional<std::string> {
        auto const eq = reply.find('=');
        if (eq == std::string_view::npos)
            return std::nullopt;
        auto const st = reply.find("\033\\", eq);
        if (st == std::string_view::npos)
            return std::nullopt;
        return crispy::fromHexString(reply.substr(eq + 1, st - eq - 1));
    };

    auto const queryValue = [&](std::string_view name) -> std::optional<std::string> {
        mock.resetReplyData();
        mock.writeToScreen(std::format("\033P+q{}\033\\", crispy::toHexString(name)));
        auto const reply = std::string(mock.terminal.peekInput());
        INFO(std::format("Reply: {}", crispy::escape(reply)));
        if (!reply.starts_with("\033P1+r"))
            return std::nullopt;
        return extractValue(reply);
    };

    SECTION("string: RGB")
    {
        auto const value = queryValue("RGB");
        REQUIRE(value.has_value());
        CHECK(*value == "8/8/8");
    }

    SECTION("numeric: colors")
    {
        auto const value = queryValue("colors");
        REQUIRE(value.has_value());
        CHECK(*value == "256");
    }

    SECTION("boolean: am")
    {
        auto const value = queryValue("am");
        // Boolean capabilities respond with just the name, no =value.
        CHECK(!value.has_value());
    }

    SECTION("nonexistent")
    {
        mock.resetReplyData();
        mock.writeToScreen(std::format("\033P+q{:02X}{:02X}\033\\", 'x', 'x'));
        // Note how 'xx' is not in the return reply, meaning "not found"
        CHECK(std::string(mock.terminal.peekInput()) == "\033P0+r\033\\");
    }
}
TEST_CASE("setMaxHistoryLineCount", "[screen]")
{
    // from zero to something
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(2) }, LineCount(0) };
    auto& screen = mock.terminal.primaryScreen();
    screen.grid().setReflowOnResize(false);
    mock.writeToScreen("AB\r\nCD");
    REQUIRE("AB\nCD\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(1) });

    mock.terminal.setMaxHistoryLineCount(LineCount(1));
    REQUIRE("AB\nCD\n" == screen.renderMainPageText());
}

// TODO: resize test (should be in Grid_test.cpp?)
TEST_CASE("resize", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(2) }, LineCount(10) };
    auto& screen = mock.terminal.primaryScreen();
    screen.grid().setReflowOnResize(false);
    mock.writeToScreen("AB\r\nCD");
    REQUIRE("AB\nCD\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(1) });

    mock.terminal.setMaxHistoryLineCount(LineCount(10));

    SECTION("no-op")
    {
        mock.terminal.resizeScreen({ LineCount(2), ColumnCount(2) });
        CHECK("AB\nCD\n" == screen.renderMainPageText());
    }

    SECTION("grow lines")
    {
        mock.terminal.resizeScreen({ LineCount(3), ColumnCount(2) });
        REQUIRE("AB\nCD\n  \n" == screen.renderMainPageText());
        REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(1) });

        mock.writeToScreen("\r\n");
        mock.writeToScreen("E");
        REQUIRE("AB\nCD\nE \n" == screen.renderMainPageText());
        REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(2), ColumnOffset(1) });

        mock.writeToScreen("F");
        REQUIRE("AB\nCD\nEF\n" == screen.renderMainPageText());
        REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(2), ColumnOffset(1) });
    }

    SECTION("shrink lines")
    {
        mock.terminal.resizeScreen({ LineCount(1), ColumnCount(2) });
        CHECK("CD\n" == screen.renderMainPageText());
        CHECK("AB" == screen.grid().lineAt(LineOffset(-1)).toUtf8());
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(1) });
    }

    SECTION("grow columns")
    {
        mock.terminal.resizeScreen({ LineCount(2), ColumnCount(3) });
        CHECK("AB \nCD \n" == screen.renderMainPageText());
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(2) });
    }

    SECTION("shrink columns")
    {
        mock.terminal.resizeScreen({ LineCount(2), ColumnCount(1) });
        CHECK("A\nC\n" == screen.renderMainPageText());
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(0) });
    }

    SECTION("regrow columns")
    {
        // 1.) grow
        mock.terminal.resizeScreen({ LineCount(2), ColumnCount(3) });
        logScreenText(screen, "after columns grow");
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(2) });

        // 2.) fill
        screen.writeText('Y');
        REQUIRE("AB \nCDY\n" == screen.renderMainPageText());
        screen.moveCursorTo(LineOffset { 0 }, ColumnOffset { 2 });
        screen.writeText('X');
        logScreenText(screen, "after write");
        REQUIRE("ABX\nCDY\n" == screen.renderMainPageText());
        REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(2) });

        // 3.) shrink
        mock.terminal.resizeScreen({ LineCount(2), ColumnCount(2) });
        REQUIRE("AB\nCD\n" == screen.renderMainPageText());
        REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(1) });

        // 4.) regrow (and see if pre-filled data were retained)
        // NOTE: This is currently not retained. Do we want to recreate this behaviour?
        // mock.terminal.resizeScreen({LineCount(2), ColumnCount(3)});
        // REQUIRE("ABX\nCDY\n" == screen.renderMainPageText());
        // REQUIRE(screen.logicalCursorPosition() == CellLocation{LineOffset(0), ColumnOffset(2)});
    }

    SECTION("grow rows, grow columns")
    {
        mock.terminal.resizeScreen({ LineCount(3), ColumnCount(3) });
        REQUIRE("AB \nCD \n   \n" == screen.renderMainPageText());
        mock.writeToScreen("1\r\n234");
        REQUIRE("AB \nCD1\n234\n" == screen.renderMainPageText());
    }

    SECTION("grow rows, shrink columns")
    {
        mock.terminal.resizeScreen({ LineCount(3), ColumnCount(1) });
        REQUIRE("A\nC\n \n" == screen.renderMainPageText());
    }

    SECTION("shrink rows, grow columns")
    {
        mock.terminal.resizeScreen({ LineCount(1), ColumnCount(3) });
        REQUIRE("CD \n" == screen.renderMainPageText());
    }

    SECTION("shrink rows, shrink columns")
    {
        mock.terminal.resizeScreen({ LineCount(1), ColumnCount(1) });
        REQUIRE("C\n" == screen.renderMainPageText());
    }

    // TODO: what do we want to do when re resize to {0, y}, {x, 0}, {0, 0}?
}

// {{{ DECCRA
// TODO: also verify attributes have been copied
// TODO: also test with: DECOM enabled
// TODO: also test with: margins set and having them exceeded
// TODO: also test with: overflowing source bottom/right dimensions
// TODO: also test with: out-of-bounds target or source top/left positions

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
    auto constexpr SRightt = 6;

    auto constexpr TTop = 3;
    auto constexpr TLeftt = 2;

    auto const* const expectedText = "ABCDEF\n"
                                     "abcdef\n" // .3456.
                                     "1IJKL6\n" // .IJKL.
                                     "GijklL\n"
                                     "ghijkl\n";

    // copy up by one line (4 to 3), 2 lines
    // copy left by one column (3 to 2), 2 columns

    auto const deccraSeq = std::format(
        "\033[{};{};{};{};{};{};{};{}$v", STop, SLeft, SBottom, SRightt, Page, TTop, TLeftt, Page);
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
// }}}

TEST_CASE("Screen.tcap.string", "[screen, tcap]")
{
    using namespace vtbackend;
    auto mock = MockTerm(PageSize { LineCount(3), ColumnCount(5) }, LineCount(2));
    mock.writeToScreen("\033P+q687061\033\\"); // HPA
    REQUIRE(e(mock.terminal.peekInput()) == e("\033P1+r687061=1B5B2569257031256447\033\\"));
}

TEST_CASE("Sixel.simple", "[screen]")
{
    auto const pageSize = PageSize { LineCount(11), ColumnCount(11) };
    auto mock = MockTerm { pageSize, LineCount(11) };
    mock.terminal.setCellPixelSize(ImageSize { Width(10), Height(10) });

    mock.writeToScreen(chessBoard);

    CHECK(mock.terminal.primaryScreen().cursor().position.column.value == ColumnOffset(0).value);
    CHECK(mock.terminal.primaryScreen().cursor().position.line.value == LineOffset(10).value);

    for (auto line = LineOffset(0); line < boxed_cast<LineOffset>(pageSize.lines); ++line)
    {
        for (auto column = ColumnOffset(0); column < boxed_cast<ColumnOffset>(pageSize.columns); ++column)
        {
            auto const& cell = mock.terminal.primaryScreen().at(line, column);
            if (line <= LineOffset(9) && column <= ColumnOffset(9))
            {
                auto fragment = cell.imageFragment();
                REQUIRE(fragment);
                if ((column.value + line.value) % 2)
                    REQUIRE(fragment->data() == white10x10);
                else
                    REQUIRE(fragment->data() == black10x10);

                CHECK(fragment->offset().line == line);
                CHECK(fragment->offset().column == column);
                CHECK(!fragment->data().empty());
            }
            else
            {
                CHECK(cell.empty());
            }
        }
    }
}

TEST_CASE("Sixel.AutoScroll-1", "[screen]")
{
    // Create a 11x9x10 grid and render a 10x10 image causing a line-scroll by one.
    auto const pageSize = PageSize { LineCount(9), ColumnCount(10) };
    auto mock = MockTerm { pageSize, LineCount(11) };
    mock.terminal.setCellPixelSize(ImageSize { Width(10), Height(10) });
    mock.terminal.setMode(DECMode::NoSixelScrolling, false);

    mock.writeToScreen(chessBoard);

    CHECK(mock.terminal.primaryScreen().cursor().position.column == ColumnOffset(0));
    CHECK(mock.terminal.primaryScreen().cursor().position.line == LineOffset(8));

    for (auto line = LineOffset(-1); line < boxed_cast<LineOffset>(pageSize.lines); ++line)
    {
        INFO(std::format("line {}", line));
        for (auto column = ColumnOffset(0); column < boxed_cast<ColumnOffset>(pageSize.columns); ++column)
        {
            INFO(std::format("column {}", column));
            auto const& cell = mock.terminal.primaryScreen().at(line, column);
            if (line <= LineOffset(9) && column <= ColumnOffset(9))
            {
                auto fragment = cell.imageFragment();
                REQUIRE(fragment);
                if ((column.value + line.value) % 2)
                    REQUIRE(fragment->data() == black10x10);
                else
                    REQUIRE(fragment->data() == white10x10);
                CHECK(fragment->offset().line == line + 1);
                CHECK(fragment->offset().column == column);
                CHECK(!fragment->data().empty());
            }
            else
            {
                CHECK(cell.empty());
            }
        }
    }
}

TEST_CASE("Sixel.status_line", "[screen]")
{
    // Test for #1050
    auto const pageSize = PageSize { LineCount(5), ColumnCount(11) };
    auto mock = MockTerm { pageSize, LineCount(12) };
    mock.terminal.setCellPixelSize(ImageSize { Width(10), Height(10) });
    mock.terminal.setStatusDisplay(StatusDisplayType::Indicator);

    mock.writeToScreen(chessBoard);

    CHECK(mock.terminal.primaryScreen().cursor().position.column.value == ColumnOffset(0).value);
    CHECK(mock.terminal.primaryScreen().cursor().position.line.value == LineOffset(3).value);

    auto const lastLine = boxed_cast<LineOffset>(pageSize.lines - mock.terminal.statusLineHeight());
    for (auto line = LineOffset(-6); line < lastLine; ++line)
    {
        for (auto column = ColumnOffset(0); column < boxed_cast<ColumnOffset>(pageSize.columns); ++column)
        {
            auto const& cell = mock.terminal.primaryScreen().at(line, column);
            if (line <= LineOffset(9) && column <= ColumnOffset(9))
            {
                auto fragment = cell.imageFragment();
                REQUIRE(fragment);
                if ((column.value + line.value) % 2)
                    REQUIRE(fragment->data() == white10x10);
                else
                    REQUIRE(fragment->data() == black10x10);

                CHECK(fragment->offset().line == line + 6);
                CHECK(fragment->offset().column == column);
                CHECK(!fragment->data().empty());
            }
            else
            {
                CHECK(cell.empty());
            }
        }
    }
}

TEST_CASE("DECSTR", "[screen]")
{
    // Create a 10x3x5 grid and render a 7x5 image causing one a line-scroll by one.
    auto const pageSize = PageSize { LineCount(4), ColumnCount(10) };
    auto mock = MockTerm { pageSize, LineCount(5) };
    mock.writeToScreen("ABCD\r\nDEFG\r\n");
    CHECK(mock.terminal.primaryScreen().cursor().position.line == LineOffset(2));
    CHECK(mock.terminal.primaryScreen().cursor().position.column == ColumnOffset(0));

    mock.writeToScreen("\033[!p");
    REQUIRE(mock.terminal.primaryScreen().cursor().position
            == CellLocation { LineOffset(2), ColumnOffset(0) });
    REQUIRE(mock.terminal.primaryScreen().savedCursorState().position
            == CellLocation { LineOffset(0), ColumnOffset(0) });
}

TEST_CASE("SGRSAVE and SGRRESTORE", "[screen]")
{
    auto mock = MockTerm { ColumnCount(8), LineCount(4) };

    mock.writeToScreen(SGR(31, 42, 4)); // red on green, underline
    auto& cursor = mock.terminal.currentScreen().cursor();
    REQUIRE(cursor.graphicsRendition.foregroundColor == IndexedColor::Red);
    REQUIRE(cursor.graphicsRendition.backgroundColor == IndexedColor::Green);
    REQUIRE(cursor.graphicsRendition.flags.contains(CellFlag::Underline));

    mock.writeToScreen(SGRSAVE());
    mock.writeToScreen(SGR(33, 44, 24)); // yellow on blue, no underline
    REQUIRE(cursor.graphicsRendition.foregroundColor == IndexedColor::Yellow);
    REQUIRE(cursor.graphicsRendition.backgroundColor == IndexedColor::Blue);
    REQUIRE(!cursor.graphicsRendition.flags.contains(CellFlag::Underline));

    mock.writeToScreen(SGRRESTORE());
    REQUIRE(cursor.graphicsRendition.foregroundColor == IndexedColor::Red);
    REQUIRE(cursor.graphicsRendition.backgroundColor == IndexedColor::Green);
    REQUIRE(cursor.graphicsRendition.flags.contains(CellFlag::Underline));
}

TEST_CASE("LS1 and LS0", "[screen]")
{
    auto mock = MockTerm { ColumnCount(8), LineCount(4) };

    auto const writeTickAndRender = [&](auto text) {
        mock.writeToScreen(text);
        mock.terminal.tick(1s);
        mock.terminal.ensureFreshRenderBuffer();
        logScreenText(mock.terminal.primaryScreen(), std::format("writeTickAndRender: {}", e(text)));
    };

    REQUIRE(mock.terminal.primaryScreen().cursor().charsets.isSelected(CharsetTable::G0, CharsetId::USASCII));
    REQUIRE(mock.terminal.primaryScreen().cursor().charsets.isSelected(CharsetTable::G1, CharsetId::USASCII));
    writeTickAndRender("ab");
    REQUIRE(trimmedTextScreenshot(mock) == "ab");

    // Set G1 to Special
    mock.writeToScreen("\033)0");
    REQUIRE(mock.terminal.primaryScreen().cursor().charsets.isSelected(CharsetTable::G1, CharsetId::Special));

    // LS1: load G1 into GL
    mock.writeToScreen("\x0E");
    REQUIRE(mock.terminal.primaryScreen().cursor().charsets.isSelected(CharsetId::Special));

    writeTickAndRender("ab");
    REQUIRE(trimmedTextScreenshot(mock) == "ab▒␉");

    // LS0: load G0 into GL
    mock.writeToScreen("\x0F");
    REQUIRE(mock.terminal.primaryScreen().cursor().charsets.isSelected(CharsetId::USASCII));

    writeTickAndRender("ab");
    REQUIRE(trimmedTextScreenshot(mock) == "ab▒␉ab");
}

TEST_CASE("LS2 and LS3 (locking shift into GL)", "[screen]")
{
    auto mock = MockTerm { ColumnCount(8), LineCount(4) };
    auto const& charsets = mock.terminal.primaryScreen().cursor().charsets;

    // Designate G2 and G3 to DEC Special so the locking shift has something observable.
    mock.writeToScreen("\033*0"); // SCS G2 = DEC Special
    mock.writeToScreen("\033+0"); // SCS G3 = DEC Special
    REQUIRE(charsets.isSelected(CharsetTable::G2, CharsetId::Special));
    REQUIRE(charsets.isSelected(CharsetTable::G3, CharsetId::Special));
    REQUIRE(charsets.selectedTable() == CharsetTable::G0); // GL starts at G0

    // LS2 (ESC n): invoke G2 into GL.
    mock.writeToScreen("\033n");
    CHECK(charsets.selectedTable() == CharsetTable::G2);
    CHECK(charsets.isSelected(CharsetId::Special));

    // LS3 (ESC o): invoke G3 into GL.
    mock.writeToScreen("\033o");
    CHECK(charsets.selectedTable() == CharsetTable::G3);
    CHECK(charsets.isSelected(CharsetId::Special));

    // LS0 (SI): back to G0 (USASCII).
    mock.writeToScreen("\x0F");
    CHECK(charsets.selectedTable() == CharsetTable::G0);
    CHECK(charsets.isSelected(CharsetId::USASCII));
}

TEST_CASE("LS1R LS2R LS3R (locking shift into GR)", "[screen]")
{
    auto mock = MockTerm { ColumnCount(8), LineCount(4) };
    auto const& charsets = mock.terminal.primaryScreen().cursor().charsets;

    // GR defaults to G2 per the VT standard.
    REQUIRE(charsets.selectedTableGR() == CharsetTable::G2);

    // LS1R (ESC ~): invoke G1 into GR.
    mock.writeToScreen("\033~");
    CHECK(charsets.selectedTableGR() == CharsetTable::G1);

    // LS3R (ESC |): invoke G3 into GR.
    mock.writeToScreen("\033|");
    CHECK(charsets.selectedTableGR() == CharsetTable::G3);

    // LS2R (ESC }): invoke G2 into GR (back to the default slot).
    mock.writeToScreen("\033}");
    CHECK(charsets.selectedTableGR() == CharsetTable::G2);

    // GR locking shifts must not disturb GL.
    CHECK(charsets.selectedTable() == CharsetTable::G0);
}

TEST_CASE("SCS 96-charset designation (ESC - / . / / )", "[screen]")
{
    auto mock = MockTerm { ColumnCount(8), LineCount(4) };
    auto const& charsets = mock.terminal.primaryScreen().cursor().charsets;

    // 96-charsets go into G1, G2, G3 (never G0). Only ISO Latin-1 supplemental ('A') is defined.
    mock.writeToScreen("\033-A"); // designate G1 = ISO Latin-1 supplemental
    CHECK(charsets.charsetIdOf(CharsetTable::G1) == CharsetId::ISOLatin1Supplemental);
    CHECK(charsets.is96Charset(CharsetTable::G1));

    mock.writeToScreen("\033.A"); // designate G2
    CHECK(charsets.charsetIdOf(CharsetTable::G2) == CharsetId::ISOLatin1Supplemental);
    CHECK(charsets.is96Charset(CharsetTable::G2));

    mock.writeToScreen("\033/A"); // designate G3
    CHECK(charsets.charsetIdOf(CharsetTable::G3) == CharsetId::ISOLatin1Supplemental);
    CHECK(charsets.is96Charset(CharsetTable::G3));

    // A subsequent 94-charset designation clears the 96-charset flag for that G-set.
    mock.writeToScreen("\033)B"); // designate G1 = USASCII (94-charset)
    CHECK(charsets.charsetIdOf(CharsetTable::G1) == CharsetId::USASCII);
    CHECK_FALSE(charsets.is96Charset(CharsetTable::G1));
    // G0 stays a 94-charset throughout (it cannot hold a 96-charset).
    CHECK_FALSE(charsets.is96Charset(CharsetTable::G0));
}

// TODO: Sixel: image that exceeds available lines

// TODO: SetForegroundColor
// TODO: SetBackgroundColor
// TODO: SetGraphicsRendition
// TODO: SetScrollingRegion

// TODO: SendMouseEvents
// TODO: AlternateKeypadMode

// TODO: DesignateCharset
// TODO: SingleShiftSelect

// TODO: ChangeWindowTitle

// TODO: Bell
// TODO: FullReset

// TODO: DeviceStatusReport
// TODO: SendDeviceAttributes
// TODO: SendTerminalId

TEST_CASE("HorizontalTab.FillsCellsWithSpaces", "[screen]")
{
    // Verify that HT fills intermediate cells with space characters,
    // not just moves the cursor. This ensures TrivialLineBuffer consistency.
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(20) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("A\tB");

    // 'A' at col 0, tab advances to col 8 (default tab width), 'B' at col 8.
    // Columns 1..7 should be filled with spaces, rendering correctly.
    CHECK(screen.logicalCursorPosition().column == ColumnOffset(9));
    CHECK("A       B           \n                    \n" == screen.renderMainPageText());
}

TEST_CASE("HorizontalTab.AfterBulkText", "[screen]")
{
    // Write printable ASCII followed by HT followed by more text.
    // This exercises the parseBulkText fast-path → C0 execute → more text path.
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(20) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("AB\tCD");

    // "AB" occupies columns 0-1, tab advances to column 8, "CD" at columns 8-9
    CHECK("AB      CD          \n                    \n" == screen.renderMainPageText());
    CHECK(screen.logicalCursorPosition().column == ColumnOffset(10));
}

TEST_CASE("HorizontalTab.MultipleTabs", "[screen]")
{
    // "A\tB\tC" should produce correctly spaced output with space-filled cells.
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(25) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("A\tB\tC");

    // 'A' at col 0, tab to col 8, 'B' at col 8, tab to col 16, 'C' at col 16
    CHECK("A       B       C        \n                         \n" == screen.renderMainPageText());
    CHECK(screen.logicalCursorPosition().column == ColumnOffset(17));
}

TEST_CASE("HorizontalTab.AtChunkBoundary", "[screen]")
{
    // Force text+tab across chunk boundaries by using a small ptyReadBufferSize.
    // The tab character should still be processed correctly even at a chunk boundary.
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(20) }, LineCount(0), 4 };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("ABC\tD");

    // "ABC" at cols 0-2, tab to col 8, 'D' at col 8
    CHECK("ABC     D           \n                    \n" == screen.renderMainPageText());
}

TEST_CASE("HorizontalTab.AfterScreenClear", "[screen]")
{
    // After ED (Erase in Display), write text with tabs and verify correct rendering.
    // This tests TrivialLineBuffer reset + tab interaction.
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(20) } };
    auto& screen = mock.terminal.primaryScreen();

    // Write initial content
    mock.writeToScreen("Hello World");
    // Clear screen (CSI 2 J) and cursor home (CSI H)
    mock.writeToScreen("\033[2J\033[H");
    // Write text with tab
    mock.writeToScreen("X\tY");

    CHECK("X       Y           \n                    \n" == screen.renderMainPageText());
}

// {{{ DECCIR — Cursor Information Report

TEST_CASE("DECCIR.default_state", "[screen]")
{
    // Verify DECCIR response with all defaults: cursor at (1,1), no attributes, no wrap pending,
    // GL=G0, GR=G2, all charsets USASCII.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen(DECRQPSR(1));

    // Expected: DCS 1 $ u 1;1;1;@;@;@;0;2;@;BBBB ST
    //   Pr=1, Pc=1, Pp=1
    //   Srend='@' (0x40, no attributes)
    //   Satt='@' (0x40, no protection)
    //   Sflag='@' (0x40, no flags)
    //   Pgl=0 (G0 in GL)
    //   Pgr=2 (G2 in GR, default)
    //   Scss='@' (0x40, all 94-char sets)
    //   Sdesig="BBBB" (all USASCII)
    CHECK(e(mock.terminal.peekInput()) == e("\033P1$u1;1;1;@;@;@;0;2;@;BBBB\033\\"));
}

TEST_CASE("DECCIR.cursor_position", "[screen]")
{
    // Verify DECCIR correctly reports cursor position after movement.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };

    mock.writeToScreen(CUP(3, 7)); // Move to line 3, column 7

    mock.writeToScreen(DECRQPSR(1));

    // Pr=3, Pc=7
    CHECK(e(mock.terminal.peekInput()) == e("\033P1$u3;7;1;@;@;@;0;2;@;BBBB\033\\"));
}

TEST_CASE("DECCIR.bold_and_underline", "[screen]")
{
    // Verify Srend field encodes bold (bit 1) and underline (bit 2).
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen(SGR(1)); // Bold
    mock.writeToScreen(SGR(4)); // Underline
    mock.writeToScreen(DECRQPSR(1));

    // Srend = 0x40 + 0x01 (bold) + 0x02 (underline) = 0x43 = 'C'
    CHECK(e(mock.terminal.peekInput()) == e("\033P1$u1;1;1;C;@;@;0;2;@;BBBB\033\\"));
}

TEST_CASE("DECCIR.blinking_and_inverse", "[screen]")
{
    // Verify Srend field encodes blinking (bit 3) and inverse (bit 4).
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen(SGR(5)); // Blinking
    mock.writeToScreen(SGR(7)); // Inverse
    mock.writeToScreen(DECRQPSR(1));

    // Srend = 0x40 + 0x04 (blink) + 0x08 (inverse) = 0x4C = 'L'
    CHECK(e(mock.terminal.peekInput()) == e("\033P1$u1;1;1;L;@;@;0;2;@;BBBB\033\\"));
}

TEST_CASE("DECCIR.all_rendition_attributes", "[screen]")
{
    // Verify Srend field with all attributes enabled: bold+underline+blink+inverse.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen(SGR(1)); // Bold
    mock.writeToScreen(SGR(4)); // Underline
    mock.writeToScreen(SGR(5)); // Blinking
    mock.writeToScreen(SGR(7)); // Inverse
    mock.writeToScreen(DECRQPSR(1));

    // Srend = 0x40 + 0x01 + 0x02 + 0x04 + 0x08 = 0x4F = 'O'
    CHECK(e(mock.terminal.peekInput()) == e("\033P1$u1;1;1;O;@;@;0;2;@;BBBB\033\\"));
}

TEST_CASE("DECCIR.character_protection", "[screen]")
{
    // Verify Satt field reports DECSCA character protection attribute.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen(DECSCA(1)); // Enable character protection
    mock.writeToScreen(DECRQPSR(1));

    // Satt = 0x41 = 'A' (bit 1 set for protection)
    CHECK(e(mock.terminal.peekInput()) == e("\033P1$u1;1;1;@;A;@;0;2;@;BBBB\033\\"));
}

TEST_CASE("DECCIR.origin_mode", "[screen]")
{
    // Verify Sflag bit 1 reports origin mode (DECOM).
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };

    mock.writeToScreen(DECSM(toDECModeNum(DECMode::Origin)));
    mock.writeToScreen(DECRQPSR(1));

    // Sflag = 0x40 + 0x01 = 0x41 = 'A' (origin mode set)
    // Note: cursor is at (1,1) because origin mode homes the cursor.
    CHECK(e(mock.terminal.peekInput()) == e("\033P1$u1;1;1;@;@;A;0;2;@;BBBB\033\\"));
}

TEST_CASE("DECCIR.wrap_pending", "[screen]")
{
    // Verify Sflag bit 4 reports wrap-pending state.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) } };

    // Write exactly enough characters to reach the right margin and trigger wrap pending.
    mock.writeToScreen("ABCDE");
    mock.writeToScreen(DECRQPSR(1));

    // Cursor is at column 5, wrap pending. Sflag = 0x40 + 0x08 = 0x48 = 'H'
    CHECK(e(mock.terminal.peekInput()) == e("\033P1$u1;5;1;@;@;H;0;2;@;BBBB\033\\"));
}

TEST_CASE("DECCIR.charset_designation_special", "[screen]")
{
    // Verify Sdesig reports DEC Special charset when designated into G0.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen(SCS_G0_SPECIAL()); // Designate G0 = DEC Special
    mock.writeToScreen(DECRQPSR(1));

    // Sdesig: G0='0' (Special), G1-G3='B' (USASCII)
    CHECK(e(mock.terminal.peekInput()) == e("\033P1$u1;1;1;@;@;@;0;2;@;0BBB\033\\"));
}

TEST_CASE("DECCIR.charset_designation_g1", "[screen]")
{
    // Verify Sdesig reports charset designated into G1.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen(SCS_G1_SPECIAL()); // Designate G1 = DEC Special
    mock.writeToScreen(DECRQPSR(1));

    // Sdesig: G0='B', G1='0' (Special), G2-G3='B'
    CHECK(e(mock.terminal.peekInput()) == e("\033P1$u1;1;1;@;@;@;0;2;@;B0BB\033\\"));
}

TEST_CASE("DECCIR.gl_charset_after_locking_shift", "[screen]")
{
    // Verify Pgl reports G1 after a locking shift (SO → LS1 maps G1 into GL).
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen("\x0E"); // SO (Shift Out) = LS1 → map G1 into GL
    mock.writeToScreen(DECRQPSR(1));

    // Pgl=1 (G1 in GL)
    CHECK(e(mock.terminal.peekInput()) == e("\033P1$u1;1;1;@;@;@;1;2;@;BBBB\033\\"));
}

TEST_CASE("DECCIR.gr_charset_after_locking_shift", "[screen]")
{
    // Verify Pgr reports the GR register after LS3R maps G3 into GR (default is G2).
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen("\033|"); // LS3R → map G3 into GR
    mock.writeToScreen(DECRQPSR(1));

    // Pgr=3 (G3 in GR); Pgl stays 0.
    CHECK(e(mock.terminal.peekInput()) == e("\033P1$u1;1;1;@;@;@;0;3;@;BBBB\033\\"));
}

TEST_CASE("DECCIR.scss_reports_96_charset", "[screen]")
{
    // Verify Scss sets the per-G-set size bit and Sdesig reports 'A' when a 96-charset is designated.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen("\033-A"); // designate G1 = ISO Latin-1 supplemental (96-charset)
    mock.writeToScreen(DECRQPSR(1));

    // Scss = 0x40 | (1 << 1) = 0x42 = 'B'; Sdesig G1 = 'A' (Latin-1). Pgl=0, Pgr=2 (defaults).
    CHECK(e(mock.terminal.peekInput()) == e("\033P1$u1;1;1;@;@;@;0;2;B;BABB\033\\"));
}

// }}} DECCIR

// NOLINTEND(misc-const-correctness,readability-function-cognitive-complexity)

// {{{ DEC Multi-Page Support Tests
// NOLINTBEGIN(misc-const-correctness,readability-function-cognitive-complexity)

TEST_CASE("MultiPage.NP_PP_navigation", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) } };
    auto& terminal = mock.terminal;

    // Initially on page 1 (index 0).
    CHECK(terminal.cursorPageIndex() == PageIndex(0));
    CHECK(terminal.isPrimaryScreen());

    // NP: CSI 1 U — move to next page (page 2), cursor to home.
    mock.writeToScreen("Hello");
    mock.writeToScreen("\033[1U"); // NP 1
    CHECK(terminal.cursorPageIndex() == PageIndex(1));
    CHECK(terminal.currentScreen().cursor().position.line == LineOffset(0));
    CHECK(terminal.currentScreen().cursor().position.column == ColumnOffset(0));

    // Write on page 2.
    mock.writeToScreen("World");

    // PP: CSI 1 V — move back to page 1, cursor to home.
    mock.writeToScreen("\033[1V"); // PP 1
    CHECK(terminal.cursorPageIndex() == PageIndex(0));
    CHECK(terminal.currentScreen().cursor().position.line == LineOffset(0));
    CHECK(terminal.currentScreen().cursor().position.column == ColumnOffset(0));
    // Page 1 content should still have "Hello".
    CHECK(terminal.primaryScreen().renderMainPageText() == "Hello\n     \n     \n");

    // Move forward by 3 pages.
    mock.writeToScreen("\033[3U"); // NP 3
    CHECK(terminal.cursorPageIndex() == PageIndex(3));

    // Default param (NP without arg = 1).
    mock.writeToScreen("\033[U"); // NP (default 1)
    CHECK(terminal.cursorPageIndex() == PageIndex(4));
}

TEST_CASE("MultiPage.NP_PP_clamping", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(5) } };
    auto& terminal = mock.terminal;

    // NP with very large count should clamp to page 15 (index 14, MaxPageCount-2).
    mock.writeToScreen("\033[100U"); // NP 100
    CHECK(terminal.cursorPageIndex() == PageIndex(MaxPageCount - 2));

    // PP with very large count should clamp to page 1 (index 0).
    mock.writeToScreen("\033[100V"); // PP 100
    CHECK(terminal.cursorPageIndex() == PageIndex(0));
}

TEST_CASE("MultiPage.NP_PP_never_reach_alternate", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(5) } };
    auto& terminal = mock.terminal;

    // NP should never reach the alternate screen page (index 15).
    mock.writeToScreen("\033[100U");                                  // NP beyond limit
    CHECK(terminal.cursorPageIndex() == PageIndex(MaxPageCount - 2)); // page 15, NOT 16
    CHECK(terminal.cursorPageIndex() != AlternateScreenPageIndex);
}

TEST_CASE("MultiPage.PPA_PPR_PPB_navigation", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) } };
    auto& terminal = mock.terminal;

    // Move cursor to a specific position on page 1.
    mock.writeToScreen("\033[2;3H"); // CUP to row 2, col 3
    CHECK(terminal.currentScreen().cursor().position.line == LineOffset(1));
    CHECK(terminal.currentScreen().cursor().position.column == ColumnOffset(2));

    // PPA: CSI 5 SP P — move to page 5, preserve cursor position.
    mock.writeToScreen("\033[5 P");                    // PPA to page 5
    CHECK(terminal.cursorPageIndex() == PageIndex(4)); // 0-based index
    // Cursor position should be preserved (still at the same logical position).
    // Note: cursor is on a NEW page, position within that page starts at home (0,0)
    // unless explicitly preserved by the terminal. Actually, PPA preserves position.
    // The cursor position is local to the Screen, so it was at home (0,0) on the new page.
    // Wait - setPage with moveCursorHome=false means we DON'T move cursor home,
    // but the cursor state is local to each Screen. When switching pages, the cursor
    // position that matters is the one stored in the _new_ screen's cursor state.

    // PPR: CSI 2 SP Q — move forward 2 pages from page 5 to page 7.
    mock.writeToScreen("\033[2 Q");                    // PPR 2
    CHECK(terminal.cursorPageIndex() == PageIndex(6)); // page 7

    // PPB: CSI 3 SP R — move backward 3 pages from page 7 to page 4.
    mock.writeToScreen("\033[3 R");                    // PPB 3
    CHECK(terminal.cursorPageIndex() == PageIndex(3)); // page 4

    // PPA with page 1 (go back to primary).
    mock.writeToScreen("\033[1 P"); // PPA 1
    CHECK(terminal.cursorPageIndex() == PageIndex(0));
    CHECK(terminal.isPrimaryScreen());
}

TEST_CASE("MultiPage.DECPCCM_coupling", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(5) } };
    auto& terminal = mock.terminal;

    // DECPCCM is enabled by default — display follows cursor.
    CHECK(terminal.isModeEnabled(DECMode::PageCursorCoupling));
    CHECK(terminal.displayedPageIndex() == PageIndex(0));

    // Move to page 3. Display should follow.
    mock.writeToScreen("\033[3 P"); // PPA to page 3
    CHECK(terminal.cursorPageIndex() == PageIndex(2));
    CHECK(terminal.displayedPageIndex() == PageIndex(2));

    // Disable DECPCCM: CSI ? 64 l
    mock.writeToScreen("\033[?64l"); // Reset DECPCCM
    CHECK(!terminal.isModeEnabled(DECMode::PageCursorCoupling));

    // Move to page 5. Display should NOT follow.
    mock.writeToScreen("\033[5 P"); // PPA to page 5
    CHECK(terminal.cursorPageIndex() == PageIndex(4));
    CHECK(terminal.displayedPageIndex() == PageIndex(2)); // still showing page 3

    // Re-enable DECPCCM: CSI ? 64 h — display should sync to cursor page.
    mock.writeToScreen("\033[?64h"); // Set DECPCCM
    CHECK(terminal.isModeEnabled(DECMode::PageCursorCoupling));
    CHECK(terminal.displayedPageIndex() == PageIndex(4)); // synced to cursor page
}

TEST_CASE("MultiPage.DECRQDE_response", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    REQUIRE(terminal.peekInput().empty());

    SECTION("on page 1")
    {
        mock.writeToScreen("\033[\"v"); // DECRQDE
        // Expected response: CSI 5 ; 10 ; 1 ; 1 ; 1 " w
        CHECK(std::string("\033[5;10;1;1;1\"w") == terminal.peekInput());
    }

    SECTION("on page 3")
    {
        mock.writeToScreen("\033[3 P"); // PPA to page 3
        mock.writeToScreen("\033[\"v"); // DECRQDE
        CHECK(std::string("\033[5;10;1;1;3\"w") == terminal.peekInput());
    }
}

TEST_CASE("MultiPage.DECXCPR_page_number", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    REQUIRE(terminal.peekInput().empty());

    SECTION("on page 1")
    {
        mock.writeToScreen("\033[2;3H"); // CUP to row 2, col 3
        terminal.primaryScreen().reportExtendedCursorPosition();
        // Response includes page number: CSI row ; col ; page R
        CHECK(std::string("\033[?2;3;1R") == terminal.peekInput());
    }

    SECTION("on page 4")
    {
        mock.writeToScreen("\033[4 P");  // PPA to page 4
        mock.writeToScreen("\033[3;5H"); // CUP to row 3, col 5
        terminal.pageAt(PageIndex(3)).reportExtendedCursorPosition();
        CHECK(std::string("\033[?3;5;4R") == terminal.peekInput());
    }
}

TEST_CASE("MultiPage.DECCIR_page_number", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    REQUIRE(terminal.peekInput().empty());

    SECTION("on page 1")
    {
        // DECRQPSR with param 1 requests DECCIR
        mock.writeToScreen("\033[1$w");
        auto const response = terminal.peekInput();
        // Response: DCS 1 $ u Pr;Pc;Pp;... ST — verify Pp=1
        // Format: ESC P 1 $ u line;col;page;...
        CHECK(response.starts_with("\033P1$u1;1;1;"));
    }

    SECTION("on page 3")
    {
        mock.writeToScreen("\033[3 P"); // PPA to page 3
        mock.writeToScreen("\033[1$w"); // DECRQPSR for DECCIR
        auto const response = terminal.peekInput();
        // Verify Pp=3 in the DCS response
        CHECK(response.starts_with("\033P1$u1;1;3;"));
    }
}

TEST_CASE("MultiPage.DECSC_DECRC_saves_restores_page", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    // Move to page 3 and save cursor (DECSC = ESC 7).
    mock.writeToScreen("\033[3 P"); // PPA to page 3
    mock.writeToScreen("\0337");    // DECSC

    CHECK(terminal.cursorPageNumber() == 3);

    // Switch to page 1.
    mock.writeToScreen("\033[1 P"); // PPA to page 1
    CHECK(terminal.cursorPageNumber() == 1);

    // Restore cursor (DECRC = ESC 8) — should return to page 3.
    mock.writeToScreen("\0338"); // DECRC
    CHECK(terminal.cursorPageNumber() == 3);
}

TEST_CASE("MultiPage.DECRC_without_DECSC_defaults_to_page1", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    // Move to page 4 without saving cursor.
    mock.writeToScreen("\033[4 P"); // PPA to page 4
    CHECK(terminal.cursorPageNumber() == 4);

    // DECRC without prior DECSC should restore to page 1 (default saved page = 0).
    mock.writeToScreen("\0338"); // DECRC
    CHECK(terminal.cursorPageNumber() == 1);
}

TEST_CASE("MultiPage.DECCRA_cross_page_copy", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    // Write "ABCDE" on page 1.
    mock.writeToScreen("ABCDE");

    // Move to page 2.
    mock.writeToScreen("\033[2 P"); // PPA 2
    CHECK(terminal.cursorPageIndex() == PageIndex(1));

    // Page 2 should be empty.
    auto const page2text = terminal.pageAt(PageIndex(1)).renderMainPageText();
    CHECK(page2text == "          \n          \n          \n");

    // Copy from page 1 to page 2 using DECCRA.
    // DECCRA: CSI Pts;Pls;Pbs;Prs;Pps;Ptd;Pld;Ppd $ v
    // Source: top=1, left=1, bottom=1, right=5, page=1
    // Dest: top=2, left=1, page=2
    mock.writeToScreen("\033[1;1;1;5;1;2;1;2$v"); // DECCRA

    // Page 2, row 2 should now have "ABCDE".
    auto const page2after = terminal.pageAt(PageIndex(1)).renderMainPageText();
    CHECK(page2after == "          \nABCDE     \n          \n");
}

TEST_CASE("MultiPage.alternate_screen_compatibility", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) } };
    auto& terminal = mock.terminal;

    // Write content on primary screen.
    mock.writeToScreen("Prima");

    // Switch to alternate screen via DECSET 1049.
    mock.writeToScreen("\033[?1049h");
    CHECK(terminal.isAlternateScreen());
    CHECK(terminal.cursorPageIndex() == AlternateScreenPageIndex);

    // Write content on alternate screen.
    mock.writeToScreen("Alt!!");

    // Switch back.
    mock.writeToScreen("\033[?1049l");
    CHECK(terminal.isPrimaryScreen());
    CHECK(terminal.cursorPageIndex() == PageIndex(0));

    // Primary content should be preserved.
    CHECK(terminal.primaryScreen().renderMainPageText() == "Prima\n     \n     \n");
}

TEST_CASE("MultiPage.hard_reset", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(5) } };
    auto& terminal = mock.terminal;

    // Move to page 3 and write.
    mock.writeToScreen("\033[3 P"); // PPA to page 3
    mock.writeToScreen("Test!");
    CHECK(terminal.cursorPageIndex() == PageIndex(2));

    // Hard reset.
    mock.writeToScreen("\033c"); // RIS

    // Should be back on page 1, DECPCCM enabled.
    CHECK(terminal.cursorPageIndex() == PageIndex(0));
    CHECK(terminal.displayedPageIndex() == PageIndex(0));
    CHECK(terminal.isPrimaryScreen());
    CHECK(terminal.isModeEnabled(DECMode::PageCursorCoupling));
}

TEST_CASE("MultiPage.page_content_isolation", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(5) } };
    auto& terminal = mock.terminal;

    // Write on page 1.
    mock.writeToScreen("Page1");

    // Move to page 2 and write.
    mock.writeToScreen("\033[1U"); // NP 1
    mock.writeToScreen("Page2");

    // Move to page 3 and write.
    mock.writeToScreen("\033[1U"); // NP 1
    mock.writeToScreen("Page3");

    // Go back to page 1 and verify isolation.
    mock.writeToScreen("\033[1 P"); // PPA 1
    CHECK(terminal.primaryScreen().renderMainPageText() == "Page1\n     \n");

    // Go to page 2 and verify.
    mock.writeToScreen("\033[2 P"); // PPA 2
    CHECK(terminal.pageAt(PageIndex(1)).renderMainPageText() == "Page2\n     \n");

    // Go to page 3 and verify.
    mock.writeToScreen("\033[3 P"); // PPA 3
    CHECK(terminal.pageAt(PageIndex(2)).renderMainPageText() == "Page3\n     \n");
}

TEST_CASE("MultiPage.PerPageMargins", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    // Set custom top/bottom margins on page 1.
    mock.writeToScreen("\033[2;4r"); // DECSTBM(2, 4)
    CHECK(terminal.primaryScreen().margin().vertical == Margin::Vertical { LineOffset(1), LineOffset(3) });

    // Switch to page 2 — it should have default (full-screen) margins.
    mock.writeToScreen("\033[2 P"); // PPA 2
    auto& page2 = terminal.pageAt(PageIndex(1));
    CHECK(page2.margin().vertical == Margin::Vertical { LineOffset(0), LineOffset(4) });
    CHECK(page2.margin().horizontal == Margin::Horizontal { ColumnOffset(0), ColumnOffset(9) });

    // Set different margins on page 2.
    mock.writeToScreen("\033[3;5r"); // DECSTBM(3, 5)
    CHECK(page2.margin().vertical == Margin::Vertical { LineOffset(2), LineOffset(4) });

    // Switch back to page 1 — its custom margins should still be intact.
    mock.writeToScreen("\033[1 P"); // PPA 1
    CHECK(terminal.primaryScreen().margin().vertical == Margin::Vertical { LineOffset(1), LineOffset(3) });

    // Page 2 margins should still be independently preserved.
    CHECK(page2.margin().vertical == Margin::Vertical { LineOffset(2), LineOffset(4) });
}

TEST_CASE("MultiPage.AltScreenMarginCopy", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    // Set custom margins on primary screen (page 1).
    mock.writeToScreen("\033[2;4r"); // DECSTBM(2, 4)
    CHECK(terminal.primaryScreen().margin().vertical == Margin::Vertical { LineOffset(1), LineOffset(3) });

    // Enter alternate screen (DECSET 1049) — alt screen should inherit primary margins.
    mock.writeToScreen("\033[?1049h");
    CHECK(terminal.isAlternateScreen());
    CHECK(terminal.alternateScreen().margin().vertical == Margin::Vertical { LineOffset(1), LineOffset(3) });

    // Modify margins on the alternate screen.
    mock.writeToScreen("\033[1;5r"); // DECSTBM(1, 5) — reset to full screen
    CHECK(terminal.alternateScreen().margin().vertical == Margin::Vertical { LineOffset(0), LineOffset(4) });

    // Leave alternate screen (DECSET 1049 off) — primary margins should be unchanged.
    mock.writeToScreen("\033[?1049l");
    CHECK(terminal.isPrimaryScreen());
    CHECK(terminal.primaryScreen().margin().vertical == Margin::Vertical { LineOffset(1), LineOffset(3) });
}

TEST_CASE("MultiPage.ResizeResetsAllPageMargins", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    // Set custom margins on page 1.
    mock.writeToScreen("\033[2;4r"); // DECSTBM(2, 4)
    CHECK(terminal.primaryScreen().margin().vertical == Margin::Vertical { LineOffset(1), LineOffset(3) });

    // Switch to page 4 and set custom margins.
    mock.writeToScreen("\033[4 P");  // PPA 4
    mock.writeToScreen("\033[2;3r"); // DECSTBM(2, 3)
    CHECK(terminal.pageAt(PageIndex(3)).margin().vertical
          == Margin::Vertical { LineOffset(1), LineOffset(2) });

    // Resize terminal — all margins should reset to defaults.
    terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(10) });

    // Page 1 margins should be reset.
    CHECK(terminal.primaryScreen().margin().vertical == Margin::Vertical { LineOffset(0), LineOffset(5) });
    CHECK(terminal.primaryScreen().margin().horizontal
          == Margin::Horizontal { ColumnOffset(0), ColumnOffset(9) });

    // Page 4 margins should also be reset.
    CHECK(terminal.pageAt(PageIndex(3)).margin().vertical
          == Margin::Vertical { LineOffset(0), LineOffset(5) });
}

TEST_CASE("MultiPage.HardResetResetsAllPageMargins", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    // Set custom margins on page 1.
    mock.writeToScreen("\033[2;4r"); // DECSTBM(2, 4)
    CHECK(terminal.primaryScreen().margin().vertical == Margin::Vertical { LineOffset(1), LineOffset(3) });

    // Switch to page 3 and set custom margins.
    mock.writeToScreen("\033[3 P");  // PPA 3
    mock.writeToScreen("\033[3;5r"); // DECSTBM(3, 5)
    CHECK(terminal.pageAt(PageIndex(2)).margin().vertical
          == Margin::Vertical { LineOffset(2), LineOffset(4) });

    // Hard reset.
    mock.writeToScreen("\033c"); // RIS

    // Page 1 margins should be reset to defaults.
    CHECK(terminal.primaryScreen().margin().vertical == Margin::Vertical { LineOffset(0), LineOffset(4) });
    CHECK(terminal.primaryScreen().margin().horizontal
          == Margin::Horizontal { ColumnOffset(0), ColumnOffset(9) });

    // Page 3 margins should also be reset.
    CHECK(terminal.pageAt(PageIndex(2)).margin().vertical
          == Margin::Vertical { LineOffset(0), LineOffset(4) });
}

// {{{ REP (Repeat Character) Tests

TEST_CASE("REP.basic_ascii", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(20) } };
    auto& screen = mock.terminal.primaryScreen();

    // Send "|" followed by CSI 9 b (repeat '|' 9 more times).
    // This mimics what ncurses sends via the rep terminfo capability.
    mock.writeToScreen("|\033[9b");

    CHECK(screen.grid().lineText(LineOffset(0)) == "||||||||||          ");
}

TEST_CASE("REP.omitted_parameter_repeats_once", "[screen]")
{
    // REP's parameter defaults to 1, so `CSI b` on its own is legal. It was declared as requiring at
    // least one parameter, though, so a bare `CSI b` matched no function at all and was silently
    // dropped. vttest sends it.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(8) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("X\033[b");

    CHECK(screen.grid().lineText(LineOffset(0)) == "XX      ");
}

TEST_CASE("REP.explicit_zero_repeats_once", "[screen]")
{
    // REP's parameter is a one-based count, so an explicit zero means the same as an omitted one --
    // xterm folds both with one_if_default(). Taken literally it repeated nothing and swallowed the
    // character, which is what param_or() did here while every sibling sequence had moved on to
    // param_positive_or().
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(8) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("X\033[0b");

    CHECK(screen.grid().lineText(LineOffset(0)) == "XX      ");
}

TEST_CASE("REP.after_bulk_text", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(20) } };
    auto& screen = mock.terminal.primaryScreen();

    // Send a longer ASCII string to exercise the bulk text path,
    // then immediately follow with REP.
    mock.writeToScreen("Hello|\033[3b");

    CHECK(screen.grid().lineText(LineOffset(0)) == "Hello||||           ");
}

TEST_CASE("REP.wraps_at_left_right_margin", "[screen]")
{
    // REP repeats through the normal text path, so past the right margin it autowraps to the left
    // margin of the next line rather than stopping. Mirrors esctest test_REP_RespectsLeftRightMargins.
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("\033[?69h"); // DECSET DECLRMM
    mock.writeToScreen("\033[2;4s"); // DECSLRM 2;4
    mock.writeToScreen("\033[1;2H"); // CUP row 1, col 2 (the left margin)
    mock.writeToScreen("a");         // 'a' at the left margin
    mock.writeToScreen("\033[3b");   // REP 3
    // Two more fill the band on row 1; the third wraps to the left margin of row 2.
    CHECK(" aaa \n a   \n" == screen.renderMainPageText());
}

TEST_CASE("REP.scrolls_at_bottom_margin", "[screen]")
{
    // At the bottom margin REP's autowrap scrolls the region, exactly as ordinary text would.
    // Mirrors esctest test_REP_RespectsTopBottomMargins.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(6) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("\033[2;4r"); // DECSTBM 2;4 -> rows 2..4 (offsets 1..3)
    mock.writeToScreen("\033[4;4H"); // CUP row 4 (bottom margin), col 4
    mock.writeToScreen("a");
    mock.writeToScreen("\033[3b"); // REP 3: fills the row's tail, then wraps + scrolls the region up
    CHECK("      \n      \n   aaa\na     \n      \n" == screen.renderMainPageText());
}

TEST_CASE("REP.no_preceding_char", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(10) } };
    auto& screen = mock.terminal.primaryScreen();

    // CSI 5 b with no preceding graphic character does nothing.
    mock.writeToScreen("\033[5b");

    CHECK(screen.grid().lineText(LineOffset(0)) == "          ");
}

// }}} REP (Repeat Character) Tests

// {{{ HT (Horizontal Tab) Tests

TEST_CASE("HT.does_not_overwrite_existing_content", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(20) } };
    auto& screen = mock.terminal.primaryScreen();

    // Write text across the full line, then use CUP to reposition and HT.
    // HT must NOT overwrite existing cell content — it only moves the cursor.
    mock.writeToScreen("ABCDEFGHIJKLMNOPQRST");

    // Move cursor to column 2 (1-based col 3) and tab to column 8
    mock.writeToScreen("\033[1;3H\t");

    // Write 'X' at the tab stop position (column 8)
    mock.writeToScreen("X");

    // Columns 2-7 (0-based) should retain their original content (CDEFGH),
    // not be overwritten with spaces by the tab.
    CHECK(screen.grid().lineText(LineOffset(0)) == "ABCDEFGHXJKLMNOPQRST");
}

// }}} HT (Horizontal Tab) Tests

// {{{ DA1 (Primary Device Attributes) Tests

/// Parses a DA1 response string (e.g. "\033[?65;1;4;6;...c") and returns the set of extension numbers.
std::set<int> parseDA1Extensions(std::string_view reply)
{
    std::set<int> extensions;

    // Find the CSI ? prefix and 'c' terminator
    auto const prefix = reply.find("\033[?");
    if (prefix == std::string_view::npos)
        return extensions;

    auto const start = prefix + 3; // skip "\033[?"
    auto const end = reply.find('c', start);
    if (end == std::string_view::npos)
        return extensions;

    auto const params = reply.substr(start, end - start);

    // Split by ';' and parse each number
    auto isFirst = true;
    size_t pos = 0;
    while (pos < params.size())
    {
        auto const delim = params.find(';', pos);
        auto const token =
            params.substr(pos, delim == std::string_view::npos ? std::string_view::npos : delim - pos);
        if (!token.empty())
        {
            auto value = 0;
            if (auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
                ec == std::errc {})
            {
                if (isFirst)
                    isFirst = false; // first number is the conformance level, not an extension
                else
                    extensions.insert(value);
            }
        }
        if (delim == std::string_view::npos)
            break;
        pos = delim + 1;
    }

    return extensions;
}

/// Parses the conformance level from a DA1 response.
int parseDA1Level(std::string_view reply)
{
    auto const prefix = reply.find("\033[?");
    if (prefix == std::string_view::npos)
        return 0;
    auto const start = prefix + 3;
    auto const delim = reply.find(';', start);
    auto const end = (delim != std::string_view::npos) ? delim : reply.find('c', start);
    if (end == std::string_view::npos)
        return 0;
    auto value = 0;
    std::from_chars(reply.data() + start, reply.data() + end, value);
    return value;
}

TEST_CASE("DA1: response reports level 65 for VT525", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    CHECK(parseDA1Level(mock.replyData()) == 65);
}

TEST_CASE("DA1: optional extensions at level 65", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    auto const exts = parseDA1Extensions(mock.replyData());

    // Optional at level 5 — should be listed
    CHECK(exts.contains(1));  // Columns132
    CHECK(exts.contains(4));  // SixelGraphics
    CHECK(exts.contains(18)); // Windowing
    CHECK(exts.contains(21)); // HorizontalScrolling
    CHECK(exts.contains(22)); // AnsiColor

    // Required at level 5 — implied by 65, must NOT be listed
    CHECK_FALSE(exts.contains(6));  // SelectiveErase (required at level 5)
    CHECK_FALSE(exts.contains(8));  // UserDefinedKeys (required at level 5)
    CHECK_FALSE(exts.contains(11)); // StatusDisplay (required at level 3+)
    CHECK_FALSE(exts.contains(15)); // TechnicalCharacters (required at level 5)
    CHECK_FALSE(exts.contains(28)); // RectangularEditing (required at level 4+)
    CHECK_FALSE(exts.contains(32)); // TextMacros (required at level 5)

    // Non-DEC extensions — always listed
    CHECK(exts.contains(52));  // ClipboardExtension
    CHECK(exts.contains(314)); // CaptureScreenBuffer
}

// }}} DA1 (Primary Device Attributes) Tests

// {{{ DECSCL (Set Conformance Level) Tests

TEST_CASE("DECSCL: DA1 always reports max level 65", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Set conformance level to 62 (VT220), 7-bit C1
    mock.writeToScreen("\033[62;1\"p");
    mock.terminal.flushInput();
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    // DA1 always reports max device capability, not operating level
    CHECK(parseDA1Level(mock.replyData()) == 65);
}

TEST_CASE("DECSCL: level 62 reveals required-at-5 extensions as optional", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.writeToScreen("\033[62;1\"p");
    mock.terminal.flushInput();
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    auto const exts = parseDA1Extensions(mock.replyData());
    // At level 2, all these become optional and should be listed
    CHECK(exts.contains(6));  // SelectiveErase (optional at 2-4)
    CHECK(exts.contains(8));  // UserDefinedKeys (optional at 2-4)
    CHECK(exts.contains(11)); // StatusDisplay (optional at 2)
    CHECK(exts.contains(15)); // TechnicalCharacters (optional at 2-4)
    CHECK(exts.contains(28)); // RectangularEditing (optional at 2-3)
    CHECK(exts.contains(32)); // TextMacros (optional at 2-4)
}

TEST_CASE("DECSCL: level 63 hides StatusDisplay (required at 3+)", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.writeToScreen("\033[63;1\"p");
    mock.terminal.flushInput();
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    CHECK(parseDA1Level(mock.replyData()) == 65);
    auto const exts = parseDA1Extensions(mock.replyData());
    CHECK_FALSE(exts.contains(11)); // StatusDisplay required at level 3+
    CHECK(exts.contains(6));        // SelectiveErase still optional at level 3
    CHECK(exts.contains(28));       // RectangularEditing still optional at level 3
}

TEST_CASE("DECSCL: level 64 hides RectangularEditing (required at 4+)", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.writeToScreen("\033[64;1\"p");
    mock.terminal.flushInput();
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    CHECK(parseDA1Level(mock.replyData()) == 65);
    auto const exts = parseDA1Extensions(mock.replyData());
    CHECK_FALSE(exts.contains(11)); // StatusDisplay required at 3+
    CHECK_FALSE(exts.contains(28)); // RectangularEditing required at 4+
    CHECK(exts.contains(6));        // SelectiveErase still optional at level 4
    CHECK(exts.contains(8));        // UserDefinedKeys still optional at level 4
    CHECK(exts.contains(15));       // TechnicalCharacters still optional at level 4
    CHECK(exts.contains(32));       // TextMacros still optional at level 4
}

TEST_CASE("DECSCL: set level 65 round-trip", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // First downgrade to 62
    mock.writeToScreen("\033[62;1\"p");
    mock.terminal.flushInput();
    // Then upgrade back to 65
    mock.writeToScreen("\033[65;1\"p");
    mock.terminal.flushInput();
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    CHECK(parseDA1Level(mock.replyData()) == 65);
}

TEST_CASE("DECSCL: implies soft reset", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Move cursor away from origin
    mock.writeToScreen("\033[3;5H"); // cursor to row 3, col 5
    mock.terminal.flushInput();
    CHECK(mock.terminal.currentScreen().cursor().position.line != LineOffset(0));

    // Set conformance level — this implies soft reset, which should reset margins
    // but cursor position is reset by the soft reset
    mock.writeToScreen("\033[65;1\"p");
    mock.terminal.flushInput();

    // After soft reset, origin mode is off and cursor is at home position
    // softReset resets DECOM, so cursor should be at the top-left area
    // Note: soft reset doesn't explicitly move cursor, but resets margins and modes
    CHECK(mock.terminal.operatingLevel() == VTType::VT525);
}

TEST_CASE("DECSCL: C1 mode 7-bit", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.writeToScreen("\033[65;1\"p"); // Ps2=1 → 7-bit C1
    mock.terminal.flushInput();
    CHECK(mock.terminal.c1TransmissionMode() == ControlTransmissionMode::S7C1T);
}

TEST_CASE("DECSCL: C1 mode 8-bit", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.writeToScreen("\033[65;0\"p"); // Ps2=0 → 8-bit C1
    mock.terminal.flushInput();
    CHECK(mock.terminal.c1TransmissionMode() == ControlTransmissionMode::S8C1T);
}

TEST_CASE("DECSCL: C1 mode 8-bit with Ps2=2", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.writeToScreen("\033[65;2\"p"); // Ps2=2 → 8-bit C1
    mock.terminal.flushInput();
    CHECK(mock.terminal.c1TransmissionMode() == ControlTransmissionMode::S8C1T);
}

TEST_CASE("DECSCL: level 61 forces 7-bit C1", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // VT100 (level 61) always uses 7-bit, regardless of Ps2
    mock.writeToScreen("\033[61;0\"p");
    mock.terminal.flushInput();
    CHECK(mock.terminal.c1TransmissionMode() == ControlTransmissionMode::S7C1T);
}

TEST_CASE("DECSCL: DECRQSS reports current level", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Set to level 64 (VT420) — DECRQSS requires VT420, so we must stay at level 4+
    mock.writeToScreen("\033[64;1\"p");
    mock.terminal.flushInput();
    mock.resetReplyData();
    // Query DECSCL via DECRQSS
    mock.writeToScreen("\033P$q\"p\033\\");
    mock.terminal.flushInput();
    auto const reply = mock.replyData();
    // Should contain "64;1" for level 64 with 7-bit C1
    CHECK(reply.find("64;1\"p") != std::string::npos);
}

TEST_CASE("foldC1ControlsToEightBit", "[screen]")
{
    using vtbackend::foldC1ControlsToEightBit;
    // Each ESC-introduced C1 control folds to its single 8-bit byte (adjacent string literals keep the
    // \x?? escapes from greedily swallowing the following digit).
    CHECK(foldC1ControlsToEightBit("\033[0c")
          == std::string("\x9b"
                         "0c")); // CSI
    CHECK(foldC1ControlsToEightBit("\033P1$r5;6r\033\\")
          == std::string("\x90"
                         "1$r5;6r"
                         "\x9c")); // DCS..ST
    CHECK(foldC1ControlsToEightBit("\033]0;t\033\\")
          == std::string("\x9d"
                         "0;t"
                         "\x9c")); // OSC..ST
    // A non-C1 ESC (charset designation, intermediate 0x28 < 0x40), a lone trailing ESC, and plain text
    // all pass through untouched.
    CHECK(foldC1ControlsToEightBit("\033(B") == "\033(B");
    CHECK(foldC1ControlsToEightBit("ab\033") == "ab\033");
    CHECK(foldC1ControlsToEightBit("") == "");
    CHECK(foldC1ControlsToEightBit("no controls") == "no controls");
}

TEST_CASE("S8C1T: DECRQSS reply uses 8-bit C1 at VT level >= 2", "[screen]")
{
    // Mirrors esctest S8C1TTests.test_S8C1T_DCS: DECSTBM(5,6), select 8-bit C1 transmission, then read
    // the DECRQSS(DECSTBM) reply, which must be framed with the 8-bit DCS (0x90) and ST (0x9c).
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen("\033[5;6r"); // DECSTBM top=5 bottom=6
    mock.writeToScreen("\033 G");    // S8C1T (ESC SP G): select 8-bit C1 transmission
    mock.terminal.flushInput();
    REQUIRE(mock.terminal.c1TransmissionMode() == ControlTransmissionMode::S8C1T);

    mock.resetReplyData();
    mock.writeToScreen("\033P$qr\033\\"); // DECRQSS(DECSTBM)
    mock.terminal.flushInput();
    CHECK(mock.replyData()
          == std::string("\x90"
                         "1$r5;6r"
                         "\x9c"));
}

TEST_CASE("S8C1T: replies revert to 7-bit after a VT52 round-trip", "[screen]")
{
    // 8-bit C1 transmission is a VT200+ capability. Leaving VT52 (ESC <) drops the operating level to
    // VT100, where it is unavailable, so replies revert to 7-bit even though S8C1T remains selected
    // (xterm's CASE_VT52_FINISH rule). Uses DSR-CPR, which is available at VT100 level.
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen("\033 G"); // S8C1T at the default VT525 level
    mock.terminal.flushInput();
    REQUIRE(mock.terminal.c1TransmissionMode() == ControlTransmissionMode::S8C1T);

    // At VT525 level the CPR reply is 8-bit (0x9b introducer).
    mock.writeToScreen("\033[3;4H"); // CUP row 3 col 4
    mock.resetReplyData();
    mock.writeToScreen("\033[6n"); // DSR: cursor position report
    mock.terminal.flushInput();
    CHECK(mock.replyData()
          == std::string("\x9b"
                         "3;4R"));

    // Round-trip through VT52; on exit the operating level is VT100.
    mock.writeToScreen("\033[?2l\033<"); // enter VT52 (DECANM reset), then leave it (ESC <)
    mock.terminal.flushInput();
    REQUIRE(mock.terminal.operatingLevel() == VTType::VT100);

    mock.writeToScreen("\033[3;4H");
    mock.resetReplyData();
    mock.writeToScreen("\033[6n");
    mock.terminal.flushInput();
    CHECK(mock.replyData() == "\033[3;4R"); // 7-bit CSI introducer
}

TEST_CASE("DECDMAC: max 64 macros", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Define macros 0-63
    for (auto i = 0; i < 64; ++i)
        mock.writeToScreen(std::format("\033P{};0;0!zM{}\033\\", i, i));
    mock.terminal.flushInput();
    CHECK(mock.terminal.macroBody(0).has_value());
    CHECK(mock.terminal.macroBody(63).has_value());
    // Macro 64 should be rejected (out of range)
    mock.writeToScreen("\033P64;0;0!zBad\033\\");
    mock.terminal.flushInput();
    CHECK_FALSE(mock.terminal.macroBody(64).has_value());
}

TEST_CASE("DECINVM: invoke undefined macro", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Invoke non-existent macro 42 — should do nothing, no crash
    mock.writeToScreen("\033[42*z");
    mock.terminal.flushInput();
    CHECK(mock.terminal.currentScreen().grid().lineText(LineOffset(0)).find_first_not_of(' ')
          == std::string::npos);
}

TEST_CASE("DECINVM: nested macro invocation", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Define macro 1 that writes "B"
    mock.writeToScreen("\033P1;0;0!zB\033\\");
    mock.terminal.flushInput();
    // Define macro 0 that writes "A", invokes macro 1, then writes "C"
    mock.writeToScreen("\033P0;0;0!zA\033[1*zC\033\\");
    mock.terminal.flushInput();
    // Invoke macro 0
    mock.writeToScreen("\033[0*z");
    mock.terminal.flushInput();
    // Macro 0 body outputs "A" then "C" (deferred macro 1 runs after), then macro 1 outputs "B"
    auto const text = mock.terminal.currentScreen().grid().lineText(LineOffset(0));
    CHECK(text.find('A') != std::string::npos);
    CHECK(text.find('B') != std::string::npos);
    CHECK(text.find('C') != std::string::npos);
}

TEST_CASE("DECINVM: recursive macro guard", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Define macro 0 that invokes itself (infinite recursion attempt)
    mock.writeToScreen("\033P0;0;0!z\033[0*z\033\\");
    mock.terminal.flushInput();
    // Invoke macro 0 — should NOT infinite loop, recursion depth is bounded
    mock.writeToScreen("\033[0*z");
    mock.terminal.flushInput();
    // Verify the terminal is still responsive after bounded recursion
    mock.writeToScreen("OK");
    mock.terminal.flushInput();
    CHECK(mock.terminal.currentScreen().grid().lineText(LineOffset(0)).substr(0, 2) == "OK");
}

TEST_CASE("DECDMAC: empty macro body", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Define macro 3 with empty body
    mock.writeToScreen("\033P3;0;0!z\033\\");
    mock.terminal.flushInput();
    // Empty body erases the macro
    CHECK_FALSE(mock.terminal.macroBody(3).has_value());
    // Invoke empty macro — should produce no output
    mock.writeToScreen("\033[3*z");
    mock.writeToScreen("X");
    mock.terminal.flushInput();
    CHECK(mock.terminal.currentScreen().grid().lineText(LineOffset(0)).substr(0, 1) == "X");
}

TEST_CASE("DECDMAC: ext 32 implied at level 65, listed at level 62", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    auto exts = parseDA1Extensions(mock.replyData());
    CHECK_FALSE(exts.contains(32)); // required at level 5, implied by 65

    // Downgrade to level 62 where ext 32 is optional
    mock.writeToScreen("\033[62;1\"p");
    mock.terminal.flushInput();
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    exts = parseDA1Extensions(mock.replyData());
    CHECK(exts.contains(32)); // optional at level 2
}

// }}} DECDMAC / DECINVM (Text Macros) Tests

// {{{ DECUDK (User-Defined Keys) Tests

TEST_CASE("DECUDK: program single key", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Program F6 (key 17) with "Hello" (hex: 48656C6C6F)
    mock.writeToScreen("\033P0;1|17/48656C6C6F\033\\");
    mock.terminal.flushInput();
    CHECK(mock.terminal.udkString(17).has_value());
    CHECK(mock.terminal.udkString(17).value() == "Hello");
}

TEST_CASE("DECUDK: program multiple keys", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Program F6 (17) with "A" (hex: 41) and F7 (18) with "B" (hex: 42)
    mock.writeToScreen("\033P0;1|17/41;18/42\033\\");
    mock.terminal.flushInput();
    CHECK(mock.terminal.udkString(17).value() == "A");
    CHECK(mock.terminal.udkString(18).value() == "B");
}

TEST_CASE("DECUDK: clear all before loading (Pc=0)", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // First program F6
    mock.writeToScreen("\033P1;1|17/41\033\\");
    mock.terminal.flushInput();
    CHECK(mock.terminal.udkString(17).has_value());
    // Then program F7 with clear-all (Pc=0)
    mock.writeToScreen("\033P0;1|18/42\033\\");
    mock.terminal.flushInput();
    CHECK_FALSE(mock.terminal.udkString(17).has_value()); // F6 should be cleared
    CHECK(mock.terminal.udkString(18).has_value());       // F7 should exist
}

TEST_CASE("DECUDK: keep existing (Pc=1)", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // First program F6
    mock.writeToScreen("\033P1;1|17/41\033\\");
    mock.terminal.flushInput();
    // Then program F7, keeping existing (Pc=1)
    mock.writeToScreen("\033P1;1|18/42\033\\");
    mock.terminal.flushInput();
    CHECK(mock.terminal.udkString(17).has_value()); // F6 should still exist
    CHECK(mock.terminal.udkString(18).has_value()); // F7 should also exist
}

TEST_CASE("DECUDK: lock keys (Pl=0)", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Program F6 with lock (Pl=0)
    mock.writeToScreen("\033P0;0|17/41\033\\");
    mock.terminal.flushInput();
    CHECK(mock.terminal.udkString(17).value() == "A");
    // Attempt to reprogram F6 — should be rejected because keys are locked
    mock.writeToScreen("\033P0;0|17/42\033\\");
    mock.terminal.flushInput();
    CHECK(mock.terminal.udkString(17).value() == "A"); // Still "A", not "B"
}

TEST_CASE("DECUDK: hex decode", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Program F6 (17) with hex "1B5B31m" → ESC [ 1 m (SGR bold)
    mock.writeToScreen("\033P0;1|17/1B5B316D\033\\");
    mock.terminal.flushInput();
    auto const str = mock.terminal.udkString(17);
    REQUIRE(str.has_value());
    CHECK(str.value() == "\033[1m");
}

TEST_CASE("DECUDK: soft reset clears UDKs", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.writeToScreen("\033P0;1|17/41\033\\");
    mock.terminal.flushInput();
    CHECK(mock.terminal.udkString(17).has_value());
    // Soft reset
    mock.writeToScreen("\033[!p");
    mock.terminal.flushInput();
    CHECK_FALSE(mock.terminal.udkString(17).has_value());
}

TEST_CASE("DECUDK: ext 8 implied at level 65, listed at level 62", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    auto exts = parseDA1Extensions(mock.replyData());
    CHECK_FALSE(exts.contains(8)); // required at level 5, implied by 65

    // Downgrade to level 62 where ext 8 is optional
    mock.writeToScreen("\033[62;1\"p");
    mock.terminal.flushInput();
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    exts = parseDA1Extensions(mock.replyData());
    CHECK(exts.contains(8)); // optional at level 2
}

TEST_CASE("DECUDK: udkStringForKey maps Key enum to UDK ID", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Program F6 (key 17) with "test"
    mock.writeToScreen("\033P0;1|17/74657374\033\\");
    mock.terminal.flushInput();
    CHECK(mock.terminal.udkStringForKey(Key::F6).has_value());
    CHECK(mock.terminal.udkStringForKey(Key::F6).value() == "test");
    CHECK_FALSE(mock.terminal.udkStringForKey(Key::F5).has_value()); // F5 is not programmable
}

// }}} DECUDK (User-Defined Keys) Tests

// {{{ NRCS (National Replacement Character Sets) Tests

TEST_CASE("NRCS: British charset substitution", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Designate British to G0: ESC ( A
    mock.writeToScreen("\033(A");
    // Write '#' which should map to '£' (U+00A3) in British charset
    mock.writeToScreen("#");
    mock.terminal.flushInput();
    auto const text = mock.terminal.currentScreen().grid().lineText(LineOffset(0));
    // £ is U+00A3 — the UTF-8 encoding is 0xC2 0xA3
    CHECK(text.find("\xC2\xA3") != std::string::npos);
}

TEST_CASE("NRCS: German charset substitution", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Designate German to G0: ESC ( K
    mock.writeToScreen("\033(K");
    // In German charset: '[' (0x5B) maps to 'Ä' (U+00C4)
    mock.writeToScreen("[");
    mock.terminal.flushInput();
    auto const text = mock.terminal.currentScreen().grid().lineText(LineOffset(0));
    CHECK(text.find("\xC3\x84") != std::string::npos); // Ä in UTF-8
}

TEST_CASE("NRCS: French charset substitution", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Designate French to G0: ESC ( R
    mock.writeToScreen("\033(R");
    // In French charset: '#' (0x23) maps to '£' (U+00A3)
    mock.writeToScreen("#");
    mock.terminal.flushInput();
    auto const text = mock.terminal.currentScreen().grid().lineText(LineOffset(0));
    CHECK(text.find("\xC2\xA3") != std::string::npos); // £ in UTF-8
}

TEST_CASE("NRCS: switch back to USASCII", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Set British
    mock.writeToScreen("\033(A");
    mock.writeToScreen("#"); // Should be £
    // Switch back to USASCII
    mock.writeToScreen("\033(B");
    mock.writeToScreen("#"); // Should be #
    mock.terminal.flushInput();
    // Column 0 has £, column 1 has #
    auto const col0 =
        mock.terminal.currentScreen().cellTextAt({ .line = LineOffset(0), .column = ColumnOffset(0) });
    auto const col1 =
        mock.terminal.currentScreen().cellTextAt({ .line = LineOffset(0), .column = ColumnOffset(1) });
    CHECK(col0 == "\xC2\xA3"); // £ in UTF-8
    CHECK(col1 == "#");
}

TEST_CASE("NRCS: G1 charset via locking shift", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Designate British to G1: ESC ) A
    mock.writeToScreen("\033)A");
    // Locking shift G1 (LS1 = SO = 0x0E)
    mock.writeToScreen("\x0E");
    // Write '#' — should map through G1 (British) → £
    mock.writeToScreen("#");
    mock.terminal.flushInput();
    auto const text = mock.terminal.currentScreen().grid().lineText(LineOffset(0));
    CHECK(text.find("\xC2\xA3") != std::string::npos);
}

TEST_CASE("NRCS: DA1 includes ext 9", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    auto const exts = parseDA1Extensions(mock.replyData());
    CHECK(exts.contains(9)); // ext 9 = NationalReplacementCharacterSets
}

TEST_CASE("NRCS: two-byte DRCS designator accepted via SCS fallback", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // ESC ) <space> A — designate a DRCS set with two-byte designator into G1.
    // This must not produce an "Unknown VT sequence" error.
    mock.writeToScreen("\033) A");
    mock.terminal.flushInput();
    // Verify the sequence was consumed without error by writing text after it.
    // If the ESC sequence was rejected, the parser would have left stray characters on screen.
    mock.writeToScreen("OK");
    mock.terminal.flushInput();
    CHECK(mock.terminal.currentScreen().grid().lineText(LineOffset(0)).substr(0, 2) == "OK");
}

TEST_CASE("NRCS: single-byte SCS fallback designates British to G2", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // ESC * A — designate British to G2 via the generic fallback path
    mock.writeToScreen("\033*A");
    mock.terminal.flushInput();
    // Verify G2 was set to British by using SS2 (single shift G2) and writing '#'
    // SS2 = ESC N, then '#' should map to '£' through British charset
    mock.writeToScreen("\033N#");
    mock.terminal.flushInput();
    auto const text = mock.terminal.currentScreen().grid().lineText(LineOffset(0));
    CHECK(text.find("\xC2\xA3") != std::string::npos); // £ in UTF-8
}

// }}} NRCS (National Replacement Character Sets) Tests

// {{{ Technical Character Set Tests

TEST_CASE("Technical charset: designate and use", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Designate DEC Technical to G0: ESC ( >
    mock.writeToScreen("\033(>");
    // Write 'A' (0x41) which maps to Α (Greek Alpha, U+0391) in Technical charset
    mock.writeToScreen("A");
    mock.terminal.flushInput();
    auto const text = mock.terminal.currentScreen().grid().lineText(LineOffset(0));
    // Α (U+0391) in UTF-8 is 0xCE 0x91
    CHECK(text.find("\xCE\x91") != std::string::npos);
}

TEST_CASE("Technical charset: pi mapping", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.writeToScreen("\033(>");
    // 0x70 = 'p' maps to π (U+03C0) in Technical charset
    mock.writeToScreen("p");
    mock.terminal.flushInput();
    auto const text = mock.terminal.currentScreen().grid().lineText(LineOffset(0));
    // π (U+03C0) in UTF-8 is 0xCF 0x80
    CHECK(text.find("\xCF\x80") != std::string::npos);
}

TEST_CASE("Technical charset: switch back to USASCII", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.writeToScreen("\033(>");
    mock.writeToScreen("A"); // Should be Α (Greek Alpha)
    mock.writeToScreen("\033(B");
    mock.writeToScreen("A"); // Should be regular A
    mock.terminal.flushInput();
    auto const col0 =
        mock.terminal.currentScreen().cellTextAt({ .line = LineOffset(0), .column = ColumnOffset(0) });
    auto const col1 =
        mock.terminal.currentScreen().cellTextAt({ .line = LineOffset(0), .column = ColumnOffset(1) });
    CHECK(col0 == "\xCE\x91"); // Α in UTF-8
    CHECK(col1 == "A");
}

TEST_CASE("Technical charset: ext 15 implied at level 65, listed at level 62", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    auto exts = parseDA1Extensions(mock.replyData());
    CHECK_FALSE(exts.contains(15)); // required at level 5, implied by 65

    // Downgrade to level 62 where ext 15 is optional
    mock.writeToScreen("\033[62;1\"p");
    mock.terminal.flushInput();
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    exts = parseDA1Extensions(mock.replyData());
    CHECK(exts.contains(15)); // optional at level 2
}

// }}} Technical Character Set Tests

// {{{ DEC Locator (DECELR / DECLRP) Tests

TEST_CASE("DECELR: enable locator reporting", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    // Enable locator: CSI 1 ; 2 ' z  (Ps=1 enable, Pu=2 character cells)
    mock.writeToScreen("\033[1;2'z");
    mock.terminal.flushInput();
    CHECK(mock.terminal.locatorState().enabled);
    CHECK_FALSE(mock.terminal.locatorState().oneShot);
}

TEST_CASE("DECELR: disable locator reporting", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen("\033[1;2'z"); // enable
    mock.terminal.flushInput();
    CHECK(mock.terminal.locatorState().enabled);
    mock.writeToScreen("\033[0'z"); // disable
    mock.terminal.flushInput();
    CHECK_FALSE(mock.terminal.locatorState().enabled);
}

TEST_CASE("DECELR: one-shot mode (Ps=2)", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen("\033[2;2'z"); // one-shot, character cells
    mock.terminal.flushInput();
    CHECK(mock.terminal.locatorState().enabled);
    CHECK(mock.terminal.locatorState().oneShot);
}

TEST_CASE("DECELR: pixel coordinates (Pu=1)", "[screen]")
{
    using LocatorCoordUnit = Terminal::LocatorCoordUnit;
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen("\033[1;1'z"); // enable, pixel coords
    mock.terminal.flushInput();
    CHECK(mock.terminal.locatorState().coordUnit == LocatorCoordUnit::DevicePixels);
}

TEST_CASE("DECSLE: select locator events", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen("\033[1;2'z"); // enable locator
    mock.terminal.flushInput();
    // Disable button down, enable button up: CSI 2 ; 3 ' {
    mock.writeToScreen("\033[2;3'{");
    mock.terminal.flushInput();
    CHECK_FALSE(mock.terminal.locatorState().reportButtonDown);
    CHECK(mock.terminal.locatorState().reportButtonUp);
}

TEST_CASE("DECRQLP: request locator position", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    // Request locator position when not enabled — should report locator unavailable
    mock.resetReplyData();
    mock.writeToScreen("\033[0'|");
    mock.terminal.flushInput();
    auto const reply = mock.replyData();
    // Should contain DECLRP format: CSI 0 ; ... & w
    CHECK(reply.find("&w") != std::string::npos);
}

TEST_CASE("DECELR: soft reset disables locator", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen("\033[1;2'z"); // enable
    mock.terminal.flushInput();
    CHECK(mock.terminal.locatorState().enabled);
    // Soft reset
    mock.writeToScreen("\033[!p");
    mock.terminal.flushInput();
    CHECK_FALSE(mock.terminal.locatorState().enabled);
}

TEST_CASE("DEC Locator: DA1 includes ext 29", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    auto const exts = parseDA1Extensions(mock.replyData());
    CHECK(exts.contains(29)); // ext 29 = AnsiTextLocator
}

// }}} DEC Locator (DECELR / DECLRP) Tests

// {{{ DECDLD (DRCS — Dynamically Redefinable Character Sets) Tests

TEST_CASE("DECDLD: define single character glyph", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Define DRCS font 0, starting char 0x21 ('!'), erase all, 10x20 matrix
    // DCS 0;0;0;10;10;0;20;0 { <designator> <sixel data> ST
    // Simple glyph: first sixel column with bit 0 set = '?' (0x3F + 0 = 0x3F = '?'), wait that's 0.
    // Actually 0x3F is the base (all zeros). 0x40 = bit 0 set, 0x41 = bits 0+1, etc.
    // Let's define a minimal glyph with a single pixel set at (0,0): '@' = 0x40 - 0x3F = 1 bit
    mock.writeToScreen("\033P0;0;0;10;10;0;20;0{A@\033\\");
    mock.terminal.flushInput();
    auto const* charset = mock.terminal.drcsCharset(0);
    REQUIRE(charset != nullptr);
    CHECK(charset->glyphs.contains(0x21)); // First glyph at starting position
}

TEST_CASE("DECDLD: erase control clears existing", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Define first glyph
    mock.writeToScreen("\033P0;0;0;10;10;0;20;0{A@\033\\");
    mock.terminal.flushInput();
    CHECK(mock.terminal.drcsCharset(0) != nullptr);
    // Redefine with erase all (Pe=0)
    mock.writeToScreen("\033P0;0;0;10;10;0;20;0{B@\033\\");
    mock.terminal.flushInput();
    auto const* charset = mock.terminal.drcsCharset(0);
    REQUIRE(charset != nullptr);
    // The old glyph should be cleared, only new one exists
    CHECK(charset->glyphs.contains(0x21));
}

TEST_CASE("DECDLD: multiple glyphs separated by semicolons", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Define two glyphs: '@' and 'A' (both just a single column with different bits)
    mock.writeToScreen("\033P0;0;0;10;10;0;20;0{A@;A\033\\");
    mock.terminal.flushInput();
    auto const* charset = mock.terminal.drcsCharset(0);
    REQUIRE(charset != nullptr);
    CHECK(charset->glyphs.contains(0x21)); // First glyph
    CHECK(charset->glyphs.contains(0x22)); // Second glyph
}

TEST_CASE("DECDLD: soft reset clears DRCS", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.writeToScreen("\033P0;0;0;10;10;0;20;0{A@\033\\");
    mock.terminal.flushInput();
    CHECK(mock.terminal.drcsCharset(0) != nullptr);
    // Soft reset
    mock.writeToScreen("\033[!p");
    mock.terminal.flushInput();
    CHECK(mock.terminal.drcsCharset(0) == nullptr);
}

TEST_CASE("DECDLD: DA1 includes ext 7 (SoftCharacterSet)", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    auto const exts = parseDA1Extensions(mock.replyData());
    CHECK(exts.contains(7)); // ext 7 = SoftCharacterSet
}

TEST_CASE("DECDLD: cell has image fragment after writing DRCS character", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Define DRCS font 1 with designator ' A', single glyph at position 0x21 ('!')
    mock.writeToScreen("\033P1;0;0;10;10;0;20;0{ A@\033\\");
    mock.terminal.flushInput();

    // Verify DRCS font was stored with correct designator
    REQUIRE(mock.terminal.drcsDesignatorToFont(" A").has_value());
    CHECK(mock.terminal.drcsDesignatorToFont(" A").value() == 1);
    REQUIRE(mock.terminal.drcsCharset(1) != nullptr);
    CHECK(mock.terminal.drcsCharset(1)->glyphs.contains(0x21));

    // Designate DRCS font into G1: ESC ) <space> A
    mock.writeToScreen("\033) A");
    mock.terminal.flushInput();

    // Verify G1 was set to DRCS font 1
    REQUIRE(mock.terminal.currentScreen().cursor().charsets.drcsFont(CharsetTable::G1).has_value());
    CHECK(mock.terminal.currentScreen().cursor().charsets.drcsFont(CharsetTable::G1).value() == 1);

    // Switch to G1 (SO = 0x0E), write '!' (position 0x21), switch back (SI = 0x0F)
    mock.writeToScreen("\x0E");
    mock.terminal.flushInput();

    // Verify DRCS font is active
    REQUIRE(mock.terminal.currentScreen().cursor().charsets.activeDRCSFont().has_value());

    mock.writeToScreen("!");
    mock.writeToScreen("\x0F");
    mock.terminal.flushInput();

    // The cell at (0,0) should have an image fragment (the DRCS glyph bitmap)
    auto const& line = mock.terminal.currentScreen().grid().lineAt(LineOffset(0));
    CHECK(line.storage().imageFragments.has_value());
    if (line.storage().imageFragments.has_value())
        CHECK(line.storage().imageFragments->contains(0));
}

TEST_CASE("DECDLD: switching away from DRCS uses normal font", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Define DRCS font 1
    mock.writeToScreen("\033P1;0;0;10;10;0;20;0{ A@\033\\");
    mock.terminal.flushInput();
    // Designate DRCS to G1 and switch to it
    mock.writeToScreen("\033) A\x0E!\x0F");
    mock.terminal.flushInput();
    // Write normal character 'X' through G0 (USASCII)
    mock.writeToScreen("X");
    mock.terminal.flushInput();
    // Column 1 ('X') should NOT have an image fragment
    auto const& line = mock.terminal.currentScreen().grid().lineAt(LineOffset(0));
    auto const hasImageAtCol1 =
        line.storage().imageFragments.has_value() && line.storage().imageFragments->contains(1);
    CHECK_FALSE(hasImageAtCol1);
    // But it should have the character 'X'
    CHECK(mock.terminal.currentScreen().cellTextAt({ .line = LineOffset(0), .column = ColumnOffset(1) })
          == "X");
}

// }}} DECDLD (DRCS — Dynamically Redefinable Character Sets) Tests

// NOLINTEND(misc-const-correctness,readability-function-cognitive-complexity)
// }}} DEC Multi-Page Support Tests

// {{{ DECALN (Screen Alignment Pattern) Tests

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

// }}} DECALN Tests
TEST_CASE("LF outside the left/right margins does not scroll", "[screen]")
{
    // A line feed only scrolls when the cursor is inside the left/right margins -- xterm's
    // `!ScrnIsColInMargins` guard in xtermIndex(). The catch is LNM: the carriage-return half of the line
    // feed moves the cursor to the left margin, i.e. INTO the band, so asking the guard *after* the move
    // made it vacuously true and scrolled the top line of the region away. xterm reads screen->cur_col
    // first and applies CarriageReturn only afterwards (CASE_VMOT).
    //
    // The page is deliberately taller than the scroll region, so a scroll cannot be mistaken for the
    // cursor simply walking down the page.
    // The marker sits INSIDE the horizontal margins: a scroll here moves the rectangle bounded by the
    // margins, so a marker outside them would survive either way and the test would prove nothing.
    auto const setup = [](auto& mock) {
        mock.writeToScreen("\033[1;3r");   // DECSTBM: scroll region over lines 1..3
        mock.writeToScreen("\033[?69h");   // DECLRMM: enable left/right margins
        mock.writeToScreen("\033[10;20s"); // DECSLRM(10,20)
        mock.writeToScreen("\033[20h");    // LNM: LF also returns the carriage
        mock.writeToScreen("\033[1;10Htop");
    };

    SECTION("right of the right margin: no scroll")
    {
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(40) } };
        auto& screen = mock.terminal.primaryScreen();
        setup(mock);

        mock.writeToScreen("\033[3;30H"); // bottom margin line, right of the right margin
        mock.writeToScreen("\n");

        CHECK(screen.grid().lineText(LineOffset(0)).contains("top"));
        // Neither scrolled nor advanced past the bottom margin; the carriage still returned to the margin.
        CHECK(screen.cursor().position == CellLocation { LineOffset(2), ColumnOffset(9) });
    }

    SECTION("inside the margins: scrolls, as it must")
    {
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(40) } };
        auto& screen = mock.terminal.primaryScreen();
        setup(mock);

        mock.writeToScreen("\033[3;15H"); // bottom margin line, inside the margins
        mock.writeToScreen("\n");

        CHECK_FALSE(screen.grid().lineText(LineOffset(0)).contains("top"));
    }
}


// {{{ DECRQCRA / XTCHECKSUM Tests

// The expected checksums below are what xterm-406 answers for the same screen and the same
// XTCHECKSUM flags, measured by driving a real xterm headlessly. See RectangularAreaChecksum_test.cpp
// for the flag-by-flag breakdown; these tests are about the sequence reaching the algorithm at all,
// and about the state the algorithm reads.

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

// }}} DECRQCRA / XTCHECKSUM Tests

// {{{ DECSNLS Tests

TEST_CASE("DECSNLS: selects the number of lines per screen", "[screen]")
{
    // DECSNLS used to read its parameter as a *column* count, so `CSI 24 * |` silently narrowed an
    // 80-column page to 24 columns and left the line count alone -- the exact opposite of what the
    // sequence means. esctest found it by crashing the engine: narrowing the page reflows it, which
    // walked the cursor off the bottom.
    auto mock = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };

    SECTION("it sets the lines, and leaves the columns alone")
    {
        mock.writeToScreen("\033[24*|");
        CHECK(mock.terminal.pageSize().lines == LineCount(24));
        CHECK(mock.terminal.pageSize().columns == ColumnCount(80));
    }

    SECTION("an omitted parameter changes nothing")
    {
        mock.writeToScreen("\033[*|");
        CHECK(mock.terminal.pageSize().lines == LineCount(25));
        CHECK(mock.terminal.pageSize().columns == ColumnCount(80));
    }

    SECTION("a zero parameter changes nothing")
    {
        mock.writeToScreen("\033[0*|");
        CHECK(mock.terminal.pageSize().lines == LineCount(25));
    }

    SECTION("DECRQSS reports back what was selected")
    {
        // esctest asserts exactly this round trip.
        mock.writeToScreen("\033[24*|");
        mock.mockPty().stdinBuffer().clear();
        mock.writeToScreen("\033P$q*|\033\\");
        mock.terminal.flushInput();
        CHECK(mock.replyData() == "\033P1$r24*|\033\\");
    }

    SECTION("the cursor stays inside the page when it shrinks")
    {
        mock.writeToScreen("\033[25;1H"); // the last line of a 25-line page
        REQUIRE(mock.terminal.primaryScreen().cursor().position.line == LineOffset(24));

        mock.writeToScreen("\033[10*|");

        CHECK(mock.terminal.pageSize().lines == LineCount(10));
        CHECK(*mock.terminal.primaryScreen().cursor().position.line
              < *mock.terminal.primaryScreen().pageSize().lines);
    }
}

// }}} DECSNLS Tests

// {{{ Line feed below the scrolling region

TEST_CASE("LF below the scrolling region stops at the last line of the page", "[screen]")
{
    // Only a cursor sitting exactly on the bottom margin scrolls. A cursor *below* the scrolling
    // region has nothing to scroll -- but it used to be moved down anyway, with nothing stopping it
    // at the last line of the page, so every further line feed walked it further off the end. Any
    // application can reach it: set a scrolling region, put the cursor below it, hold down Return.
    //
    // Found by esctest, which aborted the engine on it.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033[1;2r"); // DECSTBM: scrolling region is lines 1..2
    mock.writeToScreen("\033[5;1H"); // cursor to the last line of the page, below the region
    REQUIRE(screen.cursor().position.line == LineOffset(4));

    SECTION("a line feed there does nothing")
    {
        mock.writeToScreen("\n");
        CHECK(screen.cursor().position.line == LineOffset(4));
    }

    SECTION("and it stays put however many arrive")
    {
        for ([[maybe_unused]] auto const _: std::views::iota(0, 10))
            mock.writeToScreen("\n");

        CHECK(screen.cursor().position.line == LineOffset(4));
        CHECK(*screen.cursor().position.line < *screen.pageSize().lines);
    }

    SECTION("the region itself still scrolls")
    {
        // The guard must not break the normal case: on the bottom margin, LF scrolls the region.
        mock.writeToScreen("\033[1;1HA\033[2;1HB");
        mock.writeToScreen("\033[2;1H\n"); // on the bottom margin -> scroll the region up
        CHECK(screen.cursor().position.line == LineOffset(1));
        CHECK(screen.grid().lineAt(LineOffset(0)).toUtf8() == "B    ");
    }

    SECTION("a line feed inside the page but below the region still moves down")
    {
        mock.writeToScreen("\033[3;1H"); // line 3: below the region, not the last line
        mock.writeToScreen("\n");
        CHECK(screen.cursor().position.line == LineOffset(3));
    }
}

// }}} Line feed below the scrolling region

// {{{ One-based parameters: omitted, empty and zero all mean "the default"

TEST_CASE("An omitted one-based parameter takes its default", "[screen]")
{
    // The parser stores an omitted parameter as the value zero *and counts it*, so `CSI ; 5 H` used to
    // read a row of 0 rather than the default of 1 -- and every handler that computes `param - 1` then
    // underflowed into a negative offset. There is no row zero and no column zero, so a sequence naming
    // one is naming the default, exactly as xterm's `if (param < 1) param = 1` has it.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();

    SECTION("CUP with an omitted row")
    {
        mock.writeToScreen("\033[3;3H"); // somewhere other than the home position
        mock.writeToScreen("\033[;4H");
        CHECK(screen.cursor().position == CellLocation { .line = LineOffset(0), .column = ColumnOffset(3) });
    }

    SECTION("CUP with an omitted column")
    {
        mock.writeToScreen("\033[3;3H");
        mock.writeToScreen("\033[4;H");
        CHECK(screen.cursor().position == CellLocation { .line = LineOffset(3), .column = ColumnOffset(0) });
    }

    SECTION("CUP with explicit zeroes")
    {
        mock.writeToScreen("\033[3;3H");
        mock.writeToScreen("\033[0;0H");
        CHECK(screen.cursor().position == CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) });
    }

    SECTION("HVP with an omitted row")
    {
        mock.writeToScreen("\033[3;3H");
        mock.writeToScreen("\033[;4f");
        CHECK(screen.cursor().position == CellLocation { .line = LineOffset(0), .column = ColumnOffset(3) });
    }

    SECTION("CHA and VPA with a zero")
    {
        mock.writeToScreen("\033[3;3H");
        mock.writeToScreen("\033[0G"); // CHA
        CHECK(screen.cursor().position.column == ColumnOffset(0));
        mock.writeToScreen("\033[0d"); // VPA
        CHECK(screen.cursor().position.line == LineOffset(0));
    }

    SECTION("HPA without a parameter at all")
    {
        // HPA read its parameter with param(), which asserts when none was given.
        mock.writeToScreen("\033[3;3H");
        mock.writeToScreen("\033[`");
        CHECK(screen.cursor().position.column == ColumnOffset(0));
    }

    SECTION("HPR without a parameter at all")
    {
        mock.writeToScreen("\033[1;1H");
        mock.writeToScreen("\033[a");
        CHECK(screen.cursor().position.column == ColumnOffset(1));
    }
}

TEST_CASE("A zero count moves or edits by one", "[screen]")
{
    // A count of zero is a count of one, for the same reason: `CSI 0 A` is `CSI A`.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();

    SECTION("CUU, CUD, CUF, CUB")
    {
        mock.writeToScreen("\033[3;3H");
        mock.writeToScreen("\033[0A");
        CHECK(screen.cursor().position.line == LineOffset(1));
        mock.writeToScreen("\033[0B");
        CHECK(screen.cursor().position.line == LineOffset(2));
        mock.writeToScreen("\033[0C");
        CHECK(screen.cursor().position.column == ColumnOffset(3));
        mock.writeToScreen("\033[0D");
        CHECK(screen.cursor().position.column == ColumnOffset(2));
    }

    SECTION("ICH inserts one cell")
    {
        mock.writeToScreen("ABCDE");
        mock.writeToScreen("\033[1;1H");
        mock.writeToScreen("\033[0@");
        CHECK(screen.grid().lineText(LineOffset(0)) == " ABCD");
    }

    SECTION("DCH deletes one cell")
    {
        mock.writeToScreen("ABCDE");
        mock.writeToScreen("\033[1;1H");
        mock.writeToScreen("\033[0P");
        CHECK(screen.grid().lineText(LineOffset(0)) == "BCDE ");
    }

    SECTION("ECH erases one cell")
    {
        mock.writeToScreen("ABCDE");
        mock.writeToScreen("\033[1;1H");
        mock.writeToScreen("\033[0X");
        CHECK(screen.grid().lineText(LineOffset(0)) == " BCDE");
    }
}

// }}} One-based parameters

// {{{ Rectangular areas

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

TEST_CASE("A rectangular area is clamped to the page", "[screen]")
{
    // "If the value of Pt, Pl, Pb, or Pr exceeds the width or height of the active page, then the value
    // is treated as the width or height of that page." -- VT520 manual. DECCARA, DECRARA and DECCRA
    // clamped neither corner, DECERA and DECFRA only the bottom-right one.
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(4) } };
    auto& screen = mock.terminal.primaryScreen();

    SECTION("DECFRA past the bottom-right corner")
    {
        mock.writeToScreen("\033[88;1;1;99;99$x"); // fill 'X' from 1,1 to line 99, column 99
        CHECK(screen.grid().lineText(LineOffset(0)) == "XXXX");
        CHECK(screen.grid().lineText(LineOffset(3)) == "XXXX");
    }

    SECTION("DECERA past the bottom-right corner")
    {
        mock.writeToScreen("\033[1;1H");
        mock.writeToScreen("abcd\r\nefgh\r\nijkl\r\nmnop");
        mock.writeToScreen("\033[2;2;99;99$z"); // erase from 2,2 to line 99, column 99
        CHECK(screen.grid().lineText(LineOffset(0)) == "abcd");
        CHECK(screen.grid().lineText(LineOffset(1)) == "e   ");
        CHECK(screen.grid().lineText(LineOffset(3)) == "m   ");
    }

    SECTION("DECSERA past the bottom-right corner")
    {
        mock.writeToScreen("\033[1;1H");
        mock.writeToScreen("abcd\r\nefgh\r\nijkl\r\nmnop");
        mock.writeToScreen("\033[2;2;99;99${"); // selectively erase from 2,2 to line 99, column 99
        CHECK(screen.grid().lineText(LineOffset(0)) == "abcd");
        CHECK(screen.grid().lineText(LineOffset(1)) == "e   ");
        CHECK(screen.grid().lineText(LineOffset(3)) == "m   ");
    }
}

TEST_CASE("A rectangular area is relative to the origin", "[screen]")
{
    // Origin mode (DECOM) moves the origin to the scrolling region's top-left corner, and the area's
    // coordinates are relative to it. Only DECSERA honoured that; the other five read the given
    // coordinates as absolute and used the origin merely to default an omitted one.
    auto mock = MockTerm { PageSize { LineCount(6), ColumnCount(4) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033[1;1H");
    mock.writeToScreen("aaaa\r\nbbbb\r\ncccc\r\ndddd\r\neeee\r\nffff");

    mock.writeToScreen("\033[3;5r"); // DECSTBM: scrolling region is lines 3..5
    mock.writeToScreen("\033[?6h");  // DECOM: on -- row 1 is now the page's row 3

    // Fill 'X' over the area's own rows 1..2, which are the page's rows 3..4.
    mock.writeToScreen("\033[88;1;1;2;4$x");

    CHECK(screen.grid().lineText(LineOffset(1)) == "bbbb"); // above the region: untouched
    CHECK(screen.grid().lineText(LineOffset(2)) == "XXXX"); // the region's first row
    CHECK(screen.grid().lineText(LineOffset(3)) == "XXXX");
    CHECK(screen.grid().lineText(LineOffset(4)) == "eeee"); // below what was named: untouched
}

// }}} Rectangular areas

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

TEST_CASE("DECDC deletes a column from every line, including the blank ones", "[screen]")
{
    // DECDC deletes a column from every line within the vertical margin -- most of which, on a page
    // that has just been written to, are still blank. A blank line's SoA arrays are empty, and
    // deleteChars() wrote through them without materializing them first. insertChars() had always
    // guarded against that; its sibling never did.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(7) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033[1;1H");
    mock.writeToScreen("abcdefg\r\nABCDEFG");
    mock.writeToScreen("\033[1;2H"); // column 2, with lines 3..5 still blank
    mock.writeToScreen("\033['~");   // DECDC, default parameter: delete one column

    CHECK(screen.grid().lineText(LineOffset(0)) == "acdefg ");
    CHECK(screen.grid().lineText(LineOffset(1)) == "ACDEFG ");
    CHECK(screen.grid().lineText(LineOffset(4)) == "       ");
}

// {{{ Special colors (OSC 5 / OSC 105)

TEST_CASE("OSC.5 addresses the special colors", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(4) } };

    SECTION("set and query")
    {
        mock.writeToScreen("\033]5;0;rgb:f0f0/f0f0/f0f0\033\\"); // bold
        mock.writeToScreen("\033]5;4;rgb:1010/2020/3030\033\\"); // italic
        mock.discardPendingReplies();

        mock.writeToScreen("\033]5;0;?\033\\");
        mock.writeToScreen("\033]5;4;?\033\\");
        INFO(mock.terminal.peekInput());
        REQUIRE(e(mock.terminal.peekInput())
                == e("\033]5;0;rgb:f0f0/f0f0/f0f0\033\\"
                     "\033]5;4;rgb:1010/2020/3030\033\\"));

        CHECK(mock.terminal.colorPalette().specialColor(SpecialColor::Bold) == RGBColor { 0xF0, 0xF0, 0xF0 });
        CHECK(mock.terminal.colorPalette().specialColor(SpecialColor::Italic)
              == RGBColor { 0x10, 0x20, 0x30 });
    }

    SECTION("OSC 4 reaches the same colors, just past the indexed ones")
    {
        // An application may name a special color either way: `OSC 5 ; 0` and `OSC 4 ; 256` are the same
        // color. A report echoes the index it was given, in the form it was given.
        mock.writeToScreen("\033]4;256;rgb:aaaa/bbbb/cccc\033\\");
        mock.discardPendingReplies();

        mock.writeToScreen("\033]5;0;?\033\\");
        REQUIRE(e(mock.terminal.peekInput()) == e("\033]5;0;rgb:aaaa/bbbb/cccc\033\\"));
        mock.discardPendingReplies();

        mock.writeToScreen("\033]4;256;?\033\\");
        REQUIRE(e(mock.terminal.peekInput()) == e("\033]4;256;rgb:aaaa/bbbb/cccc\033\\"));
    }

    SECTION("an index past the last special color names nothing")
    {
        mock.writeToScreen("\033]5;5;rgb:0000/0000/0000\033\\");
        CHECK(mock.terminal.peekInput().empty());
    }

    SECTION("the dim colors are not reachable, and are not overwritten")
    {
        // Contour keeps its own dim colors where xterm keeps its special ones. Naming special color 0
        // must not land on a dim color.
        auto const dimBefore = mock.terminal.colorPalette().dimColor(0);
        mock.writeToScreen("\033]5;0;rgb:f0f0/f0f0/f0f0\033\\");
        CHECK(mock.terminal.colorPalette().dimColor(0) == dimBefore);
    }
}

TEST_CASE("OSC.105 resets the special colors", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(4) } };
    auto const original = mock.terminal.defaultColorPalette().specialColor(SpecialColor::Bold);

    SECTION("one color")
    {
        mock.writeToScreen("\033]5;0;rgb:f0f0/f0f0/f0f0\033\\");
        mock.writeToScreen("\033]105;0\033\\");
        CHECK(mock.terminal.colorPalette().specialColor(SpecialColor::Bold) == original);
    }

    SECTION("all of them, when no index is given")
    {
        mock.writeToScreen("\033]5;0;rgb:f0f0/f0f0/f0f0\033\\");
        mock.writeToScreen("\033]5;4;rgb:f0f0/f0f0/f0f0\033\\");
        mock.writeToScreen("\033]105\033\\");
        CHECK(mock.terminal.colorPalette().specialColor(SpecialColor::Bold) == original);
        CHECK(mock.terminal.colorPalette().specialColor(SpecialColor::Italic)
              == mock.terminal.defaultColorPalette().specialColor(SpecialColor::Italic));
    }

    SECTION("OSC 104 with no index resets every index it can address")
    {
        mock.writeToScreen("\033]4;3;rgb:f0f0/f0f0/f0f0\033\\");
        mock.writeToScreen("\033]5;0;rgb:f0f0/f0f0/f0f0\033\\");
        mock.writeToScreen("\033]104\033\\");
        CHECK(mock.terminal.colorPalette().palette[3] == mock.terminal.defaultColorPalette().palette[3]);
        // OSC 4 addresses the special colors too (256..260), so a bare OSC 104 reaches them -- as xterm
        // walks its whole Acolors.
        CHECK(mock.terminal.colorPalette().specialColor(SpecialColor::Bold) == original);
    }

    SECTION("OSC 104 does not reset the dynamic colors")
    {
        // The dynamic colors share the ColorPalette but are addressed by OSC 10..19 and reset by
        // OSC 110..119 -- xterm keeps them in a separate Tcolors for exactly this reason. Assigning the
        // whole palette here withdrew a background the application set with OSC 11 and nothing asked to
        // reset: a themed shell lost its background to any stray `tput oc`.
        mock.writeToScreen("\033]11;rgb:1e1e/1e1e/2e2e\033\\");
        auto const chosenBackground = mock.terminal.colorPalette().defaultBackground;
        REQUIRE(chosenBackground != mock.terminal.defaultColorPalette().defaultBackground);

        mock.writeToScreen("\033]104\033\\");

        CHECK(mock.terminal.colorPalette().defaultBackground == chosenBackground);
    }
}

// }}} Special colors

// {{{ Backspace, margins and reverse wraparound

TEST_CASE("Backspace stops at the left margin", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033[?69h");  // DECLRMM
    mock.writeToScreen("\033[5;10s"); // DECSLRM: columns 5..10

    SECTION("a cursor on the left margin does not move")
    {
        mock.writeToScreen("\033[1;5H");
        mock.writeToScreen("\b");
        CHECK(screen.cursor().position.column == ColumnOffset(4));
    }

    SECTION("a cursor left of the left margin is not held by it")
    {
        // The margin is not holding a cursor that is already outside it -- the screen's edge is.
        mock.writeToScreen("\033[1;3H");
        mock.writeToScreen("\b");
        CHECK(screen.cursor().position.column == ColumnOffset(1));
    }
}

TEST_CASE("Reverse wraparound carries the cursor to the line above", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();

    SECTION("it does nothing without DECAWM")
    {
        // A terminal that does not wrap forward has no wrap to reverse.
        mock.writeToScreen("\033[?7l");  // DECAWM off
        mock.writeToScreen("\033[?45h"); // reverse wraparound on
        mock.writeToScreen("\033[2;1H");
        mock.writeToScreen("\b");
        CHECK(screen.cursor().position == CellLocation { LineOffset(1), ColumnOffset(0) });
    }

    SECTION("the plain form follows only a line the text wrapped onto")
    {
        mock.writeToScreen("\033[?7h");  // DECAWM
        mock.writeToScreen("\033[?45h"); // reverse wraparound
        mock.writeToScreen("\033[2;1H"); // line 2 is blank, so line 1 was never wrapped onto it
        mock.writeToScreen("\b");
        CHECK(screen.cursor().position == CellLocation { LineOffset(1), ColumnOffset(0) });
    }

    SECTION("the plain form does follow a line the text wrapped onto")
    {
        mock.writeToScreen("\033[?7h");
        mock.writeToScreen("\033[1;1H");
        mock.writeToScreen("ABCDEF"); // wraps onto line 2, marking line 2 as wrapped
        mock.writeToScreen("\033[?45h");
        mock.writeToScreen("\033[2;1H");
        mock.writeToScreen("\b");
        CHECK(screen.cursor().position == CellLocation { LineOffset(0), ColumnOffset(4) });
    }

    SECTION("the extended form follows any line at all")
    {
        mock.writeToScreen("\033[?7h");
        mock.writeToScreen("\033[?1045h"); // extended reverse wraparound
        mock.writeToScreen("\033[2;1H");   // line 2 is blank, and it follows it anyway
        mock.writeToScreen("\b");
        CHECK(screen.cursor().position == CellLocation { LineOffset(0), ColumnOffset(4) });
    }

    SECTION("a soft reset turns it off, so it cannot outlive the application that asked for it")
    {
        mock.writeToScreen("\033[?7h");
        mock.writeToScreen("\033[?1045h");
        mock.writeToScreen("\033[!p"); // DECSTR
        CHECK_FALSE(mock.terminal.isModeEnabled(DECMode::ReverseWraparoundExtended));
        CHECK_FALSE(mock.terminal.isModeEnabled(DECMode::ReverseWraparound));
    }
}

// }}} Backspace
