// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/render/RenderBuffer.hpp>

#include <vtrasterizer/GridMetrics.hpp>
#include <vtrasterizer/RailGeometry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace vtbackend;
using vtrasterizer::GridMetrics;
using vtrasterizer::GutterRailMetrics;
using vtrasterizer::RailRect;

namespace
{
/// 16x32 cells, the grid at x 200, and the canvas's rail at scale 2: a 44 px strip, a 6 px bar 16 px into
/// it, rounded by 4 and kept 4 clear of a block's ends.
[[nodiscard]] GridMetrics railMetrics()
{
    auto metrics = GridMetrics {};
    metrics.cellSize = ImageSize { Width(16), Height(32) };
    metrics.pageMargin = vtrasterizer::PageMargin { .left = 200, .top = 0, .bottom = 0 };
    metrics.rail = GutterRailMetrics { .strip = 44, .bar = 6, .barInset = 16, .radius = 4, .endInset = 4 };
    return metrics;
}

/// A rail cell drawing @p segment on screen row @p line, with @p mark on it.
[[nodiscard]] RenderGutterCell railCell(int line, RailSegment segment, RailMark mark = RailMark::None)
{
    return RenderGutterCell { .lineOffset = LineOffset(line),
                              .codepoint = 0,
                              .attributes = {},
                              .column = ColumnOffset(0),
                              .rail = segment,
                              .mark = mark,
                              .outcome = CommandBlockOutcome::Failure,
                              .railColor = RGBColor(0xFF, 0x45, 0x3A) };
}

constexpr auto BarX = 200 - 44 + 16;
} // namespace

TEST_CASE("RailGeometry.aHeadIsRoundedAtTheTop", "[renderer][rail]")
{
    auto const metrics = railMetrics();

    // The bar starts endInset below the row's top and runs on into the next row; one cap row, a pixel
    // narrower each side, rounds its top.
    CHECK(vtrasterizer::railRects(railCell(3, RailSegment::Head), metrics, 0)
          == std::vector { RailRect { .x = BarX, .y = (3 * 32) + 4, .width = 6, .height = 28 },
                           RailRect { .x = BarX + 1, .y = (3 * 32) + 3, .width = 4, .height = 1 } });

    // A body row is the bar through the whole row, square.
    CHECK(vtrasterizer::railRects(railCell(3, RailSegment::Body), metrics, 0)
          == std::vector { RailRect { .x = BarX, .y = 3 * 32, .width = 6, .height = 32 } });

    // A tail ends endInset above the row's bottom, rounded there.
    CHECK(vtrasterizer::railRects(railCell(3, RailSegment::Tail), metrics, 0)
          == std::vector { RailRect { .x = BarX, .y = 3 * 32, .width = 6, .height = 28 },
                           RailRect { .x = BarX + 1, .y = (3 * 32) + 28, .width = 4, .height = 1 } });

    // A single row is kept clear of both ends, and rounded at both.
    CHECK(vtrasterizer::railRects(railCell(3, RailSegment::Single), metrics, 0)
          == std::vector { RailRect { .x = BarX, .y = (3 * 32) + 4, .width = 6, .height = 24 },
                           RailRect { .x = BarX + 1, .y = (3 * 32) + 3, .width = 4, .height = 1 },
                           RailRect { .x = BarX + 1, .y = (3 * 32) + 28, .width = 4, .height = 1 } });

    // The smooth scroll moves it with the rows.
    CHECK(vtrasterizer::railRects(railCell(3, RailSegment::Body), metrics, -5).front().y == (3 * 32) - 5);

    // No segment, no bar; and a mark on the row stands in for the bar there.
    CHECK(vtrasterizer::railRects(railCell(3, RailSegment::None, RailMark::UserMark), metrics, 0).empty());
    CHECK(vtrasterizer::railRects(railCell(3, RailSegment::Head, RailMark::Cross), metrics, 0).empty());
}

TEST_CASE("RailGeometry.theCellsMoveLeftOfTheStrip", "[renderer][rail]")
{
    // Review Focus 5: the lead-in, the timestamps and line numbers, then the strip against column 0.
    auto const metrics = railMetrics();
    CHECK(vtrasterizer::cellGutterShift(metrics) == 44);
    auto const shifted = vtrasterizer::shiftedLeft(metrics, vtrasterizer::cellGutterShift(metrics));
    CHECK(shifted.mapTopLeft(LineOffset(0), ColumnOffset(-1)).x == 200 - 44 - 16);
}

TEST_CASE("RailGeometry.theMarkIsCentredOnTheBar", "[renderer][rail]")
{
    auto const metrics = railMetrics();
    auto const box = vtrasterizer::railMarkBox(railCell(3, RailSegment::Head, RailMark::Cross), metrics, 0);
    CHECK(box.x + (box.width / 2) == BarX + 3);
    CHECK(box.width == 2 * 16); // two cells, as a two-cell block column's glyph
    CHECK(box.y == 3 * 32);     // top-aligned with the row
    CHECK(box.height == 32);

    // Column 0 of a grid moved left by the mark's shift is that box.
    auto const shifted = vtrasterizer::shiftedLeft(metrics, vtrasterizer::railMarkShift(metrics));
    CHECK(shifted.mapTopLeft(LineOffset(3), ColumnOffset(0)).x == box.x);
}

TEST_CASE("RailGeometry.noStripNoChange", "[renderer][rail]")
{
    auto metrics = railMetrics();
    metrics.rail = GutterRailMetrics {};
    CHECK(vtrasterizer::cellGutterShift(metrics) == 0);
}

TEST_CASE("RailGeometry.eachMarkHasItsGlyph", "[renderer][rail]")
{
    using vtrasterizer::railMarkCodepoint;
    CHECK(railMarkCodepoint(RailMark::None) == 0);
    CHECK(railMarkCodepoint(RailMark::Check) == 0x10F010);
    // The failure disc, with the cross knocked out of it: the glyph column's bare cross stays U+10F005.
    CHECK(railMarkCodepoint(RailMark::Cross) == 0x10F011);
    CHECK(railMarkCodepoint(RailMark::Running) == 0x10F002);
    CHECK(railMarkCodepoint(RailMark::UserMark) == 0x10F003);
}
