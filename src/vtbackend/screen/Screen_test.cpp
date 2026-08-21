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
using namespace vtbackend;
using namespace vtbackend::test;
using namespace std;
using namespace std::literals::chrono_literals;

// NOLINTBEGIN(misc-const-correctness,readability-function-cognitive-complexity)

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
    CHECK(screen.grid().lineText(LineOffset(0)) == full);                   // row 1
    CHECK(screen.grid().lineText(LineOffset(1)) == full);                   // row 2
    CHECK(screen.grid().lineText(LineOffset(2)) == full);                   // row 3
    CHECK(screen.grid().lineText(LineOffset(3)) == std::string(Cols, ' ')); // row 4 stays empty
}

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

TEST_CASE("AppendChar.VS15_selects_text_presentation_without_changing_the_width", "[screen]")
{
    // terminal-unicode-core is explicit: VS15 "will NOT change the underlying width but only change
    // the display to prefer textual non-colored presentation". A cluster already on screen cannot
    // give a column back -- that would mean un-wrapping a line that wrapped and un-scrolling content
    // that scrolled -- so the width stands and only the presentation changes.
    //
    // U+231A WATCH is the interesting base: Unicode does define a text-presentation sequence for it
    // (emoji-variation-sequences.txt), so the selector is meaningful here rather than inert, and the
    // width still must not move.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(4) } };
    auto& screen = mock.terminal.primaryScreen();

    REQUIRE(*screen.logicalCursorPosition().column == 0);
    mock.writeToScreen(U"\u231A");
    REQUIRE(*screen.logicalCursorPosition().column == 2);
    mock.writeToScreen(U"\uFE0E");
    REQUIRE(*screen.logicalCursorPosition().column == 2);

    // The selector joined the cluster -- it is what the renderer reads to pick the uncolored glyph --
    // but the two columns the watch claimed are still its own.
    auto const& c0 = screen.at(LineOffset(0), ColumnOffset(0));
    CHECK(c0.codepoints() == U"\u231A\uFE0E");
    CHECK(c0.width() == 2);
    CHECK(screen.at(LineOffset(0), ColumnOffset(1)).isFlagEnabled(CellFlag::WideCharContinuation));

    // The next character lands after the cluster, not inside it.
    mock.writeToScreen("X");
    CHECK(screen.at(LineOffset(0), ColumnOffset(2)).codepoints() == U"X");
}

TEST_CASE("AppendChar.VS15_is_inert_without_a_defined_variation_sequence", "[screen]")
{
    // A variation selector only re-presents a base Unicode defines a sequence for. U+1F600 is
    // emoji-only -- there is no text presentation to select -- so VS15 says nothing about it at all.
    // Measuring it as one column is what wcwidth's VS15_WIDE_TO_NARROW table would wrongly do for a
    // base outside that table.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(4) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen(U"\U0001F600");
    REQUIRE(*screen.logicalCursorPosition().column == 2);
    mock.writeToScreen(U"\uFE0E");

    CHECK(*screen.logicalCursorPosition().column == 2);
    auto const& c0 = screen.at(LineOffset(0), ColumnOffset(0));
    CHECK(c0.codepoints() == U"\U0001F600\uFE0E");
    CHECK(c0.width() == 2);
}

TEST_CASE("AppendChar.a_zero_width_codepoint_after_an_ASCII_base_joins_its_cell", "[screen]")
{
    // The cell-level half of Parser.BulkText_ZeroWidthAfterAsciiBase. scan_text() splits a run at the
    // ASCII/non-ASCII boundary, so a mark on an ASCII base is handed to Screen separately from its
    // base and has to find that cell again through the grapheme-state reconstruction in writeText --
    // which deliberately does NOT reuse the parser's state. Every other cluster test here uses a
    // non-ASCII base, which never crosses that seam, so this is the case that reached the screen as a
    // bare "e" while libunicode measured the handover in columns rather than bytes (< 0.9.3).
    //
    // Bytes, not codepoints: the defect lives in the UTF-8 bulk scanner, so it has to be fed UTF-8.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(10) } };
    auto& screen = mock.terminal.primaryScreen();

    SECTION("the base alone in the buffer")
    {
        mock.writeToScreen("e\xCC\x81"); // "é" decomposed: 'e' + U+0301 COMBINING ACUTE ACCENT

        CHECK(*screen.logicalCursorPosition().column == 1);
        auto const& c0 = screen.at(LineOffset(0), ColumnOffset(0));
        CHECK(c0.codepoints() == U"é");
        CHECK(c0.width() == 1);
    }

    SECTION("the base mid-run, so the scan hands over and then returns to ASCII")
    {
        mock.writeToScreen("abcde\xCC\x81"
                           "fgh");

        CHECK(*screen.logicalCursorPosition().column == 8);
        // The mark joined 'e' at column 4 rather than opening a cell of its own...
        auto const& c4 = screen.at(LineOffset(0), ColumnOffset(4));
        CHECK(c4.codepoints() == U"é");
        CHECK(c4.width() == 1);
        // ... so what follows it is still 'f', not a column further along.
        CHECK(screen.at(LineOffset(0), ColumnOffset(5)).codepoints() == U"f");
        CHECK(screen.renderMainPageText() == "abcdéfgh  \n");
    }
}

