/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <vtbackend/Grid.h>
#include <vtbackend/cell/CellConfig.h>
#include <vtbackend/primitives.h>

#include <fmt/format.h>

#include <catch2/catch.hpp>

#include <iostream>

using namespace terminal;
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
                        grid.lineText(line_offset::cast_from(line - grid.historyLineCount().as<int>())),
                        (unsigned) grid.lineAt(line_offset::cast_from(line)).flags()));
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

        grid.setLineText(line_offset::cast_from(cursor - 1), line);

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

constexpr terminal::margin fullPageMargin(PageSize pageSize)
{
    return terminal::margin { margin::vertical { line_offset(0), pageSize.lines.as<line_offset>() - 1 },
                              margin::horizontal { column_offset(0),
                                                   pageSize.columns.as<column_offset>() - 1 } };
}

[[maybe_unused]] Grid<Cell> setupGrid5x2()
{
    auto grid = Grid<Cell>(PageSize { LineCount(2), ColumnCount(5) }, true, LineCount(10));
    grid.setLineText(line_offset { 0 }, "ABCDE");
    grid.setLineText(line_offset { 1 }, "abcde");
    logGridText(grid, "setup grid at 5x2");
    return grid;
}

[[maybe_unused]] Grid<Cell> setupGrid5x2x2()
{
    auto grid = Grid<Cell>(PageSize { LineCount(2), ColumnCount(5) }, true, LineCount(2));
    grid.scrollUp(LineCount(2));
    grid.setLineText(line_offset { -1 }, "ABCDE");
    grid.setLineText(line_offset { 0 }, "FGHIJ");
    grid.setLineText(line_offset { 1 }, "KLMNO");
    grid.setLineText(line_offset { 2 }, "PQRST");
    logGridText(grid, "setup grid at 5x2x2");
    return grid;
}

[[maybe_unused]] Grid<Cell> setupGrid8x2()
{
    auto grid = Grid<Cell>(PageSize { LineCount(2), ColumnCount(8) }, true, LineCount(10));
    grid.setLineText(line_offset { 0 }, "ABCDEFGH");
    grid.setLineText(line_offset { 1 }, "abcdefgh");
    logGridText(grid, "setup grid at 5x2");
    return grid;
}

Grid<Cell> setupGridForResizeTests2x3xN(LineCount maxHistoryLineCount)
{
    auto constexpr reflowOnResize = true;
    auto constexpr pageSize = PageSize { LineCount(2), ColumnCount(3) };

    return setupGrid(pageSize, reflowOnResize, maxHistoryLineCount, { "ABC", "DEF", "GHI", "JKL" });
}

Grid<Cell> setupGridForResizeTests2x3a3()
{
    return setupGridForResizeTests2x3xN(LineCount(3));
}

} // namespace

TEST_CASE("Grid.setup", "[grid]")
{
    auto grid = Grid<Cell>(PageSize { LineCount(2), ColumnCount(5) }, true, LineCount(0));
    grid.setLineText(line_offset { 0 }, "ABCDE"sv);
    grid.setLineText(line_offset { 1 }, "abcde"sv);
    logGridText(grid, "setup grid at 5x2");

    CHECK(grid.lineText(line_offset { 0 }) == "ABCDE"sv);
    CHECK(grid.lineText(line_offset { 1 }) == "abcde"sv);
}

TEST_CASE("Grid.writeAndScrollUp", "[grid]")
{
    auto grid = Grid<Cell>(PageSize { LineCount(2), ColumnCount(5) }, true, LineCount(3));
    grid.setLineText(line_offset { 0 }, "ABCDE");
    grid.setLineText(line_offset { 1 }, "abcde");
    CHECK(grid.historyLineCount() == LineCount(0));
    CHECK(grid.lineText(line_offset(0)) == "ABCDE");
    CHECK(grid.lineText(line_offset(1)) == "abcde");

    grid.scrollUp(LineCount(1));
    grid.setLineText(line_offset(1), "12345");

    CHECK(grid.historyLineCount() == LineCount(1));
    CHECK(grid.lineText(line_offset(-1)) == "ABCDE");
    CHECK(grid.lineText(line_offset(0)) == "abcde");
    CHECK(grid.lineText(line_offset(1)) == "12345");

    grid.scrollUp(LineCount(1));
    CHECK(grid.historyLineCount() == LineCount(2));
    CHECK(grid.lineText(line_offset(-2)) == "ABCDE");
    CHECK(grid.lineText(line_offset(-1)) == "abcde");
    CHECK(grid.lineText(line_offset(0)) == "12345");
    CHECK(grid.lineText(line_offset(1)) == "     ");
}

