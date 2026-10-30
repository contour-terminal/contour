// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/input/InputGenerator.hpp>
#include <vtbackend/screen/Screen.hpp>
#include <vtbackend/screen/ScreenTestFixtures.hpp>
#include <vtbackend/screen/Viewport.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>
#include <vtbackend/vt/Charset.hpp>

#include <vtparser/BulkScan.hpp>

#include <core/Escape.hpp>
#include <core/Utils.hpp>

#include <libunicode/convert.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <charconv>
#include <cstddef>
#include <format>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
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

TEST_CASE("AppendChar.a_joiner_in_a_later_read_joins_the_last_bulk_written_cell", "[screen]")
{
    // A PTY read can end between a letter and whatever joins it. The bulk ASCII path wrote "abc" in
    // one go but remembered the run's FIRST cell as the one a following codepoint joins, so a
    // combining mark, a ZWJ or a variation selector arriving in the next read decorated the `a`.
    // A joiner in the same read as its base travels in the same text run and never reached that
    // path, which is what hid it. On either screen, and on a line whose renditions differ.
    auto const joiners = std::array {
        Scenario { .name = "U+0301 COMBINING ACUTE ACCENT", .bytes = "\xCC\x81" },
        Scenario { .name = "U+200D ZERO WIDTH JOINER", .bytes = "\xE2\x80\x8D" },
        Scenario { .name = "U+FE0F VARIATION SELECTOR-16", .bytes = "\xEF\xB8\x8F" },
    };
    auto const lines = std::array {
        Scenario { .name = "primary, uniform line", .bytes = "abc" },
        Scenario { .name = "primary, mixed line", .bytes = "\033[31mab\033[mc" },
        Scenario { .name = "alternate, uniform line", .bytes = "\033[?1049habc" },
        Scenario { .name = "alternate, mixed line", .bytes = "\033[?1049h\033[31mab\033[mc" },
    };

    for (auto const& line: lines)
        for (auto const& joiner: joiners)
        {
            INFO(std::format("{}, {}", line.name, joiner.name));
            auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(20) } };
            mock.writeToScreen(line.bytes);
            mock.writeToScreen(joiner.bytes); // a read of its own

            auto const& screen = std::as_const(mock.terminal).currentScreen();
            CHECK(screen.at(LineOffset(0), ColumnOffset(0)).codepoints() == U"a");
            CHECK(screen.at(LineOffset(0), ColumnOffset(2)).codepoints()
                  == U"c" + unicode::convert_to<char32_t>(joiner.bytes));
        }
}

TEST_CASE("AppendChar.a_joiner_after_a_bulk_run_that_filled_the_line", "[screen]")
{
    // The same seam where the run reaches the right margin and leaves a wrap pending: the joiner must
    // land exactly where it lands when the same bytes arrive in a single read. The bulk path stops one
    // column short of the margin, so this pins the boundary rather than a defect.
    struct Outcome
    {
        std::u32string firstCell;
        std::u32string lastCell;
        CellLocation cursor;
        bool wrapPending = false;
        bool operator==(Outcome const&) const = default;
    };
    auto const writeInReads = [](std::initializer_list<std::string_view> reads) {
        auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(5) } };
        for (auto const read: reads)
            mock.writeToScreen(read);
        auto const& screen = std::as_const(mock.terminal).currentScreen();
        return Outcome { .firstCell = screen.at(LineOffset(0), ColumnOffset(0)).codepoints(),
                         .lastCell = screen.at(LineOffset(0), ColumnOffset(4)).codepoints(),
                         .cursor = screen.cursor().position,
                         .wrapPending = screen.cursor().wrapPending };
    };

    // On either screen, and on a line whose renditions differ.
    auto const lines = std::array {
        Scenario { .name = "primary, uniform line", .bytes = "abcde" },
        Scenario { .name = "primary, mixed line", .bytes = "\033[31ma\033[mbcde" },
        Scenario { .name = "alternate, uniform line", .bytes = "\033[?1049habcde" },
        Scenario { .name = "alternate, mixed line", .bytes = "\033[?1049h\033[31ma\033[mbcde" },
    };
    for (auto const& line: lines)
    {
        INFO(line.name);
        auto const twoReads = writeInReads({ line.bytes, "\xCC\x81"sv });
        auto const oneRead = writeInReads({ std::string(line.bytes) + "\xCC\x81" });

        CHECK(twoReads.firstCell == U"a");
        CHECK(twoReads.lastCell == U"é");
        CHECK(twoReads == oneRead);
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

TEST_CASE("ScrollDown.aFullPageReverseScrollLeavesTheScrollbackAlone", "[screen]")
{
    // SD and RI at the top row scroll the page, the scrolling region, and nothing above it: xterm 407,
    // measured, keeps every saved line and drops the rows pushed off the page's bottom.
    struct ReverseScroll
    {
        std::string_view name;     ///< The case, for the test's report.
        std::string_view sequence; ///< What the application sends once the page holds L5 to L7.
        std::string_view rows;     ///< Every row afterwards, the four history rows first.
    };
    auto const scrolls = std::array {
        ReverseScroll { .name = "SD 2", .sequence = "\033[2T", .rows = "L1\nL2\nL3\nL4\n\n\nL5\n" },
        ReverseScroll {
            .name = "RI at the top row", .sequence = "\033[H\033M", .rows = "L1\nL2\nL3\nL4\n\nL5\nL6\n" },
    };
    for (auto const& scroll: scrolls)
    {
        INFO(scroll.name);
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) }, LineCount(10) };
        auto& screen = mock.terminal.primaryScreen();
        mock.writeToScreen("L1\r\nL2\r\nL3\r\nL4\r\nL5\r\nL6\r\nL7");
        REQUIRE(screen.historyLineCount() == LineCount(4));

        mock.writeToScreen(scroll.sequence);

        CHECK(screen.historyLineCount() == LineCount(4));
        auto rows = std::string {};
        for (auto const row: std::views::iota(-4, 3))
            rows += screen.grid().lineTextTrimmed(LineOffset(row)) + '\n';
        CHECK(rows == scroll.rows);
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
    // Mirrors esctest DECSETTests.doAltBuftest for the three alternate-screen modes. All three carry the
    // cursor into the alternate screen (xterm's continuous, terminal-level cursor); they differ in whether
    // it is carried back out (47, 1047) or the saved one restored (1049), and whether the alternate page is
    // erased on the way out (1047, 1049) or kept (47). @see Terminal::setAlternateScreen,
    // alternateScreenBehavior.
    int mode = 0;
    bool carriedOnEnter = false;  // 47, 1047, 1049: the cursor does not move entering the alternate screen
    bool carriedOnExit = false;   // 47, 1047: nor leaving it
    bool altErasedOnExit = false; // 1047, 1049: the alternate page is blank when re-entered
    SECTION("mode 47 (ALTBUF)")
    {
        mode = 47;
        carriedOnEnter = true;
        carriedOnExit = true;
        altErasedOnExit = false;
    }
    SECTION("mode 1047 (OPT_ALTBUF)")
    {
        mode = 1047;
        carriedOnEnter = true;
        carriedOnExit = true;
        altErasedOnExit = true;
    }
    SECTION("mode 1049 (OPT_ALTBUF_CURSOR)")
    {
        mode = 1049;
        carriedOnEnter = true;
        carriedOnExit = false;
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

    // Enter the alternate screen. No mode moves the cursor.
    mock.writeToScreen(set);
    REQUIRE(mock.terminal.isAlternateScreen());
    if (carriedOnEnter)
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
    CHECK(mock.terminal.currentScreen().cursor().position == (carriedOnExit ? beforeExit : primaryCursor));

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

TEST_CASE("AlternateScreen.1049_entersWithTheCursorWhereItWas", "[screen]")
{
    // ?1049h saves the cursor and switches with it: xterm's cursor is terminal-level (ToAlternate swaps the
    // line storage only), Windows Terminal copies the main cursor into its new alternate buffer, and foot's
    // cursor "retains its position from the normal grid". Not the alternate screen's own cursor, which is
    // where the previous full-screen application left it, with that application's rendition.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;
    mock.writeToScreen("\033[?1049h\033[4;6H\033[1mfull\033[?1049l"); // an earlier full-screen application
    mock.writeToScreen("\033[2;3H");                                  // the shell

    mock.writeToScreen("\033[?1049hX");
    CHECK(terminal.alternateScreen().grid().lineText(LineOffset(1)) == "  X       ");
    CHECK(terminal.alternateScreen().cursor().position == CellLocation { LineOffset(1), ColumnOffset(3) });
    CHECK_FALSE(terminal.alternateScreen().cursor().graphicsRendition.flags.contains(CellFlag::Bold));

    // Leaving, the cursor the primary page kept comes back: 1049's DECRC.
    mock.writeToScreen("\033[5;5H\033[?1049l");
    CHECK(terminal.primaryScreen().cursor().position == CellLocation { LineOffset(1), ColumnOffset(2) });
}

TEST_CASE("AlternateScreen.1049_savesTheCursorAsDECSCDoes", "[screen]")
{
    // ?1049h saves the cursor into the main screen's DECSC buffer before it switches: xterm's CursorSave
    // before ToAlternate, Windows Terminal's CursorSaveState() in _SetAlternateScreenBufferMode. A DECRC
    // after ?1049l therefore returns to where ?1049h was received, not to an older DECSC. 47 and 1047 save
    // nothing.
    auto const [mode, line, column] = GENERATE(table<int, int, int>({
        { 1049, 3, 4 }, // B, where ?1049h was received
        { 1047, 1, 2 }, // A, the DECSC
        { 47, 1, 2 },
    }));
    CAPTURE(mode);
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    mock.writeToScreen("\033[2;3H\0337"); // DECSC at A
    mock.writeToScreen("\033[4;5H");      // B
    mock.writeToScreen(std::format("\033[?{}h\033[?{}l", mode, mode));
    mock.writeToScreen("\033[H\0338"); // DECRC
    CHECK(mock.terminal.primaryScreen().cursor().position
          == CellLocation { LineOffset(line), ColumnOffset(column) });
}

TEST_CASE("AlternateScreen.1049_savesTheCursorPageAlsoFromTheStatusLine", "[screen]")
{
    // ?1049h received while DECSASD selects the status line still enters the alternate screen from the cursor
    // page, and so saves that page's cursor, the page with it, as DECSC on it would.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;
    mock.writeToScreen("\033[3 P\0337");                  // DECSC on page 3
    mock.writeToScreen("\033[1 P\033[4;5H");              // page 1, B
    mock.writeToScreen("\033[2$~\033[1$}");               // the status line
    mock.writeToScreen("\033[?1049h\033[?1049l\033[0$}"); // received there; back to the main display
    mock.writeToScreen("\033[H\0338");                    // DECRC
    CHECK(terminal.cursorPageIndex() == PageIndex(0));
    CHECK(terminal.primaryScreen().cursor().position == CellLocation { LineOffset(3), ColumnOffset(4) });
}

TEST_CASE("AlternateScreen.textAfterTheSwitchLandsWhereTheCarriedCursorIs", "[screen]")
{
    // 47 and 1047 carry the cursor across the switch both ways, so the next character is written where the
    // cursor was -- on its row, not on the row the other screen's own cursor was left on.
    auto const mode = GENERATE(47, 1047);
    CAPTURE(mode);
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) } };
    auto& terminal = mock.terminal;
    mock.writeToScreen("ab\r\nab"); // the cursor at line 1, column 2

    mock.writeToScreen(std::format("\033[?{}hX", mode));
    CHECK(terminal.alternateScreen().grid().lineText(LineOffset(0)) == "     ");
    CHECK(terminal.alternateScreen().grid().lineText(LineOffset(1)) == "  X  ");

    mock.writeToScreen(std::format("\033[3;2H\033[?{}lY", mode)); // leave from line 2, column 1
    CHECK(terminal.primaryScreen().grid().lineText(LineOffset(1)) == "ab   ");
    CHECK(terminal.primaryScreen().grid().lineText(LineOffset(2)) == " Y   ");
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