TEST_CASE("AppendChar.a_wide_char_at_the_second_to_last_column_claims_the_last_one", "[screen]")
{
    // The room a character has is its own column PLUS the writable columns to its right. Counting
    // only the ones strictly to the right makes a two-column character that exactly fills the
    // remaining space look one column too wide: the continuation cell is never written, so the
    // previous occupant of the last column survives underneath the wide glyph and is copied out
    // with it. Any full-screen application repainting a line that ends in CJK hits this.
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(10) } };
    auto const& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("AAAAAAAAAA"sv); // fill every column
    mock.writeToScreen("\033[1;9H"sv);  // cursor to column offset 8, the second-to-last
    mock.writeToScreen(U"中");          // a two-column CJK character

    auto const& head = screen.at(LineOffset(0), ColumnOffset(8));
    auto const& tail = screen.at(LineOffset(0), ColumnOffset(9));
    CHECK(head.width() == 2);
    CHECK(head.codepoints() == U"中");

    // The stale `A` must be gone, and the column must be marked as belonging to the character.
    CHECK(tail.isFlagEnabled(CellFlag::WideCharContinuation));
    CHECK(tail.codepoints().empty());

    // The character filled the line exactly, so the cursor stays put with a wrap pending rather
    // than stepping outside the page.
    CHECK(*screen.logicalCursorPosition().column == 8);
}

TEST_CASE("AppendChar.overwriting_a_wide_char_at_the_second_to_last_column_clears_its_tail", "[screen]")
{
    // The other half of the same arithmetic: the count also bounds the clearing of the cell being
    // REPLACED, so a narrow character written over a wide one at that column used to leave the old
    // right half -- a WideCharContinuation with no head -- behind at the last column.
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(10) } };
    auto const& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033[1;9H"sv);
    mock.writeToScreen(U"中");
    REQUIRE(screen.at(LineOffset(0), ColumnOffset(9)).isFlagEnabled(CellFlag::WideCharContinuation));

    mock.writeToScreen("\033[1;9H"sv);
    mock.writeToScreen("x"sv);

    CHECK(screen.at(LineOffset(0), ColumnOffset(8)).codepoints() == U"x");
    CHECK_FALSE(screen.at(LineOffset(0), ColumnOffset(9)).isFlagEnabled(CellFlag::WideCharContinuation));
}

TEST_CASE("AppendChar.a_wide_char_still_wraps_when_only_one_column_is_left", "[screen]")
{
    // The boundary the fix must NOT move: at the very last column a two-column character has no
    // room at all, and the deferred wrap still has to fire.
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(10) } };
    auto const& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033[1;10H"sv); // cursor to the last column
    mock.writeToScreen("a"sv);          // narrow write there leaves the cursor put, wrap pending
    CHECK(*screen.logicalCursorPosition().column == 9);

    // The next character lands on the following line, not past the page edge.
    mock.writeToScreen("b"sv);
    CHECK(screen.at(LineOffset(1), ColumnOffset(0)).codepoints() == U"b");
}

TEST_CASE("AppendChar.emoji_VS16_copyright_sign", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(4) } };
    auto& screen = mock.terminal.primaryScreen();
    auto const& c0 = screen.at(LineOffset(0), ColumnOffset(0));
    auto const& c1 = screen.at(LineOffset(0), ColumnOffset(1));
    auto const& c2 = screen.at(LineOffset(0), ColumnOffset(2));
    auto const& c3 = screen.at(LineOffset(0), ColumnOffset(3));

    // Print the letter-like copyright sign, then force its EMOJI presentation with VS16. The cluster
    // must widen to two columns and claim the cell to its right.
    REQUIRE(screen.cursor().position.column.value == 0);
    mock.writeToScreen(U"\u00A9");
    REQUIRE(screen.cursor().position.column.value == 1);
    CHECK(c0.codepointCount() == 1);
    CHECK(c0.width() == 1);
    mock.writeToScreen(U"\uFE0F");
    CHECK(c0.codepointCount() == 2);
    REQUIRE(screen.cursor().position.column.value == 2);
    mock.writeToScreen("X");
    REQUIRE(screen.cursor().position.column.value == 3);

    // double-width emoji with VS16
    CHECK(c0.codepoints() == U"\u00A9\uFE0F");
    CHECK(c0.width() == 2);

    // the claimed continuation cell
    CHECK(c1.isFlagEnabled(CellFlag::WideCharContinuation));

    // character after the emoji
    CHECK(c2.codepoints() == U"X");
    CHECK(c2.width() == 1);

    CHECK(c3.codepoints().empty());
}

TEST_CASE("AppendChar.width_revision_is_gated_on_mode_2027", "[screen]")
{
    // DEC mode 2027 is how an application says it expects whole clusters to be measured. With it
    // reset, the first codepoint decides and a variation selector arriving later changes nothing --
    // the older behaviour applications that reset the mode are asking for.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.terminal.setMode(DECMode::Unicode, false);
    mock.writeToScreen(U"\u2139"); // narrow on its own
    mock.writeToScreen(U"\uFE0F"); // VS16: would widen it to 2 under mode 2027
    CHECK(screen.cursor().position.column == ColumnOffset(1));
    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).width() == 1);

    // The codepoint still JOINED the cluster -- it is part of the text and must round-trip through a
    // copy; it merely did not change how many columns the cluster occupies.
    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).codepoints() == U"\u2139\uFE0F");
}

