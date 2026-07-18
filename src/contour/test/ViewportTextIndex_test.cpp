// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the grid <-> flat-offset mapping. Assistive technology addresses text by a single
// integer offset; a terminal is a grid. Every row occupies one offset MORE than it has columns -- the
// newline joining it to the row below -- which is what makes both directions plain division and modulo.

#include <contour/display/ViewportTextIndex.h>

#include <catch2/catch_test_macros.hpp>

using contour::display::cellAtFlatOffset;
using contour::display::flatOffsetOf;
using contour::display::flatTextLength;
using vtbackend::CellLocation;
using vtbackend::ColumnCount;
using vtbackend::ColumnOffset;
using vtbackend::LineCount;
using vtbackend::LineOffset;
using vtbackend::PageSize;

namespace
{

constexpr CellLocation cell(int line, int column) noexcept
{
    return { .line = LineOffset(line), .column = ColumnOffset(column) };
}

} // namespace

TEST_CASE("ViewportTextIndex.the grid origin is offset zero", "[contour][a11y]")
{
    CHECK(flatOffsetOf(cell(0, 0), ColumnCount(80)) == 0);
}

TEST_CASE("ViewportTextIndex.a row costs one offset more than its columns", "[contour][a11y]")
{
    // The extra one is the newline. Without it two adjacent rows would share offsets and a caret at the
    // end of a line would be indistinguishable from one at the start of the next.
    CHECK(flatOffsetOf(cell(0, 9), ColumnCount(10)) == 9);
    CHECK(flatOffsetOf(cell(1, 0), ColumnCount(10)) == 11);
    CHECK(flatOffsetOf(cell(2, 3), ColumnCount(10)) == 25);
}

TEST_CASE("ViewportTextIndex.the mapping round-trips across the grid", "[contour][a11y]")
{
    auto constexpr Columns = ColumnCount(7);
    for (auto line = 0; line < 5; ++line)
    {
        for (auto column = 0; column < 7; ++column)
        {
            auto const original = cell(line, column);
            auto const roundTripped = cellAtFlatOffset(flatOffsetOf(original, Columns), Columns);
            CHECK(roundTripped.line == original.line);
            CHECK(roundTripped.column == original.column);
        }
    }
}

TEST_CASE("ViewportTextIndex.an offset on a newline is the column past the end", "[contour][a11y]")
{
    // Where a caret sitting at the end of a line belongs.
    auto const atNewline = cellAtFlatOffset(10, ColumnCount(10));
    CHECK(atNewline.line == LineOffset(0));
    CHECK(atNewline.column == ColumnOffset(10));

    // ... and the very next offset is the start of the row below.
    auto const nextRow = cellAtFlatOffset(11, ColumnCount(10));
    CHECK(nextRow.line == LineOffset(1));
    CHECK(nextRow.column == ColumnOffset(0));
}

TEST_CASE("ViewportTextIndex.the length is one past the last valid offset", "[contour][a11y]")
{
    auto constexpr Page = PageSize { .lines = LineCount(3), .columns = ColumnCount(10) };

    CHECK(flatTextLength(Page) == 33);

    // The last addressable cell sits inside that length ...
    CHECK(flatOffsetOf(cell(2, 9), Page.columns) < flatTextLength(Page));
    // ... and so does the newline closing the final row, which is what makes every row equal width in
    // offset space.
    CHECK(flatOffsetOf(cell(2, 10), Page.columns) == flatTextLength(Page) - 1);
}

TEST_CASE("ViewportTextIndex.a single-column grid is not a special case", "[contour][a11y]")
{
    CHECK(flatOffsetOf(cell(0, 0), ColumnCount(1)) == 0);
    CHECK(flatOffsetOf(cell(1, 0), ColumnCount(1)) == 2);
    CHECK(cellAtFlatOffset(2, ColumnCount(1)).line == LineOffset(1));
}