TEST_CASE("search.walksTextCellsAcrossWideCharactersAndPadding", "[screen]")
{
    // A needle is matched against the text a reader sees: a wide character is one character, and neither
    // its second half nor the padding a wide character leaves at the end of a row is any. foot's search
    // steps over its spacers the same way (search.c:377).
    using vtbackend::SearchCaseSensitivity;
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(10) }, LineCount(10) };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("ABCD\xE4\xB8\xAD"
                       "E"); // U+4E2D, two columns
    auto const top = CellLocation { LineOffset(0), ColumnOffset(0) };
    auto const end = CellLocation { LineOffset(3), ColumnOffset(9) };

    auto const find = [&](std::u32string_view needle) {
        return screen.search(needle, top, SearchCaseSensitivity::Smart);
    };
    auto const findBackwards = [&](std::u32string_view needle, CellLocation from) {
        return screen.searchReverse(needle, from, SearchCaseSensitivity::Smart);
    };

    SECTION("on one row")
    {
        CHECK(find(U"D中") == CellLocation { LineOffset(0), ColumnOffset(3) });
        CHECK(find(U"中E") == CellLocation { LineOffset(0), ColumnOffset(4) });
        CHECK(findBackwards(U"D中", end) == CellLocation { LineOffset(0), ColumnOffset(3) });
        CHECK(findBackwards(U"中E", end) == CellLocation { LineOffset(0), ColumnOffset(4) });
        // The second half begins no match, and holds no space either.
        CHECK_FALSE(find(U"中 E").has_value());
    }

    SECTION("across the padding a narrowing leaves")
    {
        // "ABCD" and padding, then "中E" on the row that continues it.
        mock.terminal.resizeScreen(PageSize { LineCount(4), ColumnCount(5) });
        auto const narrowEnd = CellLocation { LineOffset(3), ColumnOffset(4) };
        REQUIRE(screen.grid().lineAt(LineOffset(0)).storage().sgr[4].flags.contains(CellFlag::WrapPadding));

        CHECK(find(U"D中") == CellLocation { LineOffset(0), ColumnOffset(3) });
        CHECK(find(U"CD中E") == CellLocation { LineOffset(0), ColumnOffset(2) });
        CHECK(find(U"中E") == CellLocation { LineOffset(1), ColumnOffset(0) });
        CHECK(findBackwards(U"D中", narrowEnd) == CellLocation { LineOffset(0), ColumnOffset(3) });
        CHECK(findBackwards(U"中E", narrowEnd) == CellLocation { LineOffset(1), ColumnOffset(0) });
        // The padding is no space.
        CHECK_FALSE(find(U"D 中").has_value());
    }

    SECTION("across a soft wrap that leaves no padding")
    {
        // "ab中" fills its row, so "cd" goes on to the next.
        mock.writeToScreen("\033[2J");
        mock.terminal.resizeScreen(PageSize { LineCount(4), ColumnCount(4) });
        mock.writeToScreen("\033[Hab\xE4\xB8\xAD"
                           "cd");
        REQUIRE(screen.grid().lineAt(LineOffset(1)).isFlagEnabled(LineFlag::Wrapped));

        CHECK(find(U"中c") == CellLocation { LineOffset(0), ColumnOffset(2) });
        CHECK(findBackwards(U"中cd", CellLocation { LineOffset(3), ColumnOffset(3) })
              == CellLocation { LineOffset(0), ColumnOffset(2) });
    }

    SECTION("not across a hard line break")
    {
        // A full row, and a row of its own after it.
        mock.writeToScreen("\r\n0123456789\r\nFG");
        CHECK_FALSE(find(U"9F").has_value());
    }
}

namespace
{
/// The cells on the page the renderer paints as a search match of @p needle, as (row, column) pairs.
std::set<std::pair<int, int>> highlightedCells(MockTerm<>& mock, std::u32string needle)
{
    // Both match colours made one recognisable colour, so a painted cell says nothing but "matched".
    auto constexpr Matched = RGBColor { 0xAB, 0xCD, 0xEF };
    auto const paint = CellRGBColorAndAlphaPair { .foreground = RGBColor { 0, 0, 0 },
                                                  .foregroundAlpha = 1.0f,
                                                  .background = Matched,
                                                  .backgroundAlpha = 1.0f };
    mock.terminal.colorPalette().searchHighlight = paint;
    mock.terminal.colorPalette().searchHighlightFocused = paint;
    // An error is no failure here: a pattern that does not compile paints no cell, which is the answer.
    std::ignore = mock.terminal.setNewSearchTerm(std::move(needle), SearchOrigin::Typed);
    mock.terminal.refreshRenderBuffer(true);

    auto cells = std::set<std::pair<int, int>> {};
    for (auto const& cell: mock.terminal.renderBuffer().get().cells)
        if (cell.attributes.backgroundColor == Matched)
            cells.emplace(unbox(cell.position.line), unbox(cell.position.column));
    return cells;
}
} // namespace

TEST_CASE("search.highlightPaintsExactlyTheCellsTheSearchMatches", "[screen]")
{
    // The highlight asks the matcher the search asks, so a match the search jumps to is painted, on the
    // cells it covers and no others: a wide character's two columns, and not the padding it left behind.
    using Cells = std::set<std::pair<int, int>>;
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) }, LineCount(10) };
    mock.writeToScreen("ABCD\xE4\xB8\xAD"
                       "E"); // U+4E2D, two columns

    SECTION("on one row")
    {
        CHECK(highlightedCells(mock, U"D中") == Cells { { 0, 3 }, { 0, 4 }, { 0, 5 } });
        CHECK(highlightedCells(mock, U"中E") == Cells { { 0, 4 }, { 0, 5 }, { 0, 6 } });
    }

    SECTION("across the padding a narrowing leaves")
    {
        // "ABCD" and padding, then "中E" on the row that continues it.
        mock.terminal.resizeScreen(PageSize { LineCount(3), ColumnCount(5) });
        CHECK(highlightedCells(mock, U"D中") == Cells { { 0, 3 }, { 1, 0 }, { 1, 1 } });
        CHECK(highlightedCells(mock, U"中E") == Cells { { 1, 0 }, { 1, 1 }, { 1, 2 } });
        CHECK(highlightedCells(mock, U"CD中E") == Cells { { 0, 2 }, { 0, 3 }, { 1, 0 }, { 1, 1 }, { 1, 2 } });
    }

    SECTION("across the page's top edge")
    {
        // The row holding "ABCD" scrolls into the history; the match it begins goes on onto the page.
        mock.terminal.resizeScreen(PageSize { LineCount(3), ColumnCount(5) });
        mock.writeToScreen("\r\n\r\n");
        REQUIRE(mock.terminal.primaryScreen().grid().lineText(LineOffset(-1)).starts_with("ABCD"));
        CHECK(highlightedCells(mock, U"D中") == Cells { { 0, 0 }, { 0, 1 } });
        CHECK(highlightedCells(mock, U"中E") == Cells { { 0, 0 }, { 0, 1 }, { 0, 2 } });
        CHECK(highlightedCells(mock, U"CD中E") == Cells { { 0, 0 }, { 0, 1 }, { 0, 2 } });
    }
}

