// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the title band shows behind it and paints itself: one table matched on the chrome row and its
/// title-band material, followed by Minimal's bare-band step and one full-screen step; and the tint the band
/// paints, taken from the terminal background. Pure and Qt-free, so the band, its snapshot rows and the tests
/// read the same rows.

#include <contour/config/ChromeDensity.hpp>
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
    BarCentre centre = BarCentre::Empty;                ///< What the band's middle holds (resolveTitleBand).
    config::ChromeDensity density = config::ChromeDensity::Comfortable; ///< The configured chrome density.
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
    /// Which variant of the OS's material a tabbed window asks for, where the backdrop is the native one.
    config::BackdropKind kind = config::BackdropKind::Tabbed;
    config::TabSurface tabs = config::TabSurface::Painted; ///< What draws the tabs' own surfaces.

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
/// the band and an empty compact toolbar making its title bar 40 pt; under AppKit's own title-bar material,
/// though, its NSTitlebarView takes the band's clicks (ruling S6, finding F13), so the shipped macOS band is
/// Solid and no row selects NativeMaterial on macOS.
///
/// The frosted and glass bands (`tab_style.material`) follow Apple's Liquid Glass guidance -- the material is
/// for the controls and navigation layer, and the system, not the app, draws their backgrounds:
///   - Frosted is the Blurred backdrop -- the title bar's own material, in a view of ours behind the Qt
///     content, which leaves the clicks in Qt's view -- with nothing painted over it.
///   - Glass paints the band in the terminal's own background, so the bar reads as the content it sits on,
///     and has the system draw each tab as a Liquid Glass shape of its own (TabSurface::Glass), in one
///     shared container so the shapes sample alike: glass on the navigation elements, never a slab of
///     it across the window, and never glass on glass. Before macOS 26 there is no Liquid Glass; the
///     material reports it unsupported, and the window paints the solid band and its tabs.
/// Both grow the title bar by the toolbar as the Solid band does. KDE Plasma's bands are the Blurred
/// backdrop over KWin's blur, under a translucent tint, with no title bar to grow.
inline constexpr auto TitleBandSurfaceRows = std::array {
    TitleBandSurfaceRow { .style = config::ChromeStyle::MacOS,
                          .material = config::Material::Frost,
                          .surface = { .backdrop = config::BandBackdrop::Blurred,
                                       .fill = config::BandFill::Clear,
                                       .growth = config::TitleBarGrowth::CompactToolbar } },
    TitleBandSurfaceRow { .style = config::ChromeStyle::MacOS,
                          .material = config::Material::Glass,
                          .surface = { .backdrop = config::BandBackdrop::Client,
                                       .fill = config::BandFill::Background,
                                       .growth = config::TitleBarGrowth::CompactToolbar,
                                       .kind = config::BackdropKind::TabbedGlass,
                                       .tabs = config::TabSurface::Glass } },
    // The shipped macOS band (ruling S6), and what Reduce Transparency resolves Glass to: the client's own
    // paint over the compact toolbar.
    TitleBandSurfaceRow { .style = config::ChromeStyle::MacOS,
                          .material = config::Material::Solid,
                          .surface = { .backdrop = config::BandBackdrop::Client,
                                       .fill = config::BandFill::Tint,
                                       .growth = config::TitleBarGrowth::CompactToolbar } },
    // Windows 11's Mica behind the joined tabs: the band is cleared over the DWM backdrop, and the panes stay
    // opaque (R-k). Solid -- no Mica below 22H2 (MaterialSupport::Unsupported), or Reduced transparency, or a
    // window whose material failed -- is the tint over the client's own paint.
    TitleBandSurfaceRow { .style = config::ChromeStyle::Fluent,
                          .material = config::Material::Mica,
                          .surface = { .backdrop = config::BandBackdrop::NativeMaterial,
                                       .fill = config::BandFill::Clear,
                                       .growth = config::TitleBarGrowth::None } },
    // Acrylic where the user asked for it (tab_style.material): the same cleared band, over DWM's blurred
    // backdrop instead of Mica Alt.
    TitleBandSurfaceRow { .style = config::ChromeStyle::Fluent,
                          .material = config::Material::Acrylic,
                          .surface = { .backdrop = config::BandBackdrop::NativeMaterial,
                                       .fill = config::BandFill::Clear,
                                       .growth = config::TitleBarGrowth::None,
                                       .kind = config::BackdropKind::TabbedGlass } },
    TitleBandSurfaceRow { .style = config::ChromeStyle::ContourLinux,
                          .material = config::Material::Frost,
                          .surface = { .backdrop = config::BandBackdrop::Blurred,
                                       .fill = config::BandFill::FrostTint,
                                       .growth = config::TitleBarGrowth::None } },
    TitleBandSurfaceRow { .style = config::ChromeStyle::ContourLinux,
                          .material = config::Material::Glass,
                          .surface = { .backdrop = config::BandBackdrop::Blurred,
                                       .fill = config::BandFill::GlassTint,
                                       .growth = config::TitleBarGrowth::None,
                                       .kind = config::BackdropKind::TabbedGlass } },
    TitleBandSurfaceRow { .style = config::ChromeStyle::Fluent,
                          .material = config::Material::Solid,
                          .surface = { .backdrop = config::BandBackdrop::Client,
                                       .fill = config::BandFill::Tint,
                                       .growth = config::TitleBarGrowth::None } },
};

