// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the kitty text sizing protocol (OSC 66): its metadata parser, the column arithmetic
// it implies, and the grid effect of laying text out through it.

#include <vtbackend/MockTerm.h>
#include <vtbackend/TextSizing.h>

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <ranges>
#include <string_view>

using namespace std::string_view_literals;
using namespace vtbackend;
using namespace vtbackend::text_sizing;

// {{{ metadata parser

TEST_CASE("TextSizing.parse.defaults", "[textsizing]")
{
    auto const request = parseRequest(";hello"sv);
    REQUIRE(request.has_value());
    // scale defaults to 1 and width to 0 -- "lay this out as you normally would".
    CHECK(request->scale == 1);
    CHECK(request->width == 0);
    CHECK(request->numerator == 0);
    CHECK(request->denominator == 0);
    CHECK(request->text == "hello");
}

TEST_CASE("TextSizing.parse.keys_are_colon_separated", "[textsizing]")
{
    // Unusual for an OSC: the metadata pairs are separated by COLONS, and the semicolon separates
    // metadata from text. Splitting on semicolons instead silently loses every key but the first.
    auto const request = parseRequest("s=2:w=3:v=1:h=2;x"sv);
    REQUIRE(request.has_value());
    CHECK(request->scale == 2);
    CHECK(request->width == 3);
    CHECK(request->verticalAlignment == 1);
    CHECK(request->horizontalAlignment == 2);
    CHECK(request->text == "x");
}

TEST_CASE("TextSizing.parse.text_may_contain_semicolons", "[textsizing]")
{
    auto const request = parseRequest("s=2;a;b"sv);
    REQUIRE(request.has_value());
    CHECK(request->text == "a;b");
}

TEST_CASE("TextSizing.parse.rejects_out_of_range", "[textsizing]")
{
    CHECK(parseRequest("s=0;x"sv).error() == Error::ValueOutOfRange); // scale is 1..7
    CHECK(parseRequest("s=8;x"sv).error() == Error::ValueOutOfRange);
    CHECK(parseRequest("w=8;x"sv).error() == Error::ValueOutOfRange);  // width is 0..7
    CHECK(parseRequest("n=16;x"sv).error() == Error::ValueOutOfRange); // numerator is 0..15
    CHECK(parseRequest("v=3;x"sv).error() == Error::ValueOutOfRange);  // alignment is 0..2
    CHECK(parseRequest("s=x;x"sv).error() == Error::MalformedKey);
    CHECK(parseRequest("s;x"sv).error() == Error::MalformedKey);
}

TEST_CASE("TextSizing.parse.fraction_must_be_proper", "[textsizing]")
{
    // d must be greater than n when set; 3/2 is not a fraction of a cell.
    CHECK(parseRequest("n=3:d=2;x"sv).error() == Error::BadFraction);
    CHECK(parseRequest("n=1:d=2;x"sv).has_value());
    // d=0 means "no fractional scaling", so n is free.
    CHECK(parseRequest("n=5;x"sv).has_value());
}

TEST_CASE("TextSizing.parse.ignores_unknown_keys", "[textsizing]")
{
    auto const request = parseRequest("s=2:Q=9;x"sv);
    REQUIRE(request.has_value());
    CHECK(request->scale == 2);
}

TEST_CASE("TextSizing.columnsFor", "[textsizing]")
{
    // With an explicit width the application states the size, so the text's own width is irrelevant.
    auto explicitWidth = Request { .scale = 2, .width = 3, .text = {} };
    CHECK(explicitWidth.columnsFor(1) == 6);
    CHECK(explicitWidth.columnsFor(2) == 6);

    // Without one, each cell the text would normally take becomes a scale-wide block.
    auto derived = Request { .scale = 3, .width = 0, .text = {} };
    CHECK(derived.columnsFor(1) == 3);
    CHECK(derived.columnsFor(2) == 6);

    // The default request changes nothing.
    CHECK(Request {}.columnsFor(1) == 1);
    CHECK(Request {}.columnsFor(2) == 2);
}

// }}}
// {{{ grid effect

TEST_CASE("TextSizing.width_advances_scale_times_width", "[textsizing]")
{
    // The probe blessed/ucs-detect uses to decide the width mechanism is supported: one space with
    // w=2 must leave the cursor two columns further right.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033]66;w=2; \a"sv);
    CHECK(screen.cursor().position.column == ColumnOffset(2));

    auto const& head = screen.at(LineOffset(0), ColumnOffset(0));
    CHECK(head.width() == 2);
    CHECK(head.codepoints() == U" ");

    // The second column belongs to the block, not to the next character.
    CHECK(screen.at(LineOffset(0), ColumnOffset(1)).isFlagEnabled(CellFlag::WideCharContinuation));
}

TEST_CASE("TextSizing.scale_advances_and_records_the_scale", "[textsizing]")
{
    // The scale probe: one space with s=2 occupies a 2x2 block, so the cursor advances two columns.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033]66;s=2; \a"sv);
    CHECK(screen.cursor().position.column == ColumnOffset(2));

    auto const& head = screen.at(LineOffset(0), ColumnOffset(0));
    CHECK(head.width() == 2);
    CHECK(head.scale() == 2);
    // The continuation carries the scale too, so the renderer sees a uniform block.
    CHECK(screen.at(LineOffset(0), ColumnOffset(1)).scale() == 2);
}

TEST_CASE("TextSizing.scale_times_width_compose", "[textsizing]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(20) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033]66;s=2:w=3;X\a"sv);
    // s*w = 6, regardless of X being one column on its own.
    CHECK(screen.cursor().position.column == ColumnOffset(6));
    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).width() == 6);
    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).scale() == 2);
}

