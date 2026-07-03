// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the IME cursor-rectangle math (Qt::ImCursorRectangle): the rectangle is expressed
// in item-local LOGICAL coordinates, derived from device-pixel grid metrics (page-margin inset +
// cell-sized cursor cell), divided by the device-pixel ratio, and widened for double-width glyphs.

#include <contour/display/ImeQueryRect.h>

#include <catch2/catch_test_macros.hpp>

using contour::display::imeCursorRectangle;
using vtbackend::CellLocation;
using vtbackend::ColumnOffset;
using vtbackend::LineOffset;

namespace
{

constexpr vtbackend::ImageSize cellSize(unsigned widthPx, unsigned heightPx) noexcept
{
    return { .width = vtbackend::Width(widthPx), .height = vtbackend::Height(heightPx) };
}

constexpr CellLocation cursorAt(int line, int column) noexcept
{
    return { .line = LineOffset(line), .column = ColumnOffset(column) };
}

} // namespace

TEST_CASE("imeCursorRectangle.grid origin lands at the page-margin inset", "[contour][ime]")
{
    auto const margin = vtrasterizer::PageMargin { .left = 10, .top = 6, .bottom = 0 };
    auto const rect = imeCursorRectangle(margin, cellSize(8, 16), cursorAt(0, 0), 1, 1.0);
    CHECK(rect == QRectF(10, 6, 8, 16));
}

TEST_CASE("imeCursorRectangle.cursor offsets scale by the cell size", "[contour][ime]")
{
    auto const margin = vtrasterizer::PageMargin { .left = 0, .top = 0, .bottom = 0 };
    auto const rect = imeCursorRectangle(margin, cellSize(8, 16), cursorAt(3, 5), 1, 1.0);
    CHECK(rect == QRectF(5 * 8, 3 * 16, 8, 16));
}

TEST_CASE("imeCursorRectangle.device pixels divide by the DPR", "[contour][ime]")
{
    // Everything (margin, cell offsets, cell extent) is device-pixel; the result is logical.
    auto const margin = vtrasterizer::PageMargin { .left = 10, .top = 6, .bottom = 0 };
    auto const rect = imeCursorRectangle(margin, cellSize(14, 28), cursorAt(2, 4), 1, 2.0);
    CHECK(rect == QRectF((10 + (4 * 14)) / 2.0, (6 + (2 * 28)) / 2.0, 14 / 2.0, 28 / 2.0));
}

TEST_CASE("imeCursorRectangle.double-width cell widens the rect only", "[contour][ime]")
{
    auto const margin = vtrasterizer::PageMargin { .left = 4, .top = 2, .bottom = 0 };
    auto const narrow = imeCursorRectangle(margin, cellSize(9, 21), cursorAt(1, 7), 1, 1.5);
    auto const wide = imeCursorRectangle(margin, cellSize(9, 21), cursorAt(1, 7), 2, 1.5);
    CHECK(wide.topLeft() == narrow.topLeft());
    CHECK(wide.height() == narrow.height());
    CHECK(wide.width() == 2 * narrow.width());
}