namespace
{
/// Replays @p screen's screenshot into a fresh terminal of the same size and checks that every cell came
/// back: its text (one codepoint per column), its width, its text sizing, and what it continues.
void checkScreenshotReplaysCellForCell(Screen const& screen, PageSize pageSize)
{
    auto replay = MockTerm { pageSize };
    replay.writeToScreen(screen.screenshot());
    auto const& replayed = replay.terminal.primaryScreen();

    for (auto const line: std::views::iota(0, unbox(pageSize.lines)))
    {
        INFO(std::format("line {}", line));
        CHECK(replayed.grid().lineAt(LineOffset(line)).toUtf8ColumnAligned()
              == screen.grid().lineAt(LineOffset(line)).toUtf8ColumnAligned());
        for (auto const column: std::views::iota(0, unbox(pageSize.columns)))
        {
            INFO(std::format("column {}", column));
            auto const at = CellLocation { LineOffset(line), ColumnOffset(column) };
            CHECK(replayed.at(at).width() == screen.at(at).width());
            CHECK(replayed.at(at).textScale() == screen.at(at).textScale());
            CHECK(replayed.at(at).isFlagEnabled(CellFlag::WideCharContinuation)
                  == screen.at(at).isFlagEnabled(CellFlag::WideCharContinuation));
            CHECK(replayed.at(at).isFlagEnabled(CellFlag::MulticellContinuation)
                  == screen.at(at).isFlagEnabled(CellFlag::MulticellContinuation));
        }
    }
}
} // namespace

TEST_CASE("search.highlightReachesAcrossRowsWithoutText", "[screen]")
{
    // A row made only of a tall text-sizing block's lower part gives a match no character, so it does not
    // count against how far above the page a match painted on it may begin. Here "aBb" begins four rows
    // above the page while being three characters long: the two rows between its "B" and its "b" are the
    // lower part of "B".
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(3) }, LineCount(10) };
    mock.writeToScreen("xxaBBBYYYZZZbcc");          // five rows, each continuing the one above
    mock.writeToScreen("\033[2;1H\033]66;s=3;B\a"); // a block three rows tall over "BBB", "YYY" and "ZZZ"
    mock.writeToScreen("\033[5;1H\n\n\n\n");        // "bcc" scrolls up to the page's top row
    auto& screen = mock.terminal.primaryScreen();
    REQUIRE(screen.grid().lineText(LineOffset(0)).starts_with("bcc"));
    REQUIRE(screen.grid()
                .lineAt(LineOffset(-2))
                .storage()
                .sgr[0]
                .flags.contains(CellFlag::MulticellContinuation));
    REQUIRE(screen.grid()
                .lineAt(LineOffset(-1))
                .storage()
                .sgr[2]
                .flags.contains(CellFlag::MulticellContinuation));
    REQUIRE(screen.search(
                U"aBb", CellLocation { LineOffset(-4), ColumnOffset(0) }, SearchCaseSensitivity::Sensitive)
            == CellLocation { LineOffset(-4), ColumnOffset(2) });

    CHECK(highlightedCells(mock, U"aBb") == std::set<std::pair<int, int>> { { 0, 0 } });
}

TEST_CASE("search.regexStepsAndWholeWordSkipsPartialWords", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) }, LineCount(10) };
    mock.writeToScreen("cat concat cat");
    REQUIRE(mock.terminal.setSearchOptions({ .wordBoundary = SearchWordBoundary::WholeWord }).has_value());
    REQUIRE(mock.terminal.setNewSearchTerm(U"cat", SearchOrigin::Typed).has_value());

    // The "cat" that ends "concat" at column 7 is no word of its own.
    CHECK(mock.terminal.searchNextMatch(CellLocation { LineOffset(0), ColumnOffset(0) })
          == CellLocation { LineOffset(0), ColumnOffset(11) });
}

TEST_CASE("search.invalidRegexHighlightsNothingAndSaysWhy", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) }, LineCount(10) };
    mock.writeToScreen("xa(b");
    REQUIRE(mock.terminal.setSearchOptions({ .syntax = SearchSyntax::Regex }).has_value());

    auto const invalid = mock.terminal.setNewSearchTerm(U"a(", SearchOrigin::Typed);
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error() == SearchPatternError::InvalidSyntax);
    CHECK(mock.terminal.search().problem == SearchPatternError::InvalidSyntax);
    CHECK(mock.terminal.search().query == nullptr);
    CHECK(mock.terminal.search().pattern == U"a(");
    CHECK_FALSE(mock.terminal.search(CellLocation {}).has_value());
    // Not even the text "a(" itself, which a literal search would find.
    CHECK(highlightedCells(mock, U"a(").empty());

    CHECK(mock.terminal.setNewSearchTerm(U"a", SearchOrigin::Typed).has_value());
    CHECK_FALSE(mock.terminal.search().problem.has_value());
    CHECK(mock.terminal.search().query != nullptr);
}

TEST_CASE("search.optionsReportWhetherTheyChanged", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) }, LineCount(10) };
    REQUIRE(mock.terminal.setNewSearchTerm(U"a", SearchOrigin::Typed) == SearchChange::Changed);
    auto const wholeWord = SearchOptions { .wordBoundary = SearchWordBoundary::WholeWord };

    CHECK(mock.terminal.setSearchOptions(wholeWord) == SearchChange::Changed);
    CHECK(mock.terminal.setSearchOptions(wholeWord) == SearchChange::Unchanged);
    CHECK(mock.terminal.setSearchOptions(
              { .syntax = SearchSyntax::Regex, .wordBoundary = SearchWordBoundary::WholeWord })
          == SearchChange::Changed);
    CHECK(mock.terminal.setNewSearchTerm(U"a", SearchOrigin::Typed) == SearchChange::Unchanged);
}

TEST_CASE("search.doubleClickWordIsLiteralUnderRegex", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) }, LineCount(10) };
    mock.writeToScreen("aab a+b");
    REQUIRE(mock.terminal.setSearchOptions({ .syntax = SearchSyntax::Regex }).has_value());

    CHECK(mock.terminal.setNewSearchTerm(U"a+b", SearchOrigin::DoubleClick).has_value());
    // The text "a+b", not the regex's "aab".
    CHECK(mock.terminal.search(CellLocation {}) == CellLocation { LineOffset(0), ColumnOffset(4) });
    CHECK(mock.terminal.search().options.syntax == SearchSyntax::Regex);

    // Typed, the same pattern is the regex again.
    CHECK(mock.terminal.setNewSearchTerm(U"a+b", SearchOrigin::Typed) == SearchChange::Changed);
    CHECK(mock.terminal.search(CellLocation {}) == CellLocation { LineOffset(0), ColumnOffset(0) });
}

TEST_CASE("search.matchStartingAtAndTheFocusedMatch", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) }, LineCount(10) };
    mock.writeToScreen("x\xE6\x97\xA5y abcd"); // x日y abcd
    auto const& screen = mock.terminal.primaryScreen();
    auto const at = [](int column) {
        return CellLocation { LineOffset(0), ColumnOffset(column) };
    };

    REQUIRE(mock.terminal.setNewSearchTerm(U"\u65E5", SearchOrigin::Typed).has_value());
    auto const query = mock.terminal.search().query;
    REQUIRE(query != nullptr);

    SECTION("a match begins at its first cell and ends on a wide character's second half")
    {
        auto const match = screen.matchStartingAt(query, at(1), mock.terminal.searchMatchMemo());
        REQUIRE(match.has_value());
        REQUIRE(match->has_value());
        CHECK((*match)->first == at(1));
        CHECK((*match)->second == at(2));
        auto const onSecondHalf = screen.matchStartingAt(query, at(2), mock.terminal.searchMatchMemo());
        REQUIRE(onSecondHalf.has_value());
        CHECK_FALSE(onSecondHalf->has_value());

        mock.terminal.moveNormalModeCursorTo(at(1));
        auto const focused = mock.terminal.focusedSearchMatch();
        REQUIRE(focused.has_value());
        CHECK(focused->first == at(1));
        CHECK(focused->second == at(2));
    }

    SECTION("a cursor inside a match is on none")
    {
        REQUIRE(mock.terminal.setNewSearchTerm(U"abc", SearchOrigin::Typed).has_value());
        mock.terminal.moveNormalModeCursorTo(at(6));
        CHECK_FALSE(mock.terminal.focusedSearchMatch().has_value());
        mock.terminal.moveNormalModeCursorTo(at(5));
        CHECK(mock.terminal.focusedSearchMatch().has_value());
    }

    SECTION("a dropped query focuses nothing")
    {
        mock.terminal.moveNormalModeCursorTo(at(1));
        REQUIRE(mock.terminal.setSearchOptions({ .syntax = SearchSyntax::Regex }).has_value());
        REQUIRE_FALSE(mock.terminal.setNewSearchTerm(U"(", SearchOrigin::Typed).has_value());
        CHECK_FALSE(mock.terminal.focusedSearchMatch().has_value());
    }
}

TEST_CASE("search.newWordDelimitersRecompileTheQuery", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) }, LineCount(10) };
    mock.writeToScreen("foo.bar");
    mock.terminal.setWordDelimiters(" ");
    REQUIRE(mock.terminal.setSearchOptions({ .wordBoundary = SearchWordBoundary::WholeWord }).has_value());
    REQUIRE(mock.terminal.setNewSearchTerm(U"foo", SearchOrigin::Typed).has_value());
    CHECK_FALSE(mock.terminal.search(CellLocation {}).has_value());

    mock.terminal.setWordDelimiters(" .");
    CHECK(mock.terminal.search(CellLocation {}) == CellLocation { LineOffset(0), ColumnOffset(0) });
}

TEST_CASE("Screen.screenshot.startsWhereTheReplayingCursorIs", "[screen]")
{
    // The state dump (Screen::inspect) prints the screenshot between lines of its own, into a log or a
    // console, so the screenshot moves no cursor to an absolute position: one would jump to the top of
    // the console it is printed on. A replay that wants the page back homes the cursor first.
    auto const pageSize = PageSize { LineCount(3), ColumnCount(5) };
    auto mock = MockTerm { pageSize };
    mock.writeToScreen("ab\r\ncd\r\nef");
    auto const& screen = mock.terminal.primaryScreen();
    auto const shot = screen.screenshot();
    CHECK_FALSE(shot.contains("\033[H"));
    CHECK_FALSE(shot.contains("\033[2J"));

    auto replay = MockTerm { pageSize };
    replay.writeToScreen("\033[2;3Hxyz\033[H"); // a cursor elsewhere, on a page that holds text
    replay.writeToScreen(shot);
    for (auto const line: std::views::iota(0, unbox(pageSize.lines)))
        CHECK(replay.terminal.primaryScreen().grid().lineText(LineOffset(line))
              == screen.grid().lineText(LineOffset(line)));
}

