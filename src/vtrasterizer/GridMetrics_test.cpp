// SPDX-License-Identifier: Apache-2.0
//
// The cell size has to be a whole number in BOTH units. Device pixels are what the renderer draws
// in; logical pixels are what applications are told about, and an application that is told a cell it
// cannot represent drops the fraction on every column. The gap that leaves is not subtle: measured
// at ~5% of a 150-column window.

#include <vtrasterizer/GridMetrics.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <cmath>

using namespace vtrasterizer;
using Catch::Approx;

// An unscaled display already measures cells in whole logical pixels; nothing to snap.
static_assert(snapToWholeLogicalPixel(19, 1.0) == 19);
static_assert(snapToWholeLogicalPixel(13, 1.0) == 13);

// The measured case. A 19x44 cell at scale 2 is 9.5x22 logical: the height divides, the width does
// not -- which is exactly why the plasma spanned vertically but stopped ~5% short horizontally.
static_assert(snapToWholeLogicalPixel(19, 2.0) == 20);
static_assert(snapToWholeLogicalPixel(44, 2.0) == 44);

// Already whole stays put -- snapping must not creep the cell wider on every font reload.
static_assert(snapToWholeLogicalPixel(20, 2.0) == 20);
static_assert(snapToWholeLogicalPixel(10, 2.0) == 10);

// A fractional scale quantizes coarsely, and that is inherent rather than a bug: whole logical
// pixels at 1.75 (= 7/4) means the cell must be a multiple of 7 device pixels.
static_assert(snapToWholeLogicalPixel(19, 1.75) == 21);
static_assert(snapToWholeLogicalPixel(21, 1.75) == 21);
static_assert(snapToWholeLogicalPixel(14, 1.75) == 14);

// 1.5 (= 3/2) needs multiples of 3; 1.25 (= 5/4) needs multiples of 5.
static_assert(snapToWholeLogicalPixel(19, 1.5) == 21);
static_assert(snapToWholeLogicalPixel(19, 1.25) == 20);

// Degenerate inputs return the extent rather than collapsing it.
static_assert(snapToWholeLogicalPixel(0, 2.0) == 0);
static_assert(snapToWholeLogicalPixel(-3, 2.0) == -3);
static_assert(snapToWholeLogicalPixel(19, 0.0) == 19);

TEST_CASE("GridMetrics.snapToWholeLogicalPixel.isWholeInBothUnits", "[vtrasterizer]")
{
    // The property the whole thing exists for: after snapping, dividing by the scale lands on a
    // whole number, so an application's own integer division recovers the cell exactly and
    // columns * cell tiles the text area with nothing left over.
    auto const scale = GENERATE(1.0, 1.25, 1.5, 1.75, 2.0, 3.0);
    auto const extent = GENERATE(range(4, 60));
    CAPTURE(scale, extent);

    auto const snapped = snapToWholeLogicalPixel(extent, scale);

    CHECK(snapped >= extent); // rounds up: a glyph's own advance still fits the cell
    CHECK(snapped - extent < 8);

    auto const logical = static_cast<double>(snapped) / scale;
    CHECK(logical == Approx(std::round(logical)).margin(1e-6));

    // And what an application actually does with it: floor the cell, tile the columns, and land on
    // exactly the area it was told about.
    auto const reportedCell = static_cast<int>(logical + 1e-6);
    for (auto const columns: { 1, 80, 143, 150, 240 })
    {
        CAPTURE(columns);
        CHECK(reportedCell * columns * scale == Approx(snapped * columns).margin(1e-6));
    }
}

TEST_CASE("GridMetrics.snapToWholeLogicalPixel.isIdempotent", "[vtrasterizer]")
{
    // Font metrics are reloaded on every DPI change and config reload, and each reload re-snaps the
    // value the last one produced. If snapping were not idempotent the cell would creep wider every
    // time the user moved the window between screens.
    auto const scale = GENERATE(1.0, 1.25, 1.5, 1.75, 2.0, 3.0);
    auto const extent = GENERATE(range(4, 60));
    CAPTURE(scale, extent);

    auto const once = snapToWholeLogicalPixel(extent, scale);
    CHECK(snapToWholeLogicalPixel(once, scale) == once);
}