TEST_CASE("iteratorAt", "[grid]")
{
    auto grid = Grid<Cell>(PageSize { LineCount(3), ColumnCount(3) }, true, LineCount(0));
    grid.setLineText(line_offset { 0 }, "ABC"sv);
    grid.setLineText(line_offset { 1 }, "DEF"sv);
    grid.setLineText(line_offset { 2 }, "GHI"sv);
    logGridText(grid);

    auto* const a00 = &grid.at(line_offset(0), column_offset(0));
    CHECK(a00->toUtf8() == "A");
    auto* const a01 = &grid.at(line_offset(0), column_offset(1));
    CHECK(a01->toUtf8() == "B");
    auto* const a02 = &grid.at(line_offset(0), column_offset(2));
    CHECK(a02->toUtf8() == "C");

    auto* const a11 = &grid.at(line_offset(1), column_offset(1));
    CHECK(a11->toUtf8() == "E");
    auto* const a22 = &grid.at(line_offset(2), column_offset(2));
    CHECK(a22->toUtf8() == "I");
}

TEST_CASE("LogicalLines.iterator", "[grid]")
{
    auto constexpr reflowOnResize = true;
    auto constexpr maxHistoryLineCount = LineCount(5);
    auto constexpr pageSize = PageSize { LineCount(2), ColumnCount(3) };

    auto grid = setupGrid(pageSize,
                          reflowOnResize,
                          maxHistoryLineCount,
                          {
                              "ABC", // -4:
                              "DEF", // -3:
                              "GHI", // -2: wrapped
                              "JKL", // -1: wrapped
                              "MNO", //  0:
                              "PQR", //  1: wrapped
                          });

    grid.lineAt(line_offset(-2)).setWrapped(true);
    grid.lineAt(line_offset(-1)).setWrapped(true);
    grid.lineAt(line_offset(1)).setWrapped(true);
    logGridText(grid, "After having set wrapped-flag.");

    LogicalLines logicalLines = grid.logicalLines();
    auto lineIt = logicalLines.begin();

    // ABC
    auto line = *lineIt;
    auto const tABC = line.text();
    REQUIRE(tABC == "ABC");
    CHECK(line.top == line_offset(-4));
    CHECK(line.bottom == line_offset(-4));

    // DEF GHI JKL
    line = *++lineIt;
    auto const tDEFGHIJKL = line.text();
    REQUIRE(tDEFGHIJKL == "DEFGHIJKL");
    CHECK(line.top == line_offset(-3));
    CHECK(line.bottom == line_offset(-1));

    // MNO PQR
    line = *++lineIt;
    auto const tMNOPQR = line.text();
    REQUIRE(tMNOPQR == "MNOPQR");
    CHECK(line.top == line_offset(0));
    CHECK(line.bottom == line_offset(1));

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
    auto constexpr reflowOnResize = true;
    auto constexpr maxHistoryLineCount = LineCount(5);
    auto constexpr pageSize = PageSize { LineCount(2), ColumnCount(3) };

    auto grid = setupGrid(pageSize,
                          reflowOnResize,
                          maxHistoryLineCount,
                          {
                              "ABC", // -4:
                              "DEF", // -3:
                              "GHI", // -2: wrapped
                              "JKL", // -1: wrapped
                              "MNO", //  0:
                              "PQR", //  1: wrapped
                          });

    grid.lineAt(line_offset(-2)).setWrapped(true);
    grid.lineAt(line_offset(-1)).setWrapped(true);
    grid.lineAt(line_offset(1)).setWrapped(true);
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

    auto const curCursorPos = cell_location { grid.pageSize().lines.as<line_offset>() - 1, column_offset(1) };
    auto const newPageSize = PageSize { LineCount(4), ColumnCount(3) };
    auto const newCursorPos0 = cell_location { curCursorPos.line + 2, curCursorPos.column };
    cell_location newCursorPos = grid.resize(newPageSize, curCursorPos, false);
    CHECK(newCursorPos.line == newCursorPos0.line);
    CHECK(newCursorPos.column == newCursorPos0.column);
    CHECK(grid.pageSize() == newPageSize);
    CHECK(grid.historyLineCount() == LineCount(0));
    CHECK(grid.lineText(line_offset(0)) == "ABC");
    CHECK(grid.lineText(line_offset(1)) == "DEF");
    CHECK(grid.lineText(line_offset(2)) == "GHI");
    CHECK(grid.lineText(line_offset(3)) == "JKL");
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

    auto const curCursorPos = cell_location { line_offset(1), column_offset(1) };
    auto const newPageSize = PageSize { LineCount(5), ColumnCount(3) };
    logGridText(grid, "BEFORE");
    cell_location newCursorPos = grid.resize(newPageSize, curCursorPos, false);
    logGridText(grid, "AFTER");
    CHECK(newCursorPos.line == line_offset(3));
    CHECK(newCursorPos.column == curCursorPos.column);
    CHECK(grid.pageSize() == newPageSize);
    CHECK(grid.historyLineCount() == LineCount(0));
    CHECK(grid.lineText(line_offset(0)) == "ABC");
    CHECK(grid.lineText(line_offset(1)) == "DEF");
    CHECK(grid.lineText(line_offset(2)) == "GHI");
    CHECK(grid.lineText(line_offset(3)) == "JKL");
    CHECK(grid.lineText(line_offset(4)) == "   ");
}

