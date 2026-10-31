// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/grid/Grid.hpp>

#include <crispy/BufferObject.hpp>

#include <core/Utils.hpp>

#include <libunicode/convert.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using namespace vtbackend;
using namespace std::string_literals;
using namespace std::string_view_literals;
using std::string;
using std::string_view;

namespace
{
void logGridText(Grid const& grid, string const& headline = "")
{
    UNSCOPED_INFO(std::format("Grid.dump(hist {}, max hist {}, size {}, ZI {}): {}",
                              grid.historyLineCount(),
                              grid.maxHistoryLineCount(),
                              grid.pageSize(),
                              grid.zeroIndex(),
                              headline));

    for (int line = -grid.historyLineCount().as<int>(); line < grid.pageSize().lines.as<int>(); ++line)
    {
        UNSCOPED_INFO(
            std::format("{:>2}: \"{}\" {}\n",
                        line,
                        grid.lineText(LineOffset::cast_from(line - grid.historyLineCount().as<int>())),
                        grid.lineAt(LineOffset::cast_from(line)).flags()));
    }
}

[[maybe_unused]] void logGridTextAlways(Grid const& grid, string const& headline = "")
{
    std::cout << std::format("Grid.dump(hist {}, max hist {}, size {}, ZI {}): {}\n",
                             grid.historyLineCount(),
                             grid.maxHistoryLineCount(),
                             grid.pageSize(),
                             grid.zeroIndex(),
                             headline);
    std::cout << std::format("{}\n", dumpGrid(grid));
}

Grid setupGrid(PageSize pageSize,
               bool reflowOnResize,
               LineCount maxHistoryLineCount,
               std::initializer_list<std::string_view> init)
{
    auto grid = Grid(pageSize, reflowOnResize, maxHistoryLineCount);

    int cursor = 0;
    for (string_view const line: init)
    {
        if (cursor == *pageSize.lines)
            grid.scrollUp(LineCount(1));
        else
            ++cursor;

        grid.setLineText(LineOffset::cast_from(cursor - 1), line);

        logGridText(grid,
                    std::format("setup grid at {}x{}x{}: line {}",
                                pageSize.columns,
                                pageSize.lines,
                                maxHistoryLineCount,
                                cursor - 1));
    }

    logGridText(grid,
                std::format("setup grid at {}x{}x{}",
                            grid.pageSize().columns,
                            grid.pageSize().lines,
                            grid.maxHistoryLineCount()));
    return grid;
}

constexpr Margin fullPageMargin(PageSize pageSize)
{
    return Margin { .vertical =
                        Margin::Vertical { .from = LineOffset(0), .to = pageSize.lines.as<LineOffset>() - 1 },
                    .horizontal = Margin::Horizontal { .from = ColumnOffset(0),
                                                       .to = pageSize.columns.as<ColumnOffset>() - 1 } };
}

[[maybe_unused]] Grid setupGrid5x2()
{
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, true, LineCount(10));
    grid.setLineText(LineOffset { 0 }, "ABCDE");
    grid.setLineText(LineOffset { 1 }, "abcde");
    logGridText(grid, "setup grid at 5x2");
    return grid;
}

[[maybe_unused]] Grid setupGrid5x2x2()
{
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, true, LineCount(2));
    grid.scrollUp(LineCount(2));
    grid.setLineText(LineOffset { -1 }, "ABCDE");
    grid.setLineText(LineOffset { 0 }, "FGHIJ");
    grid.setLineText(LineOffset { 1 }, "KLMNO");
    grid.setLineText(LineOffset { 2 }, "PQRST");
    logGridText(grid, "setup grid at 5x2x2");
    return grid;
}

[[maybe_unused]] Grid setupGrid8x2()
{
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(8) }, true, LineCount(10));
    grid.setLineText(LineOffset { 0 }, "ABCDEFGH");
    grid.setLineText(LineOffset { 1 }, "abcdefgh");
    logGridText(grid, "setup grid at 5x2");
    return grid;
}

Grid setupGridForResizeTests2x3xN(LineCount maxHistoryLineCount)
{
    auto constexpr ReflowOnResize = true;
    auto constexpr PageSize = vtbackend::PageSize { LineCount(2), ColumnCount(3) };

    return setupGrid(PageSize, ReflowOnResize, maxHistoryLineCount, { "ABC", "DEF", "GHI", "JKL" });
}

Grid setupGridForResizeTests2x3a3()
{
    return setupGridForResizeTests2x3xN(LineCount(3));
}

} // namespace

// NOLINTBEGIN(misc-const-correctness)
TEST_CASE("Grid.setup", "[grid]")
{
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, true, LineCount(0));
    grid.setLineText(LineOffset { 0 }, "ABCDE"sv);
    grid.setLineText(LineOffset { 1 }, "abcde"sv);
    logGridText(grid, "setup grid at 5x2");

    CHECK(grid.lineText(LineOffset { 0 }) == "ABCDE"sv);
    CHECK(grid.lineText(LineOffset { 1 }) == "abcde"sv);
}

TEST_CASE("Grid.writeAndScrollUp", "[grid]")
{
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, true, LineCount(3));
    grid.setLineText(LineOffset { 0 }, "ABCDE");
    grid.setLineText(LineOffset { 1 }, "abcde");
    CHECK(grid.historyLineCount() == LineCount(0));
    CHECK(grid.lineText(LineOffset(0)) == "ABCDE");
    CHECK(grid.lineText(LineOffset(1)) == "abcde");

    grid.scrollUp(LineCount(1));
    grid.setLineText(LineOffset(1), "12345");

    CHECK(grid.historyLineCount() == LineCount(1));
    CHECK(grid.lineText(LineOffset(-1)) == "ABCDE");
    CHECK(grid.lineText(LineOffset(0)) == "abcde");
    CHECK(grid.lineText(LineOffset(1)) == "12345");

    grid.scrollUp(LineCount(1));
    CHECK(grid.historyLineCount() == LineCount(2));
    CHECK(grid.lineText(LineOffset(-2)) == "ABCDE");
    CHECK(grid.lineText(LineOffset(-1)) == "abcde");
    CHECK(grid.lineText(LineOffset(0)) == "12345");
    CHECK(grid.lineText(LineOffset(1)) == "     ");
}

TEST_CASE("iteratorAt", "[grid]")
{
    auto grid = Grid(PageSize { LineCount(3), ColumnCount(3) }, true, LineCount(0));
    grid.setLineText(LineOffset { 0 }, "ABC"sv);
    grid.setLineText(LineOffset { 1 }, "DEF"sv);
    grid.setLineText(LineOffset { 2 }, "GHI"sv);
    logGridText(grid);

    auto a00 = grid.at(LineOffset(0), ColumnOffset(0));
    CHECK(a00.toUtf8() == "A");
    auto a01 = grid.at(LineOffset(0), ColumnOffset(1));
    CHECK(a01.toUtf8() == "B");
    auto a02 = grid.at(LineOffset(0), ColumnOffset(2));
    CHECK(a02.toUtf8() == "C");

    auto a11 = grid.at(LineOffset(1), ColumnOffset(1));
    CHECK(a11.toUtf8() == "E");
    auto a22 = grid.at(LineOffset(2), ColumnOffset(2));
    CHECK(a22.toUtf8() == "I");
}

TEST_CASE("LogicalLines.iterator", "[grid]")
{
    auto constexpr ReflowOnResize = true;
    auto constexpr MaxHistoryLineCount = LineCount(5);
    auto constexpr PageSize = vtbackend::PageSize { LineCount(2), ColumnCount(3) };

    auto grid = setupGrid(PageSize,
                          ReflowOnResize,
                          MaxHistoryLineCount,
                          {
                              "ABC", // -4:
                              "DEF", // -3:
                              "GHI", // -2: wrapped
                              "JKL", // -1: wrapped
                              "MNO", //  0:
                              "PQR", //  1: wrapped
                          });

    grid.lineAt(LineOffset(-2)).setWrapped(true);
    grid.lineAt(LineOffset(-1)).setWrapped(true);
    grid.lineAt(LineOffset(1)).setWrapped(true);
    logGridText(grid, "After having set wrapped-flag.");

    auto const logicalLines = grid.logicalLines();
    auto lineIt = logicalLines.begin();

    // ABC
    auto line = *lineIt;
    auto const tABC = line.text();
    REQUIRE(tABC == "ABC");
    CHECK(line.top == LineOffset(-4));
    CHECK(line.bottom == LineOffset(-4));

    // DEF GHI JKL
    line = *++lineIt;
    auto const tDEFGHIJKL = line.text();
    REQUIRE(tDEFGHIJKL == "DEFGHIJKL");
    CHECK(line.top == LineOffset(-3));
    CHECK(line.bottom == LineOffset(-1));

    // MNO PQR
    line = *++lineIt;
    auto const tMNOPQR = line.text();
    REQUIRE(tMNOPQR == "MNOPQR");
    CHECK(line.top == LineOffset(0));
    CHECK(line.bottom == LineOffset(1));

    // <<END>>
    line = *++lineIt;
    auto const endIt = logicalLines.end();
    REQUIRE(lineIt == endIt);

    // XXX backwards

    // MNO PQR
    line = *--lineIt;
    auto const rMNOPQR = line.text();
    REQUIRE(rMNOPQR == "MNOPQR");

    // DEF GHI JKL
    line = *--lineIt;
    auto const rDEFGHIJKL = line.text();
    REQUIRE(rDEFGHIJKL == "DEFGHIJKL");

    // ABC
    line = *--lineIt;
    auto const rABC = line.text();
    REQUIRE(rABC == "ABC");
}

TEST_CASE("LogicalLines.reverse_iterator", "[grid]")
{
    auto constexpr ReflowOnResize = true;
    auto constexpr MaxHistoryLineCount = LineCount(5);
    auto constexpr PageSize = vtbackend::PageSize { LineCount(2), ColumnCount(3) };

    auto grid = setupGrid(PageSize,
                          ReflowOnResize,
                          MaxHistoryLineCount,
                          {
                              "ABC", // -4:
                              "DEF", // -3:
                              "GHI", // -2: wrapped
                              "JKL", // -1: wrapped
                              "MNO", //  0:
                              "PQR", //  1: wrapped
                          });

    grid.lineAt(LineOffset(-2)).setWrapped(true);
    grid.lineAt(LineOffset(-1)).setWrapped(true);
    grid.lineAt(LineOffset(1)).setWrapped(true);
    logGridText(grid, "After having set wrapped-flag.");

    auto logicalLines = grid.logicalLinesReverse();
    auto lineIt = logicalLines.begin();

    // MNO PQR
    auto line = *lineIt;
    auto const tMNOPQR = line.text();
    REQUIRE(tMNOPQR == "MNOPQR");

    // DEF GHI JKL
    line = *++lineIt;
    auto const tDEFGHIJKL = line.text();
    REQUIRE(tDEFGHIJKL == "DEFGHIJKL");

    // ABC
    line = *++lineIt;
    auto const tABC = line.text();
    REQUIRE(tABC == "ABC");

    // <<END>>
    auto const endIt = logicalLines.end();
    line = *++lineIt;
    REQUIRE(lineIt == endIt);
}

// {{{ Resize
// TODO: test cases for resize: line grow
//
// 1. with scrollback moving into page area: partly
// 2. with scrollback moving into page area: exactly
// 3. with scrollback moving into page area: fully plus new empty lines at the bottom
//
// - XXX Make sure reflow cases are integrated
// - XXX Make sure cursor moves are tested
// - XXX Make sure grow line count is algorithmically the same with and without column changes
//
// - add test for handling scrollUp without overflow
// - add test for handling scrollUp with overflow
// - add test for handling scrollUp linesUsed = totalLineCount

TEST_CASE("resize_lines_nr2_with_scrollback_moving_fully_into_page", "[grid]")
{
    // If cursor is at the bottom and we grow in lines,
    // then we try to pull down from scrollback lines, if available. otherwise
    // we grow the remaining lines to be grown at the bottom of the main page.

    auto grid = setupGridForResizeTests2x3a3();
    CHECK(grid.maxHistoryLineCount() == LineCount(3));
    CHECK(grid.historyLineCount() == LineCount(2));

    auto const curCursorPos =
        CellLocation { .line = grid.pageSize().lines.as<LineOffset>() - 1, .column = ColumnOffset(1) };
    auto const newPageSize = PageSize { LineCount(4), ColumnCount(3) };
    auto const newCursorPos0 = CellLocation { .line = curCursorPos.line + 2, .column = curCursorPos.column };
    CellLocation newCursorPos = grid.resize(newPageSize, curCursorPos, WrapPending::No);
    CHECK(newCursorPos.line == newCursorPos0.line);
    CHECK(newCursorPos.column == newCursorPos0.column);
    CHECK(grid.pageSize() == newPageSize);
    CHECK(grid.historyLineCount() == LineCount(0));
    CHECK(grid.lineText(LineOffset(0)) == "ABC");
    CHECK(grid.lineText(LineOffset(1)) == "DEF");
    CHECK(grid.lineText(LineOffset(2)) == "GHI");
    CHECK(grid.lineText(LineOffset(3)) == "JKL");
}

TEST_CASE("resize_lines_nr3_with_scrollback_moving_into_page_overflow", "[grid]")
{
    // If cursor is at the bottom and we grow in lines,
    // then we try to pull down from scrollback lines, if available. otherwise
    // we grow the remaining lines to be grown at the bottom of the main page.

    auto grid = setupGridForResizeTests2x3a3();
    REQUIRE(grid.maxHistoryLineCount() == LineCount(3));
    REQUIRE(grid.historyLineCount() == LineCount(2));
    REQUIRE(grid.pageSize().columns == ColumnCount(3));
    REQUIRE(grid.pageSize().lines == LineCount(2));

    auto const curCursorPos = CellLocation { .line = LineOffset(1), .column = ColumnOffset(1) };
    auto const newPageSize = PageSize { LineCount(5), ColumnCount(3) };
    logGridText(grid, "BEFORE");
    CellLocation newCursorPos = grid.resize(newPageSize, curCursorPos, WrapPending::No);
    logGridText(grid, "AFTER");
    CHECK(newCursorPos.line == LineOffset(3));
    CHECK(newCursorPos.column == curCursorPos.column);
    CHECK(grid.pageSize() == newPageSize);
    CHECK(grid.historyLineCount() == LineCount(0));
    CHECK(grid.lineText(LineOffset(0)) == "ABC");
    CHECK(grid.lineText(LineOffset(1)) == "DEF");
    CHECK(grid.lineText(LineOffset(2)) == "GHI");
    CHECK(grid.lineText(LineOffset(3)) == "JKL");
    CHECK(grid.lineText(LineOffset(4)) == "   ");
}

TEST_CASE("resize_grow_lines_with_history_cursor_no_bottom", "[grid]")
{
    auto grid = setupGridForResizeTests2x3a3();
    CHECK(grid.maxHistoryLineCount() == LineCount(3));
    CHECK(grid.historyLineCount() == LineCount(2));

    auto const curCursorPos = CellLocation { .line = LineOffset(0), .column = ColumnOffset(1) };
    logGridText(grid, "before resize");
    CellLocation newCursorPos =
        grid.resize(PageSize { LineCount(3), ColumnCount(3) }, curCursorPos, WrapPending::No);
    logGridText(grid, "after resize");
    CHECK(newCursorPos.line == curCursorPos.line);
    CHECK(newCursorPos.column == curCursorPos.column);
    CHECK(grid.pageSize().columns == ColumnCount(3));
    CHECK(grid.pageSize().lines == LineCount(3));
    CHECK(grid.historyLineCount() == LineCount(2));
    CHECK(grid.lineText(LineOffset(-2)) == "ABC");
    CHECK(grid.lineText(LineOffset(-1)) == "DEF");
    CHECK(grid.lineText(LineOffset(0)) == "GHI");
    CHECK(grid.lineText(LineOffset(1)) == "JKL");
    CHECK(grid.lineText(LineOffset(2)) == "   ");
}

TEST_CASE("Grid.resize.shrinkingColumnsKeepsTheCursorOnThePage", "[grid]")
{
    auto const narrowed = PageSize { LineCount(3), ColumnCount(8) };

    SECTION("a cursor that still fits stays where it is")
    {
        // Not have its column added to itself. With reflow enabled, "ABCD" leaves nothing to reflow
        // or cut, so both narrow in place.
        auto const reflow = GENERATE(true, false);
        CAPTURE(reflow);

        auto grid = Grid(PageSize { LineCount(3), ColumnCount(10) }, reflow, LineCount(10));
        grid.setLineText(LineOffset(1), "ABCD");
        auto const cursor = CellLocation { .line = LineOffset(1), .column = ColumnOffset(4) };
        CHECK(grid.resize(narrowed, cursor, WrapPending::No) == cursor);
    }

    SECTION("without reflow, a cursor past the new width moves onto its last column")
    {
        auto grid = Grid(PageSize { LineCount(3), ColumnCount(10) }, false, LineCount(10));
        grid.setLineText(LineOffset(1), "ABCDEFGHIJ");
        auto const cursor = CellLocation { .line = LineOffset(1), .column = ColumnOffset(9) };
        CHECK(grid.resize(narrowed, cursor, WrapPending::No)
              == CellLocation { .line = LineOffset(1), .column = ColumnOffset(7) });
    }
}

namespace
{
/// The character in the cell at @p location of @p grid's page.
[[nodiscard]] char charAt(Grid const& grid, CellLocation location)
{
    return grid.lineText(location.line).at(unbox<size_t>(location.column));
}
} // namespace

TEST_CASE("Grid.resize.reflowKeepsTheCursorOnItsText", "[grid]")
{
    // A column change that reflows moves text between rows. The cursor goes with the cell it is on --
    // its logical line and its offset into that line -- as kitty, foot and WezTerm do, and not with the
    // row it was on, where other text now stands.
    auto grid = Grid(PageSize { LineCount(3), ColumnCount(10) }, true, LineCount(10));

    SECTION("inside a wrapped line, narrowing and widening again")
    {
        grid.setLineText(LineOffset(0), "ABCDEFGHIJ");
        grid.setLineText(LineOffset(1), "KLMNO");
        grid.lineAt(LineOffset(1)).setWrapped(true);
        auto const cursor = CellLocation { .line = LineOffset(1), .column = ColumnOffset(2) };
        REQUIRE(charAt(grid, cursor) == 'M');

        // "ABCD" "EFGH" "IJKL" "MNO": the first is pushed into the history, the blank row below given up.
        auto const narrowed = grid.resize(PageSize { LineCount(3), ColumnCount(4) }, cursor, WrapPending::No);
        CHECK(narrowed == CellLocation { .line = LineOffset(2), .column = ColumnOffset(0) });
        CHECK(charAt(grid, narrowed) == 'M');

        auto const widened =
            grid.resize(PageSize { LineCount(3), ColumnCount(10) }, narrowed, WrapPending::No);
        CHECK(widened == cursor);
        CHECK(charAt(grid, widened) == 'M');
    }

    SECTION("past the end of its text, narrowing and widening again")
    {
        // The cursor keeps its offset into the logical line even where that line has no text, and the
        // line gains the row it needs to hold the cursor -- as foot does -- so the next character typed
        // continues the line instead of landing on its last column or on another line.
        grid.setLineText(LineOffset(1), "ABCD");
        auto const cursor = CellLocation { .line = LineOffset(1), .column = ColumnOffset(9) };

        // The added row takes the place of the blank row below.
        auto const narrowed = grid.resize(PageSize { LineCount(3), ColumnCount(8) }, cursor, WrapPending::No);
        CHECK(narrowed == CellLocation { .line = LineOffset(2), .column = ColumnOffset(1) });
        CHECK(grid.lineText(LineOffset(1)) == "ABCD    ");
        CHECK(grid.lineAt(LineOffset(2)).isFlagEnabled(LineFlag::Wrapped));

        auto const widened =
            grid.resize(PageSize { LineCount(3), ColumnCount(10) }, narrowed, WrapPending::No);
        CHECK(widened == cursor);
        CHECK(grid.lineText(LineOffset(1)) == "ABCD      ");
    }

    SECTION("a pending wrap stands where the next character goes")
    {
        // The cursor is on the last cell, but the next character goes one past it: that is the
        // offset to keep, or the character would overwrite the "J".
        grid.setLineText(LineOffset(1), "ABCDEFGHIJ");
        auto const cursor = CellLocation { .line = LineOffset(1), .column = ColumnOffset(9) };

        // The blank row, "ABCD" "EFGH" "IJ": the first is pushed into the history.
        auto const narrowed =
            grid.resize(PageSize { LineCount(3), ColumnCount(4) }, cursor, WrapPending::Yes);
        CHECK(narrowed == CellLocation { .line = LineOffset(2), .column = ColumnOffset(2) });
        CHECK(grid.lineText(LineOffset(2)) == "IJ  ");
    }

    SECTION("a row that is cut rather than reflowed keeps the cursor on its last column")
    {
        // The narrowing reflows "abcdef" into the row below it, which may not wrap and so is cut.
        grid.setLineText(LineOffset(0), "abcdef");
        grid.setLineText(LineOffset(1), "ABCDEFGHIJ");
        grid.lineAt(LineOffset(1)).setWrappable(false);
        auto const cursor = CellLocation { .line = LineOffset(1), .column = ColumnOffset(9) };

        // "abcd" "ef" "ABCD", in place of the blank row.
        auto const narrowed = grid.resize(PageSize { LineCount(3), ColumnCount(4) }, cursor, WrapPending::No);
        CHECK(grid.lineText(LineOffset(2)) == "ABCD");
        CHECK(narrowed == CellLocation { .line = LineOffset(2), .column = ColumnOffset(3) });
    }

    SECTION("on the last row, whose text wraps further")
    {
        grid.setLineText(LineOffset(0), "abc");
        grid.setLineText(LineOffset(1), "xyz");
        grid.setLineText(LineOffset(2), "KLMNOPQRST");
        auto const cursor = CellLocation { .line = LineOffset(2), .column = ColumnOffset(7) };
        REQUIRE(charAt(grid, cursor) == 'R');

        // "abc" "xyz" "KLMN" "OPQR" "ST": the cursor's text is no longer on the last row.
        auto const narrowed = grid.resize(PageSize { LineCount(3), ColumnCount(4) }, cursor, WrapPending::No);
        CHECK(narrowed == CellLocation { .line = LineOffset(1), .column = ColumnOffset(3) });
        CHECK(charAt(grid, narrowed) == 'R');

        auto const widened =
            grid.resize(PageSize { LineCount(3), ColumnCount(10) }, narrowed, WrapPending::No);
        CHECK(widened == cursor);
        CHECK(charAt(grid, widened) == 'R');
    }
}

