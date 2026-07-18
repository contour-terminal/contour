// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the glyph scaling strategy used by the kitty text sizing protocol.

#include <vtrasterizer/GlyphScaling.h>

#include <catch2/catch_test_macros.hpp>

using namespace vtrasterizer;
using vtbackend::CellScale;

namespace
{
// One representative cell geometry, named so the arithmetic below reads as arithmetic.
constexpr auto H = 22; // cell height
constexpr auto W = 10; // cell width
constexpr auto B = 4;  // baseline above the cell bottom
constexpr auto T = 14; // glyph ink top above its baseline

[[nodiscard]] constexpr CellScale whole(uint8_t scale) noexcept
{
    return CellScale { .scale = scale };
}

[[nodiscard]] constexpr CellScale fractional(uint8_t scale,
                                             uint8_t numerator,
                                             uint8_t denominator,
                                             uint8_t verticalAlignment = 0,
                                             uint8_t horizontalAlignment = 0) noexcept
{
    return CellScale { .scale = scale,
                       .numerator = numerator,
                       .denominator = denominator,
                       .verticalAlignment = verticalAlignment,
                       .horizontalAlignment = horizontalAlignment };
}
} // namespace

TEST_CASE("CellScale.drawFactor", "[glyphscaling]")
{
    // The block's extent in cells comes from `s`; the fraction changes only how large the glyph is
    // DRAWN. An IMPROPER fraction is not a shrink and is ignored -- kitty's effective_scale agrees.
    CHECK(whole(1).drawFactor() == 1.0);
    CHECK(whole(3).drawFactor() == 3.0);
    CHECK(fractional(3, 1, 3).drawFactor() == 1.0); // 3 * 1/3 -- ordinary size in a 3-cell block
    CHECK(fractional(4, 1, 2).drawFactor() == 2.0);
    CHECK(fractional(3, 3, 3).drawFactor() == 3.0); // n == d: not a shrink
    CHECK(fractional(3, 4, 3).drawFactor() == 3.0); // n > d: not a shrink
    CHECK(fractional(3, 1, 0).drawFactor() == 3.0); // d == 0: no fraction asked for
}

TEST_CASE("CellScale.isOrdinary", "[glyphscaling]")
{
    // The predicate every hot path leans on: ordinary text must be recognisable without arithmetic.
    CHECK(CellScale {}.isOrdinary());
    CHECK(whole(1).isOrdinary());
    CHECK_FALSE(whole(2).isOrdinary());
    CHECK_FALSE(fractional(1, 1, 2).isOrdinary());
}

TEST_CASE("CellScale.extras_round_trip_through_the_cold_column", "[glyphscaling]")
{
    // The fraction and alignment are packed into one 16-bit word beside the hot `scale` byte. A
    // packing that did not round-trip would silently reshape text on the next reflow.
    for (uint8_t n = 0; n <= 15; ++n)
        for (uint8_t d = 0; d <= 15; ++d)
        {
            auto const original = fractional(3, n, d, 2, 1);
            auto const restored =
                vtbackend::unpackTextScale(original.scale, vtbackend::packTextScaleExtras(original));
            CHECK(restored == original);
        }

    // Zero is "ordinary", so a zeroed line needs no initialisation pass to be correct.
    CHECK(vtbackend::unpackTextScale(1, 0) == CellScale {});
}

TEST_CASE("GlyphScaling.stretching.scale_of_one_changes_nothing", "[glyphscaling]")
{
    // Ordinary text is the overwhelming majority of what a terminal draws, so the unscaled case must
    // cost nothing and must not perturb the tile it was given.
    auto const scaler = StretchingGlyphScaler {};
    CHECK(scaler.adjustmentFor(whole(1)).isIdentity());
    CHECK(scaler.adjustmentFor(whole(1)).factor == 1.0);
    CHECK_FALSE(scaler.adjustmentFor(whole(1)).requiresRerasterization);
}