TEST_CASE("AppendChar.width_revision_resumes_when_2027_is_set_again", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.terminal.setMode(DECMode::Unicode, false);
    mock.terminal.setMode(DECMode::Unicode, true);
    mock.writeToScreen(U"\u2139");
    mock.writeToScreen(U"\uFE0F");
    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).width() == 2);
}

TEST_CASE("AppendChar.width_revision_at_right_edge_keeps_cursor_on_page", "[screen]")
{
    // A cluster promoted to two columns can end flush against the last column: the cluster itself
    // fits, but the cursor that follows it does not. Advancing there unconditionally left the cursor
    // one column past the page -- breaking the invariant verifyState() asserts, and indexing the
    // line's storage out of bounds on the next write.
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen(U"abc");
    REQUIRE(screen.cursor().position.column == ColumnOffset(3));

    mock.writeToScreen(U"ℹ"); // lands at column 3, cursor to 4
    REQUIRE(screen.cursor().position.column == ColumnOffset(4));

    mock.writeToScreen(U"️"); // widens it to columns 3..4 -- the cursor would land at 5
    CHECK(screen.at(LineOffset(0), ColumnOffset(3)).width() == 2);
    CHECK(screen.cursor().position.column == ColumnOffset(4));
    CHECK(screen.cursor().wrapPending);

    // The deferred wrap is what makes the promotion safe: the next character starts the next line
    // rather than writing off the end of this one.
    mock.writeToScreen(U"X");
    CHECK(screen.cursor().position.line == LineOffset(1));
    CHECK(screen.at(LineOffset(1), ColumnOffset(0)).codepoints() == U"X");
}

TEST_CASE("AppendChar.abandoned_width_revision_restores_the_head_cell", "[screen]")
{
    // The width is committed to the cell BEFORE the screen decides whether it can carry the
    // promotion through. When a deferred wrap has already moved the cursor to the next line the
    // cluster is no longer live, so the promotion is abandoned -- and the committed width has to go
    // back with it, or the cell claims a column that holds no continuation of it.
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen(U"abcd");
    REQUIRE(screen.cursor().position.column == ColumnOffset(4));

    mock.writeToScreen(U"ℹ"); // written at the last column; sets wrapPending
    REQUIRE(screen.cursor().position.column == ColumnOffset(4));
    REQUIRE(screen.cursor().wrapPending);

    // The wrap fires first, so the head is now on the PREVIOUS line and cannot grow into a column
    // that does not exist.
    mock.writeToScreen(U"️");
    CHECK(screen.cursor().position.line == LineOffset(1));
    CHECK(screen.at(LineOffset(0), ColumnOffset(4)).width() == 1);
    CHECK(screen.at(LineOffset(0), ColumnOffset(4)).codepoints() == U"ℹ️");
}

TEST_CASE("Screen.copyArea_does_not_remeasure_cluster_widths", "[screen]")
{
    // DECCRA reproduces cells; it does not re-write them. Re-measuring the cluster would apply
    // whatever policy is in force NOW to text written under the policy of the time, so a cluster
    // stored one column wide would silently claim its neighbour -- which holds live text, and has no
    // continuation cell to mark it as taken.
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(8) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.terminal.setMode(DECMode::Unicode, false);
    mock.writeToScreen(U"ℹ"); // stored one column wide under FirstCodepoint
    mock.writeToScreen(U"️");
    mock.writeToScreen(U"X");
    REQUIRE(screen.at(LineOffset(0), ColumnOffset(0)).width() == 1);
    REQUIRE(screen.at(LineOffset(0), ColumnOffset(1)).codepoints() == U"X");

    // DECCRA: copy the 1x2 region at the top-left down to line 3.
    mock.writeToScreen("\033[1;1;1;2;1;3;1;1$v"sv);

    CHECK(screen.at(LineOffset(2), ColumnOffset(0)).width() == 1);
    CHECK(screen.at(LineOffset(2), ColumnOffset(0)).codepoints() == U"ℹ️");
    CHECK(screen.at(LineOffset(2), ColumnOffset(1)).codepoints() == U"X");
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

    // U+FE0F promotes the cluster to the emoji presentation, which is two columns wide.
    mock.writeToScreen(U"\uFE0F");
    REQUIRE(screen.cursor().position.column.value == 2);
    CHECK(c0.codepoints() == U"\u2139\uFE0F");
    CHECK(c0.width() == 2);

    // the claimed continuation cell
    auto const& c1 = screen.at(LineOffset(0), ColumnOffset(1));
    CHECK(c1.isFlagEnabled(CellFlag::WideCharContinuation));

    mock.writeToScreen("X");

    // X lands after the now-two-column emoji, not beside it.
    auto const& c2 = screen.at(LineOffset(0), ColumnOffset(2));
    CHECK(c2.codepoints() == U"X");
    CHECK(c2.width() == 1);

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

TEST_CASE("AppendChar.emoji_zwj_ten_codepoints", "[screen]")
{
    // 👨🏻‍❤️‍💋‍👨🏻 kiss: man, man, light skin tone -- ten codepoints, the longest
    // cluster the RGI emoji set produces. It used to exceed MaxGraphemeClusterSize and be silently truncated
    // to seven, which is what this case pins.
    //
    // Truncation is not merely cosmetic. Driven through a real PTY, the overflow also split the
    // sequence into two wide cells and advanced the cursor four columns instead of two; jquast's
    // ucs-detect counted 43 such sequences, and raising the cap took its ZWJ score from 93.29% to
    // 96.26%. That end-to-end effect is NOT reproduced here -- writing the cluster in one call to
    // writeToScreen keeps the cursor at two columns even when truncated -- so this case guards the
    // capacity, not the advance.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(6) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.terminal.setMode(DECMode::AutoWrap, false);

    auto const emoji =
        u32string_view { U"\U0001F468\U0001F3FB‍❤️‍\U0001F48B‍\U0001F468\U0001F3FB" };
    REQUIRE(emoji.size() == 10);
    mock.writeToScreen(unicode::convert_to<char>(emoji));

    auto const& c0 = screen.at(LineOffset(0), ColumnOffset(0));
    CHECK(c0.codepoints() == emoji);
    CHECK(c0.width() == 2);

    // The whole sequence is ONE cluster: column 1 is its continuation, and nothing was written beyond.
    CHECK(screen.at(LineOffset(0), ColumnOffset(2)).empty());
    CHECK(screen.at(LineOffset(0), ColumnOffset(3)).empty());

    // The cursor advanced two columns, not four.
    CHECK(screen.cursor().position.column == ColumnOffset(2));
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

TEST_CASE("SaveCursor and RestoreCursor", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.terminal.setMode(DECMode::AutoWrap, false);
    mock.terminal.currentScreen().saveCursor();

    // mutate the cursor's position and the origin/autowrap modes
    screen.moveCursorTo(LineOffset { 2 }, ColumnOffset { 2 });
    mock.terminal.setMode(DECMode::AutoWrap, true);
    mock.terminal.setMode(DECMode::Origin, true);

    // Restore: position and origin mode (DECOM) revert, because DECOM is cursor state. Autowrap
    // (DECAWM) does NOT revert -- it is a terminal mode, not cursor state (DEC STD 070 / xterm), so
    // DECRC leaves it as it currently stands (ON here).
    mock.terminal.currentScreen().restoreCursor();
    CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(0) });
    CHECK(mock.terminal.isModeEnabled(DECMode::AutoWrap)); // stays ON: DECRC does not restore autowrap
    CHECK_FALSE(mock.terminal.isModeEnabled(DECMode::Origin));
}