TEST_CASE("Grid.resize.leavesTheRowsFromUnreflowedFromAsTheyAre", "[grid]")
{
    // From the logical line it names down to the bottom of the page, a column change cuts each row at the new
    // width or pads it instead of reflowing it, and the line stays one; the rows above reflow as ever. A
    // cursor there keeps its row, and an offset the line carries keeps naming its cell.
    auto grid = Grid(PageSize { LineCount(4), ColumnCount(10) }, true, LineCount(10));
    grid.setLineText(LineOffset(0), "abcdefghij");
    grid.setLineText(LineOffset(1), "kl");
    grid.lineAt(LineOffset(1)).setWrapped(true);
    grid.setLineText(LineOffset(2), "ABCDEFGHIJ");
    grid.setLineText(LineOffset(3), "KLMNOP");
    grid.lineAt(LineOffset(3)).setWrapped(true);
    grid.lineAt(LineOffset(2)).setFlag(LineFlag::PromptEnd, true);
    grid.lineAt(LineOffset(2)).setPromptEndOffset(ColumnOffset(13)); // the "N"
    auto cursors =
        std::array { CellLocation { .line = LineOffset(3), .column = ColumnOffset(4) } }; // the "O"

    SECTION("narrowing")
    {
        // A continuation names the line it continues.
        grid.resize(PageSize { LineCount(4), ColumnCount(4) }, cursors, LineOffset(3));

        // "abcd" "efgh" "ijkl", the first of them pushed into the history; then the rows as they were, cut.
        CHECK(grid.lineText(LineOffset(-1)) == "abcd");
        CHECK(grid.lineText(LineOffset(0)) == "efgh");
        CHECK(grid.lineText(LineOffset(1)) == "ijkl");
        CHECK(grid.lineText(LineOffset(2)) == "ABCD");
        CHECK(grid.lineText(LineOffset(3)) == "KLMN");
        CHECK(grid.lineAt(LineOffset(3)).isFlagEnabled(LineFlag::Wrapped));
        CHECK(cursors[0] == CellLocation { .line = LineOffset(3), .column = ColumnOffset(3) });
        CHECK(grid.lineAt(LineOffset(2)).promptEndOffset() == ColumnOffset(7));
    }

    SECTION("widening")
    {
        grid.resize(PageSize { LineCount(4), ColumnCount(15) }, cursors, LineOffset(2));

        // "abcdefghijkl" on one row; then the rows as they were, padded; then a blank row for the page.
        CHECK(grid.lineText(LineOffset(0)) == "abcdefghijkl   ");
        CHECK(grid.lineText(LineOffset(1)) == "ABCDEFGHIJ     ");
        CHECK(grid.lineText(LineOffset(2)) == "KLMNOP         ");
        CHECK(grid.lineAt(LineOffset(2)).isFlagEnabled(LineFlag::Wrapped));
        CHECK(cursors[0] == CellLocation { .line = LineOffset(2), .column = ColumnOffset(4) });
        CHECK(grid.lineAt(LineOffset(1)).promptEndOffset() == ColumnOffset(18));
    }
}

TEST_CASE("Grid.resize.aColumnMarkedPastTheTextGivesItsLineNoRow", "[grid]")
{
    // A cursor past the end of its line's text makes the reflowed line reach it, so that it keeps its row. A
    // column the line marks there is counted on from the end of the text instead, and the line keeps the rows
    // its text needs.
    auto grid = Grid(PageSize { LineCount(3), ColumnCount(10) }, true, LineCount(10));
    grid.setLineText(LineOffset(0), "abcdefghij");
    grid.setLineText(LineOffset(1), "AB");
    grid.lineAt(LineOffset(1)).setWrapped(true);
    grid.setLineText(LineOffset(2), "next");
    auto& head = grid.lineAt(LineOffset(0));
    head.setFlag(LineFlag::PromptEnd, true);
    head.setPromptEndOffset(ColumnOffset(18)); // six columns past the "B"

    (void) grid.resize(PageSize { LineCount(3), ColumnCount(15) },
                       CellLocation { .line = LineOffset(2), .column = ColumnOffset(4) },
                       WrapPending::No);

    CHECK(grid.lineText(LineOffset(0)) == "abcdefghijAB   ");
    CHECK(grid.lineText(LineOffset(1)) == "next           ");
    CHECK(grid.lineAt(LineOffset(0)).promptEndOffset() == ColumnOffset(18)); // still six past the "B"
}

TEST_CASE("Grid.resize.rowsThatDoNotReflowHoldTheirOffsetsToTheirCells", "[grid]")
{
    // A row cut at the new width or padded to it, rather than re-chopped, keeps its cells on their row: a
    // column its line's head marks on it stays on the same row, held to the row's last column where the cut
    // took its cell.
    SECTION("a grid that does not reflow")
    {
        auto grid = Grid(PageSize { LineCount(3), ColumnCount(10) }, false, LineCount(10));
        grid.setLineText(LineOffset(0), "ABCDEFGHIJ");
        grid.setLineText(LineOffset(1), "KLMNOP");
        grid.lineAt(LineOffset(1)).setWrapped(true);
        auto& head = grid.lineAt(LineOffset(0));
        head.setFlag(LineFlags { LineFlag::PromptEnd, LineFlag::CommandEnd }, true);
        head.setPromptEndOffset(ColumnOffset(13));  // the "N", row 1 of the line
        head.setCommandEndOffset(ColumnOffset(18)); // past the text of row 1

        (void) grid.resize(PageSize { LineCount(3), ColumnCount(15) }, CellLocation {}, WrapPending::No);
        CHECK(grid.lineAt(LineOffset(0)).promptEndOffset() == ColumnOffset(15 + 3));
        CHECK(grid.lineAt(LineOffset(0)).commandEndOffset() == ColumnOffset(15 + 8));

        (void) grid.resize(PageSize { LineCount(3), ColumnCount(5) }, CellLocation {}, WrapPending::No);
        CHECK(grid.lineAt(LineOffset(0)).promptEndOffset() == ColumnOffset(5 + 3));
        CHECK(grid.lineAt(LineOffset(0)).commandEndOffset() == ColumnOffset(5 + 4));
    }

    SECTION("a row that may not wrap, in a narrowing that cuts nothing but blanks")
    {
        auto grid = Grid(PageSize { LineCount(3), ColumnCount(10) }, true, LineCount(10));
        grid.setLineText(LineOffset(0), "AB");
        auto& head = grid.lineAt(LineOffset(0));
        head.setWrappable(false);
        head.setFlag(LineFlag::PromptEnd, true);
        head.setPromptEndOffset(ColumnOffset(8));

        (void) grid.resize(PageSize { LineCount(3), ColumnCount(5) }, CellLocation {}, WrapPending::No);
        CHECK(grid.lineAt(LineOffset(0)).promptEndOffset() == ColumnOffset(4));
    }

    SECTION("a row kept as it is that does not wrap either, held once")
    {
        // The column is row 1, column 3 of the line at 15 columns, past the one row the line has, as a reflow
        // leaves a column past the text.
        auto grid = Grid(PageSize { LineCount(3), ColumnCount(15) }, true, LineCount(10));
        grid.setLineText(LineOffset(0), "abcdefgh");
        auto& head = grid.lineAt(LineOffset(0));
        head.setWrappable(false);
        head.setFlag(LineFlag::PromptEnd, true);
        head.setPromptEndOffset(ColumnOffset(15 + 3));
        auto cursors = std::array { CellLocation { .line = LineOffset(0), .column = ColumnOffset(8) } };

        grid.resize(PageSize { LineCount(3), ColumnCount(10) }, cursors, LineOffset(0));
        CHECK(grid.lineAt(LineOffset(0)).promptEndOffset() == ColumnOffset(10 + 3));
    }
}

TEST_CASE("Grid.unreflowedColumn.keepsTheRowAndHoldsTheColumnToIt", "[grid]")
{
    constexpr auto Narrowing = WidthChange { .from = ColumnCount(10), .to = ColumnCount(4) };
    CHECK(unreflowedColumn(ColumnOffset(2), Narrowing) == ColumnOffset(2));
    CHECK(unreflowedColumn(ColumnOffset(7), Narrowing) == ColumnOffset(3));
    CHECK(unreflowedColumn(ColumnOffset(13), Narrowing) == ColumnOffset(7));
    CHECK(unreflowedColumn(ColumnOffset(20), Narrowing) == ColumnOffset(8));

    constexpr auto Widening = WidthChange { .from = ColumnCount(10), .to = ColumnCount(15) };
    CHECK(unreflowedColumn(ColumnOffset(7), Widening) == ColumnOffset(7));
    CHECK(unreflowedColumn(ColumnOffset(13), Widening) == ColumnOffset(18));
}

TEST_CASE("resize_shrink_lines_with_history", "[grid]")
{
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(3) }, true, LineCount(5));
    auto const gridMargin = fullPageMargin(grid.pageSize());
    grid.scrollUp(LineCount { 1 }, GraphicsAttributes {}, gridMargin);
    grid.setLineText(LineOffset(-1), "ABC");        // history line
    grid.setLineText(LineOffset(0), "DEF");         // main page: line 1
    grid.setLineText(LineOffset(1), "GHI");         // main page: line 2
    CHECK(grid.historyLineCount() == LineCount(1)); // TODO: move line up, below scrollUp()

    // shrink by one line (=> move page one line up into scrollback)
    auto const newPageSize = PageSize { LineCount(1), ColumnCount(3) };
    auto const curCursorPos = CellLocation { .line = LineOffset(1), .column = ColumnOffset(1) };
    logGridText(grid, "BEFORE");
    CellLocation const newCursorPos = grid.resize(newPageSize, curCursorPos, WrapPending::No);
    logGridText(grid, "AFTER");
    CHECK(grid.pageSize().columns == ColumnCount(3));
    CHECK(grid.pageSize().lines == LineCount(1));
    CHECK(grid.historyLineCount() == LineCount(2)); // XXX FIXME: test failing
    CHECK(grid.lineText(LineOffset(-2)) == "ABC");
    CHECK(grid.lineText(LineOffset(-1)) == "DEF");
    CHECK(grid.lineText(LineOffset(0)) == "GHI");
    CHECK(*newCursorPos.line == 0); // clamped
    CHECK(*newCursorPos.column == 1);
}

TEST_CASE("resize_shrink_columns_with_reflow_and_unwrappable", "[grid]")
{
    // ABC  // Wrappable
    // DEF  // Wrappable
    // GHI  //
    // JKL  // Wrappable
    //
    // AB   // Wrappable
    // C    // Wrappable,Wrapped
    // DE   // Wrappable
    // F    // Wrappable,Wrapped
    // GH   // cut off
    // JK   // Wrappable
    // L    // Wrappable,Wrapped

    auto grid = setupGridForResizeTests2x3xN(LineCount(5));
    auto const newPageSize = PageSize { LineCount(2), ColumnCount(2) };
    auto const curCursorPos = CellLocation { .line = LineOffset(1), .column = ColumnOffset(1) };
    grid.lineAt(LineOffset(0)).setWrappable(false);
    logGridText(grid, "BEFORE");
    auto const newCursorPos = grid.resize(newPageSize, curCursorPos, WrapPending::No);
    (void) newCursorPos;
    logGridText(grid, "AFTER");

    CHECK(grid.historyLineCount() == LineCount(5));
    CHECK(grid.pageSize().columns == ColumnCount(2));
    CHECK(grid.pageSize().lines == LineCount(2));

    CHECK(grid.lineText(LineOffset(-5)) == "AB");
    CHECK(grid.lineText(LineOffset(-4)) == "C ");
    CHECK(grid.lineText(LineOffset(-3)) == "DE");
    CHECK(grid.lineText(LineOffset(-2)) == "F ");
    CHECK(grid.lineText(LineOffset(-1)) == "GH");
    CHECK(grid.lineText(LineOffset(0)) == "JK");
    CHECK(grid.lineText(LineOffset(1)) == "L ");

    CHECK(grid.lineAt(LineOffset(-5)).flags() == LineFlag::Wrappable);
    CHECK(grid.lineAt(LineOffset(-4)).flags() == LineFlags({ LineFlag::Wrappable, LineFlag::Wrapped }));
    CHECK(grid.lineAt(LineOffset(-3)).flags() == LineFlag::Wrappable);
    CHECK(grid.lineAt(LineOffset(-2)).flags() == LineFlags({ LineFlag::Wrappable, LineFlag::Wrapped }));
    CHECK(grid.lineAt(LineOffset(-1)).flags() == LineFlag::None);
    CHECK(grid.lineAt(LineOffset(0)).flags() == LineFlag::Wrappable);
    CHECK(grid.lineAt(LineOffset(1)).flags() == LineFlags({ LineFlag::Wrappable, LineFlag::Wrapped }));
}

TEST_CASE("resize_shrink_columns_with_reflow_grow_lines_and_unwrappable", "[grid]")
{
    // ABC
    // DEF
    // GHI
    // JKL
    //
    // AB
    // C
    // DE
    // F
    // GH   // cut off
    // JK
    // L
    auto grid = setupGridForResizeTests2x3xN(LineCount(5));
    // On the "L", which the narrowing leaves on the bottom row: the added lines then come from the history.
    auto const curCursorPos = CellLocation { .line = LineOffset(1), .column = ColumnOffset(2) };
    grid.lineAt(LineOffset(0)).setWrappable(false);
    // logGridText(grid, "BEFORE");
    auto const newCursorPos =
        grid.resize(PageSize { LineCount(4), ColumnCount(2) }, curCursorPos, WrapPending::No);
    CHECK(newCursorPos == CellLocation { .line = LineOffset(3), .column = ColumnOffset(0) });
    // logGridText(grid, "AFTER");

    CHECK(grid.lineText(LineOffset(-3)) == "AB");
    CHECK(grid.lineText(LineOffset(-2)) == "C ");
    CHECK(grid.lineText(LineOffset(-1)) == "DE");
    CHECK(grid.lineText(LineOffset(0)) == "F ");
    CHECK(grid.lineText(LineOffset(1)) == "GH");
    CHECK(grid.lineText(LineOffset(2)) == "JK");
    CHECK(grid.lineText(LineOffset(3)) == "L ");

    CHECK(grid.lineAt(LineOffset(-3)).flags() == LineFlag::Wrappable);
    CHECK(grid.lineAt(LineOffset(-2)).flags() == LineFlags({ LineFlag::Wrappable, LineFlag::Wrapped }));
    CHECK(grid.lineAt(LineOffset(-1)).flags() == LineFlag::Wrappable);
    CHECK(grid.lineAt(LineOffset(0)).flags() == LineFlags({ LineFlag::Wrappable, LineFlag::Wrapped }));
    CHECK(grid.lineAt(LineOffset(1)).flags() == LineFlag::None);
    CHECK(grid.lineAt(LineOffset(2)).flags() == LineFlag::Wrappable);
    CHECK(grid.lineAt(LineOffset(3)).flags() == LineFlags({ LineFlag::Wrappable, LineFlag::Wrapped }));
}
// }}}

// {{{ grid reflow
TEST_CASE("resize_shrink_columns_cuts_an_unwrappable_line_with_nothing_to_absorb", "[grid]")
{
    // A line marked as not reflowing (DECRST 2028 while it was written) is cut, not reflowed, whether or
    // not the line above it overflows into it -- here nothing does.
    auto grid = Grid(PageSize { LineCount(3), ColumnCount(10) }, true, LineCount(10));
    grid.setLineText(LineOffset(0), "abc");
    grid.setLineText(LineOffset(1), "ABCDEFGHIJ");
    grid.lineAt(LineOffset(1)).setWrappable(false);

    (void) grid.resize(PageSize { LineCount(3), ColumnCount(4) }, CellLocation {}, WrapPending::No);

    CHECK(grid.historyLineCount() == LineCount(0));
    CHECK(grid.lineText(LineOffset(0)) == "abc ");
    CHECK(grid.lineText(LineOffset(1)) == "ABCD");
    CHECK(grid.lineText(LineOffset(2)) == "    ");
    CHECK_FALSE(grid.lineAt(LineOffset(1)).isFlagEnabled(LineFlag::Wrappable));
}

TEST_CASE("resize_shrink_columns_leaves_the_lines_it_adds_wrappable", "[grid]")
{
    // A narrowing pads the ring with blank lines that a later growth brings onto the page. Text written
    // there must reflow like any other, so they are wrappable as every other line of a reflowing grid.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(10) }, true, LineCount(10));
    grid.setLineText(LineOffset(0), "ABCDEFGHIJ");

    (void) grid.resize(PageSize { LineCount(2), ColumnCount(8) }, CellLocation {}, WrapPending::No);
    (void) grid.resize(PageSize { LineCount(4), ColumnCount(8) }, CellLocation {}, WrapPending::No);

    for (auto const line: std::views::iota(0, 4))
    {
        CAPTURE(line);
        CHECK(grid.lineAt(LineOffset(line)).isFlagEnabled(LineFlag::Wrappable));
    }
}

TEST_CASE("resize_reflow_shrink", "[grid]")
{
    auto grid = setupGrid5x2();
    logGridText(grid, "init");

    // Shrink slowly from 5x2 to 4x2 to 3x2 to 2x2.

    // 4x2
    (void) grid.resize(PageSize { LineCount(2), ColumnCount(4) },
                       CellLocation { .line = {}, .column = {} },
                       WrapPending::No);
    logGridText(grid, "after resize 4x2");

    CHECK(*grid.historyLineCount() == 2);
    CHECK(grid.lineText(LineOffset(-2)) == "ABCD");
    CHECK(grid.lineText(LineOffset(-1)) == "E   ");

    CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(4) });
    CHECK(grid.lineText(LineOffset(0)) == "abcd");
    CHECK(grid.lineText(LineOffset(1)) == "e   ");

    // std::cout << std::format("Starting logicalLines test\n");
    auto ll = grid.logicalLines();
    auto li = ll.begin();
    auto le = ll.end();
    CHECK(li->text() == "ABCDE   ");
    ++li;
    CHECK(li->text() == "abcde   ");
    ++li;
    CHECK(li == le);

    // 3x2
    std::cout << std::format("Starting resize to 3x2\n");
    (void) grid.resize(PageSize { LineCount(2), ColumnCount(3) },
                       CellLocation { .line = {}, .column = {} },
                       WrapPending::No);
    logGridText(grid, "after resize 3x2");

    CHECK(*grid.historyLineCount() == 2);
    CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(3) });
    CHECK(grid.lineText(LineOffset(-2)) == "ABC");
    CHECK(grid.lineText(LineOffset(-1)) == "DE ");
    CHECK(grid.lineText(LineOffset(0)) == "abc");
    CHECK(grid.lineText(LineOffset(1)) == "de ");

    // 2x2
    (void) grid.resize(PageSize { LineCount(2), ColumnCount(2) },
                       CellLocation { .line = {}, .column = {} },
                       WrapPending::No);
    logGridText(grid, "after resize 2x2");

    CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(2) });
    CHECK(grid.historyLineCount() == LineCount(4));
    CHECK(grid.lineText(LineOffset(-4)) == "AB");
    CHECK(grid.lineText(LineOffset(-3)) == "CD");
    CHECK(grid.lineText(LineOffset(-2)) == "E ");
    CHECK(grid.lineText(LineOffset(-1)) == "ab");
    CHECK(grid.lineText(LineOffset(0)) == "cd");
    CHECK(grid.lineText(LineOffset(1)) == "e ");
}