TEST_CASE("GlyphScaling.stretching.scales_by_the_draw_factor", "[glyphscaling]")
{
    auto const scaler = StretchingGlyphScaler {};
    for (uint8_t scale = 2; scale <= 7; ++scale)
    {
        INFO("scale " << static_cast<int>(scale));
        CHECK(scaler.adjustmentFor(whole(scale)).factor == static_cast<double>(scale));
        CHECK_FALSE(scaler.adjustmentFor(whole(scale)).requiresRerasterization);
    }
    // A fraction shrinks the DRAWN size without touching the block.
    CHECK(scaler.adjustmentFor(fractional(3, 1, 3)).factor == 1.0);
    CHECK(scaler.method() == GlyphScalingMethod::Stretch);
}

TEST_CASE("GlyphScaling.rerasterizing.rerasterizes_only_when_the_size_actually_changes", "[glyphscaling]")
{
    // Unscaled text must take the ordinary path, including the direct-mapped fast path for ASCII,
    // which serves tiles reserved at the base size and cannot answer a re-rasterized request. A
    // fraction that lands back on 1.0 is the same case and must not rasterize either.
    auto const scaler = RerasterizingGlyphScaler {};
    CHECK_FALSE(scaler.adjustmentFor(whole(1)).requiresRerasterization);
    CHECK_FALSE(scaler.adjustmentFor(fractional(3, 1, 3)).requiresRerasterization);
    CHECK(scaler.adjustmentFor(whole(2)).requiresRerasterization);
    CHECK(scaler.adjustmentFor(fractional(4, 1, 2)).requiresRerasterization);
    CHECK(scaler.method() == GlyphScalingMethod::Rerasterize);
}

TEST_CASE("GlyphScaling.penOffset.ordinary_text_is_never_moved", "[glyphscaling]")
{
    // Whatever this arithmetic says for a block, it must be exactly zero for ordinary text, or every
    // glyph on screen shifts.
    auto const stretching = StretchingGlyphScaler {};
    auto const rerasterizing = RerasterizingGlyphScaler {};

    for (auto const* scaler:
         { static_cast<GlyphScaler const*>(&stretching), static_cast<GlyphScaler const*>(&rerasterizing) })
    {
        CHECK(scaler->penOffsetFor(CellScale {}, W, H, B, T).dx == 0);
        CHECK(scaler->penOffsetFor(CellScale {}, W, H, B, T).dy == 0);
        CHECK(scaler->penOffsetFor(whole(1), W, H, B, T).dy == 0);
    }
}

TEST_CASE("GlyphScaling.penOffset.stretching_moves_by_the_internal_leading", "[glyphscaling]")
{
    // With no fraction the drawn factor equals the scale, and the offset collapses to
    // (s-1) * (H - B - t) -- the cell's internal leading, once per extra row.
    auto const scaler = StretchingGlyphScaler {};
    constexpr auto Leading = H - B - T; // 4

    CHECK(scaler.penOffsetFor(whole(2), W, H, B, T).dy == Leading);
    CHECK(scaler.penOffsetFor(whole(3), W, H, B, T).dy == 2 * Leading);
    CHECK(scaler.penOffsetFor(whole(7), W, H, B, T).dy == 6 * Leading);
}

TEST_CASE("GlyphScaling.penOffset.rerasterizing_drops_the_bearing_term", "[glyphscaling]")
{
    // A re-rasterized tile came back from the font at `factor x` the point size, so its reported
    // bearing is ALREADY scaled. Counting it again would double it, which is why the offset does not
    // depend on the glyph at all.
    auto const scaler = RerasterizingGlyphScaler {};

    CHECK(scaler.penOffsetFor(whole(2), W, H, B, T).dy == H - B);
    CHECK(scaler.penOffsetFor(whole(2), W, H, B, 40).dy == H - B); // same for a much taller glyph
    CHECK(scaler.penOffsetFor(whole(4), W, H, B, T).dy == 3 * (H - B));
}