TEST_CASE("TextSizing.without_width_each_cluster_is_scaled", "[textsizing]")
{
    // w=0 means "split as you normally would, then make each cell a scale-wide block", so two
    // characters at s=2 occupy two separate 2-column blocks rather than one 4-column block.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(20) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033]66;s=2;ab\a"sv);
    CHECK(screen.cursor().position.column == ColumnOffset(4));
    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).codepoints() == U"a");
    CHECK(screen.at(LineOffset(0), ColumnOffset(2)).codepoints() == U"b");
}

TEST_CASE("TextSizing.ordinary_text_is_unaffected", "[textsizing]")
{
    // A default request must lay out exactly as plain text would -- otherwise every application that
    // wraps its output in OSC 66 defensively would shift.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033]66;;abc\a"sv);
    CHECK(screen.cursor().position.column == ColumnOffset(3));
    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).scale() == 1);
}

TEST_CASE("TextSizing.a_block_is_never_split_across_lines", "[textsizing]")
{
    // A block that will not fit moves to the next line whole. Splitting it would silently change the
    // size the application asked for.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("abcd"sv); // cursor at column 4, one column left
    mock.writeToScreen("\033]66;w=3;X\a"sv);

    CHECK(screen.cursor().position.line == LineOffset(1));
    CHECK(screen.cursor().position.column == ColumnOffset(3));
    CHECK(screen.at(LineOffset(1), ColumnOffset(0)).width() == 3);
}

TEST_CASE("TextSizing.malformed_request_writes_nothing", "[textsizing]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033]66;s=99;X\a"sv);
    CHECK(screen.cursor().position.column == ColumnOffset(0));
    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).empty());
}

TEST_CASE("TextSizing.scale_is_reset_by_ordinary_writes", "[textsizing]")
{
    // A scaled cell that is later overwritten by plain text must not keep its scale, or the new text
    // would be drawn at a size nobody asked for.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033]66;s=3;X\a"sv);
    REQUIRE(screen.at(LineOffset(0), ColumnOffset(0)).scale() == 3);

    mock.writeToScreen("\033[H"sv); // home
    mock.writeToScreen("y"sv);
    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).scale() == 1);
    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).width() == 1);
}

// }}}

TEST_CASE("TextSizing.a_block_wider_than_the_line_is_dropped", "[textsizing]")
{
    // Clipping would silently hand the application a different size than it asked for, so a block
    // that can never fit is not drawn at all. kitty drops it too.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(2), ColumnCount(4) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033]66;s=3:w=3;X\a"sv); // 9 columns onto a 4-column line
    CHECK(screen.cursor().position.column == ColumnOffset(0));
    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).empty());
}

TEST_CASE("TextSizing.without_autowrap_a_block_is_placed_against_the_right_edge", "[textsizing]")
{
    // kitty's move_cursor_past_multicell() clamps rather than drops when DECAWM is off.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(2), ColumnCount(6) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.terminal.setMode(DECMode::AutoWrap, false);

    mock.writeToScreen("abcde"sv); // cursor at column 5, one column left
    mock.writeToScreen("\033]66;w=3;X\a"sv);

    // Placed at columns 3..5, not dropped and not split. The cursor stays on the last column
    // rather than stepping past it; with autowrap off there is nowhere further to go.
    CHECK(screen.cursor().position.line == LineOffset(0));
    CHECK(screen.cursor().position.column == ColumnOffset(5));
    CHECK(screen.at(LineOffset(0), ColumnOffset(3)).width() == 3);
    CHECK(screen.at(LineOffset(0), ColumnOffset(3)).codepoints() == U"X");
}

TEST_CASE("TextSizing.overwriting_a_block_destroys_all_of_it", "[textsizing]")
{
    // Half a glyph cannot be drawn, so writing into the middle of a block clears the whole block --
    // not just the column written, and not just one column back. kitty's nuke_multicell_char_at()
    // does the same.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(2), ColumnCount(10) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033]66;w=4;X\a"sv);
    REQUIRE(screen.at(LineOffset(0), ColumnOffset(0)).width() == 4);

    // Write into column 2, the middle of the block.
    mock.writeToScreen("\033[1;3H"sv);
    mock.writeToScreen("y"sv);

    // The head is gone, not left behind as a stale 4-wide cell.
    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).empty());
    CHECK(screen.at(LineOffset(0), ColumnOffset(1)).empty());
    CHECK(screen.at(LineOffset(0), ColumnOffset(2)).codepoints() == U"y");
    CHECK(screen.at(LineOffset(0), ColumnOffset(3)).empty());
}

TEST_CASE("TextSizing.selection_yields_the_text_once", "[textsizing]")
{
    // The whole point of the continuation-skip in SelectionRenderer: a six-column block must copy as
    // its text, not its text followed by five spaces.
    //
    // Two lines, not one: an `s=2` block needs two rows, and a page that cannot hold it drops it
    // whole (as kitty does) -- which would leave this asserting on an empty screen rather than on
    // the selection behaviour it is about.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(2), ColumnCount(10) } };
    mock.writeToScreen("\033]66;s=2:w=3;X\a"sv);
    mock.writeToScreen("z"sv);

    mock.terminal.setSelector(std::make_unique<vtbackend::LinearSelection>(
        mock.terminal.selectionHelper(),
        CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) },
        []() {}));
    (void) mock.terminal.selector()->extend(
        CellLocation { .line = LineOffset(0), .column = ColumnOffset(6) });
    mock.terminal.selector()->complete();

    CHECK(mock.terminal.extractSelectionText() == "Xz");
}

