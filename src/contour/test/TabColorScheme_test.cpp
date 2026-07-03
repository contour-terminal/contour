// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for TabColorScheme.h — the pure color math behind fully-colorized tabs: the fade of a
// tab's color toward the tab-row background per visual state (a faithful port of Windows
// Terminal's 1.0 / 0.6 / 0.3 look), the OKLab lightness metric, and the resulting black/white text
// decision. Reference L values were cross-checked against WT's ColorFix::GetLightness pipeline.

#include <contour/TabColorScheme.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using contour::contrastingTextColor;
using contour::oklabLightness;
using contour::tabBackgroundColor;
using contour::tabColorFraction;
using contour::TabTextLightnessThreshold;
using contour::TabVisualState;
using vtbackend::RGBColor;

TEST_CASE("tabColorFraction returns the tabled fraction per state", "[tabcolor]")
{
    // Mirrors Windows Terminal: full color when active, 0.6 on hover, 0.3 when inactive; dimmer still
    // when the whole window is unfocused. Data-driven from TabColorScheme.h's TabFadeTable.
    CHECK(tabColorFraction(TabVisualState::Active) == Catch::Approx(1.0F));
    CHECK(tabColorFraction(TabVisualState::Hover) == Catch::Approx(0.6F));
    CHECK(tabColorFraction(TabVisualState::Inactive) == Catch::Approx(0.3F));
    CHECK(tabColorFraction(TabVisualState::InactiveWindowUnfocused) == Catch::Approx(0.2F));

    // Prominence is monotonically decreasing across the enum order.
    CHECK(tabColorFraction(TabVisualState::Active) > tabColorFraction(TabVisualState::Hover));
    CHECK(tabColorFraction(TabVisualState::Hover) > tabColorFraction(TabVisualState::Inactive));
    CHECK(tabColorFraction(TabVisualState::Inactive)
          > tabColorFraction(TabVisualState::InactiveWindowUnfocused));
}

TEST_CASE("tabBackgroundColor blends the tab color toward the row background", "[tabcolor]")
{
    auto const row = RGBColor { 0x20, 0x20, 0x20 }; // a dark title bar
    auto const tab = RGBColor { 0xCC, 0x33, 0x33 }; // palette red

    SECTION("an active tab keeps its full color")
    {
        CHECK(tabBackgroundColor(tab, row, TabVisualState::Active) == tab);
    }

    SECTION("an inactive tab fades 30% of the way toward the row background")
    {
        // Channel = lerp(row, tab, 0.3), truncated by the uint8 cast in vtbackend::mixColor:
        //   red:   32 + (204-32)*0.3 = 83.6 -> 83; green/blue: 32 + (51-32)*0.3 = 37.7 -> 37.
        CHECK(tabBackgroundColor(tab, row, TabVisualState::Inactive) == RGBColor { 83, 37, 37 });
    }

    SECTION("a hovered tab sits between inactive and active")
    {
        CHECK(tabBackgroundColor(tab, row, TabVisualState::Hover) == RGBColor { 135, 43, 43 });
    }

    SECTION("the unfocused-window fade is closest to the row background")
    {
        // The most-faded state is the nearest to the row color of all non-active states.
        auto const unfocused = tabBackgroundColor(tab, row, TabVisualState::InactiveWindowUnfocused);
        auto const inactive = tabBackgroundColor(tab, row, TabVisualState::Inactive);
        CHECK(vtbackend::distance(unfocused, row) < vtbackend::distance(inactive, row));
    }
}

TEST_CASE("oklabLightness tracks perceived lightness", "[tabcolor]")
{
    CHECK(oklabLightness(RGBColor { 0, 0, 0 }) == Catch::Approx(0.0F).margin(1e-4));
    CHECK(oklabLightness(RGBColor { 255, 255, 255 }) == Catch::Approx(1.0F).margin(1e-4));

    // Cross-checked against WT's ColorFix::GetLightness (sRGB -> linear -> OKLab L).
    CHECK(oklabLightness(RGBColor { 128, 128, 128 }) == Catch::Approx(0.5999F).margin(1e-3));

    SECTION("a gray ramp is strictly increasing in lightness")
    {
        auto previous = -1.0F;
        for (auto const v: { 0, 32, 64, 96, 128, 160, 192, 224, 255 })
        {
            auto const l = oklabLightness(RGBColor {
                static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v) });
            CHECK(l > previous);
            previous = l;
        }
    }
}

TEST_CASE("contrastingTextColor picks black on light, white on dark", "[tabcolor]")
{
    auto const black = RGBColor { 0, 0, 0 };
    auto const white = RGBColor { 255, 255, 255 };

    CHECK(contrastingTextColor(RGBColor { 255, 255, 255 }) == black);
    CHECK(contrastingTextColor(RGBColor { 0, 0, 0 }) == white);

    SECTION("dark palette swatches get white text")
    {
        CHECK(contrastingTextColor(RGBColor { 0x33, 0x77, 0xCC }) == white); // blue,  L~0.57
        CHECK(contrastingTextColor(RGBColor { 0xCC, 0x33, 0x33 }) == white); // red,   L~0.56
    }

    SECTION("light palette swatches get black text")
    {
        CHECK(contrastingTextColor(RGBColor { 0xEE, 0xEE, 0x66 }) == black); // bright yellow, L~0.92
        CHECK(contrastingTextColor(RGBColor { 0x33, 0xCC, 0x33 }) == black); // green,         L~0.74
        CHECK(contrastingTextColor(RGBColor { 0x66, 0xEE, 0x66 }) == black); // bright green,  L~0.84
    }

    SECTION("the decision flips exactly at the OKLab lightness threshold")
    {
        // Just below the threshold -> white; at/above -> black. Uses grays whose L straddles 0.6.
        CHECK(oklabLightness(RGBColor { 128, 128, 128 }) < TabTextLightnessThreshold);
        CHECK(contrastingTextColor(RGBColor { 128, 128, 128 }) == white);
        CHECK(oklabLightness(RGBColor { 130, 130, 130 }) >= TabTextLightnessThreshold);
        CHECK(contrastingTextColor(RGBColor { 130, 130, 130 }) == black);
    }
}
