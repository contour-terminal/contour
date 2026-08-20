// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Grid.hpp>
#include <vtbackend/Primitives.hpp>

#include <crispy/BufferObject.hpp>

#include <libunicode/convert.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <format>
#include <ranges>
#include <span>
#include <string>
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

    auto const curCursorPos = CellLocation { .line = LineOffset(1), .column = ColumnOffset(1) };
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

    auto const curCursorPos = CellLocation { .line = LineOffset(0), .column = ColumnOffset(1) };
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
    CellLocation const newCursorPos = grid.resize(newPageSize, curCursorPos, false);
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
    auto const curCursorPos = CellLocation { .line = LineOffset(1), .column = ColumnOffset(1) };
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
TEST_CASE("resize_reflow_shrink", "[grid]")
{
    auto grid = setupGrid5x2();
    logGridText(grid, "init");

    // Shrink slowly from 5x2 to 4x2 to 3x2 to 2x2.

    // 4x2
    (void) grid.resize(
        PageSize { LineCount(2), ColumnCount(4) }, CellLocation { .line = {}, .column = {} }, false);
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
    (void) grid.resize(
        PageSize { LineCount(2), ColumnCount(3) }, CellLocation { .line = {}, .column = {} }, false);
    logGridText(grid, "after resize 3x2");

    CHECK(*grid.historyLineCount() == 2);
    CHECK(grid.pageSize() == PageSize { LineCount(2), ColumnCount(3) });
    CHECK(grid.lineText(LineOffset(-2)) == "ABC");
    CHECK(grid.lineText(LineOffset(-1)) == "DE ");
    CHECK(grid.lineText(LineOffset(0)) == "abc");
    CHECK(grid.lineText(LineOffset(1)) == "de ");

    // 2x2
    (void) grid.resize(
        PageSize { LineCount(2), ColumnCount(2) }, CellLocation { .line = {}, .column = {} }, false);
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
        (void) grid.resize(
            PageSize { LineCount(2), ColumnCount(4) }, CellLocation { .line = {}, .column = {} }, false);
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

TEST_CASE("Grid.reflow.triple", "[grid]")
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
    (void) grid.resize(PageSize { LineCount(2), width + ColumnCount(1) }, CellLocation {}, false);
    // With SoA storage, all lines use LineSoA (no TrivialBuffer distinction).
    REQUIRE(grid.lineAt(LineOffset(0)).size() > ColumnCount(0));
    REQUIRE(grid.lineAt(LineOffset(1)).size() > ColumnCount(0));
    (void) grid.resize(PageSize { LineCount(2), width + ColumnCount(-1) }, CellLocation {}, false);
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
    auto lineTrivial = Line(LineFlag::None, trivial);
    grid.lineAt(LineOffset(0)) = lineTrivial;

    (void) grid.resize(PageSize { LineCount(3), ColumnCount(6) }, CellLocation {}, false);
    REQUIRE(grid.lineText(LineOffset(-1)) == "a a a ");
    REQUIRE(grid.lineText(LineOffset(0)) == "a     ");
    REQUIRE(grid.lineText(LineOffset(1)) == "      ");
    (void) grid.resize(PageSize { LineCount(3), ColumnCount(7) }, CellLocation {}, false);
    REQUIRE(grid.lineText(LineOffset(0)) == "a a a a");
    (void) grid.resize(PageSize { LineCount(3), ColumnCount(5) }, CellLocation {}, false);
    REQUIRE(grid.lineText(LineOffset(-1)) == "a a a");
    REQUIRE(grid.lineText(LineOffset(0)) == " a   ");
    REQUIRE(grid.lineText(LineOffset(1)) == "     ");
    (void) grid.resize(PageSize { LineCount(3), ColumnCount(4) }, CellLocation {}, false);
    REQUIRE(grid.lineText(LineOffset(-1)) == "a a ");
    REQUIRE(grid.lineText(LineOffset(0)) == "a a ");
    REQUIRE(grid.lineText(LineOffset(1)) == "    ");
    (void) grid.resize(PageSize { LineCount(3), ColumnCount(3) }, CellLocation {}, false);
    REQUIRE(grid.lineText(LineOffset(-2)) == "a a");
    REQUIRE(grid.lineText(LineOffset(-1)) == " a ");
    REQUIRE(grid.lineText(LineOffset(0)) == "a  ");
    REQUIRE(grid.lineText(LineOffset(1)) == "   ");
    (void) grid.resize(PageSize { LineCount(3), ColumnCount(7) }, CellLocation {}, false);
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

    void startLine(LineOffset y, [[maybe_unused]] LineFlags flags)
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

    (void) grid.resize(PageSize { LineCount(24), ColumnCount(100) }, CellLocation {}, false);
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

TEST_CASE("Grid.shrinkColumnsWrapsLongLine", "[grid][blank]")
{
    // A single 200-column line shrunk to 40 must produce 5 wrapped chunks with
    // LineFlag::Wrapped on continuations and original content preserved end-to-end.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(200) }, true, LineCount(50));
    auto const longText = std::string(200, 'A');
    grid.setLineText(LineOffset(0), longText);
    REQUIRE(grid.lineTextTrimmed(LineOffset(0)) == longText);

    (void) grid.resize(PageSize { LineCount(2), ColumnCount(40) }, CellLocation {}, false);
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

    (void) grid.resize(PageSize { LineCount(2), ColumnCount(40) }, CellLocation {}, false);
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

TEST_CASE("Grid.renderRange.staysAboveTheStableFloor", "[grid][capture][stable-id]")
{
    // renderRange must address exactly the rows the daemon's own snapshot walk does. At capacity a
    // reverse scroll wraps destroyed page rows into the oldest history slots WITHOUT resetting
    // them, which is why stableRangeFloor() exists — and a capture clamped to -historyLineCount()
    // returned those slots, so `capture-pane -S -` began with phantom rows no mirror ever held.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, false, LineCount(2));
    grid.setLineText(LineOffset(0), "AAAAA");
    grid.setLineText(LineOffset(1), "BBBBB");
    grid.scrollUp(LineCount(2));
    grid.setLineText(LineOffset(0), "CCCCC");
    grid.setLineText(LineOffset(1), "DDDDD");
    grid.scrollUp(LineCount(2));
    REQUIRE(grid.historyLineCount() == LineCount(2)); // the ring is full

    grid.scrollDown(LineCount(1), GraphicsAttributes {}, fullPageMargin(grid.pageSize()));

    // The floor now sits ABOVE base - historyLineCount(): one history slot holds a destroyed row.
    auto const floorOffset = grid.stableRangeFloor() - grid.stableLineIdOf(LineOffset(0));
    REQUIRE(floorOffset > -unbox<int64_t>(grid.historyLineCount()));

    auto valid = 0;
    grid.forEachValidLine([&](LineOffset, Line const&) { ++valid; });
    auto const captured = grid.renderRange(-boxed_cast<LineOffset>(grid.historyLineCount()),
                                           unbox<LineOffset>(grid.pageSize().lines) - LineOffset(1),
                                           CaptureRendition::PlainText,
                                           CaptureTrailingSpaces::Keep);
    CHECK(captured.size() == static_cast<std::size_t>(valid));
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
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(10) }, CellLocation {}, false);
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
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(10) }, CellLocation {}, false);
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(20) }, CellLocation {}, false);
        CHECK(reconstruct(grid) == text);
        soleHeadLine(grid, LineFlag::Marked);
        soleHeadLine(grid, LineFlag::OutputStart);
        soleHeadLine(grid, LineFlag::CommandEnd);
    }

    SECTION("a full round-trip leaves the line whole again, marks and all")
    {
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(10) }, CellLocation {}, false);
        (void) grid.resize(PageSize { LineCount(2), ColumnCount(30) }, CellLocation {}, false);
        CHECK(reconstruct(grid) == text);
        soleHeadLine(grid, LineFlag::Marked);
        soleHeadLine(grid, LineFlag::OutputStart);
        soleHeadLine(grid, LineFlag::CommandEnd);
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