TEST_CASE("SaveRestoreCursor.AltVsMain", "[screen]")
{
    // The primary and alternate screens keep independent saved cursors. A DECSC on the alternate screen
    // must not disturb the primary's saved cursor, and DECRC must not switch screens (that is DECSET
    // 47/1049's job). xterm behaves the same.
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(10) } };

    mock.writeToScreen("\033[2;3H"); // CUP row 2, col 3 (primary)
    mock.writeToScreen("\0337");     // DECSC on primary
    mock.writeToScreen("\033[?47h"); // switch to alternate screen
    mock.writeToScreen("\033[6;7H"); // CUP row 6, col 7 (alternate)
    mock.writeToScreen("\0337");     // DECSC on alternate

    mock.writeToScreen("\033[?47l"); // back to primary
    mock.writeToScreen("\0338");     // DECRC on primary
    CHECK(mock.terminal.screenType() == ScreenType::Primary);
    CHECK(mock.terminal.primaryScreen().cursor().position == CellLocation { LineOffset(1), ColumnOffset(2) });

    mock.writeToScreen("\033[?47h"); // switch to alternate
    mock.writeToScreen("\0338");     // DECRC on alternate
    CHECK(mock.terminal.screenType() == ScreenType::Alternate);
    CHECK(mock.terminal.alternateScreen().cursor().position
          == CellLocation { LineOffset(5), ColumnOffset(6) });
}