TEST_CASE("resize_grow_lines_with_history_cursor_no_bottom", "[grid]")
{
    auto grid = setupGridForResizeTests2x3a3();
    CHECK(grid.maxHistoryLineCount() == LineCount(3));
    CHECK(grid.historyLineCount() == LineCount(2));

    auto const curCursorPos = cell_location { line_offset(0), column_offset(1) };
    logGridText(grid, "before resize");
    cell_location newCursorPos = grid.resize(PageSize { LineCount(3), ColumnCount(3) }, curCursorPos, false);
    logGridText(grid, "after resize");
    CHECK(newCursorPos.line == curCursorPos.line);
    CHECK(newCursorPos.column == curCursorPos.column);
    CHECK(grid.pageSize().columns == ColumnCount(3));
    CHECK(grid.pageSize().lines == LineCount(3));
    CHECK(grid.historyLineCount() == LineCount(2));
    CHECK(grid.lineText(line_offset(-2)) == "ABC");
    CHECK(grid.lineText(line_offset(-1)) == "DEF");
    CHECK(grid.lineText(line_offset(0)) == "GHI");
    CHECK(grid.lineText(line_offset(1)) == "JKL");
    CHECK(grid.lineText(line_offset(2)) == "   ");
}

TEST_CASE("resize_shrink_lines_with_history", "[grid]")
{
    auto grid = Grid<Cell>(PageSize { LineCount(2), ColumnCount(3) }, true, LineCount(5));
    auto const gridMargin = fullPageMargin(grid.pageSize());
    grid.scrollUp(LineCount { 1 }, graphics_attributes {}, gridMargin);
    grid.setLineText(line_offset(-1), "ABC");       // history line
    grid.setLineText(line_offset(0), "DEF");        // main page: line 1
    grid.setLineText(line_offset(1), "GHI");        // main page: line 2
    CHECK(grid.historyLineCount() == LineCount(1)); // TODO: move line up, below scrollUp()

    // shrink by one line (=> move page one line up into scrollback)
    auto const newPageSize = PageSize { LineCount(1), ColumnCount(3) };
    auto const curCursorPos = cell_location { line_offset(1), column_offset(1) };
    logGridText(grid, "BEFORE");
    cell_location newCursorPos = grid.resize(newPageSize, curCursorPos, false);
    logGridText(grid, "AFTER");
    CHECK(grid.pageSize().columns == ColumnCount(3));
    CHECK(grid.pageSize().lines == LineCount(1));
    CHECK(grid.historyLineCount() == LineCount(2)); // XXX FIXME: test failing
    CHECK(grid.lineText(line_offset(-2)) == "ABC");
    CHECK(grid.lineText(line_offset(-1)) == "DEF");
    CHECK(grid.lineText(line_offset(0)) == "GHI");
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
    auto const curCursorPos = cell_location { line_offset(1), column_offset(1) };
    grid.lineAt(line_offset(0)).setWrappable(false);
    logGridText(grid, "BEFORE");
    auto const newCursorPos = grid.resize(newPageSize, curCursorPos, false);
    (void) newCursorPos;
    logGridText(grid, "AFTER");

    CHECK(grid.historyLineCount() == LineCount(5));
    CHECK(grid.pageSize().columns == ColumnCount(2));
    CHECK(grid.pageSize().lines == LineCount(2));

    CHECK(grid.lineText(line_offset(-5)) == "AB");
    CHECK(grid.lineText(line_offset(-4)) == "C ");
    CHECK(grid.lineText(line_offset(-3)) == "DE");
    CHECK(grid.lineText(line_offset(-2)) == "F ");
    CHECK(grid.lineText(line_offset(-1)) == "GH");
    CHECK(grid.lineText(line_offset(0)) == "JK");
    CHECK(grid.lineText(line_offset(1)) == "L ");

    CHECK(grid.lineAt(line_offset(-5)).flags() == line_flags::Wrappable);
    CHECK(grid.lineAt(line_offset(-4)).flags() == (line_flags::Wrappable | line_flags::Wrapped));
    CHECK(grid.lineAt(line_offset(-3)).flags() == line_flags::Wrappable);
    CHECK(grid.lineAt(line_offset(-2)).flags() == (line_flags::Wrappable | line_flags::Wrapped));
    CHECK(grid.lineAt(line_offset(-1)).flags() == line_flags::None);
    CHECK(grid.lineAt(line_offset(0)).flags() == line_flags::Wrappable);
    CHECK(grid.lineAt(line_offset(1)).flags() == (line_flags::Wrappable | line_flags::Wrapped));
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
    auto const curCursorPos = cell_location { line_offset(1), column_offset(1) };
    grid.lineAt(line_offset(0)).setWrappable(false);
    // logGridText(grid, "BEFORE");
    auto const newCursorPos = grid.resize(PageSize { LineCount(4), ColumnCount(2) }, curCursorPos, false);
    (void) newCursorPos;
    // logGridText(grid, "AFTER");

    CHECK(grid.lineText(line_offset(-3)) == "AB");
    CHECK(grid.lineText(line_offset(-2)) == "C ");
    CHECK(grid.lineText(line_offset(-1)) == "DE");
    CHECK(grid.lineText(line_offset(0)) == "F ");
    CHECK(grid.lineText(line_offset(1)) == "GH");
    CHECK(grid.lineText(line_offset(2)) == "JK");
    CHECK(grid.lineText(line_offset(3)) == "L ");

    CHECK(grid.lineAt(line_offset(-3)).flags() == line_flags::Wrappable);
    CHECK(grid.lineAt(line_offset(-2)).flags() == (line_flags::Wrappable | line_flags::Wrapped));
    CHECK(grid.lineAt(line_offset(-1)).flags() == line_flags::Wrappable);
    CHECK(grid.lineAt(line_offset(0)).flags() == (line_flags::Wrappable | line_flags::Wrapped));
    CHECK(grid.lineAt(line_offset(1)).flags() == line_flags::None);
    CHECK(grid.lineAt(line_offset(2)).flags() == line_flags::Wrappable);
    CHECK(grid.lineAt(line_offset(3)).flags() == (line_flags::Wrappable | line_flags::Wrapped));
}
// }}}