TEST_CASE("TextSizing.a_scaled_block_claims_the_rows_beneath_it", "[textsizing]")
{
    // s=3 is three cells TALL as well as wide -- the first thing in this grid that occupies more
    // than one line. The rows beneath must be claimed, or ordinary text would be written into the
    // middle of the block.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(10) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033]66;s=3;X\a"sv);

    for (auto row = 1; row < 3; ++row)
        for (auto col = 0; col < 3; ++col)
        {
            INFO("row " << row << " column " << col);
            CHECK(
                screen.at(LineOffset(row), ColumnOffset(col)).isFlagEnabled(CellFlag::MulticellContinuation));
        }

    // The row below the block is untouched.
    CHECK_FALSE(screen.at(LineOffset(3), ColumnOffset(0)).isFlagEnabled(CellFlag::MulticellContinuation));
}

TEST_CASE("TextSizing.writing_below_a_block_destroys_all_of_it", "[textsizing]")
{
    // Touching any part of a block destroys all of it -- including from a row the block merely
    // reaches down into, which the previous horizontal-only erase could not find.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(10) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033]66;s=2:w=2;X\a"sv); // 4 columns wide, 2 rows tall
    REQUIRE(screen.at(LineOffset(0), ColumnOffset(0)).width() == 4);

    // Write into the SECOND row of the block, which is not the head's line at all.
    mock.writeToScreen("\033[2;2H"sv);
    mock.writeToScreen("y"sv);

    // The head, a row above and three columns left, is gone.
    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).empty());
    CHECK(screen.at(LineOffset(0), ColumnOffset(3)).empty());
    CHECK(screen.at(LineOffset(1), ColumnOffset(1)).codepoints() == U"y");
}

TEST_CASE("TextSizing.a_block_with_no_room_below_scrolls_rather_than_being_clipped", "[textsizing]")
{
    // A block is indivisible on BOTH axes, so one written with too few rows beneath it scrolls the
    // page to make room -- it is never written short. kitty does the same in
    // handle_fixed_width_multicell_command().
    //
    // This is the ordinary case, not a corner: a terminal that has printed anything sits on its last
    // line, so nearly every block a real program writes arrives with no room below it. Clipping left
    // only the head row, and the head row draws band 0 -- the TOP slice of the glyph -- so all that
    // reached the screen was a sliver of the glyph's top edge.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(10) }, LineCount(10) };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("one\r\ntwo\r\nthree\r\nfour"sv); // fills the page; cursor on the last line
    mock.writeToScreen("\r\n\033]66;s=2;X\a"sv);

    // The block landed whole, with its continuation row beneath it...
    auto const head = screen.at(LineOffset(2), ColumnOffset(0));
    CHECK(head.codepoints() == U"X");
    CHECK(head.scale() == 2);
    CHECK(screen.at(LineOffset(3), ColumnOffset(0)).isFlagEnabled(CellFlag::MulticellContinuation));

    // ...and the page scrolled to find that room, taking the oldest line into history.
    CHECK(screen.grid().lineText(LineOffset(0)) == "three     ");
}

TEST_CASE("TextSizing.a_block_taller_than_the_page_is_dropped", "[textsizing]")
{
    // Scrolling cannot help a block taller than the scroll region itself, and a clipped block is a
    // different size from the one the application asked for -- so it is dropped whole, as kitty's
    // `height > max_height` guard does.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(2), ColumnCount(10) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033]66;s=4;X\a"sv);

    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).codepoints().empty());
    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).scale() == 1);
}

TEST_CASE("TextSizing.multicellBlockAt_finds_the_block_from_any_of_its_cells", "[textsizing]")
{
    // One authority for a block's extent, reached from anywhere inside it: the erase path and the
    // selection path must not disagree about which cells belong together.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(10) } };
    auto const& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033]66;s=2:w=3;X\a"sv); // 6 columns wide, 2 rows tall

    for (auto const row: { 0, 1 })
        for (auto const column: { 0, 1, 2, 3, 4, 5 })
        {
            INFO("row " << row << " column " << column);
            auto const block = screen.multicellBlockAt(
                CellLocation { .line = LineOffset(row), .column = ColumnOffset(column) });
            REQUIRE(block.has_value());
            CHECK(block->origin == CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) });
            CHECK(block->columns == 6);
            CHECK(block->rows == 2);
        }
}

TEST_CASE("TextSizing.multicellBlockAt_reports_no_block_for_an_ordinary_cell", "[textsizing]")
{
    // A one-cell character is not a block; reporting one would make every ordinary cell pay the
    // block-expansion walk in the selection path.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(2), ColumnCount(10) } };
    auto const& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("ab"sv);

    CHECK_FALSE(screen.multicellBlockAt(CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) })
                    .has_value());
    CHECK_FALSE(screen.multicellBlockAt(CellLocation { .line = LineOffset(1), .column = ColumnOffset(4) })
                    .has_value());
}

TEST_CASE("TextSizing.multicellBlockAt_finds_an_ordinary_wide_character", "[textsizing]")
{
    // A wide character is the degenerate block: two columns, one row. Selection must treat it as
    // indivisible for exactly the same reason a scaled block is.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(1), ColumnCount(10) } };
    auto const& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\xE4\xB8\xAD"sv); // U+4E2D, two columns

    auto const block =
        screen.multicellBlockAt(CellLocation { .line = LineOffset(0), .column = ColumnOffset(1) });
    REQUIRE(block.has_value());
    CHECK(block->origin == CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) });
    CHECK(block->columns == 2);
    CHECK(block->rows == 1);
}

