// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the curly underline's geometry -- the decision issue #1754 got wrong.

#include <vtrasterizer/UnderlineGeometry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <ranges>

using namespace vtrasterizer;

namespace
{

/// The topmost painted row, bottom-origin. A tile row @c y sits at cell-bottom offset `y + 1`, so
/// this being `tileHeight - 1` is what puts the crest at the underline position.
[[nodiscard]] constexpr int crestRow(CurlyUnderlineGeometry const& g) noexcept
{
    return g.centerY + g.amplitude + g.strokeRadius;
}

/// The bottom-most painted row, bottom-origin.
[[nodiscard]] constexpr int troughRow(CurlyUnderlineGeometry const& g) noexcept
{
    return g.centerY - g.amplitude - g.strokeRadius;
}

/// How many rows the wave paints in total.
[[nodiscard]] constexpr int bandHeight(CurlyUnderlineGeometry const& g) noexcept
{
    return crestRow(g) - troughRow(g) + 1;
}

} // namespace

TEST_CASE("UnderlineGeometry.nimbusMonoPS", "[underlinegeometry]")
{
    // The font from #1754, at 12pt/96dpi: cell 20px tall, baseline 10px above the cell bottom (of
    // which 4px is line gap, not descent), underline position 9, thickness 1.
    auto const geometry = curlyUnderlineGeometry(/*position*/ 9, /*thickness*/ 1, /*cellHeight*/ 20);

    CHECK(geometry.tileHeight == 9);
    CHECK(geometry.centerY == 6);
    CHECK(geometry.amplitude == 2);
    CHECK(geometry.strokeRadius == 0);

    // Five rows spanning cell-bottom offsets 5..9 -- against the nine rows ending ON the baseline
    // that the pre-fix code produced, in a cell whose x-height is about seven pixels.
    CHECK(bandHeight(geometry) == 5);
    CHECK(crestRow(geometry) + 1 == 9); // the row a straight underline's top occupies
}

TEST_CASE("UnderlineGeometry.jetBrainsMono", "[underlinegeometry]")
{
    // A font with no line gap: baseline 5, underline position 3. The issue reports this one as
    // already looking right, so the fix must not change its character.
    auto const geometry = curlyUnderlineGeometry(/*position*/ 3, /*thickness*/ 1, /*cellHeight*/ 22);

    CHECK(geometry.tileHeight == 3);
    CHECK(geometry.amplitude == 1);
    CHECK(bandHeight(geometry) == 3);
}

TEST_CASE("UnderlineGeometry.crestSitsWhereAStraightUnderlineDoes", "[underlinegeometry]")
{
    // The property that keeps the curl off the text: its top row is the straight underline's top
    // row, so toggling SGR 4:1 and 4:3 keeps the same top edge and neither reaches the baseline.
    for (auto const position: std::views::iota(3, 40))
    {
        auto const geometry = curlyUnderlineGeometry(position, 1, 64);
        INFO("underline position " << position);
        CHECK(geometry.tileHeight == position);
        CHECK(crestRow(geometry) == geometry.tileHeight - 1);
    }
}

TEST_CASE("UnderlineGeometry.amplitudeIsBoundedByTheRoomBelowTheUnderline", "[underlinegeometry]")
{
    // The defect itself: the wave used to be half the descender tall. It must instead be a quarter
    // of the room beneath the underline position, so the band is about half that room -- or the
    // three-row minimum where the room is too shallow even for that, since a crest, a middle and a
    // trough are what make a curl a curl.
    for (auto const position: std::views::iota(1, 60))
    {
        auto const geometry = curlyUnderlineGeometry(position, 1, 64);
        INFO("underline position " << position);
        CHECK(geometry.amplitude <= std::max(1, position / 4));
        CHECK(bandHeight(geometry) <= std::max(3, (position / 2) + 1));
    }
}

TEST_CASE("UnderlineGeometry.strokeFollowsUnderlineThickness", "[underlinegeometry]")
{
    // The stroke used to be spread along x, so thickness never reached the wave. Deep enough room
    // that the thickness is what decides, not the clamps.
    CHECK(curlyUnderlineGeometry(30, 1, 64).strokeRadius == 0);
    CHECK(curlyUnderlineGeometry(30, 2, 64).strokeRadius == 0);
    CHECK(curlyUnderlineGeometry(30, 3, 64).strokeRadius == 1);
    CHECK(curlyUnderlineGeometry(30, 5, 64).strokeRadius == 2);
    CHECK(curlyUnderlineGeometry(30, 7, 64).strokeRadius == 3);

    // A thickness of zero still draws: a stroke that is not painted cannot be seen.
    CHECK(curlyUnderlineGeometry(30, 0, 64).strokeRadius == 0);
    CHECK(bandHeight(curlyUnderlineGeometry(30, 0, 64)) >= 3);
}

TEST_CASE("UnderlineGeometry.everyBandFitsItsTile", "[underlinegeometry]")
{
    // Pixmap::paint silently drops out-of-range rows, so a geometry that does not fit its tile
    // loses part of the curl without any diagnostic. Swept over the degenerate corners too: a
    // position outside the cell, a thickness larger than the cell, a one-pixel cell.
    for (auto const cellHeight: { 1, 2, 3, 4, 8, 16, 20, 64 })
    {
        for (auto const position: std::views::iota(-2, 24))
        {
            for (auto const thickness: { -1, 0, 1, 2, 3, 8, 40 })
            {
                auto const geometry = curlyUnderlineGeometry(position, thickness, cellHeight);
                INFO("cell " << cellHeight << " position " << position << " thickness " << thickness);

                CHECK(geometry.tileHeight >= 1);
                CHECK(geometry.tileHeight <= cellHeight);
                CHECK(geometry.amplitude >= 0);
                CHECK(geometry.strokeRadius >= 0);
                CHECK(troughRow(geometry) >= 0);
                CHECK(crestRow(geometry) == geometry.tileHeight - 1);
            }
        }
    }
}
