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

TEST_CASE("GridMetrics.logicalPointsToPixels.isOneLogicalPointAtTheContentScale", "[gridmetrics]")
{
    // A logical point is Qt's device-independent pixel: Contour hands the renderer 96 DPI per unit of content
    // scale, so one point is one device pixel at 1x (96), two at 2x (192), and 1.5x (144) rounds up to two.
    STATIC_REQUIRE(vtrasterizer::logicalPointsToPixels(1.0, 96.0) == 1);
    STATIC_REQUIRE(vtrasterizer::logicalPointsToPixels(1.0, 144.0) == 2);
    STATIC_REQUIRE(vtrasterizer::logicalPointsToPixels(1.0, 192.0) == 2);
    STATIC_REQUIRE(vtrasterizer::logicalPointsToPixels(1.0, 288.0) == 3);
    // Never less than a pixel: below 1x, and with no DPI yet known.
    STATIC_REQUIRE(vtrasterizer::logicalPointsToPixels(1.0, 72.0) == 1);
    STATIC_REQUIRE(vtrasterizer::logicalPointsToPixels(1.0, 0.0) == 1);
}