TEST_CASE("TextSizing.selecting_one_cell_of_a_block_selects_all_of_it", "[textsizing]")
{
    // Half a glyph is not a thing that can be highlighted. Selecting the block's LAST column must
    // light up its first column and its second row too, as kitty's apply_selection does.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(10) } };

    mock.writeToScreen("\033]66;s=2:w=3;X\a"sv); // 6 columns wide, 2 rows tall

    auto const anchor = CellLocation { .line = LineOffset(0), .column = ColumnOffset(5) };
    mock.terminal.setSelector(
        std::make_unique<vtbackend::LinearSelection>(mock.terminal.selectionHelper(), anchor, []() {}));
    (void) mock.terminal.selector()->extend(anchor);
    mock.terminal.selector()->complete();

    for (auto const row: { 0, 1 })
        for (auto const column: { 0, 1, 2, 3, 4, 5 })
        {
            INFO("row " << row << " column " << column);
            CHECK(mock.terminal.isSelected(
                CellLocation { .line = LineOffset(row), .column = ColumnOffset(column) }));
        }

    // ...and no further: the cell just past the block stays unselected.
    CHECK_FALSE(mock.terminal.isSelected(CellLocation { .line = LineOffset(0), .column = ColumnOffset(6) }));
}

TEST_CASE("TextSizing.selection_does_not_leak_across_neighbouring_blocks", "[textsizing]")
{
    // Expansion covers the block the selected cell belongs to -- not whatever happens to sit next
    // to it. Two adjacent blocks must stay independently selectable.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(2), ColumnCount(12) } };

    mock.writeToScreen("\033]66;w=2;A\a"sv); // columns 0..1
    mock.writeToScreen("\033]66;w=2;B\a"sv); // columns 2..3

    auto const anchor = CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) };
    mock.terminal.setSelector(
        std::make_unique<vtbackend::LinearSelection>(mock.terminal.selectionHelper(), anchor, []() {}));
    (void) mock.terminal.selector()->extend(anchor);
    mock.terminal.selector()->complete();

    CHECK(mock.terminal.isSelected(CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) }));
    CHECK(mock.terminal.isSelected(CellLocation { .line = LineOffset(0), .column = ColumnOffset(1) }));
    CHECK_FALSE(mock.terminal.isSelected(CellLocation { .line = LineOffset(0), .column = ColumnOffset(2) }));
    CHECK_FALSE(mock.terminal.isSelected(CellLocation { .line = LineOffset(0), .column = ColumnOffset(3) }));
}

TEST_CASE("TextSizing.the_line_level_selection_test_sees_a_block_reaching_into_the_line", "[textsizing]")
{
    // The renderer's trivial-line fast path consults the COARSE, line-level selection test, and draws
    // a line it calls unselected uniformly -- never asking the per-cell test about any of its cells.
    // So a tall block's lower rows must read as selected at line level too, or the highlight stops at
    // the block's first row however well the per-cell test understands blocks.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(6), ColumnCount(10) } };

    mock.writeToScreen("\033]66;s=3:w=2;X\a"sv); // 6 columns wide, 3 rows tall

    auto const anchor = CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) };
    mock.terminal.setSelector(
        std::make_unique<vtbackend::LinearSelection>(mock.terminal.selectionHelper(), anchor, []() {}));
    (void) mock.terminal.selector()->extend(anchor);
    mock.terminal.selector()->complete();

    CHECK(mock.terminal.isSelected(LineOffset(0)));
    CHECK(mock.terminal.isSelected(LineOffset(1)));
    CHECK(mock.terminal.isSelected(LineOffset(2)));

    // Both granularities must agree, or the two paths render the same cell differently.
    for (auto const row: { 0, 1, 2 })
    {
        INFO("row " << row);
        CHECK(mock.terminal.isSelected(CellLocation { .line = LineOffset(row), .column = ColumnOffset(3) }));
    }
}

TEST_CASE("TextSizing.a_block_written_over_a_taller_one_destroys_all_of_it", "[textsizing]")
{
    // Writing a sized block into cells a block above reaches down into destroys that block whole.
    // Overwriting only the overlapping cells would leave its head behind, still claiming a width and
    // a scale for a body that is gone.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(6), ColumnCount(20) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033]66;s=3:w=2;X\a"sv); // 6 columns wide, 3 rows tall, at row 0
    REQUIRE(screen.at(LineOffset(0), ColumnOffset(0)).width() == 6);

    // A second block placed inside the first one's footprint, a row below its head.
    mock.writeToScreen("\033[2;3H"sv);
    mock.writeToScreen("\033]66;s=1:w=2;Y\a"sv);

    // The first block is gone in its entirety -- head included.
    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).empty());
    CHECK(screen.at(LineOffset(0), ColumnOffset(5)).empty());
    CHECK(screen.at(LineOffset(2), ColumnOffset(0)).empty());

    CHECK(screen.at(LineOffset(1), ColumnOffset(2)).codepoints() == U"Y");
}