TEST_CASE("Grid.reflow", "[grid]")
{
    auto grid = setupGrid5x2();

    SECTION("resize 4x2")
    {
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(4) },
                           CellLocation { .line = {}, .column = {} },
                           WrapPending::No);
        logGridText(grid, "after resize");

        CHECK(grid.historyLineCount() == LineCount(2));
        CHECK(grid.lineText(LineOffset(-2)) == "ABCD");
        CHECK(grid.lineText(LineOffset(-1)) == "E   ");

        CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(4) });
        CHECK(grid.lineText(LineOffset(0)) == "abcd");
        CHECK(grid.lineText(LineOffset(1)) == "e   ");
    }

    SECTION("resize 3x2")
    {
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(4) }, CellLocation {}, WrapPending::No);
        logGridText(grid, "after resize 4x2");
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(3) }, CellLocation {}, WrapPending::No);
        logGridText(grid, "after resize 3x2");

        CHECK(grid.historyLineCount() == LineCount(2));
        CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(3) });
        CHECK(grid.lineText(LineOffset(-2)) == "ABC");
        CHECK(grid.lineText(LineOffset(-1)) == "DE ");
        CHECK(grid.lineText(LineOffset(0)) == "abc");
        CHECK(grid.lineText(LineOffset(1)) == "de ");
    }

    SECTION("resize 2x2")
    {
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(4) }, CellLocation {}, WrapPending::No);
        logGridText(grid, "after resize 4x2");
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(3) }, CellLocation {}, WrapPending::No);
        logGridText(grid, "after resize 3x2");
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(2) }, CellLocation {}, WrapPending::No);
        logGridText(grid, "after resize 2x2");

        CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(2) });
        CHECK(grid.historyLineCount() == LineCount(4));
        CHECK(grid.lineText(LineOffset(-4)) == "AB");
        CHECK(grid.lineText(LineOffset(-3)) == "CD");
        CHECK(grid.lineText(LineOffset(-2)) == "E ");
        CHECK(grid.lineText(LineOffset(-1)) == "ab");
        CHECK(grid.lineText(LineOffset(0)) == "cd");
        CHECK(grid.lineText(LineOffset(1)) == "e ");

        SECTION("regrow 3x2")
        {
            logGridText(grid, "Before regrow to 3x2");
            (void) grid.resize(PageSize { LineCount(2), ColumnCount(3) }, CellLocation {}, WrapPending::No);
            logGridText(grid, "after regrow to 3x2");

            CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(3) });
            CHECK(grid.historyLineCount() == LineCount(2));
            CHECK(grid.lineText(LineOffset(-2)) == "ABC");
            CHECK(grid.lineText(LineOffset(-1)) == "DE ");
            CHECK(grid.lineText(LineOffset(0)) == "abc");
            CHECK(grid.lineText(LineOffset(1)) == "de ");

            SECTION("regrow 4x2")
            {
                (void) grid.resize(
                    PageSize { LineCount(2), ColumnCount(4) }, CellLocation {}, WrapPending::No);
                logGridText(grid, "after regrow 4x2");

                CHECK(grid.historyLineCount() == LineCount(2));
                CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(4) });
                CHECK(grid.lineText(LineOffset(-2)) == "ABCD");
                CHECK(grid.lineText(LineOffset(-1)) == "E   ");
                CHECK(grid.lineText(LineOffset(0)) == "abcd");
                CHECK(grid.lineText(LineOffset(1)) == "e   ");
            }

            SECTION("regrow 5x2")
            {
                (void) grid.resize(
                    PageSize { LineCount(2), ColumnCount(5) }, CellLocation {}, WrapPending::No);
                logGridText(grid, "after regrow 5x2");

                CHECK(grid.historyLineCount() == LineCount(0));
                CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(5) });
                CHECK(grid.lineText(LineOffset(0)) == "ABCDE");
                CHECK(grid.lineText(LineOffset(1)) == "abcde");
            }
        }
    }
}

TEST_CASE("Grid.reflow.shrink_many", "[grid]")
{
    auto grid = setupGrid5x2();
    REQUIRE(grid.pageSize() == PageSize { LineCount(2), ColumnCount(5) });
    REQUIRE(grid.lineText(LineOffset(0)) == "ABCDE"sv);
    REQUIRE(grid.lineText(LineOffset(1)) == "abcde"sv);

    (void) grid.resize(PageSize { LineCount(2), ColumnCount(2) }, CellLocation {}, WrapPending::No);
    logGridText(grid, "after resize 2x2");

    CHECK(grid.historyLineCount() == LineCount(4));
    CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(2) });
    CHECK(grid.lineText(LineOffset(-4)) == "AB");
    CHECK(grid.lineText(LineOffset(-3)) == "CD");
    CHECK(grid.lineText(LineOffset(-2)) == "E ");
    CHECK(grid.lineText(LineOffset(-1)) == "ab");
    CHECK(grid.lineText(LineOffset(0)) == "cd");
    CHECK(grid.lineText(LineOffset(1)) == "e ");
}

TEST_CASE("Grid.reflow.shrink_many_grow_many", "[grid]")
{
    auto grid = setupGrid5x2();

    (void) grid.resize(PageSize { LineCount(2), ColumnCount(2) }, CellLocation {}, WrapPending::No);
    logGridText(grid, "after resize 2x2");

    SECTION("smooth regrow 2->3->4->5")
    {
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(3) }, CellLocation {}, WrapPending::No);
        logGridText(grid, "after resize 3x2");
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(4) }, CellLocation {}, WrapPending::No);
        logGridText(grid, "after resize 4x2");
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(5) }, CellLocation {}, WrapPending::No);
        logGridText(grid, "after resize 5x2");

        CHECK(grid.historyLineCount() == LineCount(0));
        CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(5) });
        CHECK(grid.lineText(LineOffset(0)) == "ABCDE");
        CHECK(grid.lineText(LineOffset(1)) == "abcde");
    }

    SECTION("hard regrow 2->5")
    {
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(5) }, CellLocation {}, WrapPending::No);
        logGridText(grid, "after resize 5x2");

        CHECK(grid.historyLineCount() == LineCount(0));
        CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(5) });
        CHECK(grid.lineText(LineOffset(0)) == "ABCDE");
        CHECK(grid.lineText(LineOffset(1)) == "abcde");
    }
}

TEST_CASE("Grid.reflow.triple", "[grid]")
{
    // Tests reflowing text upon shrink/grow across more than two (e.g. three) wrapped lines.
    auto grid = setupGrid8x2();

    (void) grid.resize(PageSize { LineCount(2), ColumnCount(2) }, CellLocation {}, WrapPending::No);
    logGridText(grid, "after resize 3x2");

    REQUIRE(grid.historyLineCount() == LineCount(6));
    REQUIRE(grid.pageSize() == PageSize { LineCount(2), ColumnCount(2) });

    REQUIRE(!grid.lineAt(LineOffset(-6)).wrapped());
    REQUIRE(grid.lineAt(LineOffset(-5)).wrapped());
    REQUIRE(grid.lineAt(LineOffset(-4)).wrapped());
    REQUIRE(grid.lineAt(LineOffset(-3)).wrapped());
    REQUIRE(!grid.lineAt(LineOffset(-2)).wrapped());
    REQUIRE(grid.lineAt(LineOffset(-1)).wrapped());
    REQUIRE(grid.lineAt(LineOffset(0)).wrapped());
    REQUIRE(grid.lineAt(LineOffset(1)).wrapped());

    SECTION("grow from 2x2 to 8x2")
    {
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(8) }, CellLocation {}, WrapPending::No);
        logGridText(grid, "after resize 3x2");

        CHECK(grid.historyLineCount() == LineCount(0));
        CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(8) });

        CHECK(!grid.lineAt(LineOffset { 0 }).wrapped());
        CHECK(grid.lineText(LineOffset(0)) == "ABCDEFGH");

        CHECK(!grid.lineAt(LineOffset { 1 }).wrapped());
        CHECK(grid.lineText(LineOffset(1)) == "abcdefgh");
    }

    SECTION("grow from 2x2 to 3x2 to ... to 8x2")
    {
        // {{{ 3x2
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(3) }, CellLocation {}, WrapPending::No);
        logGridText(grid, "after resize 3x2");

        REQUIRE(grid.historyLineCount() == LineCount(4));
        REQUIRE(grid.pageSize() == PageSize { LineCount(2), ColumnCount(3) });

        REQUIRE(grid.lineText(LineOffset(-4)) == "ABC");
        REQUIRE(grid.lineText(LineOffset(-3)) == "DEF");
        REQUIRE(grid.lineText(LineOffset(-2)) == "GH ");
        REQUIRE(grid.lineText(LineOffset(-1)) == "abc");
        REQUIRE(grid.lineText(LineOffset(0)) == "def");
        REQUIRE(grid.lineText(LineOffset(1)) == "gh ");

        REQUIRE(!grid.lineAt(LineOffset(-4)).wrapped());
        REQUIRE(grid.lineAt(LineOffset(-3)).wrapped());
        REQUIRE(grid.lineAt(LineOffset(-2)).wrapped());
        REQUIRE(!grid.lineAt(LineOffset(-1)).wrapped());
        REQUIRE(grid.lineAt(LineOffset(0)).wrapped());
        REQUIRE(grid.lineAt(LineOffset(1)).wrapped());
        // }}}

        // {{{ 4x2
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(4) }, CellLocation {}, WrapPending::No);
        logGridText(grid, "after resize 4x2");

        REQUIRE(grid.historyLineCount() == LineCount(2));
        REQUIRE(grid.pageSize() == PageSize { LineCount(2), ColumnCount(4) });

        REQUIRE(grid.lineText(LineOffset(-2)) == "ABCD");
        REQUIRE(grid.lineText(LineOffset(-1)) == "EFGH");
        REQUIRE(grid.lineText(LineOffset(0)) == "abcd");
        REQUIRE(grid.lineText(LineOffset(1)) == "efgh");

        REQUIRE(!grid.lineAt(LineOffset(-2)).wrapped());
        REQUIRE(grid.lineAt(LineOffset(-1)).wrapped());
        REQUIRE(!grid.lineAt(LineOffset(0)).wrapped());
        REQUIRE(grid.lineAt(LineOffset(1)).wrapped());
        // }}}

        // {{{ 5x2
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(5) }, CellLocation {}, WrapPending::No);
        logGridText(grid, "after resize 5x2");

        REQUIRE(grid.historyLineCount() == LineCount(2));
        REQUIRE(grid.pageSize() == PageSize { LineCount(2), ColumnCount(5) });

        REQUIRE(grid.lineText(LineOffset(-2)) == "ABCDE");
        REQUIRE(grid.lineText(LineOffset(-1)) == "FGH  ");
        REQUIRE(grid.lineText(LineOffset(0)) == "abcde");
        REQUIRE(grid.lineText(LineOffset(1)) == "fgh  ");

        REQUIRE(!grid.lineAt(LineOffset(-2)).wrapped());
        REQUIRE(grid.lineAt(LineOffset(-1)).wrapped());
        REQUIRE(!grid.lineAt(LineOffset(0)).wrapped());
        REQUIRE(grid.lineAt(LineOffset(1)).wrapped());
        // }}}

        // {{{ 7x2
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(7) }, CellLocation {}, WrapPending::No);
        logGridText(grid, "after resize 7x2");

        REQUIRE(grid.historyLineCount() == LineCount(2));
        REQUIRE(grid.pageSize() == PageSize { LineCount(2), ColumnCount(7) });

        REQUIRE(grid.lineText(LineOffset(-2)) == "ABCDEFG");
        REQUIRE(grid.lineText(LineOffset(-1)) == "H      ");
        REQUIRE(grid.lineText(LineOffset(0)) == "abcdefg");
        REQUIRE(grid.lineText(LineOffset(1)) == "h      ");

        REQUIRE(!grid.lineAt(LineOffset(-2)).wrapped());
        REQUIRE(grid.lineAt(LineOffset(-1)).wrapped());
        REQUIRE(!grid.lineAt(LineOffset(0)).wrapped());
        REQUIRE(grid.lineAt(LineOffset(1)).wrapped());
        // }}}

        // {{{ 8x2
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(8) }, CellLocation {}, WrapPending::No);
        logGridText(grid, "after resize 8x2");

        REQUIRE(*grid.historyLineCount() == 0);
        REQUIRE(grid.pageSize() == PageSize { LineCount(2), ColumnCount(8) });

        REQUIRE(grid.lineText(LineOffset(0)) == "ABCDEFGH");
        REQUIRE(grid.lineText(LineOffset(1)) == "abcdefgh");

        REQUIRE(!grid.lineAt(LineOffset(0)).wrapped());
        REQUIRE(!grid.lineAt(LineOffset(1)).wrapped());
        // }}}
    }
}

TEST_CASE("Grid infinite", "[grid]")
{
    auto gridFinite = Grid(PageSize { LineCount(2), ColumnCount(8) }, true, LineCount(0));
    gridFinite.setLineText(LineOffset { 0 }, "ABCDEFGH"sv);
    gridFinite.setLineText(LineOffset { 1 }, "abcdefgh"sv);
    gridFinite.scrollUp(LineCount { 1 });
    REQUIRE(gridFinite.lineText(LineOffset(0)) == "abcdefgh");
    REQUIRE(gridFinite.lineText(LineOffset(-1)) == std::string(8, ' '));

    auto gridInfinite = Grid(PageSize { LineCount(2), ColumnCount(8) }, true, Infinite());
    gridInfinite.setLineText(LineOffset { 0 }, "ABCDEFGH"sv);
    gridInfinite.setLineText(LineOffset { 1 }, "abcdefgh"sv);
    gridInfinite.scrollUp(LineCount { 1 });
    REQUIRE(gridInfinite.lineText(LineOffset(0)) == "abcdefgh");
    REQUIRE(gridInfinite.lineText(LineOffset(-1)) == "ABCDEFGH");
    gridInfinite.scrollUp(LineCount { 97 });
    REQUIRE(gridInfinite.lineText(LineOffset(-97)) == "abcdefgh");
    REQUIRE(gridInfinite.lineText(LineOffset(-98)) == "ABCDEFGH");
}

TEST_CASE("Grid resize with wrap", "[grid]")
{
    auto grid = Grid(PageSize { LineCount(3), ColumnCount(5) }, true, LineCount(0));
    grid.setLineText(LineOffset { 0 }, "1");
    grid.setLineText(LineOffset { 1 }, "2");
    grid.setLineText(LineOffset { 2 }, "ABCDE");
    (void) grid.resize(PageSize { LineCount(3), ColumnCount(3) }, CellLocation {}, WrapPending::No);
    REQUIRE(grid.lineText(LineOffset(0)) == "2  ");
    REQUIRE(grid.lineText(LineOffset(1)) == "ABC");
    REQUIRE(grid.lineText(LineOffset(2)) == "DE ");
    REQUIRE(grid.historyLineCount() == LineCount(0));
    (void) grid.resize(PageSize { LineCount(3), ColumnCount(5) }, CellLocation {}, WrapPending::No);
    REQUIRE(unbox(grid.historyLineCount()) == 0);
    // With zero history, narrowing evicted "1"; widening cannot restore that row.
    REQUIRE(grid.lineText(LineOffset(0)) == "2    ");
    REQUIRE(grid.lineText(LineOffset(1)) == "ABCDE");
    REQUIRE(grid.lineText(LineOffset(2)) == "     ");
}

TEST_CASE("Grid resize", "[grid]")
{
    auto width = ColumnCount(6);
    auto grid = Grid(PageSize { LineCount(2), width }, true, LineCount(0));
    auto text = "abcd"sv;
    auto pool = crispy::BufferObjectPool<char>(32);
    auto bufferObject = pool.allocateBufferObject();
    bufferObject->writeAtEnd(text);
    auto const bufferFragment = bufferObject->ref(0, 4);
    auto const sgr = GraphicsAttributes {};
    auto const trivial = TrivialLineBuffer { .displayWidth = width,
                                             .textAttributes = sgr,
                                             .fillAttributes = sgr,
                                             .hyperlink = HyperlinkId {},
                                             .usedColumns = width,
                                             .text = bufferFragment };
    auto lineTrivial = Line(LineFlag::None, trivial);
    grid.lineAt(LineOffset(0)) = lineTrivial;
    // With SoA storage, all lines use LineSoA (no TrivialBuffer distinction).
    REQUIRE(grid.lineAt(LineOffset(0)).size() > ColumnCount(0));
    REQUIRE(grid.lineAt(LineOffset(1)).size() > ColumnCount(0));
    (void) grid.resize(PageSize { LineCount(2), width + ColumnCount(1) }, CellLocation {}, WrapPending::No);
    // With SoA storage, all lines use LineSoA (no TrivialBuffer distinction).
    REQUIRE(grid.lineAt(LineOffset(0)).size() > ColumnCount(0));
    REQUIRE(grid.lineAt(LineOffset(1)).size() > ColumnCount(0));
    (void) grid.resize(PageSize { LineCount(2), width + ColumnCount(-1) }, CellLocation {}, WrapPending::No);
    // With SoA storage, all lines use LineSoA (no TrivialBuffer distinction).
    REQUIRE(grid.lineAt(LineOffset(0)).size() > ColumnCount(0));
    REQUIRE(grid.lineAt(LineOffset(1)).size() > ColumnCount(0));
}

TEST_CASE("Grid resize with wrap and spaces", "[grid]")
{
    auto width = ColumnCount(7);
    auto grid = Grid(PageSize { LineCount(3), width }, true, LineCount(0));

    auto text = "a a a a"sv;
    auto pool = crispy::BufferObjectPool<char>(static_cast<size_t>(unbox(width) * 8));
    auto bufferObject = pool.allocateBufferObject();
    bufferObject->writeAtEnd(text);
    auto const bufferFragment = bufferObject->ref(0, unbox(width));
    auto const sgr = GraphicsAttributes {};
    auto const trivial = TrivialLineBuffer { .displayWidth = width,
                                             .textAttributes = sgr,
                                             .fillAttributes = sgr,
                                             .hyperlink = HyperlinkId {},
                                             .usedColumns = width,
                                             .text = bufferFragment };
    // Wrappable, as a line written with reflow on is: one that is not would be cut instead.
    auto lineTrivial = Line(LineFlag::Wrappable, trivial);
    grid.lineAt(LineOffset(0)) = lineTrivial;

    // The blank rows below the line make room for the rows a narrowing adds, so the line stays on the page.
    (void) grid.resize(PageSize { LineCount(3), ColumnCount(6) }, CellLocation {}, WrapPending::No);
    REQUIRE(grid.lineText(LineOffset(0)) == "a a a ");
    REQUIRE(grid.lineText(LineOffset(1)) == "a     ");
    REQUIRE(grid.lineText(LineOffset(2)) == "      ");
    (void) grid.resize(PageSize { LineCount(3), ColumnCount(7) }, CellLocation {}, WrapPending::No);
    REQUIRE(grid.lineText(LineOffset(0)) == "a a a a");
    (void) grid.resize(PageSize { LineCount(3), ColumnCount(5) }, CellLocation {}, WrapPending::No);
    REQUIRE(grid.lineText(LineOffset(0)) == "a a a");
    REQUIRE(grid.lineText(LineOffset(1)) == " a   ");
    REQUIRE(grid.lineText(LineOffset(2)) == "     ");
    (void) grid.resize(PageSize { LineCount(3), ColumnCount(4) }, CellLocation {}, WrapPending::No);
    REQUIRE(grid.lineText(LineOffset(0)) == "a a ");
    REQUIRE(grid.lineText(LineOffset(1)) == "a a ");
    REQUIRE(grid.lineText(LineOffset(2)) == "    ");
    (void) grid.resize(PageSize { LineCount(3), ColumnCount(3) }, CellLocation {}, WrapPending::No);
    REQUIRE(grid.lineText(LineOffset(0)) == "a a");
    REQUIRE(grid.lineText(LineOffset(1)) == " a ");
    REQUIRE(grid.lineText(LineOffset(2)) == "a  ");
    (void) grid.resize(PageSize { LineCount(3), ColumnCount(7) }, CellLocation {}, WrapPending::No);
    REQUIRE(grid.lineText(LineOffset(0)) == "a a a a");
}

// }}}

// {{{ Grid::render extraLines tests

namespace
{

/// Minimal mock renderer that tracks which lines were rendered and via which path.
struct MockGridRenderer
{
    std::vector<LineOffset> renderedLines;
    /// The text of each rendered line, so a test can check WHICH grid row landed on a given y -- the
    /// only thing that distinguishes an explicit row list from the linear walk.
    std::vector<std::string> renderedTexts;
    size_t trivialCount = 0;
    size_t perCellCount = 0;

