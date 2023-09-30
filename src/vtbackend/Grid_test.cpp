// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Grid.h>
#include <vtbackend/cell/CellConfig.h>
#include <vtbackend/primitives.h>

#include <fmt/format.h>

#include <catch2/catch.hpp>

#include <iostream>

#include "crispy/BufferObject.h"

using namespace vtbackend;
using namespace std::string_literals;
using namespace std::string_view_literals;
using std::string;
using std::string_view;

// Default cell type for testing.
using Cell = PrimaryScreenCell;

namespace
{
void logGridText(Grid<Cell> const& grid, string const& headline = "")
{
    UNSCOPED_INFO(fmt::format("Grid.dump(hist {}, max hist {}, size {}, ZI {}): {}",
                              grid.historyLineCount(),
                              grid.maxHistoryLineCount(),
                              grid.pageSize(),
                              grid.zero_index(),
                              headline));

    for (int line = -grid.historyLineCount().as<int>(); line < grid.pageSize().lines.as<int>(); ++line)
    {
        UNSCOPED_INFO(
            fmt::format("{:>2}: \"{}\" {}\n",
                        line,
                        grid.lineText(LineOffset::cast_from(line - grid.historyLineCount().as<int>())),
                        (unsigned) grid.lineAt(LineOffset::cast_from(line)).flags()));
    }
}

[[maybe_unused]] void logGridTextAlways(Grid<Cell> const& grid, string const& headline = "")
{
    fmt::print("Grid.dump(hist {}, max hist {}, size {}, ZI {}): {}\n",
               grid.historyLineCount(),
               grid.maxHistoryLineCount(),
               grid.pageSize(),
               grid.zero_index(),
               headline);
    fmt::print("{}\n", dumpGrid(grid));
}

Grid<Cell> setupGrid(PageSize pageSize,
                     bool reflowOnResize,
                     LineCount maxHistoryLineCount,
                     std::initializer_list<std::string_view> init)
{
    auto grid = Grid<Cell>(pageSize, reflowOnResize, maxHistoryLineCount);

    int cursor = 0;
    for (string_view line: init)
    {
        if (cursor == *pageSize.lines)
            grid.scrollUp(LineCount(1));
        else
            ++cursor;

        grid.setLineText(LineOffset::cast_from(cursor - 1), line);

        logGridText(grid,
                    fmt::format("setup grid at {}x{}x{}: line {}",
                                pageSize.columns,
                                pageSize.lines,
                                maxHistoryLineCount,
                                cursor - 1));
    }

    logGridText(grid,
                fmt::format("setup grid at {}x{}x{}",
                            grid.pageSize().columns,
                            grid.pageSize().lines,
                            grid.maxHistoryLineCount()));
    return grid;
}

constexpr Margin fullPageMargin(PageSize pageSize)
{
    return Margin { Margin::Vertical { LineOffset(0), pageSize.lines.as<LineOffset>() - 1 },
                    Margin::Horizontal { ColumnOffset(0), pageSize.columns.as<ColumnOffset>() - 1 } };
}

[[maybe_unused]] Grid<Cell> setupGrid5x2()
{
    auto grid = Grid<Cell>(PageSize { LineCount(2), ColumnCount(5) }, true, LineCount(10));
    grid.setLineText(LineOffset { 0 }, "ABCDE");
    grid.setLineText(LineOffset { 1 }, "abcde");
    logGridText(grid, "setup grid at 5x2");
    return grid;
}

[[maybe_unused]] Grid<Cell> setupGrid5x2x2()
{
    auto grid = Grid<Cell>(PageSize { LineCount(2), ColumnCount(5) }, true, LineCount(2));
    grid.scrollUp(LineCount(2));
    grid.setLineText(LineOffset { -1 }, "ABCDE");
    grid.setLineText(LineOffset { 0 }, "FGHIJ");
    grid.setLineText(LineOffset { 1 }, "KLMNO");
    grid.setLineText(LineOffset { 2 }, "PQRST");
    logGridText(grid, "setup grid at 5x2x2");
    return grid;
}

[[maybe_unused]] Grid<Cell> setupGrid8x2()
{
    auto grid = Grid<Cell>(PageSize { LineCount(2), ColumnCount(8) }, true, LineCount(10));
    grid.setLineText(LineOffset { 0 }, "ABCDEFGH");
    grid.setLineText(LineOffset { 1 }, "abcdefgh");
    logGridText(grid, "setup grid at 5x2");
    return grid;
}

Grid<Cell> setupGridForResizeTests2x3xN(LineCount maxHistoryLineCount)
{
    auto constexpr ReflowOnResize = true;
    auto constexpr PageSize = vtbackend::PageSize { LineCount(2), ColumnCount(3) };

    return setupGrid(PageSize, ReflowOnResize, maxHistoryLineCount, { "ABC", "DEF", "GHI", "JKL" });
}

Grid<Cell> setupGridForResizeTests2x3a3()
{
    return setupGridForResizeTests2x3xN(LineCount(3));
}

} // namespace

