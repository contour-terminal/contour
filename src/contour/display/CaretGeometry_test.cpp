// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the caret/prompt geometry an assistive client is handed. The cell grid renders in DEVICE
// pixels inset by the page margin, while Qt speaks logical coordinates -- so the device-pixel ratio is
// divided out exactly once, on the way out. Scaling again when lifting into screen coordinates is the
// classic HiDPI double-scale bug, and is pinned against below.

#include <contour/display/CaretGeometry.h>

#include <catch2/catch_test_macros.hpp>

using contour::display::cellRectangle;
using contour::display::rowBandRectangle;
using contour::display::toGlobalRect;
using vtbackend::CellLocation;
using vtbackend::ColumnCount;
using vtbackend::ColumnOffset;
using vtbackend::LineOffset;

namespace
{

constexpr vtbackend::ImageSize cellSize(unsigned widthPx, unsigned heightPx) noexcept
{
    return { .width = vtbackend::Width(widthPx), .height = vtbackend::Height(heightPx) };
}

constexpr CellLocation cell(int line, int column) noexcept
{
    return { .line = LineOffset(line), .column = ColumnOffset(column) };
}

} // namespace

TEST_CASE("CaretGeometry.rowBandRectangle spans whole rows at full width", "[contour][a11y]")
{
    auto const margin = vtrasterizer::PageMargin { .left = 4, .top = 6, .bottom = 0 };
    auto const band =
        rowBandRectangle(margin, cellSize(8, 16), LineOffset(1), LineOffset(3), ColumnCount(10), 1.0);

    CHECK(band.left() == 4.0);
    CHECK(band.top() == 6.0 + 16.0);
    CHECK(band.width() == 80.0);
    // Rows 1..3 inclusive is three rows, not two: an inclusive range that reported two would clip the
    // last line of every multi-line prompt.
    CHECK(band.height() == 48.0);
}

TEST_CASE("CaretGeometry.a single-row band equals that row's full width", "[contour][a11y]")
{
    auto const margin = vtrasterizer::PageMargin { .left = 0, .top = 0, .bottom = 0 };
    auto const band =
        rowBandRectangle(margin, cellSize(8, 16), LineOffset(2), LineOffset(2), ColumnCount(5), 1.0);

    CHECK(band.top() == 32.0);
    CHECK(band.height() == 16.0);
    CHECK(band.width() == 40.0);
}

TEST_CASE("CaretGeometry.a reversed band is clamped rather than inverted", "[contour][a11y]")
{
    // A prompt whose last line is above its first is nonsense, but a negative height would be worse: Qt
    // treats it as an empty or mirrored rect and the client would point at nothing.
    auto const margin = vtrasterizer::PageMargin { .left = 0, .top = 0, .bottom = 0 };
    auto const band =
        rowBandRectangle(margin, cellSize(8, 16), LineOffset(4), LineOffset(1), ColumnCount(5), 1.0);

    CHECK(band.height() == 16.0);
}

TEST_CASE("CaretGeometry.device pixels divide by the DPR exactly once", "[contour][a11y]")
{
    auto const margin = vtrasterizer::PageMargin { .left = 10, .top = 20, .bottom = 0 };
    auto const band =
        rowBandRectangle(margin, cellSize(16, 32), LineOffset(1), LineOffset(1), ColumnCount(4), 2.0);

    CHECK(band.left() == 5.0);
    CHECK(band.top() == (20.0 + 32.0) / 2.0);
    CHECK(band.width() == 32.0);
    CHECK(band.height() == 16.0);
}

TEST_CASE("CaretGeometry.toGlobalRect translates without rescaling", "[contour][a11y]")
{
    // The DPR was already divided out upstream. Applying it again here -- the obvious-looking "convert to
    // device pixels for the screen" step -- puts the caret at twice its distance from the window origin
    // on every HiDPI display.
    auto const itemLocal = QRectF { 10.0, 20.0, 8.0, 16.0 };
    auto const global = toGlobalRect(itemLocal, QPointF { 100.0, 200.0 });

    CHECK(global.x() == 110);
    CHECK(global.y() == 220);
    CHECK(global.width() == 8);
    CHECK(global.height() == 16);
}

TEST_CASE("CaretGeometry.toGlobalRect at the origin is the identity", "[contour][a11y]")
{
    auto const itemLocal = QRectF { 3.0, 4.0, 8.0, 16.0 };
    auto const global = toGlobalRect(itemLocal, QPointF { 0.0, 0.0 });

    CHECK(global.x() == 3);
    CHECK(global.y() == 4);
}

TEST_CASE("CaretGeometry.cellRectangle still answers the IME question", "[contour][a11y]")
{
    // ImeQueryRect.h is now a thin alias over this; the shared geometry must keep behaving identically.
    auto const margin = vtrasterizer::PageMargin { .left = 4, .top = 6, .bottom = 0 };
    auto const rect = cellRectangle(margin, cellSize(8, 16), cell(3, 5), 1, 1.0);

    CHECK(rect.left() == 4.0 + (5.0 * 8.0));
    CHECK(rect.top() == 6.0 + (3.0 * 16.0));
    CHECK(rect.width() == 8.0);

    // A double-width glyph widens the rect and nothing else.
    auto const wide = cellRectangle(margin, cellSize(8, 16), cell(3, 5), 2, 1.0);
    CHECK(wide.width() == 16.0);
    CHECK(wide.height() == rect.height());
    CHECK(wide.left() == rect.left());
}