TEST_CASE("Screen.inspect.endsTheScreenshotBeforeItsRule", "[screen]")
{
    // The screenshot ends without a line break after its last row, so the dump around it puts one there
    // itself: each rule of '=' stands on a line of its own.
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(5) } };
    mock.writeToScreen("ab");
    auto os = std::stringstream {};
    mock.terminal.primaryScreen().inspect("message", os);
    auto const dump = os.str();
    INFO(dump);
    auto rules = 0;
    for (auto const at: std::views::iota(size_t { 0 }, dump.size()))
    {
        if (dump[at] != '=' || (at > 0 && dump[at - 1] == '='))
            continue; // not where a rule begins
        ++rules;
        CHECK((at == 0 || dump[at - 1] == '\n'));
    }
    CHECK(rules >= 3);
}

TEST_CASE("Screen.screenshot.replaysWideCharactersInPlace", "[screen]")
{
    // A screenshot is the VT stream that redraws the screen. Replaying it must put every character back in
    // its column: a wide character's second half is covered by the character itself, so a space written
    // for it pushes the rest of the row one column to the right. Replayed at the page's own height: a line
    // break between rows, and none after the last, which would scroll the page.
    auto const pageSize = PageSize { LineCount(3), ColumnCount(5) };
    auto mock = MockTerm { pageSize };
    // "中E" on its own row; "ABCD" and the padding a wide character leaves where it does not fit, then
    // "中" on the row after.
    mock.writeToScreen("\xE4\xB8\xAD"
                       "E\r\nABCD\xE4\xB8\xAD");
    auto const& screen = mock.terminal.primaryScreen();
    REQUIRE(screen.grid().lineAt(LineOffset(1)).storage().sgr[4].flags.contains(CellFlag::WrapPadding));

    checkScreenshotReplaysCellForCell(screen, pageSize);
}

TEST_CASE("Screen.screenshot.replaysTextSizingBlocks", "[screen]")
{
    // A text-sizing block replays as the OSC 66 that made it, as kitty's own ANSI dump of a line writes
    // one for each multicell character (line.c:60, write_multicell_ansi_prefix): its explicit width, its
    // scale, its fraction and alignment. The rows a tall block reaches down into are stepped over, not
    // written: writing there would destroy the block the row above just made.
    auto const pageSize = PageSize { LineCount(4), ColumnCount(10) };
    auto mock = MockTerm { pageSize };
    mock.writeToScreen("\033]66;w=3;X\az\r\n"            // one row, three columns
                       "\033]66;s=2;Y\aq"                // two rows and two columns, its own width
                       "\033]66;s=2:n=1:d=2:v=2:h=1;F\a" // drawn at half its scale, aligned
                       "\033[3;6Hr");                    // beside the blocks' lower part
    auto const& screen = mock.terminal.primaryScreen();
    REQUIRE(screen.at(CellLocation { LineOffset(1), ColumnOffset(0) }).scale() == 2);
    REQUIRE(screen.at(CellLocation { LineOffset(2), ColumnOffset(3) })
                .isFlagEnabled(CellFlag::MulticellContinuation));

    checkScreenshotReplaysCellForCell(screen, pageSize);
}

TEST_CASE("Screen.screenshot.leavesABlockWhoseHeadLeftThePageBlank", "[screen]")
{
    // A page screenshot cannot make a block whose head has scrolled into the history: OSC 66 makes a block
    // from its head down, and the head is not on the page. kitty's dump writes nothing for such cells
    // either (text_in_cell_ansi, line.c:139). They replay as blank cells, and what stands beside them keeps
    // its column.
    auto const pageSize = PageSize { LineCount(2), ColumnCount(6) };
    auto mock = MockTerm { pageSize, LineCount(5) };
    mock.writeToScreen("\033]66;s=2;X\a\033[2;3Hyz\033[2;1H\n"); // the head row scrolls into the history
    auto const& screen = mock.terminal.primaryScreen();
    REQUIRE(screen.at(CellLocation { LineOffset(0), ColumnOffset(0) })
                .isFlagEnabled(CellFlag::MulticellContinuation));
    REQUIRE(screen.grid().lineAt(LineOffset(0)).toUtf8ColumnAligned().starts_with("  yz"));

    auto replay = MockTerm { pageSize };
    replay.writeToScreen(screen.screenshot());
    auto const& replayed = replay.terminal.primaryScreen();
    CHECK(replayed.grid().lineAt(LineOffset(0)).toUtf8ColumnAligned().starts_with("  yz"));
    for (auto const column: { 0, 1 })
    {
        auto const at = CellLocation { LineOffset(0), ColumnOffset(column) };
        CHECK(replayed.at(at).empty());
        CHECK_FALSE(replayed.at(at).isFlagEnabled(CellFlag::MulticellContinuation));
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

TEST_CASE("findMarker visits user marks as well as prompt marks", "[screen][semanticblocks]")
{
    // ScrollMarkUp/ScrollMarkDown and CopyPreviousMarkRange walk these: `mm` fed them before it became a
    // user mark, and must go on feeding them.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(4) }, LineCount(10) };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("1abc\r\n2def\r\n3ghi\r\n4jkl\r\n5mno\r\n6pqr");
    REQUIRE(screen.historyLineCount() == LineCount { 3 });

    screen.setLogicalLineFlags(LineOffset(-2), LineFlag::UserMark, true); // 2def

    auto const up = screen.findMarkerUpwards(LineOffset(0));
    REQUIRE(up.has_value());
    CHECK(*up == LineOffset(-2));

    auto const down = screen.findMarkerDownwards(LineOffset(-3));
    REQUIRE(down.has_value());
    CHECK(*down == LineOffset(-2));
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

TEST_CASE("Screen.resize.narrowingStepByStepKeepsTheCursorColumn", "[screen]")
{
    // A window drag narrows the page one column per step. Each step used to add the cursor's column
    // to itself, so within a few steps the cursor ran to the right edge and stayed pinned there.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(10) };
    auto& screen = mock.terminal.primaryScreen();

    auto const narrowStepByStep = [&]() {
        mock.writeToScreen("\r\nABCD");
        REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(4) });
        for (auto const columns: std::views::iota(12, 20) | std::views::reverse)
        {
            mock.terminal.resizeScreen({ LineCount(5), ColumnCount(columns) });
            INFO(std::format("narrowed to {} columns", columns));
            CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(4) });
        }
    };

    SECTION("reflow enabled")
    {
        narrowStepByStep();
    }

    SECTION("reflow disabled")
    {
        screen.grid().setReflowOnResize(false);
        narrowStepByStep();
    }
}

TEST_CASE("Screen.resize.aReflowKeepsTheCursorOnItsText", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(10) };

    SECTION("the next character continues a line whose wrap was pending")
    {
        // Twenty characters on a twenty-column page leave the cursor on the last one, waiting to wrap.
        // The reflow already counts that wrap: the next character goes straight after the "T", rather
        // than wrapping a second time onto a row of its own.
        auto& screen = mock.terminal.primaryScreen();
        mock.writeToScreen("ABCDEFGHIJKLMNOPQRST");
        REQUIRE(screen.cursor().wrapPending);

        // "ABCDEF" "GHIJKL" "MNOPQR" "ST", above one of the four blank rows.
        mock.terminal.resizeScreen(PageSize { LineCount(5), ColumnCount(6) });
        mock.writeToScreen("X");
        CHECK(screen.grid().lineText(LineOffset(3)) == "STX   ");
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(3), ColumnOffset(3) });
    }

    SECTION("the alternate screen does not reflow, and clamps the cursor instead")
    {
        mock.writeToScreen("\033[?1049hABCDEFGHIJKL");
        auto const& screen = mock.terminal.alternateScreen();
        REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(12) });

        mock.terminal.resizeScreen(PageSize { LineCount(5), ColumnCount(8) });
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(7) });
        CHECK(screen.grid().lineText(LineOffset(1)) == "        ");

        mock.terminal.resizeScreen(PageSize { LineCount(5), ColumnCount(20) });
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(7) });
    }
}

TEST_CASE("Screen.resize.aHeightChangeKeepsAPendingWrap", "[screen]")
{
    // Only the line count changes, so the cursor still sits on the last cell of a full row with its wrap
    // pending: the next character wraps onto the next row instead of overwriting the "T". foot keeps the
    // wrap pending likewise (grid.c, the `lcf` step), as does xterm's ScreenResize, which leaves do_wrap.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) }, LineCount(10) };
    auto& screen = mock.terminal.primaryScreen();
    auto const reflow = GENERATE(true, false);
    CAPTURE(reflow);
    screen.grid().setReflowOnResize(reflow);

    mock.writeToScreen("ABCDEFGHIJKLMNOPQRST");
    REQUIRE(screen.cursor().wrapPending);

    auto const newHeight = GENERATE(LineCount(2), LineCount(5));
    CAPTURE(newHeight);
    mock.terminal.resizeScreen(PageSize { newHeight, ColumnCount(20) });
    CHECK(screen.cursor().wrapPending);

    mock.writeToScreen("X");
    CHECK(screen.grid().lineText(LineOffset(0)) == "ABCDEFGHIJKLMNOPQRST");
    CHECK(screen.grid().lineText(LineOffset(1)) == "X                   ");
}