// {{{ grid reflow
TEST_CASE("resize_reflow_shrink", "[grid]")
{
    auto grid = setupGrid5x2();
    logGridText(grid, "init");

    // Shrink slowly from 5x2 to 4x2 to 3x2 to 2x2.

    // 4x2
    (void) grid.resize(PageSize { LineCount(2), ColumnCount(4) }, cell_location { {}, {} }, false);
    logGridText(grid, "after resize 4x2");

    CHECK(*grid.historyLineCount() == 2);
    CHECK(grid.lineText(line_offset(-2)) == "ABCD");
    CHECK(grid.lineText(line_offset(-1)) == "E   ");

    CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(4) });
    CHECK(grid.lineText(line_offset(0)) == "abcd");
    CHECK(grid.lineText(line_offset(1)) == "e   ");

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
    (void) grid.resize(PageSize { LineCount(2), ColumnCount(3) }, cell_location { {}, {} }, false);
    logGridText(grid, "after resize 3x2");

    CHECK(*grid.historyLineCount() == 2);
    CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(3) });
    CHECK(grid.lineText(line_offset(-2)) == "ABC");
    CHECK(grid.lineText(line_offset(-1)) == "DE ");
    CHECK(grid.lineText(line_offset(0)) == "abc");
    CHECK(grid.lineText(line_offset(1)) == "de ");

    // 2x2
    (void) grid.resize(PageSize { LineCount(2), ColumnCount(2) }, cell_location { {}, {} }, false);
    logGridText(grid, "after resize 2x2");

    CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(2) });
    CHECK(grid.historyLineCount() == LineCount(4));
    CHECK(grid.lineText(line_offset(-4)) == "AB");
    CHECK(grid.lineText(line_offset(-3)) == "CD");
    CHECK(grid.lineText(line_offset(-2)) == "E ");
    CHECK(grid.lineText(line_offset(-1)) == "ab");
    CHECK(grid.lineText(line_offset(0)) == "cd");
    CHECK(grid.lineText(line_offset(1)) == "e ");
}