TEST_CASE("Grid.stableId.reverseScrollRebuildsOnlyWhenSinkingBelowTheFloor", "[grid][stable-id]")
{
    auto grid = Grid(PageSize { LineCount(3), ColumnCount(5) }, false, LineCount(5));
    grid.scrollUp(LineCount(2)); // two history rows: base 2, floor 0
    auto const generationBefore = grid.generation();

    // Reverse-scrolling down TO the floor keeps row identity...
    grid.scrollDown(LineCount(2), GraphicsAttributes {}, fullPageMargin(grid.pageSize()));
    CHECK(grid.generation() == generationBefore);

    // ...but one more line sinks the base below it: identity is rebuilt wholesale.
    grid.scrollDown(LineCount(1), GraphicsAttributes {}, fullPageMargin(grid.pageSize()));
    CHECK(grid.generation() == generationBefore + 1);
    CHECK(grid.stableRangeFloor() == grid.stableLineIdOf(LineOffset(0)));
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

    std::ignore = grid.resize(PageSize { LineCount(2), ColumnCount(6) }, CellLocation {}, false);
    CHECK(grid.generation() == g0 + 1); // reflow rebuilds the whole ring

    grid.setMaxHistoryLineCount(LineCount(9));
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

    std::ignore = grid.resize(PageSize { LineCount(2), ColumnCount(7) }, CellLocation {}, false);

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

    std::ignore = grid.resize(PageSize { LineCount(2), ColumnCount(7) }, CellLocation {}, false);
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
    std::ignore = grid.resize(PageSize { LineCount(2), ColumnCount(3) }, CellLocation {}, false);
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

    std::ignore = grid.resize(PageSize { LineCount(2), ColumnCount(3) }, CellLocation {}, false);

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

    std::ignore = grid.resize(PageSize { LineCount(2), ColumnCount(6) }, CellLocation {}, false);

    CHECK(grid.stableRangeFloor()
          == grid.stableLineIdOf(LineOffset(0)) - unbox<int64_t>(grid.historyLineCount()));
    auto reported = 0;
    grid.forEachValidLine([&](LineOffset, Line const&) { ++reported; });
    CHECK(reported == unbox<int>(grid.historyLineCount()) + unbox<int>(grid.pageSize().lines));
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
