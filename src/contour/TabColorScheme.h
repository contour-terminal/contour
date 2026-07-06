// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Color.h>

#include <array>
#include <cmath>
#include <cstdint>

namespace contour
{

/// How a user-colored tab is painted: the whole tab is filled with the user's color, faded toward the
/// tab-row background as the tab loses prominence, with a black/white label chosen for contrast.
///
/// A faithful port of Windows Terminal's tab coloring (`Tab::_ApplyTabColorOnUIThread`,
/// `ColorFix::GetLightness`): the fade is an alpha blend toward the row background (not HSL
/// lightening), and the text decision uses OKLab lightness with a 0.6 threshold. Kept Qt-free so the
/// logic is unit-testable (like display/ScissorRect.h, geometry/WindowGeometry.h).

/// A tab's prominence, most to least; drives how far its color fades toward the row background.
enum class TabVisualState : std::uint8_t
{
    Active,                  //!< The active tab of a focused window: full color, no fade.
    Hover,                   //!< A non-active tab under the pointer: lightly faded.
    Inactive,                //!< A non-active tab of a focused window: faded.
    InactiveWindowUnfocused, //!< A non-active tab while the window is unfocused: faded most.
};

/// How much of the tab's own color to retain for a given state (the rest is the row background).
struct TabFade
{
    TabVisualState state; //!< The visual state this row applies to.
    float colorFraction;  //!< Fraction of the tab color retained; the rest is the row background.
};

/// Fade factors per state. Active/Hover/Inactive mirror Windows Terminal (1.0 / 0.6 / 0.3); the
/// window-unfocused value dims a little further.
inline constexpr std::array<TabFade, 4> TabFadeTable { {
    { TabVisualState::Active, 1.0F },
    { TabVisualState::Hover, 0.6F },
    { TabVisualState::Inactive, 0.3F },
    { TabVisualState::InactiveWindowUnfocused, 0.2F },
} };

/// The color-retention fraction for a tab state.
/// @param state The tab's visual state.
/// @return Fraction of the tab color to keep (1.0 = full color, 0.0 = pure row background).
[[nodiscard]] constexpr float tabColorFraction(TabVisualState state) noexcept
{
    for (auto const& row: TabFadeTable)
        if (row.state == state)
            return row.colorFraction;
    return 1.0F; // Unreachable: the table is exhaustive over the enum.
}

/// The tab background: the tab color blended toward the row background by the state's fade fraction.
/// mixColor(a, b, t) returns @p a at t=0, so this blends from the row background to the tab color.
/// @param tabColor The user's chosen tab color.
/// @param rowBackground The tab-row / title-bar background the tab sits on.
/// @param state The tab's visual state.
/// @return The color to fill the tab background with.
[[nodiscard]] constexpr vtbackend::RGBColor tabBackgroundColor(vtbackend::RGBColor tabColor,
                                                               vtbackend::RGBColor rowBackground,
                                                               TabVisualState state) noexcept
{
    return vtbackend::mixColor(rowBackground, tabColor, tabColorFraction(state));
}

namespace detail
{
    /// An 8-bit sRGB channel converted to linear light (the sRGB EOTF, which WT tabulates as a LUT).
    /// @param c The sRGB channel value in [0, 255].
    /// @return The linear-light value in [0, 1].
    [[nodiscard]] inline float srgbChannelToLinear(std::uint8_t c) noexcept
    {
        auto const v = static_cast<float>(c) / 255.0F;
        return v <= 0.04045F ? v / 12.92F : std::pow((v + 0.055F) / 1.055F, 2.4F);
    }
} // namespace detail

/// The OKLab lightness (L channel) of an sRGB color.
/// Port of WT's `ColorFix::GetLightness` (Ottosson's OKLab), computing only the L output.
/// @param color The sRGB color to measure.
/// @return The OKLab lightness, ~0.0 for black and ~1.0 for white.
[[nodiscard]] inline float oklabLightness(vtbackend::RGBColor color) noexcept
{
    auto const r = detail::srgbChannelToLinear(color.red);
    auto const g = detail::srgbChannelToLinear(color.green);
    auto const b = detail::srgbChannelToLinear(color.blue);

    // Linear sRGB -> LMS cone responses (Ottosson's M1 matrix).
    auto const l = (0.4122214708F * r) + (0.5363325363F * g) + (0.0514459929F * b);
    auto const m = (0.2119034982F * r) + (0.6806995451F * g) + (0.1073969566F * b);
    auto const s = (0.0883024619F * r) + (0.2817188376F * g) + (0.6299787005F * b);

    // Cube-root compression, then the L output row of the M2 matrix.
    auto const lp = std::cbrt(l);
    auto const mp = std::cbrt(m);
    auto const sp = std::cbrt(s);
    return (0.2104542553F * lp) + (0.7936177850F * mp) - (0.0040720468F * sp);
}

/// OKLab lightness at/above which black text out-reads white (Windows Terminal's threshold).
inline constexpr float TabTextLightnessThreshold = 0.6F;

/// A contrasting text color for text over a background, decided by OKLab lightness.
/// @param background The (already composited) background the text sits on.
/// @return Opaque black for a light background, opaque white for a dark one.
[[nodiscard]] inline vtbackend::RGBColor contrastingTextColor(vtbackend::RGBColor background) noexcept
{
    return oklabLightness(background) >= TabTextLightnessThreshold ? vtbackend::RGBColor { 0, 0, 0 }
                                                                   : vtbackend::RGBColor { 255, 255, 255 };
}

} // namespace contour
