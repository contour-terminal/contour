// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/render/RenderBuffer.hpp>

#include <vtrasterizer/GutterCells.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace vtbackend;
using vtrasterizer::gutterRenderCell;

TEST_CASE("GutterCells.aGutterCellIsDrawnAtItsOwnColumn", "[renderer][gutter]")
{
    // A multi-column gutter is the same grid at negative columns: column -4 is four cells left of
    // column 0, through the very metrics every grid cell is placed with.
    auto const marker = RenderGutterCell {
        .lineOffset = LineOffset(3), .codepoint = U'7', .attributes = {}, .column = ColumnOffset(-4)
    };
    auto const cell = gutterRenderCell(marker);
    CHECK(cell.position.line == LineOffset(3));
    CHECK(cell.position.column == ColumnOffset(-4));
    CHECK(cell.codepoints == U"7");
    CHECK(cell.width == 1);
    // Its own shaping group, so it is placed at its own pen position.
    CHECK(cell.groupStart);
    CHECK(cell.groupEnd);
}

TEST_CASE("GutterCells.aCellThatNamesNoColumnSitsBesideTheGrid", "[renderer][gutter]")
{
    auto const cell = gutterRenderCell(
        RenderGutterCell { .lineOffset = LineOffset(0), .codepoint = U'x', .attributes = {} });
    CHECK(cell.position.column == ColumnOffset(-1));
}

TEST_CASE("GutterCells.clearingABufferClearsItsAnnotations", "[renderer][gutter]")
{
    auto buffer = RenderBuffer {};
    layoutRenderText(buffer.annotations, U"x", RenderAttributes {}, CellLocation {}, ColumnOffset(10));
    REQUIRE_FALSE(buffer.annotations.empty());
    buffer.clear();
    CHECK(buffer.annotations.empty());
}
