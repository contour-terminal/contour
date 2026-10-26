// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the title band shows behind it and paints itself: one table matched on the chrome row and its
/// title-band material, followed by one full-screen step; and the tint the band paints, taken from the
/// terminal background. Pure and Qt-free, so the band, its snapshot rows and the tests read the same rows.

#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/window/TitleBandLayout.hpp>

#include <vtbackend/core/Color.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace contour::window
{

/// Everything the band's surface depends on.
struct TitleBandSurfaceInputs
{
    config::ChromeStyle style = config::ChromeStyle::ContourLinux; ///< The chrome row.
    config::Material material = config::Material::None; ///< The row's resolved @c material.titleBand.
    WindowFrame frame = WindowFrame::ClientBand;        ///< Who draws the title area.
    WindowMode mode = WindowMode::Windowed;             ///< Windowed or full screen.
};

/// What the band shows behind it, what it paints, and what the native title bar is grown with.
///
/// The default is today's band: the client area's own paint in the palette's window colour, and a bare
/// title bar.
struct TitleBandSurface
{
    config::BandBackdrop backdrop = config::BandBackdrop::Client; ///< What the OS draws behind the band.
    config::BandFill fill = config::BandFill::Palette;            ///< What the band paints.
    config::TitleBarGrowth growth = config::TitleBarGrowth::None; ///< What the title bar is grown with.

    [[nodiscard]] bool operator==(TitleBandSurface const&) const = default;
};

/// One row of the surface table.
struct TitleBandSurfaceRow
{
    config::ChromeStyle style {}; ///< The chrome row it applies to.
    config::Material material {}; ///< The material it applies to.
    TitleBandSurface surface {};  ///< The windowed answer.
};

/// The surface rows, first match wins; a row and material no row names keeps today's band.
///
/// The spike (spike-decisions.md, rulings S2 and S4) measured AppKit drawing a within-window material in
/// the band and an empty compact toolbar making its title bar 40 pt: so macOS's glass is the native
/// material under a tinted band, and either material grows the title bar by the toolbar.
inline constexpr auto TitleBandSurfaceRows = std::array {
    TitleBandSurfaceRow { .style = config::ChromeStyle::MacOS,
                          .material = config::Material::Glass,
                          .surface = { .backdrop = config::BandBackdrop::NativeMaterial,
                                       .fill = config::BandFill::Tint,
                                       .growth = config::TitleBarGrowth::CompactToolbar } },
    // Reduce Transparency resolves Glass to Solid: the same band, with nothing behind it.
    TitleBandSurfaceRow { .style = config::ChromeStyle::MacOS,
                          .material = config::Material::Solid,
                          .surface = { .backdrop = config::BandBackdrop::Client,
                                       .fill = config::BandFill::Tint,
                                       .growth = config::TitleBarGrowth::CompactToolbar } },
};

/// Decides the band's surface.
///
/// A native frame has no band of ours (R-e). In full screen no material sits behind terminal text (D2):
/// the native material becomes the client's own paint, and a cleared band becomes the tint.
/// @param inputs What the surface depends on.
/// @return The surface; @c TitleBandSurface {} where no row matches or the frame is native.
[[nodiscard]] constexpr TitleBandSurface resolveTitleBandSurface(
    TitleBandSurfaceInputs const& inputs) noexcept
{
    if (inputs.frame == WindowFrame::NativeFrame)
        return {};

    auto const row = std::ranges::find_if(TitleBandSurfaceRows, [&](TitleBandSurfaceRow const& candidate) {
        return candidate.style == inputs.style && candidate.material == inputs.material;
    });
    if (row == TitleBandSurfaceRows.end())
        return {};

    auto surface = row->surface;
    if (inputs.mode == WindowMode::FullScreen)
    {
        if (surface.backdrop == config::BandBackdrop::NativeMaterial)
            surface.backdrop = config::BandBackdrop::Client;
        if (surface.fill == config::BandFill::Clear)
            surface.fill = config::BandFill::Tint;
    }
    return surface;
}

/// The relative luminance below which a colour reads as a dark ground.
inline constexpr double TitleBandDarkLuminance = 0.18;

/// Whether a ground is light or dark.
enum class ChromeTone : uint8_t
{
    Light = 0, //!< A light ground: black ink.
    Dark,      //!< A dark ground: white ink.
};

/// Number of @c ChromeTone enumerators, derived from the last one.
inline constexpr size_t ChromeToneCount = static_cast<size_t>(ChromeTone::Dark) + 1;

/// @param color A ground colour.
/// @return @c Dark when its relative luminance is below @c TitleBandDarkLuminance, else @c Light.
[[nodiscard]] inline ChromeTone toneOf(vtbackend::RGBColor color) noexcept
{
    return vtbackend::relativeLuminance(color) < TitleBandDarkLuminance ? ChromeTone::Dark
                                                                        : ChromeTone::Light;
}

/// How far the band's tint moves the terminal background, in HSL lightness points.
inline constexpr int BandTintPoints = 4;

/// The band's tint of a terminal background: 4 lightness points lighter on a dark background, 4 darker on a
/// light one (by toneOf()), so the band stands off the grid below it without changing its hue.
///
/// @c inline rather than @c constexpr: toneOf() reads vtbackend::relativeLuminance, which is not.
/// @param background The terminal's default background.
/// @return The tint, clamped to the lightness range.
[[nodiscard]] inline vtbackend::RGBColor bandTintOf(vtbackend::RGBColor background) noexcept
{
    return config::shiftLightness(background,
                                  toneOf(background) == ChromeTone::Dark ? BandTintPoints : -BandTintPoints);
}

/// The tint the band paints on a chrome of tone @p chrome, where its labels stay legible (R-d).
/// @param background The terminal's default background.
/// @param chrome     The tone the chrome's labels are inked for.
/// @return @c bandTintOf(background) when the background has the chrome's tone, else @c std::nullopt (the
///         caller paints the palette window colour instead).
[[nodiscard]] inline std::optional<vtbackend::RGBColor> legibleBandTint(vtbackend::RGBColor background,
                                                                        ChromeTone chrome) noexcept
{
    if (toneOf(background) != chrome)
        return std::nullopt;
    return bandTintOf(background);
}

} // namespace contour::window