TEST_CASE("Screen.resize.aReflowKeepsTheBlanksInsideALine", "[screen]")
{
    // An erase inside a wrapped line leaves blank cells that are part of the line: a reflow re-chops the
    // line, but must not close the gap. Only the blanks that end the logical line may go.
    auto mock = MockTerm { PageSize { LineCount(6), ColumnCount(10) }, LineCount(10) };
    auto& grid = mock.terminal.primaryScreen().grid();
    auto const firstLogicalLine = [&]() {
        auto text = grid.logicalLinesFrom(grid.addressableTop()).begin()->text();
        text.resize(core::trimRight(text).size());
        return text;
    };

    SECTION("widening")
    {
        mock.writeToScreen("ABCDEFGHIJKLMNO\033[1;6H\033[5X");
        mock.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(20) });
        CHECK(firstLogicalLine() == "ABCDE     KLMNO");
        CHECK(grid.lineText(LineOffset(0)) == "ABCDE     KLMNO     ");
    }

    SECTION("narrowing")
    {
        mock.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(20) });
        mock.writeToScreen("ABCDEFGHIJKLMNOPQRSTUVWXY\033[1;13H\033[8X");
        mock.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(8) });
        CHECK(firstLogicalLine() == "ABCDEFGHIJKL        UVWXY");
    }
}

TEST_CASE("Screen.resize.aNarrowingFillsTheBlankRowsBelowTheCursorFirst", "[screen]")
{
    // A prompt on the top row of an otherwise empty page wraps onto more rows when the page narrows. The
    // blank rows below it make room for that, as in kitty, foot and WezTerm, which drop the empty rows at
    // the bottom before they reflow: the prompt goes into the history only once the page is full.
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(20) }, LineCount(10) };
    auto const& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("0123456789ABC");

    SECTION("there is room")
    {
        // "0123456" "789ABC" and the cursor after them.
        mock.terminal.resizeScreen(PageSize { LineCount(4), ColumnCount(7) });
        CHECK(screen.historyLineCount() == LineCount(0));
        CHECK(screen.grid().lineText(LineOffset(0)) == "0123456");
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(6) });
    }

    SECTION("there is not")
    {
        // "012" "345" "678" "9AB" "C": one row more than the page holds.
        mock.terminal.resizeScreen(PageSize { LineCount(4), ColumnCount(3) });
        CHECK(screen.historyLineCount() == LineCount(1));
        CHECK(screen.grid().lineText(LineOffset(-1)) == "012");
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(3), ColumnOffset(1) });
    }
}

TEST_CASE("Screen.resize.aReflowCarriesTheSavedCursorToo", "[screen]")
{
    // The cursor DECSC saved goes with its text like the live one, as foot and kitty track it: DECRC after
    // the resize comes back to the same character.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(10) };
    auto const& screen = mock.terminal.primaryScreen();
    auto const charUnderCursor = [&]() {
        auto const position = screen.logicalCursorPosition();
        return screen.grid().lineText(position.line).at(unbox<size_t>(position.column));
    };

    SECTION("DECSC and DECRC")
    {
        // Saved on the "C"; the live cursor on the bottom row keeps every row in use, so "0123456789"
        // goes into the history.
        mock.writeToScreen("0123456789ABCDEFGHIJKLMNO\033[1;13H\0337\033[5;1H");
        mock.terminal.resizeScreen(PageSize { LineCount(5), ColumnCount(10) });
        mock.writeToScreen("\0338");
        CHECK(charUnderCursor() == 'C');
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(2) });
    }

    SECTION("a full-screen program on the alternate screen")
    {
        // The resize happens while the alternate screen shows; the primary one follows when it comes back,
        // and its cursor is the one saved on the way out.
        mock.writeToScreen("user@host:~$ \033[?1049h");
        mock.terminal.resizeScreen(PageSize { LineCount(5), ColumnCount(6) });
        mock.writeToScreen("\033[?1049lls");
        CHECK(screen.grid().lineText(LineOffset(2)) == " ls   ");
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(2), ColumnOffset(3) });
    }
}

TEST_CASE("Screen.write.aWideCharacterThatDoesNotFitItsRow", "[screen]")
{
    // A two-column character arriving at the last column does not fit there. Measured in xterm on a
    // 10-column page: with autowrap, "ABCDEFGHI中X" leaves the last cell of the first row and puts the
    // character on the next one; without, it drops the character and the "X" lands in the last column.
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(10) }, LineCount(10) };
    auto& screen = mock.terminal.primaryScreen();
    auto const wide = std::string_view { "\xE4\xB8\xAD" }; // U+4E2D, two columns

    SECTION("autowrap moves it to the next row")
    {
        mock.writeToScreen(std::format("ABCDEFGHI{}X", wide));
        CHECK(screen.grid().lineAt(LineOffset(0)).toUtf8ColumnAligned() == "ABCDEFGHI ");
        CHECK(screen.grid().lineText(LineOffset(1)).starts_with(std::format("{}X", wide)));
        CHECK(screen.grid().lineAt(LineOffset(1)).isFlagEnabled(LineFlag::Wrapped));
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(3) });
    }

    SECTION("a widening reads the line back whole")
    {
        // The cell left behind is not part of the text: the reflow does not keep it as a blank.
        mock.writeToScreen(std::format("ABCDEFGHI{}X", wide));
        mock.terminal.resizeScreen(PageSize { LineCount(4), ColumnCount(20) });
        auto text = screen.grid().lineText(LineOffset(0));
        text.resize(core::trimRight(text).size());
        CHECK(text == std::format("ABCDEFGHI{}X", wide));
    }

    SECTION("without autowrap it is dropped")
    {
        mock.writeToScreen(std::format("\033[?7lABCDEFGHI{}X", wide));
        CHECK(screen.grid().lineText(LineOffset(0)) == "ABCDEFGHIX");
        CHECK(screen.grid().lineText(LineOffset(1)) == "          ");
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(9) });
    }

    SECTION("without autowrap it is dropped, with nothing written after it")
    {
        // Not left there half on the page for the next character to clear away.
        mock.writeToScreen(std::format("\033[?7lABCDEFGHI{}", wide));
        CHECK(screen.grid().lineText(LineOffset(0)) == "ABCDEFGHI ");
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(9) });
    }
}

TEST_CASE("Screen.resize.aNarrowingMovesAWideCharacterWhole", "[screen]")
{
    // "ABCD中EF" on 10 columns puts the two-column character on columns 4 and 5. Narrowed to 5, the
    // new edge falls between its halves: the character moves on to the next row whole, as foot and kitty
    // move it, and the cell it leaves is padding a widening drops again.
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(10) }, LineCount(10) };
    auto const& screen = mock.terminal.primaryScreen();
    auto const wide = std::string_view { "\xE4\xB8\xAD" }; // U+4E2D, two columns
    mock.writeToScreen(std::format("ABCD{}EF", wide));

    SECTION("the text")
    {
        mock.terminal.resizeScreen(PageSize { LineCount(4), ColumnCount(5) });
        CHECK(screen.grid().lineAt(LineOffset(0)).toUtf8ColumnAligned() == "ABCD ");
        CHECK(screen.grid().lineText(LineOffset(1)).starts_with(std::format("{}EF", wide)));
        CHECK(screen.grid().lineAt(LineOffset(1)).isFlagEnabled(LineFlag::Wrapped));

        mock.terminal.resizeScreen(PageSize { LineCount(4), ColumnCount(10) });
        auto text = screen.grid().lineText(LineOffset(0));
        text.resize(core::trimRight(text).size());
        CHECK(text == std::format("ABCD{}EF", wide));
    }

    SECTION("re-chopped further, and widened to an edge it straddles again")
    {
        // "ABCDE中X" narrowed to 3: the first row keeps "ABC", and the overflow "DE中X" is cut again
        // across the character, which moves on whole: "DE" and padding, then "中X". Widened to 6, the
        // joined line is cut across it once more: "ABCDE" and padding, then "中X".
        mock.writeToScreen(std::format("\r\033[KABCDE{}X", wide));
        mock.terminal.resizeScreen(PageSize { LineCount(4), ColumnCount(3) });
        CHECK(screen.grid().lineText(LineOffset(0)) == "ABC");
        CHECK(screen.grid().lineAt(LineOffset(1)).toUtf8ColumnAligned() == "DE ");
        CHECK(screen.grid().lineText(LineOffset(2)).starts_with(std::format("{}X", wide)));

        mock.terminal.resizeScreen(PageSize { LineCount(4), ColumnCount(6) });
        CHECK(screen.grid().lineAt(LineOffset(0)).toUtf8ColumnAligned() == "ABCDE ");
        CHECK(screen.grid().lineText(LineOffset(1)).starts_with(std::format("{}X", wide)));

        mock.terminal.resizeScreen(PageSize { LineCount(4), ColumnCount(10) });
        auto text = screen.grid().lineText(LineOffset(0));
        text.resize(core::trimRight(text).size());
        CHECK(text == std::format("ABCDE{}X", wide));
    }

    SECTION("the cursor on the wide character")
    {
        mock.writeToScreen("\033[1;5H");
        mock.terminal.resizeScreen(PageSize { LineCount(4), ColumnCount(5) });
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(0) });
        mock.terminal.resizeScreen(PageSize { LineCount(4), ColumnCount(10) });
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(4) });
    }

    SECTION("the cursor after a character a second cut moved")
    {
        // "ABCDE中X" narrowed to 3 is "ABC", "DE" and padding, "中X": the "X" ends on the third row.
        mock.writeToScreen(std::format("\r\033[KABCDE{}X\033[1;8H", wide)); // on the "X"
        mock.terminal.resizeScreen(PageSize { LineCount(4), ColumnCount(3) });
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(2), ColumnOffset(2) });
        mock.terminal.resizeScreen(PageSize { LineCount(4), ColumnCount(10) });
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(7) });
    }

    SECTION("the cursor after it")
    {
        mock.writeToScreen("\033[1;7H"); // on the "E"
        mock.terminal.resizeScreen(PageSize { LineCount(4), ColumnCount(5) });
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(2) });
        mock.terminal.resizeScreen(PageSize { LineCount(4), ColumnCount(10) });
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(6) });
    }
}