/// Minimal's bare band (R-f): an empty band painted in the terminal background, so the window reads
/// bar-less. Over the bare native title bar, without the compact toolbar (ruling P3): the band is as tall as
/// AppKit's own title bar (32 pt), and the traffic lights sit where a bare title bar puts them (78 pt).
inline constexpr auto BareBandSurface = TitleBandSurface {
    .backdrop = config::BandBackdrop::Client,
    .fill = config::BandFill::Background,
    .growth = config::TitleBarGrowth::None,
};

/// Decides the band's surface.
///
/// A native frame has no band of ours (R-e). Under Minimal, a row's band whose middle is empty is the bare
/// band (BareBandSurface). In full screen no material sits behind terminal text (R-b, owner decision 2):
/// the native material becomes the client's own paint, and a cleared band becomes the tint. And full screen
/// detaches the compact toolbar (ruling S7): it only sizes the windowed band, while in full screen AppKit
/// would show it in an NSToolbarFullScreenWindow strip of its own over the window's top, which takes the
/// clicks there (final review finding 3, measured).
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

    auto surface = inputs.density == config::ChromeDensity::Minimal && inputs.centre == BarCentre::Empty
                       ? BareBandSurface
                       : row->surface;
    if (inputs.mode == WindowMode::FullScreen)
    {
        if (surface.backdrop == config::BandBackdrop::NativeMaterial
            || surface.backdrop == config::BandBackdrop::Blurred)
            surface.backdrop = config::BandBackdrop::Client;
        if (surface.fill == config::BandFill::Clear || surface.fill == config::BandFill::FrostTint
            || surface.fill == config::BandFill::GlassTint)
            surface.fill = config::BandFill::Tint;
        surface.growth = config::TitleBarGrowth::None;
        // No material behind terminal text, and none on the tabs over it either.
        surface.tabs = config::TabSurface::Painted;
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

/// The terminal background the bare band paints on a chrome of tone @p chrome, where the band's ink -- the
/// host chip's text, the drift chip's -- stays legible (R-d), as legibleBandTint() decides for the tint.
/// @param background The terminal's default background.
/// @param chrome     The tone the chrome's labels are inked for.
/// @return @p background when it has the chrome's tone, else @c std::nullopt (the caller paints the palette
///         window colour instead).
[[nodiscard]] inline std::optional<vtbackend::RGBColor> legibleBandBackground(vtbackend::RGBColor background,
                                                                              ChromeTone chrome) noexcept
{
    if (toneOf(background) != chrome)
        return std::nullopt;
    return background;
}

} // namespace contour::window