TEST_CASE("TextSizing.a_wrapping_run_does_not_corrupt_the_line_above", "[textsizing]")
{
    // Each cluster of a `w=0` run becomes its own block, and a block that does not fit moves whole to
    // the next line -- onto the very row the earlier blocks of the same run reach down into. Those
    // must be destroyed as the wrapped blocks claim their cells, not silently half-overwritten.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(6), ColumnCount(10) } };
    auto const& screen = mock.terminal.primaryScreen();

    // Seven 2-wide blocks on a 10-column page: five fill row 0 exactly, and the last two wrap onto
    // row 1 -- which is precisely the row those five claim as their second cell. (Five alone would
    // fit and never wrap, which is what made an earlier version of this test prove nothing.)
    mock.writeToScreen("\033]66;s=2;abcdefg\a"sv);

    // Every surviving head must describe a block whose cells are ALL still there -- across its rows
    // as well as its columns. The wrapped blocks land on row 1, which row 0's blocks claim
    // vertically, so it is the VERTICAL extent that a missing guard leaves dangling.
    for (auto const row: { 0, 1, 2 })
        for (auto const column: std::views::iota(0, 10))
        {
            auto const cell = screen.at(LineOffset(row), ColumnOffset(column));
            if (cell.empty() || cell.isFlagEnabled(CellFlag::WideCharContinuation)
                || cell.isFlagEnabled(CellFlag::MulticellContinuation))
                continue;

            INFO("head at row " << row << " column " << column << " claims " << int(cell.width())
                                << " columns x " << int(cell.scale()) << " rows");

            for (auto const i: std::views::iota(1, static_cast<int>(cell.width())))
                CHECK(screen.at(LineOffset(row), ColumnOffset(column + i))
                          .isFlagEnabled(CellFlag::WideCharContinuation));

            for (auto const i: std::views::iota(1, static_cast<int>(cell.scale())))
                CHECK(screen.at(LineOffset(row + i), ColumnOffset(column))
                          .isFlagEnabled(CellFlag::MulticellContinuation));
        }
}

TEST_CASE("TextSizing.a_block_over_a_wide_character_clears_both_its_columns", "[textsizing]")
{
    // A wide character is the degenerate block: two columns, one row. Landing on its second column
    // must clear its first as well, or its head survives as half a glyph.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(2), ColumnCount(10) } };
    auto const& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\xE4\xB8\xAD"sv); // U+4E2D at columns 0..1
    REQUIRE(screen.at(LineOffset(0), ColumnOffset(0)).width() == 2);

    mock.writeToScreen("\033[1;2H"sv); // onto its SECOND column
    mock.writeToScreen("\033]66;w=2;Z\a"sv);

    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).empty());
    CHECK(screen.at(LineOffset(0), ColumnOffset(1)).codepoints() == U"Z");
}

TEST_CASE("TextSizing.a_neighbouring_block_survives", "[textsizing]")
{
    // The guard clears the cells the new block claims and no others. A block written flush against
    // an existing one must leave it alone, or every run would erase the one before it.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(20) } };
    auto const& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033]66;s=2:w=2;A\a"sv); // columns 0..3, rows 0..1
    mock.writeToScreen("\033]66;s=2:w=2;B\a"sv); // columns 4..7, rows 0..1 -- flush beside it

    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).codepoints() == U"A");
    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).width() == 4);
    CHECK(screen.at(LineOffset(0), ColumnOffset(4)).codepoints() == U"B");
    CHECK(screen.at(LineOffset(0), ColumnOffset(4)).width() == 4);
}

TEST_CASE("TextSizing.the_cell_carries_the_whole_sizing", "[textsizing]")
{
    // The renderer sizes and places a glyph from the cell's CellScale, so everything the request
    // asked for has to survive the write -- not just the block scale.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(6), ColumnCount(20) } };
    auto const& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033]66;s=3:w=1:n=1:d=3:v=2:h=1;x\a"sv);

    auto const cellScale = screen.at(LineOffset(0), ColumnOffset(0)).textScale();
    CHECK(cellScale.scale == 3);
    CHECK(cellScale.numerator == 1);
    CHECK(cellScale.denominator == 3);
    CHECK(cellScale.verticalAlignment == 2);
    CHECK(cellScale.horizontalAlignment == 1);
    CHECK(cellScale.drawFactor() == 1.0); // 3 * 1/3 -- ordinary size inside a 3-cell block

    // A plain scaled block carries no fraction, so it draws at its full scale.
    mock.writeToScreen("\033[2;1H\033]66;s=2;A\a"sv);
    auto const plain = screen.at(LineOffset(1), ColumnOffset(0)).textScale();
    CHECK(plain.scale == 2);
    CHECK_FALSE(plain.hasFraction());
    CHECK(plain.drawFactor() == 2.0);

    // Ordinary text must come back as ordinary, whatever was written before it.
    mock.writeToScreen("\033[3;1Hz"sv);
    CHECK(screen.at(LineOffset(2), ColumnOffset(0)).textScale().isOrdinary());
}

TEST_CASE("TextSizing.overwriting_a_block_head_releases_its_rows", "[textsizing]")
{
    // The rows a tall block claims are only ever cleaned up when something writes INTO one of them.
    // Writing over the block's HEAD hits neither continuation branch, so the rows below keep their
    // MulticellContinuation flag while the head that explained them is gone. Those cells are then
    // orphans: multicellBlockAt walks up, finds an ordinary cell, and answers nothing -- so nothing
    // ever clears them, the renderer cannot resolve them, and extractSelectionText drops the whole
    // row from a copied selection, joining the line above straight to the line below.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(10) } };
    auto const& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033]66;s=2;A\a"sv);
    REQUIRE(screen.at(LineOffset(1), ColumnOffset(0)).isFlagEnabled(CellFlag::MulticellContinuation));

    // Ordinary text straight over the head.
    mock.writeToScreen("\033[H"sv);
    mock.writeToScreen("x"sv);

    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).codepoints() == U"x");
    CHECK_FALSE(screen.at(LineOffset(1), ColumnOffset(0)).isFlagEnabled(CellFlag::MulticellContinuation));
    CHECK_FALSE(screen.at(LineOffset(1), ColumnOffset(1)).isFlagEnabled(CellFlag::MulticellContinuation));
}