TEST_CASE("Grid.setup", "[grid]")
{
    auto grid = Grid<Cell>(PageSize { LineCount(2), ColumnCount(5) }, true, LineCount(0));
    grid.setLineText(LineOffset { 0 }, "ABCDE"sv);
    grid.setLineText(LineOffset { 1 }, "abcde"sv);
    logGridText(grid, "setup grid at 5x2");

    CHECK(grid.lineText(LineOffset { 0 }) == "ABCDE"sv);
    CHECK(grid.lineText(LineOffset { 1 }) == "abcde"sv);
}

TEST_CASE("Grid.writeAndScrollUp", "[grid]")
{
    auto grid = Grid<Cell>(PageSize { LineCount(2), ColumnCount(5) }, true, LineCount(3));
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
    auto grid = Grid<Cell>(PageSize { LineCount(3), ColumnCount(3) }, true, LineCount(0));
    grid.setLineText(LineOffset { 0 }, "ABC"sv);
    grid.setLineText(LineOffset { 1 }, "DEF"sv);
    grid.setLineText(LineOffset { 2 }, "GHI"sv);
    logGridText(grid);

    auto* const a00 = &grid.at(LineOffset(0), ColumnOffset(0));
    CHECK(a00->toUtf8() == "A");
    auto* const a01 = &grid.at(LineOffset(0), ColumnOffset(1));
    CHECK(a01->toUtf8() == "B");
    auto* const a02 = &grid.at(LineOffset(0), ColumnOffset(2));
    CHECK(a02->toUtf8() == "C");

    auto* const a11 = &grid.at(LineOffset(1), ColumnOffset(1));
    CHECK(a11->toUtf8() == "E");
    auto* const a22 = &grid.at(LineOffset(2), ColumnOffset(2));
    CHECK(a22->toUtf8() == "I");
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

    LogicalLines logicalLines = grid.logicalLines();
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

    auto const curCursorPos = CellLocation { grid.pageSize().lines.as<LineOffset>() - 1, ColumnOffset(1) };
    auto const newPageSize = PageSize { LineCount(4), ColumnCount(3) };
    auto const newCursorPos0 = CellLocation { curCursorPos.line + 2, curCursorPos.column };
    CellLocation newCursorPos = grid.resize(newPageSize, curCursorPos, false);
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

    auto const curCursorPos = CellLocation { LineOffset(1), ColumnOffset(1) };
    auto const newPageSize = PageSize { LineCount(5), ColumnCount(3) };
    logGridText(grid, "BEFORE");
    CellLocation newCursorPos = grid.resize(newPageSize, curCursorPos, false);
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

    auto const curCursorPos = CellLocation { LineOffset(0), ColumnOffset(1) };
    logGridText(grid, "before resize");
    CellLocation newCursorPos = grid.resize(PageSize { LineCount(3), ColumnCount(3) }, curCursorPos, false);
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

TEST_CASE("resize_shrink_lines_with_history", "[grid]")
{
    auto grid = Grid<Cell>(PageSize { LineCount(2), ColumnCount(3) }, true, LineCount(5));
    auto const gridMargin = fullPageMargin(grid.pageSize());
    grid.scrollUp(LineCount { 1 }, GraphicsAttributes {}, gridMargin);
    grid.setLineText(LineOffset(-1), "ABC");        // history line
    grid.setLineText(LineOffset(0), "DEF");         // main page: line 1
    grid.setLineText(LineOffset(1), "GHI");         // main page: line 2
    CHECK(grid.historyLineCount() == LineCount(1)); // TODO: move line up, below scrollUp()

    // shrink by one line (=> move page one line up into scrollback)
    auto const newPageSize = PageSize { LineCount(1), ColumnCount(3) };
    auto const curCursorPos = CellLocation { LineOffset(1), ColumnOffset(1) };
    logGridText(grid, "BEFORE");
    CellLocation newCursorPos = grid.resize(newPageSize, curCursorPos, false);
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
    auto const curCursorPos = CellLocation { LineOffset(1), ColumnOffset(1) };
    grid.lineAt(LineOffset(0)).setWrappable(false);
    logGridText(grid, "BEFORE");
    auto const newCursorPos = grid.resize(newPageSize, curCursorPos, false);
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

    CHECK(grid.lineAt(LineOffset(-5)).flags() == LineFlags::Wrappable);
    CHECK(grid.lineAt(LineOffset(-4)).flags() == (LineFlags::Wrappable | LineFlags::Wrapped));
    CHECK(grid.lineAt(LineOffset(-3)).flags() == LineFlags::Wrappable);
    CHECK(grid.lineAt(LineOffset(-2)).flags() == (LineFlags::Wrappable | LineFlags::Wrapped));
    CHECK(grid.lineAt(LineOffset(-1)).flags() == LineFlags::None);
    CHECK(grid.lineAt(LineOffset(0)).flags() == LineFlags::Wrappable);
    CHECK(grid.lineAt(LineOffset(1)).flags() == (LineFlags::Wrappable | LineFlags::Wrapped));
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
    auto const curCursorPos = CellLocation { LineOffset(1), ColumnOffset(1) };
    grid.lineAt(LineOffset(0)).setWrappable(false);
    // logGridText(grid, "BEFORE");
    auto const newCursorPos = grid.resize(PageSize { LineCount(4), ColumnCount(2) }, curCursorPos, false);
    (void) newCursorPos;
    // logGridText(grid, "AFTER");

    CHECK(grid.lineText(LineOffset(-3)) == "AB");
    CHECK(grid.lineText(LineOffset(-2)) == "C ");
    CHECK(grid.lineText(LineOffset(-1)) == "DE");
    CHECK(grid.lineText(LineOffset(0)) == "F ");
    CHECK(grid.lineText(LineOffset(1)) == "GH");
    CHECK(grid.lineText(LineOffset(2)) == "JK");
    CHECK(grid.lineText(LineOffset(3)) == "L ");

    CHECK(grid.lineAt(LineOffset(-3)).flags() == LineFlags::Wrappable);
    CHECK(grid.lineAt(LineOffset(-2)).flags() == (LineFlags::Wrappable | LineFlags::Wrapped));
    CHECK(grid.lineAt(LineOffset(-1)).flags() == LineFlags::Wrappable);
    CHECK(grid.lineAt(LineOffset(0)).flags() == (LineFlags::Wrappable | LineFlags::Wrapped));
    CHECK(grid.lineAt(LineOffset(1)).flags() == LineFlags::None);
    CHECK(grid.lineAt(LineOffset(2)).flags() == LineFlags::Wrappable);
    CHECK(grid.lineAt(LineOffset(3)).flags() == (LineFlags::Wrappable | LineFlags::Wrapped));
}
// }}}

// {{{ grid reflow
TEST_CASE("resize_reflow_shrink", "[grid]")
{
    auto grid = setupGrid5x2();
    logGridText(grid, "init");

    // Shrink slowly from 5x2 to 4x2 to 3x2 to 2x2.

    // 4x2
    (void) grid.resize(PageSize { LineCount(2), ColumnCount(4) }, CellLocation { {}, {} }, false);
    logGridText(grid, "after resize 4x2");

    CHECK(*grid.historyLineCount() == 2);
    CHECK(grid.lineText(LineOffset(-2)) == "ABCD");
    CHECK(grid.lineText(LineOffset(-1)) == "E   ");

    CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(4) });
    CHECK(grid.lineText(LineOffset(0)) == "abcd");
    CHECK(grid.lineText(LineOffset(1)) == "e   ");

    // fmt::print("Starting logicalLines test\n");
    auto ll = grid.logicalLines();
    auto li = ll.begin();
    auto le = ll.end();
    CHECK(li->text() == "ABCDE   ");
    ++li;
    CHECK(li->text() == "abcde   ");
    ++li;
    CHECK(li == le);

    // 3x2
    fmt::print("Starting resize to 3x2\n");
    (void) grid.resize(PageSize { LineCount(2), ColumnCount(3) }, CellLocation { {}, {} }, false);
    logGridText(grid, "after resize 3x2");

    CHECK(*grid.historyLineCount() == 2);
    CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(3) });
    CHECK(grid.lineText(LineOffset(-2)) == "ABC");
    CHECK(grid.lineText(LineOffset(-1)) == "DE ");
    CHECK(grid.lineText(LineOffset(0)) == "abc");
    CHECK(grid.lineText(LineOffset(1)) == "de ");

    // 2x2
    (void) grid.resize(PageSize { LineCount(2), ColumnCount(2) }, CellLocation { {}, {} }, false);
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
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(4) }, CellLocation { {}, {} }, false);
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
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(4) }, CellLocation {}, false);
        logGridText(grid, "after resize 4x2");
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(3) }, CellLocation {}, false);
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
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(4) }, CellLocation {}, false);
        logGridText(grid, "after resize 4x2");
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(3) }, CellLocation {}, false);
        logGridText(grid, "after resize 3x2");
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(2) }, CellLocation {}, false);
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
            (void) grid.resize(PageSize { LineCount(2), ColumnCount(3) }, CellLocation {}, false);
            logGridText(grid, "after regrow to 3x2");

            CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(3) });
            CHECK(grid.historyLineCount() == LineCount(2));
            CHECK(grid.lineText(LineOffset(-2)) == "ABC");
            CHECK(grid.lineText(LineOffset(-1)) == "DE ");
            CHECK(grid.lineText(LineOffset(0)) == "abc");
            CHECK(grid.lineText(LineOffset(1)) == "de ");

            SECTION("regrow 4x2")
            {
                (void) grid.resize(PageSize { LineCount(2), ColumnCount(4) }, CellLocation {}, false);
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
                (void) grid.resize(PageSize { LineCount(2), ColumnCount(5) }, CellLocation {}, false);
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

    (void) grid.resize(PageSize { LineCount(2), ColumnCount(2) }, CellLocation {}, false);
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

    (void) grid.resize(PageSize { LineCount(2), ColumnCount(2) }, CellLocation {}, false);
    logGridText(grid, "after resize 2x2");

    SECTION("smooth regrow 2->3->4->5")
    {
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(3) }, CellLocation {}, false);
        logGridText(grid, "after resize 3x2");
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(4) }, CellLocation {}, false);
        logGridText(grid, "after resize 4x2");
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(5) }, CellLocation {}, false);
        logGridText(grid, "after resize 5x2");

        CHECK(grid.historyLineCount() == LineCount(0));
        CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(5) });
        CHECK(grid.lineText(LineOffset(0)) == "ABCDE");
        CHECK(grid.lineText(LineOffset(1)) == "abcde");
    }

    SECTION("hard regrow 2->5")
    {
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(5) }, CellLocation {}, false);
        logGridText(grid, "after resize 5x2");

        CHECK(grid.historyLineCount() == LineCount(0));
        CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(5) });
        CHECK(grid.lineText(LineOffset(0)) == "ABCDE");
        CHECK(grid.lineText(LineOffset(1)) == "abcde");
    }
}