TEST_CASE("Grid.reflow", "[grid]")
{
    auto grid = setupGrid5x2();

    SECTION("resize 4x2")
    {
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(4) }, cell_location { {}, {} }, false);
        logGridText(grid, "after resize");

        CHECK(grid.historyLineCount() == LineCount(2));
        CHECK(grid.lineText(line_offset(-2)) == "ABCD");
        CHECK(grid.lineText(line_offset(-1)) == "E   ");

        CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(4) });
        CHECK(grid.lineText(line_offset(0)) == "abcd");
        CHECK(grid.lineText(line_offset(1)) == "e   ");
    }

    SECTION("resize 3x2")
    {
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(4) }, cell_location {}, false);
        logGridText(grid, "after resize 4x2");
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(3) }, cell_location {}, false);
        logGridText(grid, "after resize 3x2");

        CHECK(grid.historyLineCount() == LineCount(2));
        CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(3) });
        CHECK(grid.lineText(line_offset(-2)) == "ABC");
        CHECK(grid.lineText(line_offset(-1)) == "DE ");
        CHECK(grid.lineText(line_offset(0)) == "abc");
        CHECK(grid.lineText(line_offset(1)) == "de ");
    }

    SECTION("resize 2x2")
    {
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(4) }, cell_location {}, false);
        logGridText(grid, "after resize 4x2");
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(3) }, cell_location {}, false);
        logGridText(grid, "after resize 3x2");
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(2) }, cell_location {}, false);
        logGridText(grid, "after resize 2x2");

        CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(2) });
        CHECK(grid.historyLineCount() == LineCount(4));
        CHECK(grid.lineText(line_offset(-4)) == "AB");
        CHECK(grid.lineText(line_offset(-3)) == "CD");
        CHECK(grid.lineText(line_offset(-2)) == "E ");
        CHECK(grid.lineText(line_offset(-1)) == "ab");
        CHECK(grid.lineText(line_offset(0)) == "cd");
        CHECK(grid.lineText(line_offset(1)) == "e ");

        SECTION("regrow 3x2")
        {
            logGridText(grid, "Before regrow to 3x2");
            (void) grid.resize(PageSize { LineCount(2), ColumnCount(3) }, cell_location {}, false);
            logGridText(grid, "after regrow to 3x2");

            CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(3) });
            CHECK(grid.historyLineCount() == LineCount(2));
            CHECK(grid.lineText(line_offset(-2)) == "ABC");
            CHECK(grid.lineText(line_offset(-1)) == "DE ");
            CHECK(grid.lineText(line_offset(0)) == "abc");
            CHECK(grid.lineText(line_offset(1)) == "de ");

            SECTION("regrow 4x2")
            {
                (void) grid.resize(PageSize { LineCount(2), ColumnCount(4) }, cell_location {}, false);
                logGridText(grid, "after regrow 4x2");

                CHECK(grid.historyLineCount() == LineCount(2));
                CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(4) });
                CHECK(grid.lineText(line_offset(-2)) == "ABCD");
                CHECK(grid.lineText(line_offset(-1)) == "E   ");
                CHECK(grid.lineText(line_offset(0)) == "abcd");
                CHECK(grid.lineText(line_offset(1)) == "e   ");
            }

            SECTION("regrow 5x2")
            {
                (void) grid.resize(PageSize { LineCount(2), ColumnCount(5) }, cell_location {}, false);
                logGridText(grid, "after regrow 5x2");

                CHECK(grid.historyLineCount() == LineCount(0));
                CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(5) });
                CHECK(grid.lineText(line_offset(0)) == "ABCDE");
                CHECK(grid.lineText(line_offset(1)) == "abcde");
            }
        }
    }
}