TEST_CASE("TextSizing.a_block_honours_insert_mode", "[textsizing]")
{
    // Every other entry into the write path honours IRM: writeCharToCurrentAndAdvance shifts the line
    // right before writing, and applyClusterWidthChange compensates for a cluster that grows. A sized
    // block is text too, so an editor using insert mode plus OSC 66 must not lose the characters the
    // block would otherwise overwrite.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(2), ColumnCount(10) } };
    auto const& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("abcdef"sv);
    mock.writeToScreen("\033[H"sv);  // home
    mock.writeToScreen("\033[4h"sv); // IRM on
    mock.writeToScreen("\033]66;w=3;X\a"sv);

    // The block took the first three columns and pushed the text right rather than destroying "abc".
    // The two columns between are the block's own continuations, which carry no text of their own.
    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).codepoints() == U"X");
    CHECK(screen.at(LineOffset(0), ColumnOffset(3)).codepoints() == U"a");
    CHECK(screen.at(LineOffset(0), ColumnOffset(8)).codepoints() == U"f");
}

TEST_CASE("TextSizing.a_purely_fractional_request_leaves_the_line_non_trivial", "[textsizing]")
{
    // kitty's documented half-size example, `OSC 66 ; n=1:d=2:w=1`, is scale 1 in a single ordinary
    // column: it writes no continuation columns and no continuation rows, so the ONLY state it
    // changes is the cell's sizing. The trivial-line fast path knows nothing about sizing, so a line
    // left trivial renders through it and the fraction is silently dropped.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };
    auto const& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033]66;n=1:d=2:w=1;Ha\a"sv);

    auto const sizing = screen.at(LineOffset(0), ColumnOffset(0)).textScale();
    REQUIRE(sizing.numerator == 1);
    REQUIRE(sizing.denominator == 2);
    CHECK_FALSE(sizing.isOrdinary());
    CHECK_FALSE(screen.grid().lineAt(LineOffset(0)).isTrivialBuffer());
}

TEST_CASE("TextSizing.a_block_below_the_scroll_region_does_not_scroll_it", "[textsizing]")
{
    // The room-to-grow test measures from the bottom margin, which is BELOW the cursor only while the
    // cursor is inside the region. With the cursor beneath it (legal with DECOM off) the difference
    // goes negative, `height > available` is trivially true, and the shortfall exceeds the block's own
    // height -- so the region scrolls by more rows than the block needs and the block lands inside the
    // region instead of where the cursor was.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(25), ColumnCount(20) } };
    auto const& screen = mock.terminal.primaryScreen();

    // Fill the page so any scroll is observable, then confine the region to the top ten rows.
    for (auto const line: std::views::iota(0, 25))
        mock.writeToScreen(std::format("\033[{};1Hline{}", line + 1, line));

    mock.writeToScreen("\033[1;10r"sv); // DECSTBM: region is rows 1..10
    mock.writeToScreen("\033[20;1H"sv); // cursor to row 20 -- below the region
    REQUIRE(screen.cursor().position.line == LineOffset(19));

    mock.writeToScreen("\033]66;s=3;A\a"sv);

    // Rows 20..22 were free, so nothing had to move: the block is written where the cursor stood and
    // the rows inside the scroll region are untouched.
    CHECK(screen.cursor().position.line == LineOffset(19));
    CHECK(screen.at(LineOffset(19), ColumnOffset(0)).codepoints() == U"A");
    CHECK(screen.grid().lineAt(LineOffset(0)).toUtf8Trimmed() == "line0");
    CHECK(screen.grid().lineAt(LineOffset(9)).toUtf8Trimmed() == "line9");
}

TEST_CASE("TextSizing.a_block_takes_a_deferred_wrap_before_placing_itself", "[textsizing]")
{
    // writeSizedText is a second entry point into writing text, parallel to the ordinary path, so it
    // owes the same prologue: a wrap deferred by the previous character is still outstanding. Its own
    // relocation test compares the BLOCK's extent against the margin, and a one-column block always
    // fits -- so without the prologue the block overwrote the character in the last column, and left
    // wrapPending set so every following block clobbered the same cell.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(5) } };
    auto const& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("abcde"sv); // fills the line; cursor parks at the last column
    REQUIRE(screen.cursor().wrapPending);

    mock.writeToScreen("\033]66;s=1;X\a"sv);

    CHECK(screen.at(LineOffset(0), ColumnOffset(4)).codepoints() == U"e");
    CHECK(screen.at(LineOffset(1), ColumnOffset(0)).codepoints() == U"X");
}

TEST_CASE("TextSizing.copying_a_block_carries_its_scale", "[textsizing]")
{
    // DECCRA copies the continuation flags along with the rest of the cell's attributes. Dropping
    // the scale but keeping those flags orphans the rows beneath: the renderer finds a block of
    // height 1 whose origin is above them and redraws the head's text on each, and
    // eraseMulticellBlockAt -- which also reads the scale -- never reaches them to clean up.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(8), ColumnCount(10) } };
    auto const& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033]66;s=2;A\a"sv);
    REQUIRE(screen.at(LineOffset(0), ColumnOffset(0)).textScale().scale == 2);

    // Copy the 2x2 region at the top-left down to line 5.
    mock.writeToScreen("\033[1;1;2;2;1;5;1;1$v"sv);

    auto const head = screen.at(LineOffset(4), ColumnOffset(0));
    CHECK(head.textScale().scale == 2);
    CHECK(head.codepoints() == U"A");
    // The row beneath stays a continuation of a block that is still two rows tall.
    CHECK(screen.at(LineOffset(5), ColumnOffset(0)).textScale().scale == 2);
}