    void startLine(LineOffset y, [[maybe_unused]] LineFlags flags, [[maybe_unused]] ContextId contextId)
    {
        renderedLines.push_back(y);
        ++perCellCount;
    }

    void renderCell([[maybe_unused]] ConstCellProxy cell,
                    [[maybe_unused]] LineOffset line,
                    [[maybe_unused]] ColumnOffset column)
    {
    }

    void endLine() {}

    void renderTrivialLine([[maybe_unused]] TrivialLineBuffer const& lineBuffer,
                           LineOffset y,
                           [[maybe_unused]] LineFlags flags,
                           [[maybe_unused]] ContextId contextId,
                           std::u32string_view textOverride = {})
    {
        renderedLines.push_back(y);
        renderedTexts.push_back(unicode::convert_to<char>(textOverride));
        ++trivialCount;
    }

    void finish() {}
};

} // namespace

TEST_CASE("Grid.render_extraLines.renders_extra_line_above_viewport", "[grid]")
{
    // Create a grid with 2 visible lines and 5 history lines.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, true, LineCount(5));

    // Write enough lines to fill some history.
    for (auto i = 0; i < 5; ++i)
    {
        grid.scrollUp(LineCount(1));
        grid.setLineText(LineOffset(1), std::format("L{:03}", i));
    }

    auto renderer = MockGridRenderer {};
    (void) grid.render(renderer, ScrollOffset(0), HighlightSearchMatches::No, LineCount(1));

    // Should have rendered 3 lines: y=-1 (extra), y=0, y=1.
    CHECK(renderer.renderedLines.size() == 3);
    CHECK(renderer.renderedLines[0] == LineOffset(-1));
    CHECK(renderer.renderedLines[1] == LineOffset(0));
    CHECK(renderer.renderedLines[2] == LineOffset(1));
}

TEST_CASE("Grid.render_extraLines.clamps_to_available_history", "[grid]")
{
    // Create a grid with 2 visible lines and only 1 history line.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, true, LineCount(5));
    grid.scrollUp(LineCount(1));
    grid.setLineText(LineOffset(1), "hist1");

    auto renderer = MockGridRenderer {};
    // Request 5 extra lines, but only 1 history line is available.
    (void) grid.render(renderer, ScrollOffset(0), HighlightSearchMatches::No, LineCount(5));

    // Should have rendered 3 lines: y=-1 (the one available extra), y=0, y=1.
    CHECK(renderer.renderedLines.size() == 3);
    CHECK(renderer.renderedLines[0] == LineOffset(-1));
    CHECK(renderer.renderedLines[1] == LineOffset(0));
    CHECK(renderer.renderedLines[2] == LineOffset(1));
}

TEST_CASE("Grid.render_extraLines.zero_extra_lines_unchanged", "[grid]")
{
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, true, LineCount(5));
    for (auto i = 0; i < 5; ++i)
    {
        grid.scrollUp(LineCount(1));
        grid.setLineText(LineOffset(1), std::format("L{:03}", i));
    }

    auto renderer = MockGridRenderer {};
    (void) grid.render(renderer, ScrollOffset(0), HighlightSearchMatches::No, LineCount(0));

    // Should have rendered exactly 2 lines: y=0, y=1 (no extras).
    CHECK(renderer.renderedLines.size() == 2);
    CHECK(renderer.renderedLines[0] == LineOffset(0));
    CHECK(renderer.renderedLines[1] == LineOffset(1));
}

// }}}
// {{{ Lazy-blank line behavior in Grid

TEST_CASE("Grid.spawnWithLargeHistory.leavesHistoryUnmaterialized", "[grid][blank]")
{
    // Constructing a Grid with a very large history must not eagerly allocate per-column
    // SoA storage for every line. Assert the deterministic invariant — every slot stays
    // blank (un-materialized) — rather than wall-clock timing, which is CI-flaky.
    auto grid = Grid(PageSize { LineCount(24), ColumnCount(80) }, true, LineCount(500'000));

    auto blankCount = size_t { 0 };
    for (auto i = -static_cast<int>(grid.maxHistoryLineCount().as<size_t>());
         i < grid.pageSize().lines.as<int>();
         ++i)
    {
        if (grid.lineAt(LineOffset::cast_from(i)).isBlank())
            ++blankCount;
    }
    // Every slot should be blank initially.
    CHECK(blankCount == grid.maxHistoryLineCount().as<size_t>() + grid.pageSize().lines.as<size_t>());
}

TEST_CASE("Grid.resizeColumnsWithLargeHistory.keepsBlank", "[grid][blank]")
{
    // After a column resize, a previously-all-blank history must still be all-blank
    // (lazy path must not materialize any line during the resize).
    auto grid = Grid(PageSize { LineCount(24), ColumnCount(80) }, true, LineCount(500'000));

    (void) grid.resize(PageSize { LineCount(24), ColumnCount(100) }, CellLocation {}, WrapPending::No);
    CHECK(grid.pageSize().columns == ColumnCount(100));

    auto blankCount = size_t { 0 };
    for (auto i = -static_cast<int>(grid.maxHistoryLineCount().as<size_t>());
         i < grid.pageSize().lines.as<int>();
         ++i)
    {
        if (grid.lineAt(LineOffset::cast_from(i)).isBlank())
            ++blankCount;
    }
    CHECK(blankCount == grid.maxHistoryLineCount().as<size_t>() + grid.pageSize().lines.as<size_t>());
}

TEST_CASE("Grid.growLinesAfterGrowColumnsWidensTheNewPageLines", "[grid]")
{
    // Growing the columns without anything to reflow widens only the used lines; the unused tail of
    // the ring keeps its old width. Growing the page afterwards pulls exactly those tail lines into
    // it, and each must then be as wide as the page -- the terminal writes to any page column
    // without asking the line how wide it is. Contour's startup negotiates exactly this sequence.
    auto const checkPageLinesAreFullWidth = [](Grid const& grid) {
        for (auto const y: std::views::iota(0, grid.pageSize().lines.as<int>()))
        {
            INFO(std::format("page line {}", y));
            CHECK(grid.lineAt(LineOffset(y)).size() == grid.pageSize().columns);
        }
    };

    SECTION("tail lines never used")
    {
        auto grid = Grid(PageSize { LineCount(3), ColumnCount(10) }, true, LineCount(20));
        (void) grid.resize(PageSize { LineCount(3), ColumnCount(15) }, CellLocation {}, WrapPending::No);
        (void) grid.resize(PageSize { LineCount(6), ColumnCount(15) }, CellLocation {}, WrapPending::No);
        checkPageLinesAreFullWidth(grid);
    }

    SECTION("page lines cut off by a shrink and grown back")
    {
        auto grid = Grid(PageSize { LineCount(6), ColumnCount(10) }, true, LineCount(20));
        (void) grid.resize(PageSize { LineCount(3), ColumnCount(10) }, CellLocation {}, WrapPending::No);
        (void) grid.resize(PageSize { LineCount(3), ColumnCount(15) }, CellLocation {}, WrapPending::No);
        (void) grid.resize(PageSize { LineCount(6), ColumnCount(15) }, CellLocation {}, WrapPending::No);
        checkPageLinesAreFullWidth(grid);
    }
}

TEST_CASE("Grid.shrinkColumnsWrapsLongLine", "[grid][blank]")
{
    // A single 200-column line shrunk to 40 must produce 5 wrapped chunks with
    // LineFlag::Wrapped on continuations and original content preserved end-to-end.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(200) }, true, LineCount(50));
    auto const longText = std::string(200, 'A');
    grid.setLineText(LineOffset(0), longText);
    REQUIRE(grid.lineTextTrimmed(LineOffset(0)) == longText);

    (void) grid.resize(PageSize { LineCount(2), ColumnCount(40) }, CellLocation {}, WrapPending::No);
    CHECK(grid.pageSize().columns == ColumnCount(40));

    // After shrink to 40 cols, 200 chars of 'A' span 5 lines of 40 cells each.
    // Reconstruct by walking history + page.
    std::string reconstructed;
    for (int i = -grid.historyLineCount().as<int>(); i < grid.pageSize().lines.as<int>(); ++i)
    {
        reconstructed += grid.lineTextTrimmed(LineOffset::cast_from(i));
    }
    CHECK(reconstructed == longText);
}

TEST_CASE("Grid.shrinkColumnsWrapsTextWithBlankHistory", "[grid][blank]")
{
    // Mix text and blank history lines, then shrink columns. Text must wrap correctly,
    // and surrounding blank lines must remain blank with the new column count.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(80) }, true, LineCount(20));

    // Scroll up enough to push 5 history lines, then write text only on some of them.
    for (int i = 0; i < 5; ++i)
        grid.scrollUp(LineCount(1));

    // Write a wide-enough line at history offset -3 that will wrap when shrunk to 40 cols.
    auto const wideText = std::string(60, 'B'); // 60 chars > 40 cols → must wrap once
    grid.setLineText(LineOffset(-3), wideText);
    REQUIRE(grid.lineTextTrimmed(LineOffset(-3)) == wideText);

    // Lines -5, -4, -2, -1 remain blank (never written).
    REQUIRE(grid.lineAt(LineOffset(-5)).isBlank());
    REQUIRE(grid.lineAt(LineOffset(-4)).isBlank());
    REQUIRE(grid.lineAt(LineOffset(-2)).isBlank());
    REQUIRE(grid.lineAt(LineOffset(-1)).isBlank());

    (void) grid.resize(PageSize { LineCount(2), ColumnCount(40) }, CellLocation {}, WrapPending::No);
    CHECK(grid.pageSize().columns == ColumnCount(40));

    // Reconstruct the wide line: it should still appear contiguously in history
    // (now spread across multiple wrapped lines).
    std::string reconstructed;
    for (int i = -grid.historyLineCount().as<int>(); i < grid.pageSize().lines.as<int>(); ++i)
        reconstructed += grid.lineTextTrimmed(LineOffset::cast_from(i));

    INFO("Reconstructed history+page: " << reconstructed);
    // The wide text must survive the reflow somewhere in history.
    CHECK(reconstructed.contains(wideText));
}

TEST_CASE("Grid.render.blankLineWithSearchHighlight.usesTrivialPath", "[grid][blank]")
{
    // Regression: blank (un-materialized) lines must be rendered via the trivial path
    // even when HighlightSearchMatches::Yes is set. The per-cell branch would otherwise
    // construct ConstCellProxy on an empty SoA and hit the assert in debug (UB in release).
    auto grid = Grid(PageSize { LineCount(3), ColumnCount(10) }, true, LineCount(5));

    auto renderer = MockGridRenderer {};
    (void) grid.render(renderer, ScrollOffset(0), HighlightSearchMatches::Yes, LineCount(0));

    CHECK(renderer.renderedLines.size() == 3);
    CHECK(renderer.trivialCount == 3);
    CHECK(renderer.perCellCount == 0);
}

TEST_CASE("Grid.scrollUp.partialHorizontal.blankLinesDifferingFillAttrsMaterialize", "[grid][blank]")
{
    // When two blank lines have differing fillAttrs, a partial-horizontal scrollUp must
    // still propagate the source's attrs into the destination's copied range (i.e. the
    // destination must be materialized). The old skip-on-both-blank would drop this.
    auto grid = Grid(PageSize { LineCount(4), ColumnCount(10) }, true, LineCount(0));

    // Seed two rows with different fill attrs, each still blank (no writes).
    auto redBg = GraphicsAttributes {};
    redBg.backgroundColor = RGBColor { 255, 0, 0 };
    auto blueBg = GraphicsAttributes {};
    blueBg.backgroundColor = RGBColor { 0, 0, 255 };

    grid.lineAt(LineOffset(1)).reset(LineFlags {}, redBg);
    grid.lineAt(LineOffset(2)).reset(LineFlags {}, blueBg);
    REQUIRE(grid.lineAt(LineOffset(1)).isBlankWithFillAttrs(redBg));
    REQUIRE(grid.lineAt(LineOffset(2)).isBlankWithFillAttrs(blueBg));

    // Partial-horizontal scrollUp: vertical margin [1..2], horizontal [2..7].
    // Row 2 (blueBg) is source, row 1 (redBg) is target. Differing attrs force materialization.
    auto const margin =
        Margin { .vertical = Margin::Vertical { .from = LineOffset(1), .to = LineOffset(2) },
                 .horizontal = Margin::Horizontal { .from = ColumnOffset(2), .to = ColumnOffset(7) } };
    grid.scrollUp(LineCount(1), GraphicsAttributes {}, margin);

    // Target row 1 must no longer be blank — the copied range carries the source's blueBg.
    CHECK_FALSE(grid.lineAt(LineOffset(1)).isBlank());
}

TEST_CASE("Grid.scrollDown.partialHorizontal.blankLinesDifferingFillAttrsMaterialize", "[grid][blank]")
{
    // Symmetric to scrollUp: partial-horizontal scrollDown between two blank lines with
    // differing fillAttrs must materialize the destination rather than skipping.
    auto grid = Grid(PageSize { LineCount(4), ColumnCount(10) }, true, LineCount(0));

    auto redBg = GraphicsAttributes {};
    redBg.backgroundColor = RGBColor { 255, 0, 0 };
    auto blueBg = GraphicsAttributes {};
    blueBg.backgroundColor = RGBColor { 0, 0, 255 };

    grid.lineAt(LineOffset(1)).reset(LineFlags {}, redBg);
    grid.lineAt(LineOffset(2)).reset(LineFlags {}, blueBg);
    REQUIRE(grid.lineAt(LineOffset(1)).isBlankWithFillAttrs(redBg));
    REQUIRE(grid.lineAt(LineOffset(2)).isBlankWithFillAttrs(blueBg));

    auto const margin =
        Margin { .vertical = Margin::Vertical { .from = LineOffset(1), .to = LineOffset(2) },
                 .horizontal = Margin::Horizontal { .from = ColumnOffset(2), .to = ColumnOffset(7) } };
    grid.scrollDown(LineCount(1), GraphicsAttributes {}, margin);

    // Row 2 is target (row 1 is source under scrollDown); it must materialize.
    CHECK_FALSE(grid.lineAt(LineOffset(2)).isBlank());
}

TEST_CASE("Grid.scrollUp.partialHorizontal.blankLinesMatchingFillAttrsStayBlank", "[grid][blank]")
{
    // The skip optimization must still apply when both lines share fillAttrs: the copy
    // would be a no-op, so both lines remain un-materialized (memory stays cheap).
    auto grid = Grid(PageSize { LineCount(4), ColumnCount(10) }, true, LineCount(0));

    auto attrs = GraphicsAttributes {};
    attrs.backgroundColor = RGBColor { 128, 128, 128 };
    grid.lineAt(LineOffset(1)).reset(LineFlags {}, attrs);
    grid.lineAt(LineOffset(2)).reset(LineFlags {}, attrs);

    auto const margin =
        Margin { .vertical = Margin::Vertical { .from = LineOffset(1), .to = LineOffset(2) },
                 .horizontal = Margin::Horizontal { .from = ColumnOffset(2), .to = ColumnOffset(7) } };
    grid.scrollUp(LineCount(1), GraphicsAttributes {}, margin);

    CHECK(grid.lineAt(LineOffset(1)).isBlank());
}

TEST_CASE("Grid.renderRange.trimsTrailingSpacesUnlessAsked", "[grid][capture]")
{
    // capture-pane's contract: tmux trims each row at its last non-default cell (grid_line_length)
    // and keeps the padding only for -J/-N. Rendering the full page width unconditionally handed
    // every client rows tmux would never emit.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(8) }, false, LineCount(0));
    grid.setLineText(LineOffset(0), "hi");

    auto const trimmed = grid.renderRange(
        LineOffset(0), LineOffset(0), CaptureRendition::PlainText, CaptureTrailingSpaces::Trim);
    REQUIRE(trimmed.size() == 1);
    CHECK(trimmed.front() == "hi");

    auto const kept = grid.renderRange(
        LineOffset(0), LineOffset(0), CaptureRendition::PlainText, CaptureTrailingSpaces::Keep);
    REQUIRE(kept.size() == 1);
    CHECK(kept.front() == "hi      ");

    // A row that is default cells throughout trims away entirely, exactly as tmux reports it.
    auto const blank = grid.renderRange(
        LineOffset(1), LineOffset(1), CaptureRendition::PlainText, CaptureTrailingSpaces::Trim);
    REQUIRE(blank.size() == 1);
    CHECK(blank.front().empty());
}

TEST_CASE("Grid.renderRange.aColouredBlankRowSurvivesTheTrim", "[grid][capture]")
{
    // The trim drops DEFAULT cells, not spaces: a region cleared under a coloured pen is something
    // the application drew, and dropping it would silently erase its colour from `capture-pane -e`.
    auto grid = Grid(PageSize { LineCount(1), ColumnCount(4) }, false, LineCount(0));
    auto redBg = GraphicsAttributes {};
    redBg.backgroundColor = RGBColor { 255, 0, 0 };
    grid.lineAt(LineOffset(0)).reset(LineFlags {}, redBg);

    auto const captured = grid.renderRange(
        LineOffset(0), LineOffset(0), CaptureRendition::PlainText, CaptureTrailingSpaces::Trim);
    REQUIRE(captured.size() == 1);
    CHECK(captured.front() == "    ");
}

TEST_CASE("Grid.renderRange.aReverseScrollAtCapacityLeavesTheWholeHistoryCapturable",
          "[grid][capture][stable-id]")
{
    // renderRange must address exactly the rows the daemon's own snapshot walk does. A reverse scroll
    // with the ring full once wrapped the rows it pushed off the page into the oldest history slots,
    // which then lay below the stable floor, and a capture clamped to -historyLineCount() returned
    // them, so `capture-pane -S -` began with phantom rows no mirror ever held. The scroll leaves
    // the history alone now: every history row is valid, and the capture returns each one.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, false, LineCount(2));
    grid.setLineText(LineOffset(0), "AAAAA");
    grid.setLineText(LineOffset(1), "BBBBB");
    grid.scrollUp(LineCount(2));
    grid.setLineText(LineOffset(0), "CCCCC");
    grid.setLineText(LineOffset(1), "DDDDD");
    grid.scrollUp(LineCount(2));
    REQUIRE(grid.historyLineCount() == LineCount(2)); // the ring is full

    grid.scrollDown(LineCount(1), GraphicsAttributes {}, fullPageMargin(grid.pageSize()));

    auto const floorOffset = grid.stableRangeFloor() - grid.stableLineIdOf(LineOffset(0));
    CHECK(floorOffset == -unbox<int64_t>(grid.historyLineCount()));

    auto valid = 0;
    grid.forEachValidLine([&](LineOffset, Line const&) { ++valid; });
    auto const captured = grid.renderRange(-boxed_cast<LineOffset>(grid.historyLineCount()),
                                           unbox<LineOffset>(grid.pageSize().lines) - LineOffset(1),
                                           CaptureRendition::PlainText,
                                           CaptureTrailingSpaces::Keep);
    CHECK(captured.size() == static_cast<std::size_t>(valid));
    CHECK(captured == std::vector<std::string> { "CCCCC", "DDDDD", "     ", "     " });
}

TEST_CASE("Grid.reflow.semanticMarksStayOnTheHeadLine", "[grid]")
{
    // A shell's semantic marks (OSC 133, and Contour's own SETMARK) name the line a prompt starts on and
    // the line a command's output starts on. Reflow re-splits the logical line those marks sit on, and the
    // marks must ride along with its HEAD alone: one prompt must not become several when the window is
    // widened, and it must not evaporate when the window is narrowed. findMarkerUpwards() and the
    // command-block scan both walk these flags, and to either of them a duplicate reads as a second prompt.
    auto const linesWith = [](Grid const& grid, LineFlag flag) {
        auto result = std::vector<LineOffset> {};
        for (auto const i:
             std::views::iota(-grid.historyLineCount().as<int>(), grid.pageSize().lines.as<int>()))
            if (grid.lineAt(LineOffset::cast_from(i)).isFlagEnabled(flag))
                result.push_back(LineOffset::cast_from(i));
        return result;
    };

    auto const soleHeadLine = [&](Grid const& grid, LineFlag flag) {
        auto const carriers = linesWith(grid, flag);
        REQUIRE(carriers.size() == 1);
        CHECK_FALSE(grid.lineAt(carriers.front()).wrapped());
    };

    auto const text = std::string(30, 'A');
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(30) }, true, LineCount(10));
    grid.setLineText(LineOffset(0), text);
    grid.lineAt(LineOffset(0))
        .setFlag(LineFlags { LineFlag::Marked, LineFlag::OutputStart, LineFlag::CommandEnd }, true);

    auto const reconstruct = [](Grid const& grid) {
        auto result = std::string {};
        for (auto const i:
             std::views::iota(-grid.historyLineCount().as<int>(), grid.pageSize().lines.as<int>()))
            result += grid.lineTextTrimmed(LineOffset::cast_from(i));
        return result;
    };

    SECTION("shrink splits the line but not its marks")
    {
        // 30 columns of text into 10 => a head plus two continuations.
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(10) }, CellLocation {}, WrapPending::No);
        CHECK(reconstruct(grid) == text);
        soleHeadLine(grid, LineFlag::Marked);
        soleHeadLine(grid, LineFlag::OutputStart);
        soleHeadLine(grid, LineFlag::CommandEnd);
    }

    SECTION("grow re-splits the line but not its marks")
    {
        // Narrow first so the logical line really is wrapped, then widen to a width it still overflows:
        // 30 columns of text into 20 => a head plus one continuation. This is the case that used to stamp
        // the head's flags onto every chunk it produced.
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(10) }, CellLocation {}, WrapPending::No);
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(20) }, CellLocation {}, WrapPending::No);
        CHECK(reconstruct(grid) == text);
        soleHeadLine(grid, LineFlag::Marked);
        soleHeadLine(grid, LineFlag::OutputStart);
        soleHeadLine(grid, LineFlag::CommandEnd);
    }

    SECTION("a full round-trip leaves the line whole again, marks and all")
    {
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(10) }, CellLocation {}, WrapPending::No);
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(30) }, CellLocation {}, WrapPending::No);
        CHECK(reconstruct(grid) == text);
        soleHeadLine(grid, LineFlag::Marked);
        soleHeadLine(grid, LineFlag::OutputStart);
        soleHeadLine(grid, LineFlag::CommandEnd);
    }
}