TEST_CASE("Grid.reflow.shrink_many", "[grid]")
{
    auto grid = setupGrid5x2();
    REQUIRE(grid.pageSize() == PageSize { LineCount(2), ColumnCount(5) });
    REQUIRE(grid.lineText(line_offset(0)) == "ABCDE"sv);
    REQUIRE(grid.lineText(line_offset(1)) == "abcde"sv);

    (void) grid.resize(PageSize { LineCount(2), ColumnCount(2) }, cell_location {}, false);
    logGridText(grid, "after resize 2x2");

    CHECK(grid.historyLineCount() == LineCount(4));
    CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(2) });
    CHECK(grid.lineText(line_offset(-4)) == "AB");
    CHECK(grid.lineText(line_offset(-3)) == "CD");
    CHECK(grid.lineText(line_offset(-2)) == "E ");
    CHECK(grid.lineText(line_offset(-1)) == "ab");
    CHECK(grid.lineText(line_offset(0)) == "cd");
    CHECK(grid.lineText(line_offset(1)) == "e ");
}

TEST_CASE("Grid.reflow.shrink_many_grow_many", "[grid]")
{
    auto grid = setupGrid5x2();

    (void) grid.resize(PageSize { LineCount(2), ColumnCount(2) }, cell_location {}, false);
    logGridText(grid, "after resize 2x2");

    SECTION("smooth regrow 2->3->4->5")
    {
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(3) }, cell_location {}, false);
        logGridText(grid, "after resize 3x2");
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(4) }, cell_location {}, false);
        logGridText(grid, "after resize 4x2");
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(5) }, cell_location {}, false);
        logGridText(grid, "after resize 5x2");

        CHECK(grid.historyLineCount() == LineCount(0));
        CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(5) });
        CHECK(grid.lineText(line_offset(0)) == "ABCDE");
        CHECK(grid.lineText(line_offset(1)) == "abcde");
    }

    SECTION("hard regrow 2->5")
    {
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(5) }, cell_location {}, false);
        logGridText(grid, "after resize 5x2");

        CHECK(grid.historyLineCount() == LineCount(0));
        CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(5) });
        CHECK(grid.lineText(line_offset(0)) == "ABCDE");
        CHECK(grid.lineText(line_offset(1)) == "abcde");
    }
}

