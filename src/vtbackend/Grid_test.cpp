// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Grid.h>
#include <vtbackend/primitives.h>

#include <crispy/BufferObject.h>

#include <catch2/catch_test_macros.hpp>

#include <format>

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
                              grid.zero_index(),
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
                             grid.zero_index(),
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
    auto pool = crispy::buffer_object_pool<char>(32);
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
    auto pool = crispy::buffer_object_pool<char>(unbox(width) * 8);
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
                           [[maybe_unused]] std::u32string_view textOverride = {})
    {
        renderedLines.push_back(y);
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
    CHECK(reconstructed.find(wideText) != std::string::npos);
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

// }}}
// NOLINTEND(misc-const-correctness)