namespace
{
/// Writes @p text into the row at @p offset and attributes the row to @p block, first reached at @p bornAt.
void writeOwnedRow(
    Grid& grid, LineOffset offset, std::string_view text, CommandBlockId block, uint32_t bornAt)
{
    grid.setLineText(offset, text);
    grid.lineAt(offset).adoptBlock(block);
    grid.lineAt(offset).stampBornAt(bornAt);
}

/// Every non-blank row of @p grid, top first, as "text block@bornAt".
[[nodiscard]] std::vector<std::string> rowOwners(Grid const& grid)
{
    auto result = std::vector<std::string> {};
    for (auto const i: std::views::iota(-grid.historyLineCount().as<int>(), grid.pageSize().lines.as<int>()))
    {
        auto const offset = LineOffset::cast_from(i);
        auto const text = grid.lineTextTrimmed(offset);
        if (!text.empty())
            result.push_back(
                std::format("{} {}@{}", text, grid.lineAt(offset).blockId(), grid.lineAt(offset).bornAt()));
    }
    return result;
}
} // namespace

TEST_CASE("Grid.reflow.blockIdAndBirthTimeRideOnEveryChunk", "[grid][semanticblocks]")
{
    // Unlike the semantic marks above, a row's block and birth time are NOT head-only: they say which
    // command produced the row and when, and re-chopping a logical line into different physical pieces
    // changes neither. Losing them would erase a running command's colour and timestamps the moment the
    // window is resized.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(30) }, true, LineCount(10));
    writeOwnedRow(grid, LineOffset(0), std::string(30, 'A'), CommandBlockId { 7 }, 42);
    writeOwnedRow(grid, LineOffset(1), "BBBB", CommandBlockId { 8 }, 50);

    SECTION("narrowing splits the line into chunks that all keep it")
    {
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(10) }, CellLocation {}, WrapPending::No);
        CHECK(rowOwners(grid)
              == std::vector<std::string> {
                  "AAAAAAAAAA 7@42", "AAAAAAAAAA 7@42", "AAAAAAAAAA 7@42", "BBBB 8@50" });
    }

    SECTION("narrowing twice rebuilds continuations that still keep it")
    {
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(10) }, CellLocation {}, WrapPending::No);
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(7) }, CellLocation {}, WrapPending::No);
        CHECK(rowOwners(grid)
              == std::vector<std::string> {
                  "AAAAAAA 7@42", "AAAAAAA 7@42", "AAAAAAA 7@42", "AAAAAAA 7@42", "AA 7@42", "BBBB 8@50" });
    }

    SECTION("widening rejoins the chunks without losing it")
    {
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(10) }, CellLocation {}, WrapPending::No);
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(20) }, CellLocation {}, WrapPending::No);
        CHECK(rowOwners(grid)
              == std::vector<std::string> { "AAAAAAAAAAAAAAAAAAAA 7@42", "AAAAAAAAAA 7@42", "BBBB 8@50" });
    }
}

TEST_CASE("Grid.reflow.overflowCarriesTheRowItWasCutFrom", "[grid][semanticblocks]")
{
    // One logical line whose rows the cursor reached at different times -- and, to tell them apart, in
    // different blocks. Narrowing it rebuilds both continuations in place, each keeping its own values, and
    // the columns the LAST of them can no longer hold become new rows. Those were cut from that last row,
    // so they carry its values; the head's would make the birth times run backwards down the line.
    //
    // 30 columns into 7: the head keeps 7 and hands 3 on, the first continuation absorbs them and hands 6
    // on, and the second absorbs those and is left with 9 it cannot hold -- two new rows, both cut from it.
    auto grid = Grid(PageSize { LineCount(5), ColumnCount(10) }, true, LineCount(10));
    auto const writeLogicalLine = [&](int top) {
        writeOwnedRow(grid, LineOffset::cast_from(top), std::string(10, 'A'), CommandBlockId { 1 }, 10);
        writeOwnedRow(grid, LineOffset::cast_from(top + 1), std::string(10, 'B'), CommandBlockId { 2 }, 20);
        writeOwnedRow(grid, LineOffset::cast_from(top + 2), std::string(10, 'C'), CommandBlockId { 3 }, 30);
        grid.lineAt(LineOffset::cast_from(top + 1)).setWrapped(true);
        grid.lineAt(LineOffset::cast_from(top + 2)).setWrapped(true);
    };

    SECTION("before another line, which flushes the overflow")
    {
        writeLogicalLine(0);
        writeOwnedRow(grid, LineOffset(3), "DDDD", CommandBlockId { 4 }, 40);
        (void) grid.resize(PageSize { LineCount(5), ColumnCount(7) }, CellLocation {}, WrapPending::No);
        CHECK(rowOwners(grid)
              == std::vector<std::string> {
                  "AAAAAAA 1@10", "AAABBBB 2@20", "BBBBBBC 3@30", "CCCCCCC 3@30", "CC 3@30", "DDDD 4@40" });
    }

    SECTION("on the bottom row, where the page end flushes the overflow")
    {
        writeLogicalLine(2);
        (void) grid.resize(PageSize { LineCount(5), ColumnCount(7) }, CellLocation {}, WrapPending::No);
        CHECK(rowOwners(grid)
              == std::vector<std::string> {
                  "AAAAAAA 1@10", "AAABBBB 2@20", "BBBBBBC 3@30", "CCCCCCC 3@30", "CC 3@30" });
    }
}

TEST_CASE("Grid.reflow.aContinuationWithoutItsHeadKeepsItsOwnValues", "[grid][semanticblocks]")
{
    // Widening rejoins a logical line under its head's values. A continuation whose head is gone has none
    // to take, and keeps its own -- neither defaults nor the values of whichever line came before it.
    SECTION("the head row was erased to blank")
    {
        // Printing past the margin, then CUU and EL 2 over the head: the continuation stays wrapped, and the
        // nearest non-blank head above it is an unrelated line -- a prompt, here.
        auto grid = Grid(PageSize { LineCount(4), ColumnCount(10) }, true, LineCount(10));
        writeOwnedRow(grid, LineOffset(0), "HHHH", CommandBlockId { 7 }, 70);
        grid.lineAt(LineOffset(0)).setMarked(true);
        writeOwnedRow(grid, LineOffset(2), "WWWW", CommandBlockId { 9 }, 90);
        grid.lineAt(LineOffset(2)).setWrapped(true);
        writeOwnedRow(grid, LineOffset(3), "FFFF", CommandBlockId { 10 }, 100);

        (void) grid.resize(PageSize { LineCount(4), ColumnCount(20) }, CellLocation {}, WrapPending::No);
        CHECK(rowOwners(grid) == std::vector<std::string> { "HHHH 7@70", "WWWW 9@90", "FFFF 10@100" });

        // Nor does it take that line's semantic marks: there is still one prompt, not two.
        REQUIRE(grid.lineTextTrimmed(LineOffset(2)) == "WWWW");
        CHECK_FALSE(grid.lineAt(LineOffset(2)).isFlagEnabled(LineFlag::Marked));
    }

    SECTION("the head row was evicted from the history")
    {
        auto grid = Grid(PageSize { LineCount(2), ColumnCount(10) }, true, LineCount(1));
        grid.setLineText(LineOffset(0), std::string(10, 'H'));
        writeOwnedRow(grid, LineOffset(1), "WWWW", CommandBlockId { 9 }, 90);
        grid.lineAt(LineOffset(1)).setWrapped(true);
        grid.scrollUp(LineCount(2)); // H is evicted, and W becomes the oldest history row
        REQUIRE(grid.historyLineCount() == LineCount(1));
        REQUIRE(grid.lineTextTrimmed(LineOffset(-1)) == "WWWW");
        REQUIRE(grid.lineAt(LineOffset(-1)).wrapped());
        writeOwnedRow(grid, LineOffset(0), "FFFF", CommandBlockId { 10 }, 100);

        (void) grid.resize(PageSize { LineCount(2), ColumnCount(20) }, CellLocation {}, WrapPending::No);
        CHECK(rowOwners(grid) == std::vector<std::string> { "WWWW 9@90", "FFFF 10@100" });
    }
}

// }}}

// {{{ stable row identity
TEST_CASE("Grid.stableId.roundTripBelowCapacity", "[grid][stable-id]")
{
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, false, LineCount(5));
    grid.setLineText(LineOffset(0), "AAAAA");
    grid.setLineText(LineOffset(1), "BBBBB");

    auto const idA = grid.stableLineIdOf(LineOffset(0));
    grid.scrollUp(LineCount(1)); // A scrolls into history

    // The id names the same PHYSICAL row across the rotation.
    auto const offset = grid.lineOffsetOf(idA);
    REQUIRE(offset.has_value());
    CHECK(*offset == LineOffset(-1));
    CHECK(grid.lineText(*offset) == "AAAAA");
    // Page row 0 is a new physical row: the id space advanced with the scroll.
    CHECK(grid.stableLineIdOf(LineOffset(0)) == idA + 1);
}

TEST_CASE("Grid.stableId.evictionAdvancesTheFloorMonotonically", "[grid][stable-id]")
{
    // Ring capacity: 2 page + 1 history = 3 slots.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, false, LineCount(1));
    grid.setLineText(LineOffset(0), "AAAAA");
    auto const idA = grid.stableLineIdOf(LineOffset(0));
    auto const floorBefore = grid.stableRangeFloor();

    grid.scrollUp(LineCount(1)); // A -> history, still addressable
    REQUIRE(grid.lineOffsetOf(idA).has_value());
    CHECK(grid.stableRangeFloor() >= floorBefore);

    grid.scrollUp(LineCount(1)); // ring full: A is evicted
    CHECK(grid.lineOffsetOf(idA) == std::nullopt);
    CHECK(grid.stableRangeFloor() > floorBefore);
}

TEST_CASE("Grid.stableId.scrollDownKeepsRowIdentity", "[grid][stable-id]")
{
    auto grid = Grid(PageSize { LineCount(3), ColumnCount(5) }, false, LineCount(5));
    grid.setLineText(LineOffset(0), "AAAAA");
    auto const idA = grid.stableLineIdOf(LineOffset(0));

    // A full-page scrollDown pushes row A downward: same id, new offset.
    grid.scrollDown(LineCount(1), GraphicsAttributes {}, fullPageMargin(grid.pageSize()));

    auto const offset = grid.lineOffsetOf(idA);
    REQUIRE(offset.has_value());
    CHECK(*offset == LineOffset(1));
    CHECK(grid.lineText(*offset) == "AAAAA");
}

TEST_CASE("Grid.stableId.reverseScrollOnZeroHistoryKeepsIdentity", "[grid][stable-id]")
{
    // Zero history (the alternate screen's shape): a full-page reverse scroll
    // sinks the base below the floor, but with no history slots there is no
    // garbage to re-validate and the exposed top row takes a strictly-fresh id
    // that was never issued. Row identity survives WITHOUT a generation bump, so
    // an attached mirror gets an incremental delta, not a whole-screen resnapshot.
    auto grid = Grid(PageSize { LineCount(3), ColumnCount(5) }, false, LineCount(0));
    grid.setLineText(LineOffset(0), "AAAAA");
    auto const generationBefore = grid.generation();
    auto const idA = grid.stableLineIdOf(LineOffset(0));

    grid.scrollDown(LineCount(1), GraphicsAttributes {}, fullPageMargin(grid.pageSize()));

    CHECK(grid.generation() == generationBefore); // no wholesale rebuild
    CHECK(grid.stableRangeFloor() == grid.stableLineIdOf(LineOffset(0)));
    CHECK(grid.lineOffsetOf(idA) == LineOffset(1)); // A kept its id, moved down
    CHECK(grid.lineText(LineOffset(1)) == "AAAAA");
    for (auto const offset: std::views::iota(0, 3))
        CHECK(grid.lineOffsetOf(grid.stableLineIdOf(LineOffset(offset))) == LineOffset(offset));
}

TEST_CASE("Grid.stableId.aReverseScrollWithHistoryNeverRebuildsIdentity", "[grid][stable-id]")
{
    // With history, a full-page reverse scroll moves the page's rows and leaves the stable base where it
    // is, so however far it scrolls, it never sinks the base below the floor and never renames a row.
    auto grid = Grid(PageSize { LineCount(3), ColumnCount(5) }, false, LineCount(5));
    grid.scrollUp(LineCount(2)); // two history rows: base 2, floor 0
    auto const generationBefore = grid.generation();
    auto const baseBefore = grid.stableLineIdOf(LineOffset(0));
    auto const floorBefore = grid.stableRangeFloor();

    for (auto const count: std::array { 2, 1, 3 })
    {
        grid.scrollDown(LineCount(count), GraphicsAttributes {}, fullPageMargin(grid.pageSize()));
        CHECK(grid.generation() == generationBefore);
        CHECK(grid.stableLineIdOf(LineOffset(0)) == baseBefore);
        CHECK(grid.stableRangeFloor() == floorBefore);
    }
}

TEST_CASE("Grid.stableId.unscrollPullsHistoryRowsBackUnderTheirIds", "[grid][stable-id]")
{
    auto grid = Grid(PageSize { LineCount(3), ColumnCount(5) }, false, LineCount(5));
    grid.setLineText(LineOffset(0), "AAAAA");
    grid.scrollUp(LineCount(1)); // A -> history
    auto const idA = grid.stableLineIdOf(LineOffset(-1));
    auto const floorBefore = grid.stableRangeFloor();

    grid.unscroll(LineCount(1), GraphicsAttributes {});

    auto const offset = grid.lineOffsetOf(idA);
    REQUIRE(offset.has_value());
    CHECK(*offset == LineOffset(0));
    CHECK(grid.lineText(*offset) == "AAAAA");
    CHECK(grid.stableRangeFloor() >= floorBefore); // the floor never regresses
}

namespace
{
/// Which scroll a RowMove case makes.
enum class ScrollKind : uint8_t
{
    Up = 0,   ///< scrollUp(n, ..., margin)
    Down,     ///< scrollDown(n, ..., margin)
    Unscroll, ///< unscroll(n, ...), whose margin is the whole page
};

/// One scroll, on the 6-row page Grid.stableId.rowMoveSaysWhereEachIdsContentWent sets up, rows @ref top to
/// @ref bottom.
struct RowMoveCase
{
    std::string_view name;
    ScrollKind kind {};
    int count = 0;
    int top = 0;
    int bottom = 0;
};

constexpr auto RowMoveCases = std::array {
    RowMoveCase { .name = "the whole page up", .kind = ScrollKind::Up, .count = 1, .top = 0, .bottom = 5 },
    RowMoveCase {
        .name = "a region at the top, up", .kind = ScrollKind::Up, .count = 2, .top = 0, .bottom = 3 },
    RowMoveCase {
        .name = "a region below the top, up", .kind = ScrollKind::Up, .count = 1, .top = 1, .bottom = 4 },
    RowMoveCase { .name = "all of a region, up", .kind = ScrollKind::Up, .count = 4, .top = 1, .bottom = 4 },
    RowMoveCase { .name = "a region, down", .kind = ScrollKind::Down, .count = 1, .top = 1, .bottom = 4 },
    RowMoveCase {
        .name = "the whole page down", .kind = ScrollKind::Down, .count = 2, .top = 0, .bottom = 5 },
    RowMoveCase { .name = "unscroll", .kind = ScrollKind::Unscroll, .count = 1, .top = 0, .bottom = 5 },
};
} // namespace

TEST_CASE("Grid.stableId.rowMoveSaysWhereEachIdsContentWent", "[grid][stable-id]")
{
    // A scroll inside margins moves rows' contents while each stable id keeps naming its place, and a reverse
    // scroll drops rows. rowMoveOfScrollUp() and rowMoveOfScrollDown() say so ahead of the scroll, for those
    // who hold ids; checked here against what the scroll then does to every row the grid holds.
    for (auto const& testCase: RowMoveCases)
    {
        INFO(testCase.name);
        auto grid = setupGrid(PageSize { LineCount(6), ColumnCount(10) },
                              false,
                              LineCount(10),
                              { "h0", "h1", "p0", "p1", "p2", "p3", "p4", "p5" });
        auto const margin =
            Margin { .vertical = Margin::Vertical { .from = LineOffset(testCase.top),
                                                    .to = LineOffset(testCase.bottom) },
                     .horizontal = Margin::Horizontal { .from = ColumnOffset(0), .to = ColumnOffset(9) } };
        auto const n = LineCount(testCase.count);

        auto before = std::vector<std::pair<int64_t, std::string>> {};
        for (auto const row: std::views::iota(unbox<int>(grid.addressableTop()), 6))
            before.emplace_back(grid.stableLineIdOf(LineOffset(row)), grid.lineTextTrimmed(LineOffset(row)));

        auto move = std::optional<RowMove> {};
        switch (testCase.kind)
        {
            case ScrollKind::Up:
                move = grid.rowMoveOfScrollUp(n, margin);
                grid.scrollUp(n, GraphicsAttributes {}, margin);
                break;
            case ScrollKind::Down:
                move = grid.rowMoveOfScrollDown(n, margin);
                grid.scrollDown(n, GraphicsAttributes {}, margin);
                break;
            case ScrollKind::Unscroll:
                move = grid.rowMoveOfUnscroll(n);
                grid.unscroll(n, GraphicsAttributes {});
                break;
        }

        // Every row's text is distinct: content the move says survived is found under the id it says, and
        // content it says went is no longer under its old id.
        for (auto const& [id, text]: before)
        {
            auto const destination = move ? move->destinationOf(id) : std::optional { id };
            INFO(std::format("{} named \"{}\", the move sends it to {}", id, text, destination.value_or(-1)));
            if (!destination)
            {
                auto const offset = grid.lineOffsetOf(id);
                CHECK((!offset || grid.lineTextTrimmed(*offset) != text));
                continue;
            }
            auto const offset = grid.lineOffsetOf(*destination);
            REQUIRE(offset.has_value());
            CHECK(grid.lineTextTrimmed(*offset) == text);
        }
    }
}

TEST_CASE("Grid.stableId.aScrollBetweenLeftAndRightMarginsMovesNoRow", "[grid][stable-id]")
{
    // Cells move between rows there, but no row's content leaves its id: each row keeps its marks and block.
    auto const grid = Grid(PageSize { LineCount(6), ColumnCount(10) }, false, LineCount(10));
    auto const margin =
        Margin { .vertical = Margin::Vertical { .from = LineOffset(1), .to = LineOffset(4) },
                 .horizontal = Margin::Horizontal { .from = ColumnOffset(1), .to = ColumnOffset(9) } };

    CHECK_FALSE(grid.rowMoveOfScrollUp(LineCount(1), margin).has_value());
    CHECK_FALSE(grid.rowMoveOfScrollDown(LineCount(1), margin).has_value());
}

TEST_CASE("Grid.stableId.clearHistoryEvictsAllHistoryIds", "[grid][stable-id]")
{
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, false, LineCount(5));
    grid.setLineText(LineOffset(0), "AAAAA");
    grid.scrollUp(LineCount(2));
    REQUIRE(grid.historyLineCount() == LineCount(2));

    auto const idHistory = grid.stableLineIdOf(LineOffset(-1));
    auto const idPage = grid.stableLineIdOf(LineOffset(0));
    auto const generationBefore = grid.generation();

    grid.clearHistory();

    // History ids are evicted via the floor jump; page identity is untouched
    // and NO generation bump happened (clients drop history without a resend).
    CHECK(grid.lineOffsetOf(idHistory) == std::nullopt);
    CHECK(grid.lineOffsetOf(idPage) == LineOffset(0));
    CHECK(grid.stableRangeFloor() == grid.stableLineIdOf(LineOffset(0)));
    CHECK(grid.generation() == generationBefore);
}