TEST_CASE("AlternateScreen.DECSET_47_1047_1049", "[screen]")
{
    // Mirrors esctest DECSETTests.doAltBuftest for the three alternate-screen modes. They differ only
    // in whether the cursor is carried across the switch (47, 1047 -- xterm's continuous, terminal-level
    // cursor) or saved and restored (1049), and whether the alternate page is erased on the way out
    // (1047, 1049) or kept (47). @see Terminal::setAlternateScreen, alternateScreenBehavior.
    int mode = 0;
    bool cursorCarried = false;   // 47, 1047: the cursor does not move across enter/exit
    bool altErasedOnExit = false; // 1047, 1049: the alternate page is blank when re-entered
    SECTION("mode 47 (ALTBUF)")
    {
        mode = 47;
        cursorCarried = true;
        altErasedOnExit = false;
    }
    SECTION("mode 1047 (OPT_ALTBUF)")
    {
        mode = 1047;
        cursorCarried = true;
        altErasedOnExit = true;
    }
    SECTION("mode 1049 (OPT_ALTBUF_CURSOR)")
    {
        mode = 1049;
        cursorCarried = false;
        altErasedOnExit = true;
    }
    CAPTURE(mode);

    auto const set = "\033[?" + std::to_string(mode) + "h";
    auto const reset = "\033[?" + std::to_string(mode) + "l";

    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };

    // Scribble on the primary screen: "abc" / "abc", leaving the cursor at (line 1, col 3).
    mock.writeToScreen("abc\r\nabc");
    auto const primaryCursor = mock.terminal.currentScreen().cursor().position;
    REQUIRE(primaryCursor == CellLocation { LineOffset(1), ColumnOffset(3) });

    // Enter the alternate screen. Modes 47 and 1047 must not move the cursor.
    mock.writeToScreen(set);
    REQUIRE(mock.terminal.isAlternateScreen());
    if (cursorCarried)
        CHECK(mock.terminal.currentScreen().cursor().position == primaryCursor);

    // Erase the alternate page and scribble "def" on lines 2 and 3 (1-based), exactly as esctest does.
    mock.writeToScreen("\033[2J\033[2;1Hdef\r\ndef");
    CHECK(mock.terminal.currentScreen().grid().lineText(LineOffset(0)) == "     ");
    CHECK(mock.terminal.currentScreen().grid().lineText(LineOffset(1)) == "def  ");
    CHECK(mock.terminal.currentScreen().grid().lineText(LineOffset(2)) == "def  ");

    // Leave the alternate screen. 47/1047 carry the cursor continuously; 1049 restores the saved one.
    auto const beforeExit = mock.terminal.currentScreen().cursor().position;
    mock.writeToScreen(reset);
    REQUIRE(mock.terminal.isPrimaryScreen());
    CHECK(mock.terminal.currentScreen().cursor().position == (cursorCarried ? beforeExit : primaryCursor));

    // The primary content is untouched by any of the three modes.
    CHECK(mock.terminal.currentScreen().grid().lineText(LineOffset(0)) == "abc  ");
    CHECK(mock.terminal.currentScreen().grid().lineText(LineOffset(1)) == "abc  ");
    CHECK(mock.terminal.currentScreen().grid().lineText(LineOffset(2)) == "     ");

    // Re-enter the alternate screen: mode 47 kept "def"; modes 1047 and 1049 erased it.
    mock.writeToScreen(set);
    REQUIRE(mock.terminal.isAlternateScreen());
    CHECK(mock.terminal.currentScreen().grid().lineText(LineOffset(1))
          == (altErasedOnExit ? "     " : "def  "));
    CHECK(mock.terminal.currentScreen().grid().lineText(LineOffset(2))
          == (altErasedOnExit ? "     " : "def  "));
}

TEST_CASE("DCH.worksOutsideTopBottomMargin", "[screen]")
{
    // DCH deletes characters even when the cursor sits outside the top/bottom scrolling margin (xterm
    // patch 316) -- it is confined only by the left/right margins.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("abcde");                             // row 1 = "abcde"
    mock.writeToScreen("\033[2;3r");                         // DECSTBM(2,3): vertical margin is rows 2..3
    mock.writeToScreen("\033[1;1H");                         // cursor to row 1, outside the vertical margin
    mock.writeToScreen("\033[99P");                          // DCH(99)
    CHECK(screen.grid().lineText(LineOffset(0)) == "     "); // row 1 was still deleted
}

TEST_CASE("CBT.ignoresLeftRightMargin", "[screen]")
{
    // CBT (cursor backward tab) ignores the left/right margin (xterm): from column 9 it tabs back past
    // the left margin (5) to column 1, not stopping at the margin.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("\033[?69h");                           // DECLRMM: enable left/right margins
    mock.writeToScreen("\033[5;30s");                          // DECSLRM(5,30)
    mock.writeToScreen("\033[1;9H");                           // CUP to column 9
    mock.writeToScreen("\033[2Z");                             // CBT(2)
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

    SECTION("overflow")
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

    SECTION("overflow")
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
        optional<CellLocation> const qr =
            screen.searchReverse(U"qr", cursorPosition, vtbackend::SearchCaseSensitivity::Smart);
        REQUIRE(qr.value() == CellLocation { LineOffset(2), ColumnOffset(2) });

        // Find something in main page area.
        optional<CellLocation> const mn =
            screen.searchReverse(U"mn", cursorPosition, vtbackend::SearchCaseSensitivity::Smart);
        REQUIRE(mn.value() == CellLocation { LineOffset(1), ColumnOffset(1) });

        // Search for something that doesn't exist.
        optional<CellLocation> const nnOut =
            screen.searchReverse(U"XY", *mn, vtbackend::SearchCaseSensitivity::Smart);
        REQUIRE(!nnOut.has_value());

        // Check that we can find a term in the top-most scrollback line.
        optional<CellLocation> const oneAB =
            screen.searchReverse(U"1ab", *mn, vtbackend::SearchCaseSensitivity::Smart);
        REQUIRE(oneAB.value() == CellLocation { LineOffset(-3), ColumnOffset(0) });

        mock.writeToScreen("7abcd");

        // Find text that got wrapped
        optional<CellLocation> const cd =
            screen.searchReverse(U"cd", screen.cursor().position, vtbackend::SearchCaseSensitivity::Smart);
        REQUIRE(cd.value() == CellLocation { LineOffset(1), ColumnOffset(3) });

        // Find text larger than the line length
        optional<CellLocation> const longSearch = screen.searchReverse(
            U"6pqr7abcd", screen.cursor().position, vtbackend::SearchCaseSensitivity::Smart);
        REQUIRE(longSearch.value() == CellLocation { LineOffset(0), ColumnOffset(0) });
    }
}