TEST_CASE("Grid.reflow.tripple", "[grid]")
{
    // Tests reflowing text upon shrink/grow across more than two (e.g. three) wrapped lines.
    auto grid = setupGrid8x2();

    (void) grid.resize(PageSize { LineCount(2), ColumnCount(2) }, CellLocation {}, false);
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
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(8) }, CellLocation {}, false);
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
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(3) }, CellLocation {}, false);
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
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(4) }, CellLocation {}, false);
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
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(5) }, CellLocation {}, false);
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
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(7) }, CellLocation {}, false);
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
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(8) }, CellLocation {}, false);
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
    auto gridFinite = Grid<Cell>(PageSize { LineCount(2), ColumnCount(8) }, true, LineCount(0));
    gridFinite.setLineText(LineOffset { 0 }, "ABCDEFGH"sv);
    gridFinite.setLineText(LineOffset { 1 }, "abcdefgh"sv);
    gridFinite.scrollUp(LineCount { 1 });
    REQUIRE(gridFinite.lineText(LineOffset(0)) == "abcdefgh");
    REQUIRE(gridFinite.lineText(LineOffset(-1)) == std::string(8, ' '));

    auto gridInfinite = Grid<Cell>(PageSize { LineCount(2), ColumnCount(8) }, true, Infinite());
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
    auto grid = Grid<Cell>(PageSize { LineCount(3), ColumnCount(5) }, true, LineCount(0));
    grid.setLineText(LineOffset { 0 }, "1");
    grid.setLineText(LineOffset { 1 }, "2");
    grid.setLineText(LineOffset { 2 }, "ABCDE");
    (void) grid.resize(PageSize { LineCount(3), ColumnCount(3) }, CellLocation {}, false);
    REQUIRE(grid.lineText(LineOffset(0)) == "2  ");
    REQUIRE(grid.lineText(LineOffset(1)) == "ABC");
    REQUIRE(grid.lineText(LineOffset(2)) == "DE ");
    (void) grid.resize(PageSize { LineCount(3), ColumnCount(5) }, CellLocation {}, false);
    REQUIRE(unbox(grid.historyLineCount()) == 0);
    REQUIRE(grid.lineText(LineOffset(0)) == "1    ");
    REQUIRE(grid.lineText(LineOffset(1)) == "2    ");
    REQUIRE(grid.lineText(LineOffset(2)) == "ABCDE");
}