TEST_CASE("Screen.resize.aReflowMovesATextSizingBlockWhole", "[screen]")
{
    // An OSC 66 block is no more divisible than a wide character, and one taller than a row reaches into the
    // rows below its head: a reflow moves it whole, its part on those rows along with it, as kitty's rewrap
    // does. The cells it leaves at the end of a row are padding, which a widening drops again.
    auto mock = MockTerm { PageSize { LineCount(6), ColumnCount(10) }, LineCount(10) };
    auto const& screen = mock.terminal.primaryScreen();
    // The block a cell belongs to, as "line,column columnsxrows" of its head, or "none".
    auto const blockAt = [&](int line, int column) -> std::string {
        auto const block = screen.multicellBlockAt(
            CellLocation { .line = LineOffset(line), .column = ColumnOffset(column) });
        if (!block)
            return "none";
        return std::format(
            "{},{} {}x{}", block->origin.line, block->origin.column, block->columns, block->rows);
    };
    auto const resizeTo = [&](int columns) {
        mock.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(columns) });
    };
    auto const trimmedText = [&](int line) {
        auto text = screen.grid().lineText(LineOffset(line));
        text.resize(core::trimRight(text).size());
        return text;
    };

    SECTION("three columns wide, straddling the new edge")
    {
        mock.writeToScreen("ABCD\033]66;w=3;X\aEF"); // the block on columns 4 to 6
        REQUIRE(blockAt(0, 6) == "0,4 3x1");

        resizeTo(6);
        CHECK(trimmedText(0) == "ABCD");
        CHECK(blockAt(1, 2) == "1,0 3x1");
        CHECK(trimmedText(1) == "XEF");

        resizeTo(10);
        CHECK(blockAt(0, 6) == "0,4 3x1");
        CHECK(trimmedText(0) == "ABCDXEF");
    }

    SECTION("two rows tall, straddling the new edge")
    {
        mock.writeToScreen("ABCD\033]66;s=2;X\aEF"); // the block on columns 4 and 5 of rows 0 and 1
        REQUIRE(blockAt(1, 5) == "0,4 2x2");

        resizeTo(5);
        CHECK(trimmedText(0) == "ABCD");
        CHECK(blockAt(1, 0) == "1,0 2x2");
        CHECK(blockAt(2, 1) == "1,0 2x2");
        CHECK(blockAt(2, 4) == "none");
        CHECK(trimmedText(1) == "XEF");

        resizeTo(10);
        CHECK(blockAt(1, 5) == "0,4 2x2");
        CHECK(blockAt(2, 0) == "none");
        CHECK(trimmedText(0) == "ABCDXEF");
    }

    SECTION("a run of tall blocks, re-chopped")
    {
        // Four blocks two rows tall in a row. Narrowed to 5, a row takes two of them, and the row below each
        // pair is theirs: the second pair starts below the first one's, as kitty lays it out.
        mock.writeToScreen("\033]66;s=2;abcd\a");
        REQUIRE(blockAt(1, 7) == "0,6 2x2");

        resizeTo(5);
        CHECK(blockAt(1, 3) == "0,2 2x2");
        CHECK(blockAt(2, 0) == "2,0 2x2");
        CHECK(blockAt(3, 3) == "2,2 2x2");
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(2), ColumnOffset(4) });

        resizeTo(10);
        CHECK(blockAt(1, 7) == "0,6 2x2");
        CHECK(blockAt(1, 1) == "0,0 2x2");
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(8) });
        CHECK(trimmedText(2).empty());
    }

    SECTION("wider than the new width")
    {
        // kitty drops a block it has no row wide enough for, and the text after it closes up, the cursor with
        // it: past "CD", on the row the narrowing adds for it.
        mock.writeToScreen("AB\033]66;w=3;X\aCD");
        resizeTo(2);
        CHECK(trimmedText(0) == "AB");
        CHECK(trimmedText(1) == "CD");
        CHECK(screen.grid().lineAt(LineOffset(1)).isFlagEnabled(LineFlag::Wrapped));
        CHECK(screen.logicalCursorPosition() == CellLocation { LineOffset(2), ColumnOffset(0) });
        for (auto const column: std::views::iota(0, 2))
            CHECK(blockAt(0, column) == "none");
    }

    SECTION("taller and wider than the new width")
    {
        mock.writeToScreen("\033]66;s=3;Y\a"); // three columns, three rows
        resizeTo(2);
        for (auto const line: std::views::iota(0, 6))
            for (auto const column: std::views::iota(0, 2))
                CHECK(blockAt(line, column) == "none");
    }
}

TEST_CASE("Screen.resize.whereACommandEndedMovesWithItsTextPastTextSizingBlocks", "[screen]")
{
    // ;D's column names the cell after the output's last character. A reflow that drops a block before it on
    // its row closes the text up, and one that makes a row for a tall block's lower part inside the line
    // moves the rows after it down: either way the column must still name that cell.
    auto mock = MockTerm { PageSize { LineCount(8), ColumnCount(12) }, LineCount(10) };
    auto const& screen = mock.terminal.primaryScreen();
    auto const& grid = screen.grid();
    // The logical column of the cell after the last "Y" on the page.
    auto const afterY = [&]() -> ColumnOffset {
        for (auto const row: std::views::iota(0, 8) | std::views::reverse)
        {
            auto const& cells = grid.lineAt(LineOffset(row)).storage();
            for (auto const column: std::views::iota(size_t { 0 }, cells.codepoints.size()))
                if (cells.codepoints[column] == U'Y')
                    return grid.logicalColumnOf(CellLocation {
                        .line = LineOffset(row), .column = ColumnOffset::cast_from(column + 1) });
        }
        return ColumnOffset(-1);
    };

    SECTION("a block dropped before it")
    {
        mock.writeToScreen("\033]133;C\033\\AB\033]66;w=3;X\aXY\033]133;D;0\033\\\r\n");
        mock.terminal.resizeScreen(PageSize { LineCount(8), ColumnCount(2) });
        CHECK(grid.lineAt(LineOffset(0)).commandEndOffset() == afterY());
    }

    SECTION("a row made for a tall block's lower part")
    {
        mock.writeToScreen("\033]133;C\033\\\033]66;s=2;abcd\aXY\033]133;D;0\033\\\r\n\r\n");
        mock.terminal.resizeScreen(PageSize { LineCount(8), ColumnCount(5) });
        CHECK(grid.lineAt(LineOffset(0)).commandEndOffset() == afterY());
    }
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

    core::locked(mock.terminal, [&]() { mock.terminal.hardReset(); });
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
    // A row's first visit stamps its birth time, and dirties it once: the image's trailing linefeed
    // would reach row 10 for the first time, so visit it before draining.
    mock.writeToScreen("\033[11;1H\033[H");
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

TEST_CASE("DECSWBV: sets the warning bell volume", "[screen]")
{
    // Ps 1 is off, 2-4 low, 0/5-8 high for the warning bell (VT510 manual).
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(4) } };

    mock.writeToScreen("\033[1 t");
    CHECK(mock.terminal.settings().warningBellVolume == BellVolume::Off);

    mock.writeToScreen("\033[3 t");
    CHECK(mock.terminal.settings().warningBellVolume == BellVolume::Low);

    mock.writeToScreen("\033[8 t");
    CHECK(mock.terminal.settings().warningBellVolume == BellVolume::High);

    mock.writeToScreen("\033[1 t");
    REQUIRE(mock.terminal.settings().warningBellVolume == BellVolume::Off);

    mock.writeToScreen("\033[0 t");
    CHECK(mock.terminal.settings().warningBellVolume == BellVolume::High);
}

TEST_CASE("DECSWBV: a Ps outside 0-8 is rejected", "[screen]")
{
    // Ps 9 falls past the 5-8 (High) case into the switch's default, which is Invalid; the
    // volume must be left exactly as it was rather than snapping to some fallback level.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(4) } };

    mock.writeToScreen("\033[3 t");
    REQUIRE(mock.terminal.settings().warningBellVolume == BellVolume::Low);

    mock.writeToScreen("\033[9 t");
    CHECK(mock.terminal.settings().warningBellVolume == BellVolume::Low);
}

TEST_CASE("DECSWBV: a hand-built two-parameter sequence is rejected", "[screen]")
{
    // DECSWBV is registered with maximumParameters = 1, so Functions::select() already refuses a
    // two-parameter "CSI Ps ; Ps SP t" before Screen::apply() is ever called -- writeToScreen() can't
    // reach the handler's own multi-parameter guard at all. This bypasses the parser and calls
    // apply() directly, to pin down what the guard itself does if that dispatch-layer invariant is
    // ever weakened, rather than relying on select() alone to keep bad input out.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(4) } };

    mock.writeToScreen("\033[3 t");
    REQUIRE(mock.terminal.settings().warningBellVolume == BellVolume::Low);

    auto seq = Sequence {};
    seq.setCategory(FunctionCategory::CSI);
    seq.intermediateCharacters() = " ";
    seq.setFinalChar('t');
    auto builder = SequenceParameterBuilder { seq.parameters() };
    builder.set(1);
    builder.nextParameter();
    builder.set(2);
    builder.fixiate();
    REQUIRE(seq.parameterCount() == 2);

    auto const result = mock.terminal.primaryScreen().apply(DECSWBV, seq);
    CHECK(result == ApplyResult::Invalid);
    CHECK(mock.terminal.settings().warningBellVolume == BellVolume::Low);
}

TEST_CASE("DECSWBV: default parameter selects high", "[screen]")
{
    // An omitted Ps maps to 0, which the VT510 manual defines as high -- the warning bell's default.
    // (xterm's ctlseqs lists 0 as off here; DEC's own manual is followed instead.)
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(4) } };

    mock.writeToScreen("\033[1 t");
    REQUIRE(mock.terminal.settings().warningBellVolume == BellVolume::Off);

    mock.writeToScreen("\033[ t");
    CHECK(mock.terminal.settings().warningBellVolume == BellVolume::High);
}

