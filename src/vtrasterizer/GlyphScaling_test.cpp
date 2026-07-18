// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the glyph scaling strategy used by the kitty text sizing protocol.

#include <vtrasterizer/GlyphScaling.h>

#include <catch2/catch_test_macros.hpp>

using namespace vtrasterizer;

TEST_CASE("GlyphScaling.stretching.scale_of_one_changes_nothing", "[glyphscaling]")
{
    // Ordinary text is the overwhelming majority of what a terminal draws, so the unscaled case must
    // cost nothing and must not perturb the tile it was given.
    auto const scaler = StretchingGlyphScaler {};
    auto const adjustment = scaler.adjustmentFor(1);
    CHECK(adjustment.widthFactor == 1);
    CHECK(adjustment.heightFactor == 1);
    CHECK_FALSE(adjustment.requiresRerasterization);
}

TEST_CASE("GlyphScaling.stretching.scales_both_axes", "[glyphscaling]")
{
    // A scaled block is `s` cells wide AND `s` cells tall, so the glyph grows on both axes.
    auto const scaler = StretchingGlyphScaler {};
    for (uint8_t scale = 2; scale <= 7; ++scale)
    {
        INFO("scale " << static_cast<int>(scale));
        auto const adjustment = scaler.adjustmentFor(scale);
        CHECK(adjustment.widthFactor == scale);
        CHECK(adjustment.heightFactor == scale);
    }
}

TEST_CASE("GlyphScaling.stretching.never_rerasterizes", "[glyphscaling]")
{
    // The whole point of this strategy: it reuses the tile already in the atlas, so it costs nothing
    // at rasterization time and adds no atlas entries however many scales are in use.
    auto const scaler = StretchingGlyphScaler {};
    for (uint8_t scale = 1; scale <= 7; ++scale)
        CHECK_FALSE(scaler.adjustmentFor(scale).requiresRerasterization);

    CHECK(scaler.method() == GlyphScalingMethod::Stretch);
}

TEST_CASE("GlyphScaling.stretching.treats_a_zero_scale_as_one", "[glyphscaling]")
{
    // Scale is 1..7 by the protocol and validated on the way in, but a strategy that divides the
    // renderer's geometry by it must not be the thing that breaks if a zero ever reaches it.
    auto const scaler = StretchingGlyphScaler {};
    auto const adjustment = scaler.adjustmentFor(0);
    CHECK(adjustment.widthFactor == 1);
    CHECK(adjustment.heightFactor == 1);
}

TEST_CASE("GlyphScaling.rerasterizing.scale_of_one_does_not_rerasterize", "[glyphscaling]")
{
    // Unscaled text must take the ordinary path, including the direct-mapped fast path for ASCII,
    // which serves tiles reserved at the base size and cannot answer a re-rasterized request.
    auto const scaler = RerasterizingGlyphScaler {};
    auto const adjustment = scaler.adjustmentFor(1);
    CHECK(adjustment.widthFactor == 1);
    CHECK_FALSE(adjustment.requiresRerasterization);
}

TEST_CASE("GlyphScaling.rerasterizing.asks_for_a_new_raster_when_scaled", "[glyphscaling]")
{
    auto const scaler = RerasterizingGlyphScaler {};
    for (uint8_t scale = 2; scale <= 7; ++scale)
    {
        INFO("scale " << static_cast<int>(scale));
        auto const adjustment = scaler.adjustmentFor(scale);
        CHECK(adjustment.requiresRerasterization);
        // The factors still report the size asked for, so the caller can scale the glyph request --
        // but the tile that comes back is already final, and stretching it again would double it.
        CHECK(adjustment.widthFactor == scale);
        CHECK(adjustment.heightFactor == scale);
    }
    CHECK(scaler.method() == GlyphScalingMethod::Rerasterize);
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