TEST_CASE("Grid.generation.bumpsOnlyOnWholesaleRebuilds", "[grid][stable-id]")
{
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, true, LineCount(5));
    auto const g0 = grid.generation();

    grid.scrollUp(LineCount(1));
    CHECK(grid.generation() == g0); // scrolling never destroys identity

    grid.clearHistory();
    CHECK(grid.generation() == g0); // the floor jump suffices

    std::ignore = grid.resize(PageSize { LineCount(2), ColumnCount(6) }, CellLocation {}, WrapPending::No);
    CHECK(grid.generation() == g0 + 1); // reflow rebuilds the whole ring

    grid.setHistoryLimits(HistoryLimits::plain(LineCount(9)));
    CHECK(grid.generation() == g0 + 2);

    grid.reset();
    CHECK(grid.generation() == g0 + 3);
}
// }}}

// {{{ delta queries
namespace
{
/// Drains all pending changes so a test starts from a clean cursor.
GridDeltaCursor drainedCursor(Grid& grid)
{
    auto cursor = GridDeltaCursor {};
    std::ignore = grid.forEachLineChangedSince(cursor, [](LineOffset, Line const&) {});
    return cursor;
}

std::vector<int> changedOffsets(Grid& grid, GridDeltaCursor& cursor)
{
    auto out = std::vector<int> {};
    std::ignore = grid.forEachLineChangedSince(
        cursor, [&](LineOffset offset, Line const&) { out.push_back(unbox<int>(offset)); });
    return out;
}
} // namespace

TEST_CASE("Grid.delta.bootstrapReportsEveryPageLineThenIdles", "[grid][delta]")
{
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, false, LineCount(5));

    auto cursor = GridDeltaCursor {};
    CHECK(changedOffsets(grid, cursor) == std::vector { 0, 1 }); // fresh lines are pending

    // Idle idempotence: nothing changed, nothing reported, the seqno holds still.
    auto const seqnoBefore = grid.seqno();
    CHECK(changedOffsets(grid, cursor).empty());
    CHECK(grid.seqno() == seqnoBefore);
}

TEST_CASE("Grid.delta.onlyTheWrittenLineIsReported", "[grid][delta]")
{
    auto grid = Grid(PageSize { LineCount(3), ColumnCount(5) }, false, LineCount(5));
    auto cursor = drainedCursor(grid);

    grid.setLineText(LineOffset(1), "hello");

    CHECK(changedOffsets(grid, cursor) == std::vector { 1 });
}

TEST_CASE("Grid.delta.scrolledOutRowsReportAtTheirNegativeOffset", "[grid][delta]")
{
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, false, LineCount(5));
    auto cursor = drainedCursor(grid);

    grid.setLineText(LineOffset(0), "AAAAA");
    grid.scrollUp(LineCount(1));

    // The written row scrolled to -1 within the same batch and must be stamped
    // there; the new bottom row is fresh; the untouched middle row (now at 0)
    // moved by pure rotation -- same id, same content, NOT reported.
    CHECK(changedOffsets(grid, cursor) == std::vector { -1, 1 });
}

TEST_CASE("Grid.delta.marginScrollMovedRowsAreReported", "[grid][delta]")
{
    auto grid = Grid(PageSize { LineCount(3), ColumnCount(5) }, false, LineCount(0));
    grid.setLineText(LineOffset(0), "AAAAA");
    grid.setLineText(LineOffset(1), "BBBBB");
    grid.setLineText(LineOffset(2), "CCCCC");
    auto cursor = drainedCursor(grid);

    // Scroll region rows 1..2: row 2 moves into row 1 (move assignment dirties
    // the destination), row 2 is blanked. Row 0 is outside the region.
    auto const margin =
        Margin { .vertical = Margin::Vertical { .from = LineOffset(1), .to = LineOffset(2) },
                 .horizontal = Margin::Horizontal { .from = ColumnOffset(0), .to = ColumnOffset(4) } };
    std::ignore = grid.scrollUp(LineCount(1), GraphicsAttributes {}, margin);

    CHECK(changedOffsets(grid, cursor) == std::vector { 1, 2 });
    CHECK(grid.lineText(LineOffset(1)) == "CCCCC");
}

TEST_CASE("Grid.delta.resizeForcesOneResyncThenDeltas", "[grid][delta]")
{
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, true, LineCount(5));
    auto cursor = drainedCursor(grid);

    std::ignore = grid.resize(PageSize { LineCount(2), ColumnCount(7) }, CellLocation {}, WrapPending::No);

    auto reported = 0;
    CHECK(grid.forEachLineChangedSince(cursor, [&](LineOffset, Line const&) { ++reported; })
          == GridDeltaResult::ResyncRequired);
    CHECK(reported == 0); // a resync never reports lines; snapshot instead:

    auto snapshot = 0;
    grid.forEachValidLine([&](LineOffset, Line const&) { ++snapshot; });
    CHECK(snapshot == 2);

    // The re-anchored cursor resumes plain delta service.
    CHECK(changedOffsets(grid, cursor).empty());
    grid.setLineText(LineOffset(0), "after");
    CHECK(changedOffsets(grid, cursor) == std::vector { 0 });
}

TEST_CASE("Grid.delta.zeroHistoryReverseScrollStaysIncremental", "[grid][delta]")
{
    // The attach-daemon scenario: a client follows the alternate screen (zero
    // history) and the app reverse-scrolls (RI at the top). With no history slots
    // the exposed top row takes a strictly-fresh id, so this stays an INCREMENTAL
    // delta -- only the exposed row reports, not a whole-screen resync to every
    // mirror. (The clamp is also never handed an inverted range.)
    auto grid = Grid(PageSize { LineCount(3), ColumnCount(5) }, false, LineCount(0));
    grid.setLineText(LineOffset(1), "MID");
    auto cursor = drainedCursor(grid);
    auto const generationBefore = grid.generation();

    grid.scrollDown(LineCount(1), GraphicsAttributes {}, fullPageMargin(grid.pageSize()));

    // No resync (generation held); only the freshly exposed top row reports --
    // rows that merely shifted down kept their ids, content and revision.
    auto reported = std::vector<int> {};
    CHECK(grid.forEachLineChangedSince(cursor, [&](LineOffset offset, Line const&) {
        reported.push_back(unbox<int>(offset));
    }) == GridDeltaResult::Delta);
    CHECK(grid.generation() == generationBefore);
    CHECK(reported == std::vector { 0 });

    // The whole page stays addressable, and plain delta service continues.
    auto offsets = std::vector<int> {};
    grid.forEachValidLine([&](LineOffset offset, Line const&) { offsets.push_back(unbox<int>(offset)); });
    CHECK(offsets == std::vector { 0, 1, 2 });
    CHECK(changedOffsets(grid, cursor).empty());
    grid.setLineText(LineOffset(2), "after");
    CHECK(changedOffsets(grid, cursor) == std::vector { 2 });
}

TEST_CASE("Grid.delta.clearHistoryNeedsNoResend", "[grid][delta]")
{
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, false, LineCount(5));
    grid.setLineText(LineOffset(0), "AAAAA");
    grid.scrollUp(LineCount(2));
    auto cursor = drainedCursor(grid);

    grid.clearHistory();

    // The floor jump evicted the history ids; nothing was rewritten, so the
    // delta stream stays silent -- clients just drop their history.
    CHECK(changedOffsets(grid, cursor).empty());
}
// }}}

// NOLINTEND(misc-const-correctness)

TEST_CASE("Grid.delta.aHistoryRowDirtiedInPlaceIsReported", "[grid][delta]")
{
    // A scrollback row can be dirtied with NOTHING scrolling: Screen's OSC 133 handlers stamp the
    // semantic marks on a logical line's HEAD, and logicalLineHead() walks up wrapped rows all the
    // way into the history. Such a row lies outside the scrolled-out prefix the scans covered, so
    // it was neither stamped nor reported — the attached client's mirror kept the previous
    // PromptEnd/CommandEnd flags for that logical line forever, and every feature built on them
    // (copy-last-command-output, prompt-aware selection) selected the wrong range client-side while
    // working correctly on the daemon.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, false, LineCount(5));
    grid.setLineText(LineOffset(0), "AAAAA");
    grid.setLineText(LineOffset(1), "BBBBB");
    grid.scrollUp(LineCount(2)); // both rows now live at -2 and -1
    auto cursor = drainedCursor(grid);
    REQUIRE(changedOffsets(grid, cursor).empty());

    // Mark the deepest history row, exactly as OSC 133;B does on a wrapped prompt's head.
    grid.changingLineAt(LineOffset(-2)).setFlag(LineFlag::PromptEnd, true);

    CHECK(changedOffsets(grid, cursor) == std::vector { -2 });
    // ...and it settles again: the extension is not a permanent widening of the scan.
    CHECK(changedOffsets(grid, cursor).empty());
}

TEST_CASE("Grid.delta.aHistoryRowChangeReachesALaggingConsumer", "[grid][delta]")
{
    // Two consumers (two attached clients) share one grid, and either one's pump finalizes for
    // both. A consumer that missed the batch the history row was stamped in must still be told —
    // its own scrolled-out prefix is empty, so nothing else would ever bring the row into range.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, false, LineCount(5));
    grid.setLineText(LineOffset(0), "AAAAA");
    grid.scrollUp(LineCount(1));
    auto fast = drainedCursor(grid);
    auto slow = drainedCursor(grid);

    grid.changingLineAt(LineOffset(-1)).setPromptEndOffset(ColumnOffset(3));
    CHECK(changedOffsets(grid, fast) == std::vector { -1 });

    // The slow consumer polls only now, a batch later, and must see the same row.
    grid.setLineText(LineOffset(1), "later");
    CHECK(changedOffsets(grid, slow) == std::vector { -1, 1 });
}

TEST_CASE("Grid.delta.readingTheScrollbackDoesNotWidenTheScan", "[grid][delta]")
{
    // The extension is armed by changingLineAt and by nothing else — deliberately NOT by picking
    // the non-const lineAt overload, because which overload a caller lands on follows the constness
    // of the enclosing FUNCTION: Screen::captureBuffer walks the whole scrollback from a non-const
    // method, and that read would otherwise pin every later delta scan at the top of the history.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, false, LineCount(5));
    grid.setLineText(LineOffset(0), "AAAAA");
    grid.setLineText(LineOffset(1), "BBBBB");
    grid.scrollUp(LineCount(2));
    auto cursor = drainedCursor(grid);

    CHECK(grid.lineText(LineOffset(-2)) == "AAAAA");
    std::ignore = grid.lineAt(LineOffset(-2)).isBlank(); // the MUTABLE overload, from a read
    std::ignore = grid.at(LineOffset(-2), ColumnOffset(0));

    CHECK(changedOffsets(grid, cursor).empty());
}

TEST_CASE("Grid.delta.aGenerationBumpForgetsTheHistoryWatermark", "[grid][delta]")
{
    // The watermark is keyed by stable id, and a generation bump destroys row identity wholesale.
    // Keeping it would point the scan at ids that no longer mean anything.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, true, LineCount(5));
    grid.setLineText(LineOffset(0), "AAAAA");
    grid.scrollUp(LineCount(1));
    auto cursor = drainedCursor(grid);
    grid.changingLineAt(LineOffset(-1)).setFlag(LineFlag::CommandEnd, true);

    std::ignore = grid.resize(PageSize { LineCount(2), ColumnCount(7) }, CellLocation {}, WrapPending::No);
    CHECK(grid.forEachLineChangedSince(cursor, [](LineOffset, Line const&) {})
          == GridDeltaResult::ResyncRequired);
    // The re-anchored cursor resumes plain, un-widened delta service.
    CHECK(changedOffsets(grid, cursor).empty());
}

TEST_CASE("Grid.delta.reflowShrinkKeepsItsNewHistoryAddressable", "[grid][delta]")
{
    // The reflow-shrink path rebuilds the whole ring and GROWS the history. Rotating the local
    // vector instead of going through rotateBuffersLeft left _stableBase where it was, and
    // syncStableFloor()'s max() can only RAISE the floor, never lower one — so every row reflow had
    // just created sat below _stableFloor and forEachValidLine() began its walk above them. A client
    // attaching after a narrowing resize received a grid with no scrollback at all, though the
    // daemon still held the rows and capture-pane would happily return them.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(6) }, true, LineCount(10));
    // A fresh grid: base 0, floor 0, no history — precisely the state whose max() cannot recover.
    REQUIRE(grid.historyLineCount() == LineCount(0));
    REQUIRE(grid.stableRangeFloor() == grid.stableLineIdOf(LineOffset(0)));

    grid.setLineText(LineOffset(0), "ABCDEF");
    grid.setLineText(LineOffset(1), "GHIJKL");
    grid.lineAt(LineOffset(0)).setWrappable(true);
    grid.lineAt(LineOffset(1)).setWrappable(true);

    // Narrow to three columns: each six-column row reflows into two, so two rows are pushed into
    // history and the page keeps the last two.
    std::ignore = grid.resize(PageSize { LineCount(2), ColumnCount(3) }, CellLocation {}, WrapPending::No);
    REQUIRE(grid.historyLineCount() == LineCount(2));

    // The floor must have followed the base down, or the rows below it are unaddressable.
    CHECK(grid.stableRangeFloor()
          == grid.stableLineIdOf(LineOffset(0)) - unbox<int64_t>(grid.historyLineCount()));

    // The snapshot every attaching client and every ResyncRequired takes must report them.
    auto reported = std::vector<int> {};
    grid.forEachValidLine([&](LineOffset offset, Line const&) { reported.push_back(unbox<int>(offset)); });
    CHECK(reported == std::vector { -2, -1, 0, 1 });

    // And the rows are the reflowed content, not blanks.
    CHECK(grid.lineText(LineOffset(-2)) == "ABC");
    CHECK(grid.lineText(LineOffset(-1)) == "DEF");
    CHECK(grid.lineText(LineOffset(0)) == "GHI");
    CHECK(grid.lineText(LineOffset(1)) == "JKL");

    // Every id in the reported range resolves back to the offset it was reported at.
    for (auto const offset: reported)
        CHECK(grid.lineOffsetOf(grid.stableLineIdOf(LineOffset::cast_from(offset)))
              == LineOffset::cast_from(offset));
}

TEST_CASE("Grid.delta.reflowShrinkOverExistingHistoryStaysAddressable", "[grid][delta]")
{
    // The same, starting from a grid that ALREADY has history and a base above zero — the state a
    // long-lived session is in when the user narrows the window.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(6) }, true, LineCount(10));
    for (auto const row: std::views::iota(0, 4))
    {
        grid.setLineText(LineOffset(1), std::format("{}23456", row));
        grid.lineAt(LineOffset(1)).setWrappable(true);
        grid.scrollUp(LineCount(1));
    }
    REQUIRE(grid.historyLineCount() > LineCount(0));

    std::ignore = grid.resize(PageSize { LineCount(2), ColumnCount(3) }, CellLocation {}, WrapPending::No);

    CHECK(grid.stableRangeFloor()
          == grid.stableLineIdOf(LineOffset(0)) - unbox<int64_t>(grid.historyLineCount()));
    auto reported = 0;
    grid.forEachValidLine([&](LineOffset, Line const&) { ++reported; });
    CHECK(reported == unbox<int>(grid.historyLineCount()) + unbox<int>(grid.pageSize().lines));
}

TEST_CASE("Grid.delta.aScrollTheFastPathSkippedReportsNothing", "[grid][delta][blank]")
{
    // The partial-horizontal scroll's no-op fast path decides that a row needs no write at all, so
    // it must not stamp the row it declines to touch. Reading the target through the MUTABLE
    // Line::storage() overload — which dirties pessimistically — re-shipped a provably unchanged
    // WireLine to every attached client, once per skipped row per scroll, on the hottest path in
    // the emulator. std::as_const picks the const overload and costs nothing.
    auto grid = Grid(PageSize { LineCount(4), ColumnCount(10) }, true, LineCount(0));
    auto attrs = GraphicsAttributes {};
    attrs.backgroundColor = RGBColor { 128, 128, 128 };
    grid.lineAt(LineOffset(1)).reset(LineFlags {}, attrs);
    grid.lineAt(LineOffset(2)).reset(LineFlags {}, attrs);
    auto cursor = drainedCursor(grid);

    auto const margin =
        Margin { .vertical = Margin::Vertical { .from = LineOffset(1), .to = LineOffset(2) },
                 .horizontal = Margin::Horizontal { .from = ColumnOffset(2), .to = ColumnOffset(7) } };
    std::ignore = grid.scrollUp(LineCount(1), attrs, margin);
    CHECK(changedOffsets(grid, cursor).empty());

    grid.scrollDown(LineCount(1), attrs, margin); // the same shape, mirrored
    CHECK(changedOffsets(grid, cursor).empty());
}

TEST_CASE("Grid.delta.growColumnsKeepsItsHistoryAddressable", "[grid][delta]")
{
    // The symmetric case, which always worked — pinned so the two reflow paths cannot drift apart
    // again.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(3) }, true, LineCount(10));
    grid.setLineText(LineOffset(0), "ABC");
    grid.setLineText(LineOffset(1), "DEF");
    grid.lineAt(LineOffset(0)).setWrappable(true);
    grid.lineAt(LineOffset(1)).setWrappable(true);
    grid.scrollUp(LineCount(1));

    std::ignore = grid.resize(PageSize { LineCount(2), ColumnCount(6) }, CellLocation {}, WrapPending::No);

    CHECK(grid.stableRangeFloor()
          == grid.stableLineIdOf(LineOffset(0)) - unbox<int64_t>(grid.historyLineCount()));
    auto reported = 0;
    grid.forEachValidLine([&](LineOffset, Line const&) { ++reported; });
    CHECK(reported == unbox<int>(grid.historyLineCount()) + unbox<int>(grid.pageSize().lines));
}

namespace
{
/// How a resize changes the page in the stamping test below.
enum class PageChange : uint8_t
{
    Lines = 0, ///< Fewer lines: the ring rotates, and every row keeps its stable id.
    Columns,   ///< More columns: no row keeps its identity, though nothing reflows here.
    Narrower,  ///< Fewer columns than the rows' text: the reflow wraps them, and rebuilds the ring.
};

/// The page a resize in the stamping test below leaves, from a 3x5 one.
/// @param change How the resize changes it.
/// @return The new page size.
[[nodiscard]] PageSize resizedPage(PageChange change)
{
    switch (change)
    {
        case PageChange::Lines: return PageSize { LineCount(2), ColumnCount(5) };
        case PageChange::Columns: return PageSize { LineCount(3), ColumnCount(7) };
        case PageChange::Narrower: return PageSize { LineCount(3), ColumnCount(3) };
    }
    return PageSize { LineCount(3), ColumnCount(5) };
}
} // namespace

TEST_CASE("Grid.finalize.stampsTheRowsThatScrolledOutBeforeAResize", "[grid][delta]")
{
    // Rows written and scrolled into the history since the last finalize wait for its stamp. A resize in
    // between must not hide them from it, whichever way it changes the page.
    auto const change = GENERATE(PageChange::Lines, PageChange::Columns, PageChange::Narrower);
    INFO("change: " << static_cast<int>(change));
    auto grid = Grid(PageSize { LineCount(3), ColumnCount(5) }, true, LineCount(20));
    grid.finalizeRevisions();
    for (auto const i: std::views::iota(0, 6))
    {
        grid.setLineText(LineOffset(2), std::format("row{}", i));
        grid.lineAt(LineOffset(2)).setWrappable(true);
        grid.scrollUp(LineCount(1));
    }
    REQUIRE(grid.historyLineCount() == LineCount(6));

    auto const newSize = resizedPage(change);
    std::ignore = grid.resize(newSize, CellLocation { .line = LineOffset(2) }, WrapPending::No);
    if (change == PageChange::Narrower)
        REQUIRE(grid.historyLineCount() > LineCount(6)); // "rowN" wrapped onto a second row: reflowed
    grid.finalizeRevisions();

    for (auto const offset: std::views::iota(-unbox<int>(grid.historyLineCount()), unbox<int>(newSize.lines)))
    {
        INFO("row " << offset);
        auto const& line = std::as_const(grid).lineAt(LineOffset(offset));
        CHECK_FALSE(line.isDirty());
        CHECK(line.revision() != 0);
    }
}