TEST_CASE("DECSWBV: the volume is restored by RIS", "[screen]")
{
    // If a program turns off the warning bell volume and then it exits, the user
    // is left with a terminal with no bell volume. RIS serves the purpose of restoring
    // the bell volume when this happens and this test enforces that.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(4) } };

    mock.writeToScreen("\033[1 t");
    REQUIRE(mock.terminal.settings().warningBellVolume == BellVolume::Off);

    mock.writeToScreen("\033c"); // RIS
    CHECK(mock.terminal.settings().warningBellVolume == BellVolume::High);
}

TEST_CASE("DECSMBV: sets the margin bell volume", "[screen]")
{
    // Ps 0/1 is off, 2-4 is low, 5-8 is high for the margin bell (VT510 manual).
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(4) } };

    mock.writeToScreen("\033[8 u");
    CHECK(mock.terminal.settings().marginBellVolume == BellVolume::High);

    mock.writeToScreen("\033[3 u");
    CHECK(mock.terminal.settings().marginBellVolume == BellVolume::Low);

    mock.writeToScreen("\033[1 u");
    CHECK(mock.terminal.settings().marginBellVolume == BellVolume::Off);

    mock.writeToScreen("\033[8 u");
    REQUIRE(mock.terminal.settings().marginBellVolume == BellVolume::High);

    mock.writeToScreen("\033[0 u");
    CHECK(mock.terminal.settings().marginBellVolume == BellVolume::Off);
}

TEST_CASE("DECSMBV: default parameter turns the margin bell off", "[screen]")
{
    // An omitted Ps maps to 0, which the VT510 manual defines as off -- the margin bell's default.
    // (xterm's ctlseqs lists 0 as high here; DEC's own manual is followed instead.)
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(4) } };

    mock.writeToScreen("\033[8 u");
    REQUIRE(mock.terminal.settings().marginBellVolume == BellVolume::High);

    mock.writeToScreen("\033[ u");
    CHECK(mock.terminal.settings().marginBellVolume == BellVolume::Off);
}

TEST_CASE("DECSMBV: the margin bell starts off and RIS restores that", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(4) } };
    REQUIRE(mock.terminal.settings().marginBellVolume == BellVolume::Off);

    mock.writeToScreen("\033[8 u");
    REQUIRE(mock.terminal.settings().marginBellVolume == BellVolume::High);

    mock.writeToScreen("\033c"); // RIS
    CHECK(mock.terminal.settings().marginBellVolume == BellVolume::Off);
}

TEST_CASE("DECSMBV: a Ps outside 0-8 is rejected", "[screen]")
{
    // Ps 9 falls past every defined value into the switch's default, which is Invalid; the volume
    // must be left exactly as it was rather than snapping to some fallback level.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(4) } };

    mock.writeToScreen("\033[3 u");
    REQUIRE(mock.terminal.settings().marginBellVolume == BellVolume::Low);

    mock.writeToScreen("\033[9 u");
    CHECK(mock.terminal.settings().marginBellVolume == BellVolume::Low);
}

TEST_CASE("DECSMBV: a hand-built two-parameter sequence is rejected", "[screen]")
{
    // DECSMBV is registered with maximumParameters = 1, so Functions::select() already refuses a
    // two-parameter "CSI Ps ; Ps SP u" before Screen::apply() is ever called -- writeToScreen() can't
    // reach the handler's own multi-parameter guard at all. This bypasses the parser and calls
    // apply() directly, to pin down what the guard itself does if that dispatch-layer invariant is
    // ever weakened, rather than relying on select() alone to keep bad input out.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(4) } };

    mock.writeToScreen("\033[3 u");
    REQUIRE(mock.terminal.settings().marginBellVolume == BellVolume::Low);

    auto seq = Sequence {};
    seq.setCategory(FunctionCategory::CSI);
    seq.intermediateCharacters() = " ";
    seq.setFinalChar('u');
    auto builder = SequenceParameterBuilder { seq.parameters() };
    builder.set(1);
    builder.nextParameter();
    builder.set(2);
    builder.fixiate();
    REQUIRE(seq.parameterCount() == 2);

    auto const result = mock.terminal.primaryScreen().apply(DECSMBV, seq);
    CHECK(result == ApplyResult::Invalid);
    CHECK(mock.terminal.settings().marginBellVolume == BellVolume::Low);
}

TEST_CASE("LineShape.neverUndercutsTheCellsAfterAnEdit", "[screen][LineShape]")
{
    // Every fast path trusts LineSoA::shape: the renderer batches a Uniform line, and the parser's bulk
    // write needs anything short of Spanning. So after ANY edit -- on lines that had already left
    // Uniform -- the stored shape must be at least as conservative as what the cells hold, and writing
    // into the edited line again must keep it so.
    for (auto const& scenario: EditsOfShapedLines)
    {
        INFO(scenario.name);
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
        mock.writeToScreen(scenario.bytes);
        mock.writeToScreen("\033[1;1Hxyz"sv); // and write into the edited line again
        checkShapeCoversCells(mock.terminal.primaryScreen());
    }
}

TEST_CASE("LineShape.neverUndercutsTheCellsAfterAResize", "[screen][LineShape]")
{
    // A resize rebuilds lines -- reflow copies cells into new ones on the primary screen, while the
    // alternate screen only cuts or pads them -- and both must carry the shape along.
    for (auto const screenSelect: { ""sv, "\033[?1049h"sv })
    {
        INFO((screenSelect.empty() ? "primary (reflows)" : "alternate (does not reflow)"));
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) }, LineCount(5) };
        mock.writeToScreen(screenSelect);
        mock.writeToScreen("\033[44mblu\033[m e\xCC\x81 ta\r\n"sv); // Mixed: a colour and a cluster
        mock.writeToScreen("ab\xE4\xB8\xAD\xE4\xB8\xAD"
                           "cd\r\n"sv); // Spanning: wide characters
        mock.terminal.resizeScreen(PageSize { LineCount(3), ColumnCount(6) });
        mock.terminal.resizeScreen(PageSize { LineCount(3), ColumnCount(12) });
        mock.writeToScreen("\033[1;1Hxyz"sv);
        checkShapeCoversCells(std::as_const(mock.terminal).currentScreen());
    }
}

TEST_CASE("LineShape.anImageFragmentSpans", "[screen][LineShape]")
{
    auto mock = MockTerm { PageSize { LineCount(11), ColumnCount(11) }, LineCount(11) };
    mock.terminal.setCellPixelSize(ImageSize { Width(10), Height(10) });
    mock.writeToScreen(ChessBoard);
    CHECK(mock.terminal.primaryScreen().grid().lineAt(LineOffset(0)).storage().shape == LineShape::Spanning);
}

namespace
{
/// What a page shows: every line's text with its renditions (as `capture-pane -e` renders it, with
/// ESC escaped) and the width of each cell, then the cursor.
std::vector<std::string> pageSnapshotOf(Screen const& screen)
{
    auto const columns = screen.pageSize().columns;
    auto snapshot = std::vector<std::string> {};
    for (auto const lineNo: std::views::iota(0, *screen.pageSize().lines))
    {
        auto const& line = screen.grid().lineAt(LineOffset(lineNo));
        auto widths = std::string {};
        for (auto const column: std::views::iota(0, *columns))
            widths += static_cast<char>('0' + line.cellWidthAt(ColumnOffset(column)));
        snapshot.push_back(
            std::format("{} [{}]",
                        core::escape(line.toUtf8WithSgr(ColumnOffset(0), boxed_cast<ColumnOffset>(columns))),
                        widths));
    }
    auto const& cursor = screen.cursor();
    snapshot.push_back(std::format("cursor {}:{}{}",
                                   *cursor.position.line,
                                   *cursor.position.column,
                                   cursor.wrapPending ? " (wrap pending)" : ""));
    return snapshot;
}
} // namespace

TEST_CASE("BulkText.theGateFollowsTheLineShapeOnEveryScreen", "[screen][LineShape]")
{
    // Only a line holding a spanning cell is refused the bulk write -- not the alternate screen, and not
    // a line that has left the batched render path.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    mock.writeToScreen("\033[?1049h"sv);
    CHECK(mock.terminal.maxBulkTextSequenceWidth() == 9);

    mock.writeToScreen("\033[31mab\033[mc"sv); // a Mixed line
    REQUIRE(std::as_const(mock.terminal).currentScreen().currentLine().storage().shape == LineShape::Mixed);
    CHECK(mock.terminal.maxBulkTextSequenceWidth() == 6);

    mock.writeToScreen("\xE4\xB8\xAD"sv); // a wide character: the line now spans
    CHECK(mock.terminal.maxBulkTextSequenceWidth() == 0);
}

TEST_CASE("BulkText.theRunIsSizedOnTheStatusLineBeingWritten", "[screen]")
{
    // DECSASD sends text to the host-writable status line, which has margins of its own. The cursor was
    // already the status line's -- DECSASD repoints the current screen at it -- but the run was sized
    // from the MAIN page's right margin: with the status line's cursor right of it, `margin - cursor`
    // went negative and wrapped to a huge width, overflowing the status line.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    mock.writeToScreen("\033[?69h\033[1;4s"sv); // the main page: right margin at column 4
    mock.writeToScreen("\033[2$~\033[1$}"sv);   // DECSSDT: host-writable; DECSASD: write to it
    mock.terminal.setMode(DECMode::AutoWrap, false);

    mock.writeToScreen("abcde"sv);
    mock.writeToScreen("fghijk"sv); // one past the end of the line

    CHECK(mock.terminal.hostWritableStatusLineDisplay().grid().lineText(LineOffset(0)) == "abcdefghik");
}

TEST_CASE("BulkText.theGateAsksTheLineOfTheDisplayBeingWritten", "[screen][LineShape]")
{
    // The gate asked page 0's current line whether it may be bulk-written, even while DECSASD sends text
    // to the status line: a blank main page let the bulk write into a status line that spans.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    mock.writeToScreen("\033[2$~\033[1$}"sv); // DECSSDT: host-writable; DECSASD: write to it
    mock.writeToScreen("\xE4\xB8\xAD"sv);     // a wide character: the status line now spans
    CHECK(mock.terminal.maxBulkTextSequenceWidth() == 0);
}