TEST_CASE("Grid.reflow.tripple", "[grid]")
{
    // Tests reflowing text upon shrink/grow across more than two (e.g. three) wrapped lines.
    auto grid = setupGrid8x2();

    (void) grid.resize(PageSize { LineCount(2), ColumnCount(2) }, cell_location {}, false);
    logGridText(grid, "after resize 3x2");

    REQUIRE(grid.historyLineCount() == LineCount(6));
    REQUIRE(grid.pageSize() == PageSize { LineCount(2), ColumnCount(2) });

    REQUIRE(!grid.lineAt(line_offset(-6)).wrapped());
    REQUIRE(grid.lineAt(line_offset(-5)).wrapped());
    REQUIRE(grid.lineAt(line_offset(-4)).wrapped());
    REQUIRE(grid.lineAt(line_offset(-3)).wrapped());
    REQUIRE(!grid.lineAt(line_offset(-2)).wrapped());
    REQUIRE(grid.lineAt(line_offset(-1)).wrapped());
    REQUIRE(grid.lineAt(line_offset(0)).wrapped());
    REQUIRE(grid.lineAt(line_offset(1)).wrapped());

    SECTION("grow from 2x2 to 8x2")
    {
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(8) }, cell_location {}, false);
        logGridText(grid, "after resize 3x2");

        CHECK(grid.historyLineCount() == LineCount(0));
        CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(8) });

        CHECK(!grid.lineAt(line_offset { 0 }).wrapped());
        CHECK(grid.lineText(line_offset(0)) == "ABCDEFGH");

        CHECK(!grid.lineAt(line_offset { 1 }).wrapped());
        CHECK(grid.lineText(line_offset(1)) == "abcdefgh");
    }

    SECTION("grow from 2x2 to 3x2 to ... to 8x2")
    {
        // {{{ 3x2
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(3) }, cell_location {}, false);
        logGridText(grid, "after resize 3x2");

        REQUIRE(grid.historyLineCount() == LineCount(4));
        REQUIRE(grid.pageSize() == PageSize { LineCount(2), ColumnCount(3) });

        REQUIRE(grid.lineText(line_offset(-4)) == "ABC");
        REQUIRE(grid.lineText(line_offset(-3)) == "DEF");
        REQUIRE(grid.lineText(line_offset(-2)) == "GH ");
        REQUIRE(grid.lineText(line_offset(-1)) == "abc");
        REQUIRE(grid.lineText(line_offset(0)) == "def");
        REQUIRE(grid.lineText(line_offset(1)) == "gh ");

        REQUIRE(!grid.lineAt(line_offset(-4)).wrapped());
        REQUIRE(grid.lineAt(line_offset(-3)).wrapped());
        REQUIRE(grid.lineAt(line_offset(-2)).wrapped());
        REQUIRE(!grid.lineAt(line_offset(-1)).wrapped());
        REQUIRE(grid.lineAt(line_offset(0)).wrapped());
        REQUIRE(grid.lineAt(line_offset(1)).wrapped());
        // }}}

        // {{{ 4x2
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(4) }, cell_location {}, false);
        logGridText(grid, "after resize 4x2");

        REQUIRE(grid.historyLineCount() == LineCount(2));
        REQUIRE(grid.pageSize() == PageSize { LineCount(2), ColumnCount(4) });

        REQUIRE(grid.lineText(line_offset(-2)) == "ABCD");
        REQUIRE(grid.lineText(line_offset(-1)) == "EFGH");
        REQUIRE(grid.lineText(line_offset(0)) == "abcd");
        REQUIRE(grid.lineText(line_offset(1)) == "efgh");

        REQUIRE(!grid.lineAt(line_offset(-2)).wrapped());
        REQUIRE(grid.lineAt(line_offset(-1)).wrapped());
        REQUIRE(!grid.lineAt(line_offset(0)).wrapped());
        REQUIRE(grid.lineAt(line_offset(1)).wrapped());
        // }}}

        // {{{ 5x2
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(5) }, cell_location {}, false);
        logGridText(grid, "after resize 5x2");

        REQUIRE(grid.historyLineCount() == LineCount(2));
        REQUIRE(grid.pageSize() == PageSize { LineCount(2), ColumnCount(5) });

        REQUIRE(grid.lineText(line_offset(-2)) == "ABCDE");
        REQUIRE(grid.lineText(line_offset(-1)) == "FGH  ");
        REQUIRE(grid.lineText(line_offset(0)) == "abcde");
        REQUIRE(grid.lineText(line_offset(1)) == "fgh  ");

        REQUIRE(!grid.lineAt(line_offset(-2)).wrapped());
        REQUIRE(grid.lineAt(line_offset(-1)).wrapped());
        REQUIRE(!grid.lineAt(line_offset(0)).wrapped());
        REQUIRE(grid.lineAt(line_offset(1)).wrapped());
        // }}}

        // {{{ 7x2
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(7) }, cell_location {}, false);
        logGridText(grid, "after resize 7x2");

        REQUIRE(grid.historyLineCount() == LineCount(2));
        REQUIRE(grid.pageSize() == PageSize { LineCount(2), ColumnCount(7) });

        REQUIRE(grid.lineText(line_offset(-2)) == "ABCDEFG");
        REQUIRE(grid.lineText(line_offset(-1)) == "H      ");
        REQUIRE(grid.lineText(line_offset(0)) == "abcdefg");
        REQUIRE(grid.lineText(line_offset(1)) == "h      ");

        REQUIRE(!grid.lineAt(line_offset(-2)).wrapped());
        REQUIRE(grid.lineAt(line_offset(-1)).wrapped());
        REQUIRE(!grid.lineAt(line_offset(0)).wrapped());
        REQUIRE(grid.lineAt(line_offset(1)).wrapped());
        // }}}

        // {{{ 8x2
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(8) }, cell_location {}, false);
        logGridText(grid, "after resize 8x2");

        REQUIRE(*grid.historyLineCount() == 0);
        REQUIRE(grid.pageSize() == PageSize { LineCount(2), ColumnCount(8) });

        REQUIRE(grid.lineText(line_offset(0)) == "ABCDEFGH");
        REQUIRE(grid.lineText(line_offset(1)) == "abcdefgh");

        REQUIRE(!grid.lineAt(line_offset(0)).wrapped());
        REQUIRE(!grid.lineAt(line_offset(1)).wrapped());
        // }}}
    }
}

