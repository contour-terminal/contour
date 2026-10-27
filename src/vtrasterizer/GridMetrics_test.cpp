// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/GridMetrics.hpp>

#include <catch2/catch_test_macros.hpp>

using vtbackend::CellLocation;
using vtbackend::ColumnOffset;
using vtbackend::FractionalCellLocation;
using vtbackend::LineOffset;

TEST_CASE("GridMetrics.map.aPositionBetweenCellsMapsBetweenTheirPixels", "[vtrasterizer]")
{
    // A cursor moving between two cells is drawn part of the way along: its position maps as a cell's top
    // left does, and part of the way to the next cell is as much of the way to that cell's pixels.
    auto metrics = vtrasterizer::GridMetrics {};
    metrics.cellSize = vtbackend::ImageSize { vtbackend::Width(10), vtbackend::Height(20) };
    metrics.pageMargin = vtrasterizer::PageMargin { .left = 3, .top = 5, .bottom = 0 };

    CHECK(metrics.map(FractionalCellLocation { .line = 2.0f, .column = 4.0f })
          == metrics.map(CellLocation { .line = LineOffset(2), .column = ColumnOffset(4) }));
    CHECK(metrics.map(FractionalCellLocation { .line = 2.5f, .column = 4.75f }, 7)
          == crispy::Point { .x = 3 + 48, .y = 5 + 50 + 7 }); // 47.5 pixels to the nearest
}

TEST_CASE("GridMetrics.pointsToPixels.isOnePointAtTheDpiAndNeverLessThanAPixel", "[gridmetrics]")
{
    // One point is 1/72 inch: a pixel at 72 DPI, two on a 2x display (144), three at 216. At 96 DPI it
    // rounds to one, and below 72 DPI a point still draws a whole pixel rather than none.
    STATIC_REQUIRE(vtrasterizer::pointsToPixels(1.0, 72.0) == 1);
    STATIC_REQUIRE(vtrasterizer::pointsToPixels(1.0, 144.0) == 2);
    STATIC_REQUIRE(vtrasterizer::pointsToPixels(1.0, 96.0) == 1);
    STATIC_REQUIRE(vtrasterizer::pointsToPixels(1.0, 216.0) == 3);
    STATIC_REQUIRE(vtrasterizer::pointsToPixels(1.0, 36.0) == 1);
}