TEST_CASE("GlyphScaling.penOffset.lands_the_baseline_on_the_block", "[glyphscaling]")
{
    // The property the arithmetic exists for, stated end to end: after the offset, the drawn baseline
    // must sit `f * B` above the block's BOTTOM.
    auto const scaler = StretchingGlyphScaler {};
    constexpr auto PenY = 100; // bottom of the block's first row

    for (uint8_t scale = 1; scale <= 7; ++scale)
    {
        INFO("scale " << static_cast<int>(scale));
        auto const cellScale = whole(scale);
        auto const factor = cellScale.drawFactor();
        auto const tileTop = PenY - B - T + scaler.penOffsetFor(cellScale, W, H, B, T).dy;
        auto const drawnBaseline = tileTop + static_cast<int>(factor * T);
        auto const wantedBaseline = PenY + ((scale - 1) * H) - static_cast<int>(factor * B);
        CHECK(drawnBaseline == wantedBaseline);
    }
}

TEST_CASE("GlyphScaling.penOffset.alignment_places_a_fractional_glyph_in_its_block", "[glyphscaling]")
{
    // A fraction leaves slack -- (s - f) cells on each axis -- and v/h say where in it the glyph
    // sits. `s=3:n=1:d=3` draws at ordinary size inside a 3-cell block, leaving 2 cells of slack.
    auto const scaler = StretchingGlyphScaler {};
    constexpr auto Slack = 2; // cells: scale 3 - drawn 1

    // Horizontal: 0 = left (flush, no shift), 1 = right, 2 = centered.
    CHECK(scaler.penOffsetFor(fractional(3, 1, 3, 0, 0), W, H, B, T).dx == 0);
    CHECK(scaler.penOffsetFor(fractional(3, 1, 3, 0, 1), W, H, B, T).dx == Slack * W);
    CHECK(scaler.penOffsetFor(fractional(3, 1, 3, 0, 2), W, H, B, T).dx == Slack * W / 2);

    // Vertical is measured from the bottom-aligned placement the baseline formula produces, so
    // bottom moves nothing, top moves up by the whole slack, and centre by half of it.
    auto const bottom = scaler.penOffsetFor(fractional(3, 1, 3, 1, 0), W, H, B, T).dy;
    auto const top = scaler.penOffsetFor(fractional(3, 1, 3, 0, 0), W, H, B, T).dy;
    auto const centered = scaler.penOffsetFor(fractional(3, 1, 3, 2, 0), W, H, B, T).dy;

    CHECK(top == bottom - (Slack * H));
    CHECK(centered == bottom - (Slack * H / 2));
}

TEST_CASE("GlyphScaling.penOffset.a_fraction_that_cancels_draws_like_ordinary_text", "[glyphscaling]")
{
    // `s=3:n=1:d=3` draws at exactly ordinary size. Top-aligned in its block, the glyph must land
    // precisely where unscaled text on that row would -- no fractional drift.
    auto const scaler = StretchingGlyphScaler {};
    auto const offset = scaler.penOffsetFor(fractional(3, 1, 3, 0, 0), W, H, B, T);
    CHECK(offset.dx == 0);
    CHECK(offset.dy == 0);
}

TEST_CASE("GlyphScaling.blockPlacement.canvas_is_whole_cells", "[glyphscaling]")
{
    // The canvas is cut into cell-sized tiles, so a canvas that is not a whole number of cells
    // would yield a ragged last tile on every block.
    for (uint8_t scale = 1; scale <= 7; ++scale)
        for (auto const cells: { 1, 2 })
        {
            INFO("scale " << static_cast<int>(scale) << " x " << cells << " cells");
            auto const placement = blockPlacementFor(whole(scale), W, H, B, cells, 0, 0);
            CHECK(unbox<int>(placement.canvasSize.width) == scale * cells * W);
            CHECK(unbox<int>(placement.canvasSize.height) == scale * H);
        }
}