TEST_CASE("Grid infinite", "[grid]")
{
    auto grid_finite = Grid<Cell>(PageSize { LineCount(2), ColumnCount(8) }, true, LineCount(0));
    grid_finite.setLineText(line_offset { 0 }, "ABCDEFGH"sv);
    grid_finite.setLineText(line_offset { 1 }, "abcdefgh"sv);
    grid_finite.scrollUp(LineCount { 1 });
    REQUIRE(grid_finite.lineText(line_offset(0)) == "abcdefgh");
    REQUIRE(grid_finite.lineText(line_offset(-1)) == std::string(8, ' '));

    auto grid_infinite = Grid<Cell>(PageSize { LineCount(2), ColumnCount(8) }, true, infinite());
    grid_infinite.setLineText(line_offset { 0 }, "ABCDEFGH"sv);
    grid_infinite.setLineText(line_offset { 1 }, "abcdefgh"sv);
    grid_infinite.scrollUp(LineCount { 1 });
    REQUIRE(grid_infinite.lineText(line_offset(0)) == "abcdefgh");
    REQUIRE(grid_infinite.lineText(line_offset(-1)) == "ABCDEFGH");
    grid_infinite.scrollUp(LineCount { 97 });
    REQUIRE(grid_infinite.lineText(line_offset(-97)) == "abcdefgh");
    REQUIRE(grid_infinite.lineText(line_offset(-98)) == "ABCDEFGH");
}

TEST_CASE("Grid resize with wrap", "[grid]")
{
    auto grid = Grid<Cell>(PageSize { LineCount(3), ColumnCount(5) }, true, LineCount(0));
    grid.setLineText(line_offset { 0 }, "1");
    grid.setLineText(line_offset { 1 }, "2");
    grid.setLineText(line_offset { 2 }, "ABCDE");
    (void) grid.resize(PageSize { LineCount(3), ColumnCount(3) }, cell_location {}, false);
    REQUIRE(grid.lineText(line_offset(0)) == "2  ");
    REQUIRE(grid.lineText(line_offset(1)) == "ABC");
    REQUIRE(grid.lineText(line_offset(2)) == "DE ");
    (void) grid.resize(PageSize { LineCount(3), ColumnCount(5) }, cell_location {}, false);
    REQUIRE(unbox<int>(grid.historyLineCount()) == 0);
    REQUIRE(grid.lineText(line_offset(0)) == "1    ");
    REQUIRE(grid.lineText(line_offset(1)) == "2    ");
    REQUIRE(grid.lineText(line_offset(2)) == "ABCDE");
}

TEST_CASE("Grid resize", "[grid]")
{
    auto width = ColumnCount(6);
    auto grid = Grid<Cell>(PageSize { LineCount(2), width }, true, LineCount(0));
    auto text = "abcd"sv;
    auto pool = crispy::BufferObjectPool<char>(32);
    auto bufferObject = pool.allocateBufferObject();
    bufferObject->writeAtEnd(text);
    auto const bufferFragment = bufferObject->ref(0, 4);
    auto const sgr = graphics_attributes {};
    auto const trivial = trivial_line_buffer { width, sgr, sgr, hyperlink_id {}, width, bufferFragment };
    auto line_trivial = line<Cell>(line_flags::None, trivial);
    grid.lineAt(line_offset(0)) = line_trivial;
    REQUIRE(grid.lineAt(line_offset(0)).isTrivialBuffer());
    REQUIRE(grid.lineAt(line_offset(1)).isTrivialBuffer());
    (void) grid.resize(PageSize { LineCount(2), width + ColumnCount(1) }, cell_location {}, false);
    REQUIRE(grid.lineAt(line_offset(0)).isTrivialBuffer());
    REQUIRE(grid.lineAt(line_offset(1)).isTrivialBuffer());
    (void) grid.resize(PageSize { LineCount(2), width + ColumnCount(-1) }, cell_location {}, false);
    REQUIRE(grid.lineAt(line_offset(0)).isTrivialBuffer());
    REQUIRE(grid.lineAt(line_offset(1)).isTrivialBuffer());
}

// }}}