TEST_CASE("search.smartCaseIsCodepointAware", "[screen]")
{
    // "Smart case" asks whether the needle holds an uppercase character, and the comparison then folds
    // case unless it does. Both halves used to be asked of <cctype> -- std::isupper() and std::tolower()
    // -- which are defined only for values representable as unsigned char, so every codepoint above
    // U+00FF was undefined behaviour: an out-of-bounds read into the ctype table on glibc, an
    // invalid-parameter abort on the MSVC debug CRT. Both now go through the UCD, which answers for the
    // actual codepoint and so makes smart case work for non-Latin scripts as well.
    //
    // Cyrillic П is U+041F, far outside what either function may be asked about.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) }, LineCount(10) };
    mock.writeToScreen("Привет");

    auto& screen = mock.terminal.primaryScreen();
    auto const start = CellLocation { LineOffset(0), ColumnOffset(0) };

    SECTION("an uppercase non-ASCII needle selects a case-sensitive search")
    {
        CHECK(screen.search(U"Привет", start, vtbackend::SearchCaseSensitivity::Smart)
                  .has_value()); // matches exactly
        CHECK(!screen.search(U"ПРИВЕТ", start, vtbackend::SearchCaseSensitivity::Smart)
                   .has_value()); // case-sensitive, so this must not match
    }

    SECTION("an all-lowercase non-ASCII needle stays case-insensitive")
    {
        CHECK(screen.search(U"привет", start, vtbackend::SearchCaseSensitivity::Smart).has_value());
    }

    SECTION("codepoints far above the ctype table are safe to scan")
    {
        // Reaching these at all used to be the crash; not matching is the only expected outcome.
        CHECK(!screen.search(U"\U0001F600", start, vtbackend::SearchCaseSensitivity::Smart)
                   .has_value()); // emoji, U+1F600
        CHECK(!screen.search(U"\U00010400", start, vtbackend::SearchCaseSensitivity::Smart)
                   .has_value()); // Deseret capital, U+10400
    }
}

TEST_CASE("tallyMatches", "[screen]")
{
    using vtbackend::SearchCaseSensitivity;
    using vtbackend::SearchMatchTally;
    using vtbackend::TallyExactness;

    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(4) }, LineCount(10) };
    mock.writeToScreen("ab1a"); // -3: +
    mock.writeToScreen("2abc"); // -2: | history
    mock.writeToScreen("3ghi"); // -1: +
    mock.writeToScreen("4jAb"); //  0: +
    mock.writeToScreen("5mno"); //  1: | main screen
    mock.writeToScreen("6pab"); //  2: +

    auto& screen = mock.terminal.primaryScreen();
    auto constexpr NoLimit = size_t { 1000 };

    SECTION("an empty needle tallies nothing")
    {
        auto const tally = screen.tallyMatches(U"", CellLocation {}, SearchCaseSensitivity::Smart, NoLimit);
        CHECK(tally == SearchMatchTally {});
        CHECK(tally.empty());
    }

    SECTION("a needle that matches nothing tallies nothing, but is not an error")
    {
        auto const tally =
            screen.tallyMatches(U"zzz", CellLocation {}, SearchCaseSensitivity::Smart, NoLimit);
        CHECK(tally.total == 0);
        CHECK(tally.exactness == TallyExactness::Exact);
    }

    SECTION("counts every match across scrollback and main screen")
    {
        // "ab" appears at -3:0, -2:1, 0:2 (as "Ab") and 2:2. Lowercase needle, so smart case folds and
        // the capitalised one counts too.
        auto const tally = screen.tallyMatches(U"ab", CellLocation {}, SearchCaseSensitivity::Smart, NoLimit);
        CHECK(tally.total == 4);
        CHECK(tally.exactness == TallyExactness::Exact);
    }

    SECTION("the ordinal reports where the caller stands among the matches")
    {
        auto const at = [&](LineOffset line, ColumnOffset column) {
            return screen
                .tallyMatches(U"ab",
                              CellLocation { .line = line, .column = column },
                              SearchCaseSensitivity::Smart,
                              NoLimit)
                .ordinal;
        };

        // Ordinals run top-down through the grid, scrollback first.
        CHECK(at(LineOffset(-3), ColumnOffset(0)) == 1);
        CHECK(at(LineOffset(-2), ColumnOffset(1)) == 2);
        CHECK(at(LineOffset(0), ColumnOffset(2)) == 3);
        CHECK(at(LineOffset(2), ColumnOffset(2)) == 4);

        // Standing anywhere that is not the START of a match reports 0 rather than the nearest one:
        // "3 of 27" is a claim about being ON a match, and guessing would make it a lie.
        CHECK(at(LineOffset(-3), ColumnOffset(1)) == 0); // inside the first match, not at its start
        CHECK(at(LineOffset(1), ColumnOffset(0)) == 0);  // a line with no match at all
    }

    SECTION("case sensitivity decides whether the capitalised match counts")
    {
        auto const total = [&](SearchCaseSensitivity mode) {
            return screen.tallyMatches(U"ab", CellLocation {}, mode, NoLimit).total;
        };

        CHECK(total(SearchCaseSensitivity::Insensitive) == 4);
        CHECK(total(SearchCaseSensitivity::Sensitive) == 3); // "Ab" on line 0 drops out
        CHECK(total(SearchCaseSensitivity::Smart) == 4);     // needle is lowercase, so it folds

        // An uppercase needle pins smart case, which is what makes it "smart".
        CHECK(screen.tallyMatches(U"Ab", CellLocation {}, SearchCaseSensitivity::Smart, NoLimit).total == 1);
    }

    SECTION("counting stops at the limit and says so")
    {
        auto const capped = screen.tallyMatches(U"ab", CellLocation {}, SearchCaseSensitivity::Smart, 2);
        CHECK(capped.total == 2);
        CHECK(capped.exactness == TallyExactness::Capped);

        // A limit exactly equal to the match count is NOT capped: nothing was left uncounted.
        auto const exact = screen.tallyMatches(U"ab", CellLocation {}, SearchCaseSensitivity::Smart, 4);
        CHECK(exact.total == 4);
        CHECK(exact.exactness == TallyExactness::Exact);

        // A zero limit is a degenerate but legal request, not a crash.
        auto const none = screen.tallyMatches(U"ab", CellLocation {}, SearchCaseSensitivity::Smart, 0);
        CHECK(none.total == 0);
    }

    SECTION("overlapping matches are counted, because navigation visits them")
    {
        // "aa" over "ab1a" + "2abc": the wrapped logical line contains "aa" only once, but the point
        // here is that the tally advances by one cell like searchNextMatch does, so a needle that can
        // overlap itself is not silently collapsed.
        auto overlapping = MockTerm { PageSize { LineCount(2), ColumnCount(4) }, LineCount(4) };
        overlapping.writeToScreen("aaaa");
        auto const tally = overlapping.terminal.primaryScreen().tallyMatches(
            U"aa", CellLocation {}, SearchCaseSensitivity::Smart, NoLimit);
        CHECK(tally.total == 3); // at columns 0, 1 and 2
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

        // underflow: one below scroll bottom
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

        // visible screen
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

        // visible screen
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

TEST_CASE("setMaxHistoryLineCount", "[screen]")
{
    // from zero to something
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(2) }, LineCount(0) };
    auto& screen = mock.terminal.primaryScreen();
    screen.grid().setReflowOnResize(false);
    mock.writeToScreen("AB\r\nCD");
    REQUIRE("AB\nCD\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(1) });

    mock.terminal.setHistoryLimits(HistoryLimits::plain(LineCount(1)));
    REQUIRE("AB\nCD\n" == screen.renderMainPageText());
}