TEST_CASE("GlyphScaling.blockPlacement.lands_the_baseline_on_the_block", "[glyphscaling]")
{
    // The property the arithmetic exists for: the drawn baseline must sit `f * B` above the
    // block's BOTTOM. Stated against the canvas, whose bottom is its height.
    for (uint8_t scale = 1; scale <= 7; ++scale)
    {
        INFO("scale " << static_cast<int>(scale));
        auto const cellScale = whole(scale);
        auto const factor = cellScale.drawFactor();

        // A raster of bearing T -- its ink top T above the baseline. Its top lands at originY, so
        // the baseline is originY + T.
        auto const placement = blockPlacementFor(cellScale, W, H, B, 1, 0, static_cast<int>(factor * T));
        auto const drawnBaseline = placement.originY + static_cast<int>(factor * T);
        auto const wantedBaseline = unbox<int>(placement.canvasSize.height) - static_cast<int>(factor * B);

        CHECK(drawnBaseline == wantedBaseline);
    }
}

TEST_CASE("GlyphScaling.blockPlacement.ordinary_text_sits_where_it_always_did", "[glyphscaling]")
{
    // scale 1 must reduce to a one-cell canvas with the ordinary baseline, or every unscaled glyph
    // on screen moves.
    auto const placement = blockPlacementFor(whole(1), W, H, B, 1, 3, T);
    CHECK(placement.canvasSize == vtbackend::ImageSize { vtbackend::Width(W), vtbackend::Height(H) });
    CHECK(placement.originX == 3);         // the bearing, unmodified
    CHECK(placement.originY == H - B - T); // ink top = baseline - bearing
}

TEST_CASE("GlyphScaling.blockPlacement.alignment_places_a_fractional_glyph", "[glyphscaling]")
{
    // `s=3:n=1:d=3` draws at ordinary size inside a 3-cell block, leaving 2 cells of slack on each
    // axis. v/h choose where in that slack it sits.
    constexpr auto Slack = 2;

    auto const topLeft = blockPlacementFor(fractional(3, 1, 3, 0, 0), W, H, B, 1, 0, T);
    auto const bottomRight = blockPlacementFor(fractional(3, 1, 3, 1, 1), W, H, B, 1, 0, T);
    auto const centered = blockPlacementFor(fractional(3, 1, 3, 2, 2), W, H, B, 1, 0, T);

    // Top-left is flush with the canvas origin, and lands exactly where ordinary text would.
    CHECK(topLeft.originX == 0);
    CHECK(topLeft.originY == H - B - T);

    CHECK(bottomRight.originX == topLeft.originX + (Slack * W));
    CHECK(bottomRight.originY == topLeft.originY + (Slack * H));

    CHECK(centered.originX == topLeft.originX + (Slack * W / 2));
    CHECK(centered.originY == topLeft.originY + (Slack * H / 2));
}

TEST_CASE("GlyphScaling.method_names_round_trip", "[glyphscaling]")
{
    // The config reader parses these names and the settings pane formats them; a method that
    // formatted one way and parsed another would silently reset itself on every save.
    for (auto const method: { GlyphScalingMethod::Stretch, GlyphScalingMethod::Rerasterize })
    {
        INFO(nameOf(method));
        CHECK(methodFromName(nameOf(method)) == method);
    }
    CHECK_FALSE(methodFromName("nonsense").has_value());
    CHECK_FALSE(methodFromName("").has_value());
}

TEST_CASE("GlyphScaling.factory_returns_the_requested_strategy", "[glyphscaling]")
{
    CHECK(glyphScalerFor(GlyphScalingMethod::Stretch).method() == GlyphScalingMethod::Stretch);
    CHECK(glyphScalerFor(GlyphScalingMethod::Rerasterize).method() == GlyphScalingMethod::Rerasterize);
}