TEST_CASE("Grid resize", "[grid]")
{
    auto width = ColumnCount(6);
    auto grid = Grid<Cell>(PageSize { LineCount(2), width }, true, LineCount(0));
    auto text = "abcd"sv;
    auto pool = crispy::buffer_object_pool<char>(32);
    auto bufferObject = pool.allocateBufferObject();
    bufferObject->writeAtEnd(text);
    auto const bufferFragment = bufferObject->ref(0, 4);
    auto const sgr = GraphicsAttributes {};
    auto const trivial = TrivialLineBuffer { width, sgr, sgr, HyperlinkId {}, width, bufferFragment };
    auto lineTrivial = Line<Cell>(LineFlags::None, trivial);
    grid.lineAt(LineOffset(0)) = lineTrivial;
    REQUIRE(grid.lineAt(LineOffset(0)).isTrivialBuffer());
    REQUIRE(grid.lineAt(LineOffset(1)).isTrivialBuffer());
    (void) grid.resize(PageSize { LineCount(2), width + ColumnCount(1) }, CellLocation {}, false);
    REQUIRE(grid.lineAt(LineOffset(0)).isTrivialBuffer());
    REQUIRE(grid.lineAt(LineOffset(1)).isTrivialBuffer());
    (void) grid.resize(PageSize { LineCount(2), width + ColumnCount(-1) }, CellLocation {}, false);
    REQUIRE(grid.lineAt(LineOffset(0)).isTrivialBuffer());
    REQUIRE(grid.lineAt(LineOffset(1)).isTrivialBuffer());
}

// }}}