TEST_CASE("resize", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(2) }, LineCount(10) };
    auto& screen = mock.terminal.primaryScreen();
    screen.grid().setReflowOnResize(false);
    mock.writeToScreen("AB\r\nCD");
    REQUIRE("AB\nCD\n" == screen.renderMainPageText());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(1) });

    mock.terminal.setHistoryLimits(HistoryLimits::plain(LineCount(10)));

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

    mock.writeToScreen(ChessBoard);

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
                    REQUIRE(fragment->data() == white10x10());
                else
                    REQUIRE(fragment->data() == black10x10());

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

    mock.writeToScreen(ChessBoard);

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
                    REQUIRE(fragment->data() == black10x10());
                else
                    REQUIRE(fragment->data() == white10x10());
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

    mock.writeToScreen(ChessBoard);

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
                    REQUIRE(fragment->data() == white10x10());
                else
                    REQUIRE(fragment->data() == black10x10());

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

TEST_CASE("DEC Locator: DA1 includes ext 29", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    auto const exts = parseDA1Extensions(mock.replyData());
    CHECK(exts.contains(29)); // ext 29 = AnsiTextLocator
}

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

TEST_CASE("A hard reset leaves VT52", "[screen]")
{
    // VT52 has no ANSI grammar, so `ESC <` is the only sequence that leaves it -- not even RIS gets
    // through, as the section below pins. A program that dies in VT52 therefore leaves a terminal that
    // nothing the host sends can recover, and the user's Reset action (which calls hardReset() under the
    // lock, @see TerminalSession::operator()(actions::ClearHistoryAndReset)) is the only way out. It must
    // restore the ANSI parser, and restore the configured level while it is at it.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033[?2l"); // DECANM reset: enter VT52
    REQUIRE(mock.terminal.isVT52Mode());

    // RIS cannot do it: there is no RIS in the VT52 grammar, so the sequence never reaches hardReset().
    mock.writeToScreen("\033c");
    REQUIRE(mock.terminal.isVT52Mode());

    crispy::locked(mock.terminal, [&]() { mock.terminal.hardReset(); });
    CHECK_FALSE(mock.terminal.isVT52Mode());

    // Unlike `ESC <`, which lands at VT100, RIS restores the level the terminal was configured with.
    CHECK(mock.terminal.operatingLevel() == VTType::VT525);

    // And the ANSI grammar answers again.
    mock.writeToScreen("\033[3;4H");
    CHECK(screen.cursor().position == CellLocation { LineOffset(2), ColumnOffset(3) });
}

TEST_CASE("VPR: moves the cursor down, keeping its column", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033[2;3H"); // row 2, column 3
    REQUIRE(screen.cursor().position == CellLocation { LineOffset(1), ColumnOffset(2) });

    SECTION("an omitted parameter moves down one line")
    {
        mock.writeToScreen("\033[e");
        CHECK(screen.cursor().position == CellLocation { LineOffset(2), ColumnOffset(2) });
    }

    SECTION("Ps moves down that many lines")
    {
        mock.writeToScreen("\033[2e");
        CHECK(screen.cursor().position == CellLocation { LineOffset(3), ColumnOffset(2) });
    }

    SECTION("movement is clamped to the page")
    {
        mock.writeToScreen("\033[99e");
        CHECK(screen.cursor().position == CellLocation { LineOffset(4), ColumnOffset(2) });
    }
}

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

TEST_CASE("The icon and window titles are set independently", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(4) } };

    SECTION("OSC 0 sets both")
    {
        mock.writeToScreen("\033]0;both\033\\");
        CHECK(mock.iconTitle == "both");
        CHECK(mock.windowTitle == "both");
    }

    SECTION("OSC 1 sets the icon's alone")
    {
        mock.writeToScreen("\033]2;window\033\\");
        mock.writeToScreen("\033]1;icon\033\\");
        CHECK(mock.iconTitle == "icon");
        CHECK(mock.windowTitle == "window"); // OSC 1 used to be silently ignored entirely
    }

    SECTION("OSC 2 sets the window's alone")
    {
        mock.writeToScreen("\033]1;icon\033\\");
        mock.writeToScreen("\033]2;window\033\\");
        CHECK(mock.iconTitle == "icon");
        CHECK(mock.windowTitle == "window");
    }
}