TEST_CASE("Grid.render_rows.draws_exactly_the_listed_rows", "[grid]")
{
    // A 2-line page over 5 lines of history. An explicit row list picks rows that are NOT contiguous --
    // which is the whole reason the parameter exists: a collapsed fold leaves a gap no linear walk can
    // express.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, true, LineCount(5));
    for (auto i = 0; i < 5; ++i)
    {
        grid.scrollUp(LineCount(1));
        grid.setLineText(LineOffset(1), std::format("L{:03}", i));
    }

    // The writes above leave L000 at offset -3 and L002 at offset -1, with L001 between them: the row
    // list steps straight over it, which a linear walk could not.
    auto const rows = std::array { LineOffset(-3), LineOffset(-1) };
    auto renderer = MockGridRenderer {};
    (void) grid.render(renderer, ScrollOffset(0), HighlightSearchMatches::No, LineCount(0), rows);

    REQUIRE(renderer.renderedLines.size() == 2);
    // Two rows for a 2-line page: they fill it, so the first lands at the top.
    CHECK(renderer.renderedLines[0] == LineOffset(0));
    CHECK(renderer.renderedLines[1] == LineOffset(1));
    // ... and they are the rows that were asked for, in order, gap and all.
    REQUIRE(renderer.renderedTexts.size() == 2);
    CHECK(renderer.renderedTexts[0] == "L000");
    CHECK(renderer.renderedTexts[1] == "L002");
}

TEST_CASE("Grid.render_rows.extra_rows_land_above_the_page", "[grid]")
{
    // More rows than the page holds: the surplus are the smooth-scrolling ones and belong ABOVE it, so
    // the LAST row still lands on the bottom of the page.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, true, LineCount(5));
    for (auto i = 0; i < 5; ++i)
    {
        grid.scrollUp(LineCount(1));
        grid.setLineText(LineOffset(1), std::format("L{:03}", i));
    }

    auto const rows = std::array { LineOffset(-3), LineOffset(-2), LineOffset(-1) };
    auto renderer = MockGridRenderer {};
    (void) grid.render(renderer, ScrollOffset(0), HighlightSearchMatches::No, LineCount(0), rows);

    REQUIRE(renderer.renderedLines.size() == 3);
    CHECK(renderer.renderedLines[0] == LineOffset(-1));
    CHECK(renderer.renderedLines[1] == LineOffset(0));
    CHECK(renderer.renderedLines[2] == LineOffset(1));
}

TEST_CASE("Grid.render_rows.empty_list_is_the_linear_walk", "[grid]")
{
    // The default. An empty row list must be byte-for-byte what the grid always did, extraLines and all
    // -- every existing render path passes no rows.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, true, LineCount(5));
    for (auto i = 0; i < 5; ++i)
    {
        grid.scrollUp(LineCount(1));
        grid.setLineText(LineOffset(1), std::format("L{:03}", i));
    }

    auto withDefault = MockGridRenderer {};
    (void) grid.render(withDefault, ScrollOffset(0), HighlightSearchMatches::No, LineCount(1));

    auto withEmptyRows = MockGridRenderer {};
    (void) grid.render(withEmptyRows,
                       ScrollOffset(0),
                       HighlightSearchMatches::No,
                       LineCount(1),
                       std::span<LineOffset const> {});

    CHECK(withDefault.renderedLines == withEmptyRows.renderedLines);
    CHECK(withDefault.renderedTexts == withEmptyRows.renderedTexts);
}

TEST_CASE("Grid.resize.enforcesFiniteHistoryCapacity", "[grid][history][reflow]")
{
    auto const capacity = LineCount(GENERATE(0, 1, 100));
    auto const guaranteed = GENERATE(0, 1) == 0 ? capacity : capacity / 2;
    auto grid = Grid(PageSize { LineCount(3), ColumnCount(80) },
                     true,
                     HistoryLimits { .guaranteed = guaranteed, .capacity = capacity });
    for (auto const i: std::views::iota(0, 200))
    {
        grid.setLineText(LineOffset(2), std::string(80, static_cast<char>('A' + (i % 26))));
        grid.scrollUp(LineCount(1));
    }
    REQUIRE(grid.historyLineCount() == capacity);

    auto cursors = std::array { CellLocation { LineOffset(2), ColumnOffset(0) },
                                CellLocation { LineOffset(1), ColumnOffset(60) } };
    for (auto const [rows, columns]: { std::pair { 3, 20 }, { 3, 80 }, { 5, 10 }, { 5, 80 }, { 2, 20 } })
    {
        CAPTURE(capacity, guaranteed, rows, columns);
        // Start each resize with identical content: previously evicted text cannot be recovered.
        // The unlimited copy supplies the expected newest rows and cursor positions after reflow.
        auto reference = grid;
        reference.setHistoryLimits(HistoryLimits::plain(Infinite {}));
        auto referenceCursors = cursors;
        auto const pageSize = PageSize { LineCount(rows), ColumnCount(columns) };
        grid.resize(pageSize, cursors, std::nullopt);
        reference.resize(pageSize, referenceCursors, std::nullopt);
        REQUIRE(grid.historyLineCount() <= capacity);
        CHECK(cursors == referenceCursors);
        for (auto const row: std::views::iota(-unbox<int>(grid.historyLineCount()), rows))
        {
            auto const offset = LineOffset(row);
            CHECK(grid.lineText(offset) == reference.lineText(offset));
            CHECK(grid.lineOffsetOf(grid.stableLineIdOf(offset)) == offset);
        }
        CHECK(grid.stableRangeFloor() == grid.stableLineIdOf(grid.addressableTop()));
        auto rowsVisited = 0;
        grid.forEachValidLine([&](LineOffset, Line const&) { ++rowsVisited; });
        CHECK(rowsVisited == unbox<int>(grid.historyLineCount()) + rows);

        // Further scrolling must recycle a bounded ring, not fill slots left behind by reflow.
        for ([[maybe_unused]] auto const i: std::views::iota(0, 200))
        {
            grid.setLineText(LineOffset(rows - 1), std::string(static_cast<size_t>(columns), 'Z'));
            reference.setLineText(LineOffset(rows - 1), std::string(static_cast<size_t>(columns), 'Z'));
            grid.scrollUp(LineCount(1));
            reference.scrollUp(LineCount(1));
        }
        CHECK(grid.historyLineCount() <= capacity);
    }
}

TEST_CASE("Grid.resize.infiniteHistoryPreservesEveryReflowedRow", "[grid][history][reflow]")
{
    auto const guaranteed =
        GENERATE(0, 1) == 0 ? MaxHistoryLineCount { Infinite {} } : MaxHistoryLineCount { LineCount(2) };
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(4) },
                     true,
                     HistoryLimits { .guaranteed = guaranteed, .capacity = Infinite {} });
    grid.setLineText(LineOffset(0), "AAAA");
    grid.setLineText(LineOffset(1), "BBBB");
    grid.scrollUp(LineCount(2));
    grid.setLineText(LineOffset(0), "CCCC");
    grid.setLineText(LineOffset(1), "DDDD");
    auto cursor = CellLocation { LineOffset(1), ColumnOffset(3) };

    for (auto const columns: { 2, 4, 1, 4 })
    {
        cursor = grid.resize(PageSize { LineCount(2), ColumnCount(columns) }, cursor, WrapPending::No);
        CHECK(grid.historyLineCount() == LineCount((16 / columns) - 2));
        CHECK(grid.evictedRowCount() == 0);
        auto text = std::string {};
        grid.forEachValidLine([&](LineOffset, Line const& line) { text += line.toUtf8(); });
        CHECK(text == "AAAABBBBCCCCDDDD");
    }
}

TEST_CASE("Grid.setHistoryLimits.growingKeepsTheScrollback", "[grid][history]")
{
    // Ring capacity: 2 page + 2 history = 4 slots, all of them used.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, false, LineCount(2));
    grid.setLineText(LineOffset(0), "AAAAA");
    grid.setLineText(LineOffset(1), "BBBBB");
    grid.scrollUp(LineCount(2));
    grid.setLineText(LineOffset(0), "CCCCC");
    grid.setLineText(LineOffset(1), "DDDDD");

    REQUIRE(grid.historyLineCount() == LineCount(2));
    REQUIRE(grid.lineText(LineOffset(-2)) == "AAAAA");
    REQUIRE(grid.lineText(LineOffset(-1)) == "BBBBB");

    // Growing the limit must leave every retained row exactly where it was: the fresh slots
    // belong at the OLD end of the history, not between the newest history row and the page.
    grid.setHistoryLimits(HistoryLimits::plain(LineCount(5)));

    CHECK(grid.historyLineCount() == LineCount(2));
    CHECK(grid.lineText(LineOffset(-2)) == "AAAAA");
    CHECK(grid.lineText(LineOffset(-1)) == "BBBBB");
    CHECK(grid.lineText(LineOffset(0)) == "CCCCC");
    CHECK(grid.lineText(LineOffset(1)) == "DDDDD");
}

TEST_CASE("Grid.setHistoryLimits.shrinkingDropsTheOldestRows", "[grid][history]")
{
    // Ring capacity: 1 page + 3 history = 4 slots.
    auto grid = Grid(PageSize { LineCount(1), ColumnCount(5) }, false, LineCount(3));
    grid.setLineText(LineOffset(0), "AAAAA");
    grid.scrollUp(LineCount(1));
    grid.setLineText(LineOffset(0), "BBBBB");
    grid.scrollUp(LineCount(1));
    grid.setLineText(LineOffset(0), "CCCCC");
    grid.scrollUp(LineCount(1));
    grid.setLineText(LineOffset(0), "DDDDD");

    REQUIRE(grid.historyLineCount() == LineCount(3));
    REQUIRE(grid.lineText(LineOffset(-3)) == "AAAAA");

    // Shrinking keeps the NEWEST rows: what falls out is the oldest end of the scrollback.
    grid.setHistoryLimits(HistoryLimits::plain(LineCount(1)));

    CHECK(grid.historyLineCount() == LineCount(1));
    CHECK(grid.lineText(LineOffset(-1)) == "CCCCC");
    CHECK(grid.lineText(LineOffset(0)) == "DDDDD");
}

// {{{ block-atomic history eviction (issue #836)
namespace
{
/// Whether an appended line begins a command block, as OSC 133;A would mark it.
enum class BlockStart : uint8_t
{
    No = 0,
    Yes,
};

[[nodiscard]] Grid makeGrid(LineCount pageLines, LineCount guaranteed, LineCount capacity)
{
    return Grid(PageSize { pageLines, ColumnCount(5) },
                false,
                HistoryLimits { .guaranteed = guaranteed, .capacity = capacity });
}

/// Appends one line of text to the bottom of the page, scrolling the previous one into history.
void appendLine(Grid& grid, std::string_view text, BlockStart blockStart = BlockStart::No)
{
    grid.scrollUp(LineCount(1));
    auto const bottom = LineOffset::cast_from(unbox<int>(grid.pageSize().lines) - 1);
    grid.setLineText(bottom, text);
    grid.lineAt(bottom).setMarked(blockStart == BlockStart::Yes);
}

/// Appends a numbered line, e.g. `L7`, so a test can tell which rows survived.
void appendLine(Grid& grid, char prefix, int index, BlockStart blockStart = BlockStart::No)
{
    appendLine(grid, std::format("{}{}", prefix, index), blockStart);
}

/// A full-page reverse scroll -- what RI past the top, or CSI T, does.
void reverseScrollFullPage(Grid& grid, LineCount lines)
{
    grid.scrollDown(lines, GraphicsAttributes {}, fullPageMargin(grid.pageSize()));
}

/// The scrollback, oldest first, as one string per line.
[[nodiscard]] std::vector<std::string> historyText(Grid const& grid)
{
    // Through renderRange rather than a loop from -historyLineCount(): it starts at addressableTop(), the
    // top every walk over the rows this grid has starts from, so the answer is the rows a mirror holds.
    return grid.renderRange(
        grid.addressableTop(), LineOffset(-1), CaptureRendition::PlainText, CaptureTrailingSpaces::Keep);
}
} // namespace

TEST_CASE("Grid.historyEviction.evictsAWholeBlockRatherThanCuttingMidCommand", "[grid][history-eviction]")
{
    // Deliberately uneven block lengths, so that where a line-wise cut lands is not a coincidence of
    // the block size dividing the capacity.
    auto const blocks = std::array { 2, 5, 3, 4, 2, 6 };
    auto const writeBlocks = [&](Grid& grid) {
        for (auto const [block, length]: core::views::enumerate(blocks))
        {
            appendLine(grid, std::format("P{}", block), BlockStart::Yes);
            for (auto const i: std::views::iota(0, length))
                appendLine(grid, std::format("o{}{}", block, i));
        }
    };

    auto withHeadroom = makeGrid(LineCount(1), LineCount(6), LineCount(12));
    auto plain = Grid(PageSize { LineCount(1), ColumnCount(5) }, false, LineCount(12));
    writeBlocks(withHeadroom);
    writeBlocks(plain);

    // THE invariant the issue asks for: the oldest line still in the scrollback is a prompt, with
    // its command's output whole beneath it.
    CHECK(withHeadroom.lineAt(withHeadroom.addressableTop()).marked());

    // And the same buffer without headroom is what that is worth: it cut wherever the limit fell,
    // leaving an orphaned tail of output whose prompt is gone.
    CHECK(!plain.lineAt(plain.addressableTop()).marked());
    CHECK(historyText(plain).front().starts_with("o"));

    CHECK(withHeadroom.historyLineCount() >= LineCount(6));
    CHECK(withHeadroom.historyLineCount() <= LineCount(12));
}

TEST_CASE("Grid.historyEviction.neverRetainsFewerThanTheGuaranteedLines", "[grid][history-eviction]")
{
    auto grid = makeGrid(LineCount(1), LineCount(4), LineCount(8));

    // Every line is a block start, so the trim could snap anywhere it liked -- including all the way
    // down to the newest, which is exactly what the guarantee forbids.
    for (auto const i: std::views::iota(0, 40))
    {
        appendLine(grid, 'L', i, BlockStart::Yes);
        CHECK(grid.historyLineCount() <= LineCount(8));
        // Only once the buffer has accumulated that much: a guarantee is a floor on what eviction
        // may take, not on what has been written yet.
        if (i >= 8)
            CHECK(grid.historyLineCount() >= LineCount(4));
    }
}

TEST_CASE("Grid.historyEviction.withoutSemanticMarksBehavesExactlyAsBefore", "[grid][history-eviction]")
{
    // Same capacity, one with headroom and one without. With no block start anywhere, the trim can
    // honour no boundary and must leave the scrollback to the at-capacity path -- so both grids end
    // up holding the very same rows.
    auto withHeadroom = makeGrid(LineCount(1), LineCount(4), LineCount(8));
    auto plain = Grid(PageSize { LineCount(1), ColumnCount(5) }, false, LineCount(8));

    for (auto const i: std::views::iota(0, 30))
    {
        appendLine(withHeadroom, 'L', i);
        appendLine(plain, 'L', i);
    }

    CHECK(withHeadroom.historyLineCount() == plain.historyLineCount());
    CHECK(historyText(withHeadroom) == historyText(plain));
}

TEST_CASE("Grid.historyEviction.aUserMarkIsNotABlockStart", "[grid][history-eviction][semanticblocks]")
{
    // Vi's `mm` used to stamp LineFlag::Marked, so a bookmark dropped mid-output became a place where
    // block-atomic eviction would cut. With nothing but user marks in the scrollback, eviction must behave
    // exactly as with no marks at all.
    auto withUserMarks = makeGrid(LineCount(1), LineCount(4), LineCount(8));
    auto plain = Grid(PageSize { LineCount(1), ColumnCount(5) }, false, LineCount(8));

    for (auto const i: std::views::iota(0, 30))
    {
        appendLine(withUserMarks, 'L', i);
        withUserMarks.lineAt(LineOffset(0)).setFlag(LineFlag::UserMark, i % 3 == 0);
        appendLine(plain, 'L', i);

        // After every line, not once at the end: a trim that snapped to a bookmark refills to capacity
        // within a few lines, so a single final comparison can land where the two happen to agree.
        CHECK(withUserMarks.historyLineCount() == plain.historyLineCount());
        CHECK(historyText(withUserMarks) == historyText(plain));
    }
}

TEST_CASE("Grid.historyEviction.aBlockLargerThanTheHeadroomFallsBackToLineWiseEviction",
          "[grid][history-eviction]")
{
    auto grid = makeGrid(LineCount(1), LineCount(4), LineCount(8));

    // One prompt, then far more output than the whole buffer can hold -- `cat` on a large file.
    appendLine(grid, "P0", BlockStart::Yes);
    for (auto const i: std::views::iota(0, 40))
        appendLine(grid, 'o', i);

    // The prompt is long gone and the ceiling is what bounds the buffer: truncating this one
    // command's history is the only alternative to unbounded memory.
    CHECK(grid.historyLineCount() == LineCount(8));
}

TEST_CASE("Grid.historyEviction.raisesTheFloorAndKeepsRowIdentity", "[grid][history-eviction]")
{
    auto grid = makeGrid(LineCount(1), LineCount(2), LineCount(6));

    appendLine(grid, "P0", BlockStart::Yes);
    auto const evictedId = grid.stableLineIdOf(LineOffset(0));
    auto const generationBefore = grid.stableIdGeneration();
    for (auto const i: std::views::iota(0, 3))
        appendLine(grid, 'o', i);
    appendLine(grid, "P1", BlockStart::Yes);
    auto const survivorId = grid.stableLineIdOf(LineOffset(0));
    for (auto const i: std::views::iota(0, 4))
        appendLine(grid, 'q', i);

    // The first block is gone, the second is not, and no id was renamed on the way.
    CHECK(grid.lineOffsetOf(evictedId) == std::nullopt);
    CHECK(grid.lineOffsetOf(survivorId).has_value());
    CHECK(grid.stableIdGeneration() == generationBefore);
    CHECK(grid.stableRangeFloor() > evictedId);
}

TEST_CASE("Grid.historyEviction.aReverseScrollAtCapacityLeavesNoFalseBlockStart", "[grid][history-eviction]")
{
    auto grid = makeGrid(LineCount(2), LineCount(2), LineCount(4));

    // Fill to capacity with marked rows, then reverse-scroll: the marked rows pushed off the page's
    // bottom must not turn up in the history, where the trim would read them as block starts.
    for (auto const i: std::views::iota(0, 8))
        appendLine(grid, 'L', i, BlockStart::Yes);
    reverseScrollFullPage(grid, LineCount(2));

    for (auto const i: std::views::iota(0, 6))
        appendLine(grid, 'M', i, BlockStart::Yes);

    // Whatever it snapped to, it stayed inside the rows the grid actually has.
    CHECK(grid.historyLineCount() <= LineCount(4));
    CHECK(grid.stableRangeFloor() >= grid.stableLineIdOf(grid.addressableTop()));
}

TEST_CASE("Grid.historyEviction.theGuaranteeSurvivesAReverseScroll", "[grid][history-eviction]")
{
    auto grid = makeGrid(LineCount(2), LineCount(4), LineCount(10));

    for (auto const i: std::views::iota(0, 12))
        appendLine(grid, 'L', i, BlockStart::Yes);

    // A full-page reverse scroll (RI past the top / CSI T) moves the page's rows under a stable base
    // that stays put; the scan bound cached on that base must come through it with the guarantee kept.
    reverseScrollFullPage(grid, LineCount(2));

    for (auto const i: std::views::iota(0, 12))
    {
        appendLine(grid, 'M', i, BlockStart::Yes);
        CHECK(grid.historyLineCount() >= LineCount(4));
    }
}
// }}}

TEST_CASE("Grid.scrollUp.atCapacityKeepsTheEvictedLinesStorage", "[grid]")
{
    // At full scrollback, the line scrolled in at the bottom IS the line evicted from the top of the
    // ring, and the next write fills it at once, so its storage is kept rather than released and
    // allocated again.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, true, LineCount(1));
    grid.setLineText(LineOffset(0), "ABCDE");
    grid.setLineText(LineOffset(1), "abcde");

    SECTION("one line at a time")
    {
        grid.scrollUp(LineCount(1)); // "ABCDE" fills the only history slot: the ring is full now
        grid.setLineText(LineOffset(1), "12345");
        REQUIRE(grid.historyLineCount() == LineCount(1));
        auto const* const evicted = std::as_const(grid).lineAt(LineOffset(-1)).storage().codepoints.data();

        grid.scrollUp(LineCount(1));

        auto const& scrolledIn = std::as_const(grid).lineAt(LineOffset(1));
        CHECK(scrolledIn.isBlank());
        CHECK(grid.lineText(LineOffset(1)) == "     ");
        CHECK(grid.lineText(LineOffset(-1)) == "abcde");
        CHECK(scrolledIn.storage().codepoints.data() == evicted);
    }

    SECTION("more lines than the ring has free")
    {
        // One free slot, two lines to scroll: the first is appended, the second evicts "ABCDE".
        grid.scrollUp(LineCount(2));

        REQUIRE(grid.historyLineCount() == LineCount(1));
        CHECK(grid.lineText(LineOffset(-1)) == "abcde");
        CHECK(grid.lineText(LineOffset(0)) == "     "); // the appended line
        CHECK(grid.lineText(LineOffset(1)) == "     "); // the line "ABCDE" was evicted from
        auto const keptStorage = std::ranges::count_if(std::views::iota(0, 2), [&](int lineNo) {
            return std::as_const(grid).lineAt(LineOffset(lineNo)).storage().codepoints.capacity() >= 5;
        });
        CHECK(keptStorage == 1); // the evicted line; the appended one never had storage
    }
}

