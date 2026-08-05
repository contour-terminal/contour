// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the IME cursor-rectangle math (Qt::ImCursorRectangle): the rectangle is expressed
// in item-local LOGICAL coordinates, derived from device-pixel grid metrics (page-margin inset +
// cell-sized cursor cell), divided by the device-pixel ratio, and widened for double-width glyphs.

#include <contour/display/ImeQueryRect.h>

#include <catch2/catch_test_macros.hpp>

using contour::display::imeCursorAddressable;
using contour::display::imeCursorRectangle;
using vtbackend::CellLocation;
using vtbackend::ColumnCount;
using vtbackend::ColumnOffset;
using vtbackend::LineCount;
using vtbackend::LineOffset;
using vtbackend::PageSize;

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

namespace
{

constexpr PageSize page(int lines, int columns) noexcept
{
    return { .lines = LineCount(lines), .columns = ColumnCount(columns) };
}

} // namespace

TEST_CASE("imeCursorAddressable.every interior cell is addressable", "[contour][ime]")
{
    CHECK(imeCursorAddressable(cursorAt(0, 0), page(24, 80)));
    CHECK(imeCursorAddressable(cursorAt(12, 40), page(24, 80)));
    CHECK(imeCursorAddressable(cursorAt(23, 79), page(24, 80))); // last cell of the page
}

TEST_CASE("imeCursorAddressable.a cursor beyond a shrunk page is refused", "[contour][ime]")
{
    // A cursor captured against a taller page must never index a grid that has since shrunk:
    // resizing reallocates the grid's lines, so a stale line offset dereferences freed storage.
    CHECK(imeCursorAddressable(cursorAt(37, 0), page(40, 80)));
    CHECK_FALSE(imeCursorAddressable(cursorAt(37, 0), page(30, 80)));
    CHECK_FALSE(imeCursorAddressable(cursorAt(0, 100), page(24, 80)));
}

TEST_CASE("imeCursorAddressable.the wrap-pending sentinel column names no cell", "[contour][ime]")
{
    // Terminal::contains() admits column == page width as an off-by-one sentinel; grid access at
    // that column would still be out of bounds, so the IME guard is strict.
    CHECK_FALSE(imeCursorAddressable(cursorAt(0, 80), page(24, 80)));
    CHECK_FALSE(imeCursorAddressable(cursorAt(24, 0), page(24, 80)));
}

TEST_CASE("imeCursorAddressable.negative coordinates are refused", "[contour][ime]")
{
    // The lexicographic CellLocation ordering alone would admit {0, -1} — the guard must not.
    CHECK_FALSE(imeCursorAddressable(cursorAt(-1, 0), page(24, 80)));
    CHECK_FALSE(imeCursorAddressable(cursorAt(0, -1), page(24, 80)));
}