TEST_CASE("BulkText.aCursorRightOfTheRightMarginGetsNoBulkRun", "[screen]")
{
    // With DECLRMM set, CUP can put the cursor right of the right margin, where the margin no longer
    // applies. Measuring the run as `right margin - cursor` went negative there and wrapped to a huge
    // width, handing Screen::writeText more cells than the line has left.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    auto const& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("\033[?69h\033[2;4s\033[1;7H"sv); // margins at columns 2..4, cursor at column 7
    CHECK(mock.terminal.maxBulkTextSequenceWidth() == 0);

    mock.writeToScreen("abcdefgh"sv);
    CHECK(screen.grid().lineText(LineOffset(0)) == "      abcd");
}

TEST_CASE("BulkText.aPendingWrapGetsNoBulkRun", "[screen][LineShape]")
{
    // A pending wrap moves the run to the NEXT line before it is written, and the gate asks only about
    // the current one. At a real deferred wrap the cursor stands on the right margin, where the gate
    // returns 0 anyway; but the wrap survives a margin widened under it, which leaves the cursor left of
    // the margin. The run then landed on a line holding a wide character and overwrote its head without
    // erasing its continuation.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    auto const& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("\033[2;1Hab\xE4\xB8\xAD"sv); // line 1 holds a wide character at columns 2..3

    SECTION("a margin widened under it")
    {
        mock.writeToScreen("\033[?69h\033[1;5s"sv); // DECSLRM at columns 1..5, which homes the cursor
        mock.writeToScreen("abcde\033[?69l"sv);     // a wrap pending at column 4, then DECLRMM reset
        REQUIRE(screen.cursor().wrapPending);
        CHECK(mock.terminal.maxBulkTextSequenceWidth() == 0);

        mock.writeToScreen("xyz"sv);
        CHECK(screen.at(LineOffset(1), ColumnOffset(2)).codepoints() == U"z");
        CHECK_FALSE(screen.at(LineOffset(1), ColumnOffset(3)).isFlagEnabled(CellFlag::WideCharContinuation));
    }
    SECTION("restored after a resize")
    {
        // The widening takes the saved cursor's wrap as it takes the live one's
        // (Screen::applyPageSizeToMainDisplay): the row has room now for what would have wrapped, so DECRC
        // comes back right after its text, and the run leaves the next line's wide character whole.
        mock.writeToScreen("\033[1;1H0123456789\0337"sv); // a wrap pending at column 9, then DECSC
        mock.terminal.resizeScreen(PageSize { LineCount(3), ColumnCount(14) });
        mock.writeToScreen("\0338"sv); // DECRC
        REQUIRE_FALSE(screen.cursor().wrapPending);
        REQUIRE(screen.cursor().position
                == CellLocation { .line = LineOffset(0), .column = ColumnOffset(10) });

        mock.writeToScreen("xyz"sv);
        CHECK(screen.grid().lineText(LineOffset(0)) == "0123456789xyz ");
        CHECK(screen.at(LineOffset(1), ColumnOffset(2)).codepoints() == U"中");
        CHECK(screen.at(LineOffset(1), ColumnOffset(3)).isFlagEnabled(CellFlag::WideCharContinuation));
    }
}

TEST_CASE("BulkText.aSingleShiftMapsOneCharacterOnly", "[screen]")
{
    // SS2 invokes G2 for the next graphic character alone. The bulk write asked whether the table for
    // that one character holds US-ASCII, and then wrote the whole run through it.
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(10) } };
    mock.writeToScreen("\033(0\033*B"sv); // G0: DEC special graphics, G2: US-ASCII
    mock.writeToScreen("\033Nqqq"sv);     // SS2, then three times "q"
    CHECK(mock.terminal.primaryScreen().grid().lineText(LineOffset(0)) == "q\xE2\x94\x80\xE2\x94\x80       ");
}

TEST_CASE("BulkText.theAlternateScreenWritesWhatThePrimaryWrites", "[screen]")
{
    // Both screens take the same text paths, so they must agree on every mode those paths test. The
    // comparison against a per-codepoint parser is BulkScan_test's.
    auto const scenarios = std::array {
        Scenario { .name = "insert mode", .bytes = "XYZ\r\033[4hab" },
        Scenario { .name = "autowrap off", .bytes = "\033[?7labcdefghijkl" },
        Scenario { .name = "left/right margins", .bytes = "\033[?69h\033[3;6s\033[1;3Habcdefgh" },
        Scenario { .name = "origin mode inside margins",
                   .bytes = "\033[2;3r\033[?69h\033[3;6s\033[?6h\033[1;1Habcdefghij" },
        Scenario { .name = "coloured runs",
                   .bytes = "\033[31mred\033[0m plain\r\n\033[1;44mbold-blue\033[m" },
        Scenario { .name = "a cluster, then ASCII",
                   .bytes = "e\xCC\x81"
                            "xyz" },
        Scenario { .name = "ASCII over a wide character", .bytes = "\xE4\xB8\xAD\xE4\xB8\xAD\r ab" },
        Scenario { .name = "ASCII over a wide character's tail",
                   .bytes = "\xE4\xB8\xAD\xE4\xB8\xAD\033[1;2Hxy" },
        Scenario { .name = "DEC special graphics", .bytes = "\033(0lqk\033(Bok" },
        Scenario { .name = "autowrap across lines", .bytes = "0123456789abcdefghijklmn" },
    };

    auto const pageAfter = [](std::string_view screenSelect, std::string_view bytes) {
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
        mock.writeToScreen(screenSelect);
        mock.writeToScreen(bytes);
        return pageSnapshotOf(mock.terminal.currentScreen());
    };

    for (auto const& scenario: scenarios)
    {
        INFO(scenario.name);
        CHECK(pageAfter(""sv, scenario.bytes) == pageAfter("\033[?1049h"sv, scenario.bytes));
    }
}

TEST_CASE("BulkText.aRecycledLineIsCleanWhateverItHeld", "[screen][LineShape]")
{
    // At full scrollback each new line is the storage of the one evicted from the top. Whatever that
    // line held -- colours, a cluster, wide characters -- must be gone from the line it becomes, and
    // ASCII written there must land in a clean, uniform line.
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(10) }, LineCount(1) };
    auto const& screen = mock.terminal.primaryScreen();
    auto const cursorLine = [&]() -> Line const& {
        return screen.grid().lineAt(screen.cursor().position.line);
    };

    mock.writeToScreen("\033[41mred\033[m e\xCC\x81\r\n"sv); // Mixed: a colour and a cluster
    mock.writeToScreen("\xE4\xB8\xAD\xE4\xB8\xAD\r\n"sv);    // Spanning: wide characters
    mock.writeToScreen("filler\r\n"sv);                      // the ring is full from here on

    mock.writeToScreen("plain"sv); // lands on the line that held "red é"
    CHECK(cursorLine().toUtf8WithSgr(ColumnOffset(0), ColumnOffset(10)) == "plain     ");
    CHECK(cursorLine().storage().shape == LineShape::Uniform);

    mock.writeToScreen("\r\nmore"sv); // lands on the line that held the wide characters
    CHECK(cursorLine().toUtf8WithSgr(ColumnOffset(0), ColumnOffset(10)) == "more      ");
    CHECK(cursorLine().storage().shape == LineShape::Uniform);
    for (auto const column: std::views::iota(0, 10))
        CHECK(cursorLine().cellWidthAt(ColumnOffset(column)) == 1);
}

TEST_CASE("BulkText.aReadEndingInsideACharacterKeepsItWhole", "[screen]")
{
    // The read ends after the lead byte of "ä", with nothing complete before it since the SGR. The
    // parser once decoded that lead byte twice, so the screen showed U+FFFD ahead of the "ä" and the
    // cursor stood one cell too far right.
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(5) } };
    auto const& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("a\033[m\xC3"sv);
    mock.writeToScreen("\xA4"sv);

    CHECK(screen.grid().lineText(LineOffset(0)) == "a\xC3\xA4   ");
    CHECK(screen.cursor().position == CellLocation { LineOffset(0), ColumnOffset(2) });
}

TEST_CASE("BulkText.anInvalidByteInsideARunShowsAsAReplacementCharacter", "[screen]")
{
    // The state machine prints U+FFFD for a byte that is not UTF-8. The bulk text path hands such a
    // byte on as it came, inside its run, and Screen::writeText decoded the run with a decoder that
    // drops it: "\xFF│X" showed "│X". Compared against a parser that runs without its bulk scanners.
    auto const pageSize = PageSize { LineCount(2), ColumnCount(6) };
    auto const pageAfter = [&](std::string_view screenSelect, vtparser::BulkScan bulkScan) {
        auto settings = MockTerm<>::createSettings(pageSize, HistoryLimits {}, 1024);
        settings.parserBulkScan = bulkScan;
        auto mock = MockTerm<> { std::move(settings) };
        mock.writeToScreen(screenSelect);
        mock.writeToScreen("\xFF\xE2\x94\x82X"sv);
        auto snapshot = pageSnapshotOf(mock.terminal.currentScreen());
        snapshot.insert(snapshot.begin(), mock.terminal.currentScreen().grid().lineText(LineOffset(0)));
        return snapshot;
    };

    for (auto const screenSelect: { ""sv, "\033[?1049h"sv })
    {
        INFO((screenSelect.empty() ? "primary" : "alternate"));
        auto const reference = pageAfter(screenSelect, vtparser::BulkScan::Disabled);
        REQUIRE(reference.front() == "\xEF\xBF\xBD\xE2\x94\x82X   ");
        CHECK(pageAfter(screenSelect, vtparser::BulkScan::Enabled) == reference);
    }
}

// NOLINTEND(misc-const-correctness,readability-function-cognitive-complexity)