// {{{ evicted row count (the gutter's absolute line numbers)
namespace
{
/// How many rows the grid can still address: the history above the floor plus the page.
[[nodiscard]] uint64_t addressableRowCount(Grid const& grid)
{
    return static_cast<uint64_t>(unbox<int64_t>(boxed_cast<LineOffset>(grid.pageSize().lines))
                                 - unbox<int64_t>(grid.addressableTop()));
}
} // namespace

TEST_CASE("Grid.evictedRowCount.lineWiseEvictionAtCapacity", "[grid][evicted]")
{
    // Ring capacity: 2 page + 1 history = 3 slots.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, false, LineCount(1));
    CHECK(grid.evictedRowCount() == 0);

    grid.scrollUp(LineCount(1)); // fills the one history slot
    CHECK(grid.evictedRowCount() == 0);

    grid.scrollUp(LineCount(1)); // at capacity: the oldest row goes
    CHECK(grid.evictedRowCount() == 1);

    grid.scrollUp(LineCount(3)); // three at once, all past capacity
    CHECK(grid.evictedRowCount() == 4);
}

TEST_CASE("Grid.evictedRowCount.aScrollThatFillsAndOverflowsCountsOnlyTheOverflow", "[grid][evicted]")
{
    // One free history slot and a three-line scroll: one row is kept, two are dropped.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, false, LineCount(1));
    grid.scrollUp(LineCount(3));
    CHECK(grid.historyLineCount() == LineCount(1));
    CHECK(grid.evictedRowCount() == 2);
}

TEST_CASE("Grid.evictedRowCount.anInfiniteScrollbackNeverEvicts", "[grid][evicted]")
{
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, false, Infinite {});
    for ([[maybe_unused]] auto const _: std::views::iota(0, 100))
        grid.scrollUp(LineCount(1));
    CHECK(grid.evictedRowCount() == 0);
}

TEST_CASE("Grid.evictedRowCount.aBlockAtomicTrimCountsEveryRowItDrops", "[grid][evicted]")
{
    // Headroom, and a prompt every third line: the trim snaps to a block start and drops whole blocks.
    auto grid = makeGrid(LineCount(1), LineCount(2), LineCount(6));
    auto appended = uint64_t { 0 };
    for (auto const i: std::views::iota(0, 30))
    {
        appendLine(grid, 'L', i, i % 3 == 0 ? BlockStart::Yes : BlockStart::No);
        ++appended;
        // Every row the grid ever held -- the page row it started with and each appended line -- is
        // either still addressable or counted as evicted. Absolute line numbers rest on this identity.
        CHECK(addressableRowCount(grid) + grid.evictedRowCount() == 1 + appended);
    }
    CHECK(grid.evictedRowCount() > 0);
}

TEST_CASE("Grid.evictedRowCount.clearingTheScrollbackCountsIt", "[grid][evicted]")
{
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, false, LineCount(5));
    grid.scrollUp(LineCount(3));
    REQUIRE(grid.historyLineCount() == LineCount(3));

    grid.clearHistory(); // ED 3
    CHECK(grid.evictedRowCount() == 3);
}

TEST_CASE("Grid.evictedRowCount.shrinkingTheHistoryLimitCountsWhatFellOut", "[grid][evicted]")
{
    auto grid = Grid(PageSize { LineCount(1), ColumnCount(5) }, false, LineCount(3));
    grid.scrollUp(LineCount(3));
    REQUIRE(grid.historyLineCount() == LineCount(3));

    grid.setHistoryLimits(HistoryLimits::plain(LineCount(1)));
    CHECK(grid.historyLineCount() == LineCount(1));
    CHECK(grid.evictedRowCount() == 2);
}

TEST_CASE("Grid.evictedRowCount.aHeightChangeKeepsEveryRowAccountedFor", "[grid][evicted]")
{
    auto grid = Grid(PageSize { LineCount(4), ColumnCount(5) }, false, LineCount(2));
    grid.scrollUp(LineCount(2));
    auto const rowsEver = uint64_t { 6 }; // the four page rows it started with, and two scrolled in
    REQUIRE(addressableRowCount(grid) + grid.evictedRowCount() == rowsEver);

    // Shrinking with the cursor on the bottom row pushes the top of the page into the scrollback.
    (void) grid.resize(PageSize { LineCount(2), ColumnCount(5) },
                       CellLocation { .line = LineOffset(3), .column = ColumnOffset(0) },
                       WrapPending::No);
    CHECK(addressableRowCount(grid) + grid.evictedRowCount() == rowsEver);

    // Growing with the cursor at the top appends two blank rows below it.
    (void) grid.resize(PageSize { LineCount(4), ColumnCount(5) },
                       CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) },
                       WrapPending::No);
    CHECK(addressableRowCount(grid) + grid.evictedRowCount() == rowsEver + 2);
}

namespace
{
/// Resizes @p grid to @p lines rows with the cursor on its bottom row, which is how a status line
/// appearing or going away resizes the primary screen: a shrink pushes the page's top into the history,
/// and a grow pulls the history back onto it.
void resizeLinesWithCursorAtBottom(Grid& grid, LineCount lines)
{
    auto const bottom = boxed_cast<LineOffset>(grid.pageSize().lines) - 1;
    (void) grid.resize(PageSize { lines, grid.pageSize().columns },
                       CellLocation { .line = bottom, .column = ColumnOffset(0) },
                       WrapPending::No);
}
} // namespace

TEST_CASE("Grid.evictedRowCount.aShrinkBelowCapacityKeepsEveryHistoryRowAddressable", "[grid][evicted]")
{
    auto grid = makeGrid(LineCount(4), LineCount(10), LineCount(10));
    for (auto const i: std::views::iota(0, 4))
        grid.setLineText(LineOffset(i), std::format("P{}", i));
    for (auto const i: std::views::iota(0, 2))
        appendLine(grid, 'L', i);
    REQUIRE(grid.historyLineCount() == LineCount(2));

    resizeLinesWithCursorAtBottom(grid, LineCount(3));

    // The page's top row went into the history, which had room for it: nothing left the grid.
    CHECK(grid.historyLineCount() == LineCount(3));
    CHECK(grid.addressableTop() == -boxed_cast<LineOffset>(grid.historyLineCount()));
    CHECK(grid.evictedRowCount() == 0);
    CHECK(historyText(grid) == std::vector<std::string> { "P0   ", "P1   ", "P2   " });
}

TEST_CASE("Grid.evictedRowCount.aShrinkAndGrowAtCapacityLosesNoRow", "[grid][evicted]")
{
    // A status line toggled on and off with the scrollback full: the ring holds the rows the shorter page
    // pushes up, so the round trip loses none of them and counts none.
    auto grid = makeGrid(LineCount(4), LineCount(2), LineCount(2));
    for (auto const i: std::views::iota(0, 8))
        appendLine(grid, 'L', i);
    REQUIRE(grid.historyLineCount() == LineCount(2));
    auto const evictedBefore = grid.evictedRowCount();
    auto const historyBefore = historyText(grid);

    resizeLinesWithCursorAtBottom(grid, LineCount(2));
    CHECK(grid.addressableTop() == -boxed_cast<LineOffset>(grid.historyLineCount()));
    CHECK(grid.evictedRowCount() == evictedBefore);

    resizeLinesWithCursorAtBottom(grid, LineCount(4));
    CHECK(grid.addressableTop() == -boxed_cast<LineOffset>(grid.historyLineCount()));
    CHECK(grid.evictedRowCount() == evictedBefore);
    CHECK(historyText(grid) == historyBefore);
    CHECK(grid.lineTextTrimmed(LineOffset(0)) == "L4");
}

TEST_CASE("Grid.evictedRowCount.aShrinkAndGrowWithLessHistoryThanTheShrinkKeepsRowIdentity",
          "[grid][evicted][stable-id]")
{
    // A fresh session whose output has just filled the page: the shrink pushes more rows up than the
    // history held, and the grow brings them back under the ids they had.
    auto grid = makeGrid(LineCount(4), LineCount(10), LineCount(10));
    for (auto const i: std::views::iota(0, 4))
        grid.setLineText(LineOffset(i), std::format("P{}", i));
    auto const generationBefore = grid.stableIdGeneration();
    auto const topRowId = grid.stableLineIdOf(LineOffset(0));

    resizeLinesWithCursorAtBottom(grid, LineCount(2));
    CHECK(grid.addressableTop() == -boxed_cast<LineOffset>(grid.historyLineCount()));

    resizeLinesWithCursorAtBottom(grid, LineCount(4));
    CHECK(grid.stableIdGeneration() == generationBefore);
    CHECK(grid.evictedRowCount() == 0);
    CHECK(grid.lineOffsetOf(topRowId) == LineOffset(0));
    CHECK(grid.lineTextTrimmed(LineOffset(0)) == "P0");
}

TEST_CASE("Grid.evictedRowCount.aReflowCountsRowsBeyondCapacity", "[grid][evicted]")
{
    // Ring capacity: 2 page + 2 history. Four full-width lines, so the two blank rows the page started
    // with are what filling it evicts.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(4) }, true, LineCount(2));
    for (auto const text: { "AAAA"sv, "BBBB"sv, "CCCC"sv, "DDDD"sv })
        appendLine(grid, text);
    REQUIRE(grid.evictedRowCount() == 2);

    // Narrowing re-chops every line in two: eight rows must fit two page plus two history rows.
    // The four oldest physical rows are evicted; rekeying the survivors is not itself an eviction.
    (void) grid.resize(PageSize { LineCount(2), ColumnCount(2) },
                       CellLocation { .line = LineOffset(1), .column = ColumnOffset(1) },
                       WrapPending::No);
    CHECK(grid.evictedRowCount() == 6);
    CHECK(grid.historyLineCount() == LineCount(2));

    // ...and the next scroll counts exactly the row it pushes out.
    grid.scrollUp(LineCount(1));
    CHECK(grid.evictedRowCount() == 7);

    // Widening rejoins them, which renames rows again and drops none.
    (void) grid.resize(PageSize { LineCount(2), ColumnCount(4) },
                       CellLocation { .line = LineOffset(1), .column = ColumnOffset(1) },
                       WrapPending::No);
    CHECK(grid.evictedRowCount() == 7);
}

TEST_CASE("Grid.evictedRowCount.aReverseScrollEvictsNothing", "[grid][evicted]")
{
    // At capacity, a full-page reverse scroll leaves every history row where it is, so it counts nothing,
    // and the next line scrolled into the full ring evicts the oldest of them, as it would have anyway.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, false, LineCount(2));
    for ([[maybe_unused]] auto const _: std::views::iota(0, 4))
        grid.scrollUp(LineCount(1));
    REQUIRE(grid.evictedRowCount() == 2);

    reverseScrollFullPage(grid, LineCount(1));
    CHECK(grid.evictedRowCount() == 2);

    grid.scrollUp(LineCount(1));
    CHECK(grid.evictedRowCount() == 3);
}

TEST_CASE("Grid.evictedRowCount.aResetStartsOver", "[grid][evicted]")
{
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, false, LineCount(1));
    grid.scrollUp(LineCount(5));
    REQUIRE(grid.evictedRowCount() > 0);

    grid.reset(); // RIS
    CHECK(grid.evictedRowCount() == 0);
}
// }}}

// {{{ full-page reverse scroll
namespace
{
/// The text of every row from @p from to the page's bottom row, each without its trailing blanks.
///
/// Deliberately not clamped to addressableTop(): a row the ring wraps into the history must show up here.
/// @param grid The grid to read.
/// @param from The topmost row to read; negative reaches into the history.
/// @return One string per row, top first.
[[nodiscard]] std::vector<std::string> rowsFrom(Grid const& grid, LineOffset from)
{
    return std::ranges::to<std::vector>(
        std::views::iota(unbox<int>(from), unbox<int>(grid.pageSize().lines))
        | std::views::transform([&](int row) { return grid.lineTextTrimmed(LineOffset(row)); }));
}

/// The history, oldest row first, read from -historyLineCount() rather than from addressableTop().
/// @param grid The grid to read.
/// @return One string per history row.
[[nodiscard]] std::vector<std::string> wholeHistoryText(Grid const& grid)
{
    auto rows = rowsFrom(grid, -boxed_cast<LineOffset>(grid.historyLineCount()));
    rows.resize(unbox<size_t>(grid.historyLineCount()));
    return rows;
}

/// How full a ring of five history rows is before a full-page reverse scroll.
struct HistoryFill
{
    std::string_view name; ///< The case, for the test's report.
    int appended = 0;      ///< Lines appended below the page's three numbered rows.
};

constexpr auto HistoryFills = std::array {
    HistoryFill { .name = "the ring has room", .appended = 3 },
    HistoryFill { .name = "the ring is full", .appended = 9 },
};
} // namespace

TEST_CASE("Grid.scrollDown.aFullPageReverseScrollLeavesTheHistoryAlone", "[grid][stable-id]")
{
    // SD, and RI at the top row, move the page's rows down and drop those pushed off its bottom. The rows
    // above the page are not the scrolling region's: none of them is blanked, moved or renamed, whether the
    // ring has room or every slot is in use.
    for (auto const& fill: HistoryFills)
    {
        for (auto const count: std::array { 1, 2, 3 })
        {
            INFO(std::format("{}, {} line(s)", fill.name, count));
            auto grid = Grid(PageSize { LineCount(3), ColumnCount(5) }, false, LineCount(5));
            for (auto const row: std::views::iota(0, 3))
                grid.setLineText(LineOffset(row), std::format("I{}", row));
            for (auto const i: std::views::iota(0, fill.appended))
                appendLine(grid, 'L', i);
            REQUIRE(grid.historyLineCount() == LineCount(std::min(fill.appended, 5)));

            auto const historyBefore = wholeHistoryText(grid);
            auto const pageBefore = rowsFrom(grid, LineOffset(0));
            auto const historyLinesBefore = grid.historyLineCount();
            auto const evictedBefore = grid.evictedRowCount();
            auto const topBefore = grid.addressableTop();
            auto const baseBefore = grid.stableLineIdOf(LineOffset(0));
            auto const floorBefore = grid.stableRangeFloor();
            auto const move = grid.rowMoveOfScrollDown(LineCount(count), fullPageMargin(grid.pageSize()));

            reverseScrollFullPage(grid, LineCount(count));

            // The history: every row where it was, under the id it had.
            CHECK(wholeHistoryText(grid) == historyBefore);
            CHECK(grid.historyLineCount() == historyLinesBefore);
            CHECK(grid.evictedRowCount() == evictedBefore);
            CHECK(grid.addressableTop() == topBefore);
            CHECK(grid.stableLineIdOf(LineOffset(0)) == baseBefore);
            CHECK(grid.stableRangeFloor() == floorBefore);
            REQUIRE(move.has_value());
            for (auto const row: std::views::iota(-unbox<int>(historyLinesBefore), 0))
                CHECK(move->destinationOf(baseBefore + row) == baseBefore + row);

            // The page: shifted down, its top rows blank.
            auto pageExpected = std::vector<std::string>(static_cast<size_t>(count));
            std::ranges::copy(pageBefore | std::views::take(3 - count), std::back_inserter(pageExpected));
            CHECK(rowsFrom(grid, LineOffset(0)) == pageExpected);

            // The rows pushed off the page's bottom are gone: nowhere in the history, nowhere on the page.
            auto const everyRow = rowsFrom(grid, -boxed_cast<LineOffset>(grid.historyLineCount()));
            for (auto const& pushedOff: pageBefore | std::views::drop(3 - count))
                CHECK_FALSE(std::ranges::contains(everyRow, pushedOff));
        }
    }
}
// }}}

// {{{ dropRowsAbove (ClearToPrompt)
namespace
{
/// Page of 3 rows, room for 5 in the history: A and B scrolled into it, C, D, E on the page.
[[nodiscard]] Grid fiveRowsOnAThreeRowPage()
{
    return setupGrid(PageSize { LineCount(3), ColumnCount(5) },
                     false,
                     LineCount(5),
                     { "AAAAA", "BBBBB", "CCCCC", "DDDDD", "EEEEE" });
}
} // namespace

TEST_CASE("Grid.dropRowsAbove.aPageHeadMovesToTheTopAndTheHistoryGoes", "[grid][semanticblocks]")
{
    auto grid = fiveRowsOnAThreeRowPage();
    REQUIRE(grid.historyLineCount() == LineCount(2));
    auto const evictedBefore = grid.evictedRowCount();
    auto const idOfD = grid.stableLineIdOf(LineOffset(1));

    CHECK(grid.dropRowsAbove(LineOffset(1)) == LineCount(1));

    CHECK(grid.historyLineCount() == LineCount(0));
    CHECK(grid.lineText(LineOffset(0)) == "DDDDD");
    CHECK(grid.lineText(LineOffset(1)) == "EEEEE");
    CHECK(grid.lineTextTrimmed(LineOffset(2)).empty());
    CHECK(grid.evictedRowCount() == evictedBefore + 3); // A, B and C
    CHECK(grid.lineOffsetOf(idOfD) == LineOffset(0));   // the kept rows keep their identity
    CHECK(grid.stableRangeFloor() == idOfD);
}

TEST_CASE("Grid.dropRowsAbove.aHistoryAtCapacityCountsEveryRowOnce", "[grid][semanticblocks]")
{
    // The history is full, so the scroll that lifts the head to the top evicts its oldest row on the way.
    auto grid = setupGrid(PageSize { LineCount(3), ColumnCount(5) },
                          false,
                          LineCount(2),
                          { "AAAAA", "BBBBB", "CCCCC", "DDDDD", "EEEEE" });
    REQUIRE(grid.historyLineCount() == LineCount(2));
    auto const evictedBefore = grid.evictedRowCount();

    CHECK(grid.dropRowsAbove(LineOffset(2)) == LineCount(2));

    CHECK(grid.historyLineCount() == LineCount(0));
    CHECK(grid.lineText(LineOffset(0)) == "EEEEE");
    CHECK(grid.evictedRowCount() == evictedBefore + 4); // A, B, C and D
}

TEST_CASE("Grid.dropRowsAbove.aHistoryHeadDropsOnlyTheOlderRows", "[grid][semanticblocks]")
{
    auto grid = fiveRowsOnAThreeRowPage();
    auto const evictedBefore = grid.evictedRowCount();

    CHECK(grid.dropRowsAbove(LineOffset(-1)) == LineCount(0));

    CHECK(grid.historyLineCount() == LineCount(1));
    CHECK(grid.addressableTop() == LineOffset(-1));
    CHECK(grid.lineText(LineOffset(-1)) == "BBBBB");
    CHECK(grid.lineText(LineOffset(0)) == "CCCCC");
    CHECK(grid.evictedRowCount() == evictedBefore + 1); // A
}

TEST_CASE("Grid.dropRowsAbove.theTopmostRowDropsNothing", "[grid][semanticblocks]")
{
    auto grid = fiveRowsOnAThreeRowPage();
    auto const evictedBefore = grid.evictedRowCount();

    CHECK(grid.dropRowsAbove(grid.addressableTop()) == LineCount(0));

    CHECK(grid.historyLineCount() == LineCount(2));
    CHECK(grid.evictedRowCount() == evictedBefore);
}

TEST_CASE("Grid.dropRowsAbove.thePageTopDropsTheHistoryOnly", "[grid][semanticblocks]")
{
    auto grid = fiveRowsOnAThreeRowPage();
    auto const evictedBefore = grid.evictedRowCount();

    CHECK(grid.dropRowsAbove(LineOffset(0)) == LineCount(0));

    CHECK(grid.historyLineCount() == LineCount(0));
    CHECK(grid.lineText(LineOffset(0)) == "CCCCC");
    CHECK(grid.evictedRowCount() == evictedBefore + 2);
}
// }}}

TEST_CASE("Grid.historyEviction.theFirstTrimSeesTheBoundariesAlreadyInTheWindow", "[grid][history-eviction]")
{
    // Guaranteed two, capacity eight. The scrollback reaches the capacity before any trim has run, so the
    // first one finds both prompts already inside the window it may cut from. Cutting line-wise instead
    // would leave `x` -- no prompt -- at the top.
    auto grid = makeGrid(LineCount(1), LineCount(2), LineCount(8));
    appendLine(grid, "x"sv);
    appendLine(grid, "P0"sv, BlockStart::Yes);
    appendLine(grid, "o0"sv);
    appendLine(grid, "o0"sv);
    appendLine(grid, "P1"sv, BlockStart::Yes);
    for ([[maybe_unused]] auto const _: std::views::iota(0, 3))
        appendLine(grid, "o1"sv);
    REQUIRE(grid.historyLineCount() == LineCount(8)); // the page's blank row, x, P0 o0 o0, P1 o1 o1

    appendLine(grid, "o1"sv);
    CHECK(grid.lineAt(grid.addressableTop()).marked());
    CHECK(historyText(grid).front().starts_with("P1"));
}