TEST_CASE("TextSizing.a_drag_inside_one_row_of_blocks_stays_on_one_line", "[textsizing]")
{
    // A scale>1 block is two screen rows tall but reads as one line of text. Dragging along it, the
    // pointer strays into the row below; without the clamp that one-row wobble becomes a two-line
    // selection, which takes the first line to its right margin and swallows the caption after it.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(6), ColumnCount(20) } };
    mock.writeToScreen("\033]66;s=2;ab\a  cap"sv); // blocks at cols 0-3, caption at cols 4+

    using namespace vtbackend;
    auto constexpr UiHandled = false;
    auto constexpr Pixels = vtbackend::PixelCoordinate {};

    mock.terminal.tick(std::chrono::steady_clock::time_point {});
    mock.terminal.sendMouseMoveEvent(
        Modifier::None, CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) }, Pixels, UiHandled);
    (void) mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, Pixels, UiHandled);

    // The pointer slips one row down while still over the same blocks.
    mock.terminal.sendMouseMoveEvent(
        Modifier::None, CellLocation { .line = LineOffset(1), .column = ColumnOffset(3) }, Pixels, UiHandled);

    // Still a single-line selection: the caption on row 0 is NOT swept in.
    CHECK_FALSE(mock.terminal.isSelected(CellLocation { .line = LineOffset(0), .column = ColumnOffset(6) }));
    CHECK(mock.terminal.isSelected(CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) }));
    CHECK(mock.terminal.isSelected(CellLocation { .line = LineOffset(1), .column = ColumnOffset(0) }));
}

TEST_CASE("TextSizing.a_drag_that_leaves_the_blocks_still_selects_two_lines", "[textsizing]")
{
    // The clamp must release the moment the pointer is no longer over a block of the same shape, or
    // a genuine multi-line selection could never be made from inside sized text.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(6), ColumnCount(20) } };
    mock.writeToScreen("\033]66;s=2;ab\a\r\n\r\nplain"sv); // blocks rows 0-1, plain text on row 2

    using namespace vtbackend;
    auto constexpr UiHandled = false;
    auto constexpr Pixels = vtbackend::PixelCoordinate {};

    mock.terminal.tick(std::chrono::steady_clock::time_point {});
    mock.terminal.sendMouseMoveEvent(
        Modifier::None, CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) }, Pixels, UiHandled);
    (void) mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, Pixels, UiHandled);
    mock.terminal.sendMouseMoveEvent(
        Modifier::None, CellLocation { .line = LineOffset(2), .column = ColumnOffset(3) }, Pixels, UiHandled);

    CHECK(mock.terminal.isSelected(CellLocation { .line = LineOffset(2), .column = ColumnOffset(0) }));
}

TEST_CASE("TextSizing.copying_a_block_row_yields_no_blank_trailing_line", "[textsizing]")
{
    // A block's lower rows carry no text of their own. Selecting them must not copy as an empty
    // trailing line -- kitty trims exactly those rows in flag_selection_to_extract_text().
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(6), ColumnCount(20) } };
    mock.writeToScreen("\033]66;s=2;ab\a"sv);

    auto const anchor = CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) };
    mock.terminal.setSelector(
        std::make_unique<vtbackend::LinearSelection>(mock.terminal.selectionHelper(), anchor, []() {}));
    (void) mock.terminal.selector()->extend(
        CellLocation { .line = LineOffset(1), .column = ColumnOffset(3) });
    mock.terminal.selector()->complete();

    CHECK(mock.terminal.extractSelectionText() == "ab");
}

TEST_CASE("TextSizing.a_block_stays_a_block_once_scrolled_into_history", "[textsizing]")
{
    // The renderer resolves a continuation row back to its head so that a block whose head has
    // scrolled above the viewport still draws. That lookup has to keep working once the block is in
    // the scrollback, which is exactly when it matters.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(5), ColumnCount(20) }, LineCount(50) };
    auto const& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033]66;s=4;X\a\r\n\r\n\r\n\r\n"sv);
    for (auto const i: std::views::iota(0, 10))
        mock.writeToScreen("filler" + std::to_string(i) + "\r\n");

    // Find where the block ended up rather than predicting it, then ask from a row it merely reaches
    // down into -- the lookup the renderer actually performs for a continuation row.
    auto headLine = std::optional<LineOffset> {};
    for (auto const line: std::views::iota(-static_cast<int>(screen.historyLineCount().value), 0))
        if (screen.at(CellLocation { .line = LineOffset(line), .column = ColumnOffset(0) }).codepoints()
            == U"X")
            headLine = LineOffset(line);
    REQUIRE(headLine.has_value());

    auto const block = screen.multicellBlockAt(
        CellLocation { .line = *headLine + LineOffset(2), .column = ColumnOffset(0) });

    REQUIRE(block.has_value());
    CHECK(block->origin.line == *headLine);
    CHECK(block->rows == 4);
    CHECK(screen.at(block->origin).codepoints() == U"X");
}

