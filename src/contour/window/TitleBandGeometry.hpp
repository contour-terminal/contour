// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How tall the title band is and where it rests in full screen: the band's token height reconciled with
/// the native title bar's, per the chrome row's traffic-light fit. Pure and Qt-free.

#include <contour/config/ChromeDensity.hpp>
#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/window/EnumFromInt.hpp>
#include <contour/window/TitleBandLayout.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace contour::window
{

/// The band's own tokens.
struct TitleBandTokens
{
    config::ChromeStyle style = config::ChromeStyle::ContourLinux; ///< The chrome row.
    config::Material material = config::Material::None; ///< The row's resolved title-band material.
    /// The band's token height in logical pixels: the provider's @c bandHeight (38 on MacOS, today's strip
    /// height on the Flat rows, one cell on the Terminal row), or @c size.bareBand for a band whose middle
    /// is empty (Minimal: 32 on MacOS, 28 on Fluent and ContourLinux).
    double height = 0;
    double cellHeight = 0; ///< One chrome cell's height on the Terminal row; 0 on the native rows.
    config::ChromeDensity density = config::ChromeDensity::Comfortable; ///< The configured chrome density.
};

/// How a band makes room for the native traffic lights.
enum class TrafficLightFit : uint8_t
{
    AppKit = 0,      //!< The band is at least as tall as the native title bar, snapped to device pixels.
    GrowBandToCells, //!< The band grows by whole cell rows until it holds the native title bar.
};

/// The Terminal row's fit. Not a snap of the lights onto the first cell row: AppKit re-places them at first
/// expose, in full screen and on every resize (spike-decisions.md §D3, ruling S3).
inline constexpr TrafficLightFit TerminalRowTrafficLightFit = TrafficLightFit::GrowBandToCells;

/// Everything the band's geometry depends on.
struct TitleBandGeometryInputs
{
    TitleBandTokens tokens {};              ///< The band's tokens.
    WindowMode mode = WindowMode::Windowed; ///< Windowed or full screen.
    double devicePixelRatio = 1;            ///< The window's device-pixel ratio.
    /// The native title bar's height; 0 where there is none (Linux, Windows, offscreen, a native frame).
    double nativeHeight = 0;
    /// Where AppKit's revealed full-screen title bar starts, window-relative (below the menu bar or the
    /// notch).
    double fullScreenTop = 0;
};

/// The band's height and its full-screen resting place.
struct TitleBandGeometry
{
    double height = 0; ///< The band's height in logical pixels.
    /// Where a full-screen band rests below the menu bar: below AppKit's revealed title bar; 0 when windowed.
    double revealTop = 0;

    [[nodiscard]] bool operator==(TitleBandGeometry const&) const = default;
};

/// Decides the band's geometry.
///
/// With no native title bar the token height stands verbatim, so the Linux and Windows bands (and every
/// offscreen one) keep their extent at fractional scales. Otherwise @c AppKit takes the taller of the two,
/// rounded up to whole device pixels, and @c GrowBandToCells the smallest whole number of cells that holds
/// both.
///
/// In full screen the band rests BELOW AppKit's revealed title bar, not under it: that bar is a window of its
/// own (NSToolbarFullScreenWindow) stacked above ours, and it takes the clicks over its whole extent
/// (measured with the window server, final review finding 3) -- even bare, once ruling S7 has detached the
/// compact toolbar, though then only while revealed. So the reveal top is the bar's top plus its height, the
/// native title bar's.
///
/// @c inline rather than @c constexpr: it calls std::ceil, which is constexpr only where the library
/// implements P0533.
/// @param inputs What the geometry depends on.
/// @return The geometry.
[[nodiscard]] inline TitleBandGeometry resolveTitleBandGeometry(
    TitleBandGeometryInputs const& inputs) noexcept
{
    auto const& tokens = inputs.tokens;
    auto const revealTop = inputs.mode == WindowMode::FullScreen
                               ? std::max(0.0, inputs.fullScreenTop) + std::max(0.0, inputs.nativeHeight)
                               : 0.0;
    if (inputs.nativeHeight <= 0)
        return TitleBandGeometry { .height = tokens.height, .revealTop = revealTop };

    auto const tallest = std::max(tokens.height, inputs.nativeHeight);
    auto const fit = tokens.style == config::ChromeStyle::Terminal && tokens.cellHeight > 0
                         ? TerminalRowTrafficLightFit
                         : TrafficLightFit::AppKit;
    switch (fit)
    {
        case TrafficLightFit::GrowBandToCells:
            return TitleBandGeometry { .height = tokens.cellHeight * std::ceil(tallest / tokens.cellHeight),
                                       .revealTop = revealTop };
        case TrafficLightFit::AppKit: break;
    }
    auto const dpr = inputs.devicePixelRatio > 0 ? inputs.devicePixelRatio : 1.0;
    return TitleBandGeometry { .height = std::ceil(tallest * dpr) / dpr, .revealTop = revealTop };
}

/// The widths a band is made of, in logical pixels, measured by the band itself: what the window's minimum
/// width is the sum of.
struct BandWidths
{
    double leading = 0;        ///< The leading parts but the location chip: window buttons, toggle, items.
    double trailing = 0;       ///< The trailing parts: the new-tab button, the drift chip, window buttons.
    double chipMargins = 0;    ///< The gaps either side of the location chip; 0 where it does not show.
    double chip = 0;           ///< The chip at its last fit step; 0 where it does not show.
    double dragReserve = 0;    ///< The gap left to drag the window by.
    double tabStripMargin = 0; ///< The gap before the tab strip.
    double tabWell = 0;        ///< The tab strip at its least: its fixed parts and one tab-well minimum.
};

/// One part of a @c BandWidths: the key the band measures it by, and where it lives.
struct BandWidthPart
{
    std::string_view key;       ///< The name the band reports the part by.
    double BandWidths::* width; ///< Where the part lives.
};

/// Every part of a band's minimum width. The single source of the keys and of what is summed, so a new part
/// is one row here.
inline constexpr auto BandWidthParts = std::array {
    BandWidthPart { .key = "leading", .width = &BandWidths::leading },
    BandWidthPart { .key = "trailing", .width = &BandWidths::trailing },
    BandWidthPart { .key = "chipMargins", .width = &BandWidths::chipMargins },
    BandWidthPart { .key = "chip", .width = &BandWidths::chip },
    BandWidthPart { .key = "dragReserve", .width = &BandWidths::dragReserve },
    BandWidthPart { .key = "tabStripMargin", .width = &BandWidths::tabStripMargin },
    BandWidthPart { .key = "tabWell", .width = &BandWidths::tabWell },
};

/// @param widths What the band is made of.
/// @return The narrowest the band can be while keeping its fixed parts, the location chip at its last fit
///         step (@c LocationChipFit::WorkspaceElided) and one tab-well minimum. A negative part counts as 0.
[[nodiscard]] constexpr double bandMinimumWidth(BandWidths const& widths) noexcept
{
    auto total = 0.0;
    for (auto const& part: BandWidthParts)
        total += std::max(0.0, widths.*(part.width));
    return total;
}

/// @param widthOf Gives a part's width by its key (a missing key answering 0), as a QML map does.
/// @return bandMinimumWidth() of the parts @p widthOf names.
template <typename WidthOf>
[[nodiscard]] constexpr double bandMinimumWidthFrom(WidthOf const& widthOf)
{
    auto widths = BandWidths {};
    for (auto const& part: BandWidthParts)
        widths.*(part.width) = widthOf(part.key);
    return bandMinimumWidth(widths);
}

/// Where the band sits against the workspace sidebar.
enum class BandPlacement : uint8_t
{
    FullWidth = 0, ///< The band spans the window: the sidebar is below it, or not shown.
    BesideSidebar, ///< The band starts at the sidebar's edge, so the window is the sidebar wider than it.
};

/// Every @c BandPlacement, the table its count and its conversion from QML's int come from.
inline constexpr auto BandPlacements = std::array { BandPlacement::FullWidth, BandPlacement::BesideSidebar };
static_assert(enumValuesAreDense(BandPlacements));

/// Number of @c BandPlacement enumerators.
inline constexpr std::size_t BandPlacementCount = BandPlacements.size();

/// @param value A window::BandPlacement's underlying value, as QML hands it over.
/// @return The enumerator, or nothing for a value that is none of them.
[[nodiscard]] constexpr std::optional<BandPlacement> bandPlacementFromInt(int value) noexcept
{
    return enumFromInt(BandPlacements, value);
}

/// @param hintMinimum  The window's minimum width from its page and chrome, in logical pixels.
/// @param bandMinimum  What the band needs (bandMinimumWidth()), in logical pixels.
/// @param sidebarWidth The workspace sidebar's width, 0 while it is hidden.
/// @param placement    Whether the band starts at the sidebar's edge.
/// @return The window's minimum width: the hint, or the band's need rounded up to whole pixels (plus the
///         sidebar when the band sits beside it), whichever is wider.
[[nodiscard]] inline int minimumWidthWith(int hintMinimum,
                                          double bandMinimum,
                                          int sidebarWidth,
                                          BandPlacement placement) noexcept
{
    auto const beside = placement == BandPlacement::BesideSidebar ? sidebarWidth : 0;
    return std::max(hintMinimum, static_cast<int>(std::ceil(bandMinimum)) + beside);
}

} // namespace contour::window