TEST_CASE("A title is reported with its own OSC", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(4) } };

    mock.writeToScreen("\033]1;the-icon\033\\");
    mock.writeToScreen("\033]2;the-window\033\\");
    mock.resetReplyData();

    mock.writeToScreen("\033[20t"); // report the icon's title
    mock.writeToScreen("\033[21t"); // report the window's title
    INFO(mock.terminal.peekInput());
    REQUIRE(e(mock.terminal.peekInput())
            == e("\033]Lthe-icon\033\\"
                 "\033]lthe-window\033\\"));
}

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

TEST_CASE("Delta.imagePlacementBumpsExactlyTheCoveredLines", "[screen][delta]")
{
    auto const pageSize = PageSize { LineCount(11), ColumnCount(11) };
    auto mock = MockTerm { pageSize, LineCount(11) };
    mock.terminal.setCellPixelSize(ImageSize { Width(10), Height(10) });

    auto& grid = mock.terminal.primaryScreen().grid();
    auto cursor = drainedDeltaCursor(grid);

    mock.writeToScreen(ChessBoard); // a 10x10-cell sixel image over rows 0..9

    auto const changed = changedLineOffsets(grid, cursor);
    CHECK(changed == std::vector { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 });
}

TEST_CASE("Delta.sizedTextBumpsHeadAndContinuationRows", "[screen][delta]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };
    auto& grid = mock.terminal.primaryScreen().grid();
    auto cursor = drainedDeltaCursor(grid);

    // OSC 66 with s=2: the head lands on row 0, the continuation cells on row 1.
    mock.writeToScreen("\033]66;s=2:w=2;W\a"sv);

    CHECK(changedLineOffsets(grid, cursor) == std::vector { 0, 1 });
}

TEST_CASE("Delta.decdwlBumpsTheLine", "[screen][delta]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };
    auto& grid = mock.terminal.primaryScreen().grid();
    auto cursor = drainedDeltaCursor(grid);

    mock.writeToScreen("\033#6"sv); // DECDWL mutates the line via non-const flags()

    CHECK(changedLineOffsets(grid, cursor) == std::vector { 0 });
}

TEST_CASE("Delta.hyperlinkedTextCarriesItsIdThroughADeltaCycle", "[screen][delta]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };
    auto& grid = mock.terminal.primaryScreen().grid();
    auto cursor = drainedDeltaCursor(grid);

    mock.writeToScreen("\033]8;;https://example.com\033\\ab\033]8;;\033\\"sv);

    auto linkedColumns = 0;
    std::ignore = grid.forEachLineChangedSince(cursor, [&](LineOffset offset, Line const& line) {
        REQUIRE(offset == LineOffset(0));
        for (auto const& id: line.storage().hyperlinks)
            if (id != HyperlinkId {})
                ++linkedColumns;
    });
    CHECK(linkedColumns == 2); // "ab" carries the hyperlink id through the delta
}

TEST_CASE("a prompt mark on a scrolled-out logical line reaches the change stream", "[screen][delta]")
{
    // The end-to-end shape of Grid.delta.aHistoryRowDirtiedInPlaceIsReported: OSC 133;B stamps the
    // mark on the LOGICAL line's head, and logicalLineHead() walks up wrapped rows into the
    // scrollback. Nothing scrolls at that moment, so the row lies outside the prefix the delta scan
    // covers — and a daemon-hosted session's clients would keep the previous PromptEnd offset for
    // that logical line forever, making every feature built on it (copy last command output,
    // prompt-aware selection) select the wrong range client-side while working on the daemon.
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(4) }, LineCount(10) };
    auto& terminal = mock.terminal;
    auto& grid = terminal.primaryScreen().grid();

    // A wrapped logical line whose head then scrolls above the page.
    mock.writeToScreen("abcdefgh\r\n");
    mock.writeToScreen("\r\n");
    REQUIRE(unbox<int>(grid.historyLineCount()) >= 2);

    auto cursor = GridDeltaCursor {};
    std::ignore = grid.forEachLineChangedSince(cursor, [](LineOffset, Line const&) {});

    // The shell marks the prompt on the line the cursor sits on; its logical head is in history.
    mock.writeToScreen("\033]133;B\033\\");

    auto reported = std::vector<int> {};
    std::ignore = grid.forEachLineChangedSince(
        cursor, [&](LineOffset offset, Line const&) { reported.push_back(unbox<int>(offset)); });

    // Whichever row the head resolved to, the change reached the stream — before the fix an
    // above-the-page head reported nothing at all.
    CHECK_FALSE(reported.empty());
}

// NOLINTEND(misc-const-correctness,readability-function-cognitive-complexity)