TEST_CASE("TextSizing.every_row_of_a_visible_block_is_emitted_with_its_band", "[textsizing]")
{
    // The plain, unscrolled case the demo shows: a block sitting on screen must reach the renderer
    // as `scale` rows, each naming its own band. The scrolled case below covers the harder variant,
    // but it was passing while the ordinary one was never asserted at all.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(6), ColumnCount(20) } };
    auto constexpr ClockBase = std::chrono::steady_clock::time_point {};
    mock.terminal.tick(ClockBase);

    mock.writeToScreen("\033]66;s=2;X\a"sv);

    mock.terminal.tick(ClockBase + std::chrono::milliseconds(100));
    mock.terminal.ensureFreshRenderBuffer();
    auto const buffer = mock.terminal.renderBuffer();

    auto bands = std::set<int> {};
    for (auto const& cell: buffer.get().cells)
        if (cell.codepoints == U"X")
        {
            INFO("cell at line " << cell.position.line.value << " column " << cell.position.column.value);
            CHECK(cell.sizing.scale.scale == 2);
            bands.insert(static_cast<int>(cell.sizing.band));
        }

    INFO("emitted bands: " << bands.size());
    CHECK(bands == std::set<int> { 0, 1 });
}

TEST_CASE("TextSizing.a_block_written_after_the_page_scrolled_still_emits_every_band", "[textsizing]")
{
    // The demo's real situation, and the one an unscrolled test cannot reach: by the time a block is
    // written, the page has already scrolled, so its lines live at a different place in the ring
    // buffer than the first screenful did. Every row must still resolve back to its head.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(6), ColumnCount(20) }, LineCount(50) };
    auto constexpr ClockBase = std::chrono::steady_clock::time_point {};
    mock.terminal.tick(ClockBase);

    for (auto const i: std::views::iota(0, 12))
        mock.writeToScreen("filler" + std::to_string(i) + "\r\n");
    mock.writeToScreen("\033]66;s=2;X\a"sv);

    mock.terminal.tick(ClockBase + std::chrono::milliseconds(100));
    mock.terminal.ensureFreshRenderBuffer();
    auto const buffer = mock.terminal.renderBuffer();

    auto bands = std::set<int> {};
    for (auto const& cell: buffer.get().cells)
        if (cell.codepoints == U"X")
            bands.insert(static_cast<int>(cell.sizing.band));

    INFO("emitted bands: " << bands.size());
    CHECK(bands == std::set<int> { 0, 1 });
}

TEST_CASE("TextSizing.a_block_whose_head_scrolled_above_the_viewport_still_draws", "[textsizing]")
{
    // Only the head cell of a block used to draw anything, so scrolling the head above the viewport
    // took the whole block with it instead of clipping it. Each row now resolves back to its head and
    // draws that row's band, which is what keeps the visible part on screen.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(5), ColumnCount(20) }, LineCount(50) };
    auto const& screen = mock.terminal.primaryScreen();
    auto constexpr ClockBase = std::chrono::steady_clock::time_point {};
    mock.terminal.tick(ClockBase);

    mock.writeToScreen("\033]66;s=4;X\a\r\n\r\n\r\n\r\n"sv);
    for (auto const i: std::views::iota(0, 10))
        mock.writeToScreen("filler" + std::to_string(i) + "\r\n");

    auto headLine = std::optional<LineOffset> {};
    for (auto const line: std::views::iota(-static_cast<int>(screen.historyLineCount().value), 0))
        if (screen.at(CellLocation { .line = LineOffset(line), .column = ColumnOffset(0) }).codepoints()
            == U"X")
            headLine = LineOffset(line);
    REQUIRE(headLine.has_value());

    // Scroll so the viewport's TOP row is the block's SECOND row: the head is off-screen above.
    mock.terminal.viewport().scrollUp(LineCount(-(headLine->value + 1)));
    REQUIRE(mock.terminal.viewport().scrollOffset().value == -(headLine->value + 1));

    mock.terminal.tick(ClockBase + std::chrono::milliseconds(100));
    mock.terminal.ensureFreshRenderBuffer();
    auto const buffer = mock.terminal.renderBuffer();

    // The head's glyph must be emitted for the visible rows, carrying the band each one is.
    auto bands = std::vector<int> {};
    for (auto const& cell: buffer.get().cells)
        if (cell.codepoints == U"X")
            bands.push_back(static_cast<int>(cell.sizing.band));

    INFO("emitted bands: " << bands.size());
    REQUIRE_FALSE(bands.empty());
    // Rows 1..3 of the block are on screen; row 0 is not.
    CHECK(std::ranges::find(bands, 0) == bands.end());
    CHECK(std::ranges::find(bands, 1) != bands.end());
}

TEST_CASE("ZZZ.probe_cursor_below_bottom_margin", "[textsizing]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(25), ColumnCount(20) }, LineCount(0) };
    auto& screen = mock.terminal.primaryScreen();

    // Fill every row with a recognizable marker.
    for (int i = 1; i <= 25; ++i)
        mock.writeToScreen(std::format("\033[{};1H"
                                       "row{}",
                                       i,
                                       i));

    // Scroll region rows 1..10, cursor to row 20 (below the region; DECOM off).
    mock.writeToScreen("\033[1;10r"sv);
    mock.writeToScreen("\033[20;1H"sv);
    INFO("cursor before: line=" << screen.cursor().position.line.value);
    CHECK(screen.cursor().position.line.value == 19);

    mock.writeToScreen("\033]66;s=3;A\a"sv);

    INFO("cursor after: line=" << screen.cursor().position.line.value);
    for (int i = 0; i < 25; ++i)
        UNSCOPED_INFO("line " << i << " = '" << screen.grid().lineText(LineOffset(i)) << "'");
    CHECK(screen.cursor().position.line.value == 19);
    CHECK(screen.at(LineOffset(19), ColumnOffset(0)).codepoints() == U"A");
}
