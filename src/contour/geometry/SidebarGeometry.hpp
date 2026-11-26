// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How wide the workspace sidebar is, in logical pixels: the one place that turns the sidebar's tokens and
/// what the user last chose into the horizontal chrome (Chrome::width) a window declares. Pure and Qt-free.

#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/GuiStateStore.hpp>
#include <contour/config/TabBarMode.hpp>
#include <contour/config/UiStyle.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>

namespace contour::geometry
{

/// What one horizontal unit of the sidebar's tokens is in logical pixels, and what counts the units.
///
/// A native row counts pixels, so one unit is one pixel and any width is a whole number of units. The
/// Terminal row counts character cells, so one unit is a cell's width and a width the user drags to is
/// snapped to a whole number of them.
struct SidebarScale
{
    config::LengthUnit unit = config::LengthUnit::Pixel; ///< What the row counts its extents in.
    double unitPixels = 1.0;                             ///< One horizontal unit, in logical pixels.

    [[nodiscard]] constexpr bool operator==(SidebarScale const&) const noexcept = default;
};

/// The sidebar's bounds and what counts them: what a window decides its width against.
struct SidebarMetrics
{
    config::SidebarTokens tokens {}; ///< The chrome row's sidebar extents, in its own units.
    SidebarScale scale {};           ///< What one unit of them is.

    [[nodiscard]] constexpr bool operator==(SidebarMetrics const&) const noexcept = default;
};

/// @param units A width in the row's units.
/// @param tokens The row's sidebar tokens.
/// @return @p units kept within the row's bounds.
[[nodiscard]] constexpr int clampSidebarUnits(int units, config::SidebarTokens const& tokens) noexcept
{
    return std::clamp(units, tokens.minWidth, tokens.maxWidth);
}

/// The width a requested pixel width becomes, in the row's units: snapped to whole cells on a row that
/// counts them, then kept within the row's bounds.
/// @param pixels A width in logical pixels, such as a drag ends at.
/// @param tokens The row's sidebar tokens.
/// @param scale What one unit of them is.
/// @return The width in the row's units.
[[nodiscard]] inline int sidebarUnitsOf(int pixels,
                                        config::SidebarTokens const& tokens,
                                        SidebarScale scale) noexcept
{
    auto const units = scale.unitPixels > 0.0 ? static_cast<int>(std::lround(pixels / scale.unitPixels)) : 0;
    return clampSidebarUnits(units, tokens);
}

/// The sidebar's title row as its heading needs it, in logical pixels, from the sidebar's leading edge to its
/// trailing one: what stands before the heading, the heading, and the controls after it.
struct SidebarTitleRow
{
    double lead = 0.0; ///< Before the heading: the row's inset, and a toggle with its gap where one stands.
    double heading = 0.0;     ///< The heading at its natural width, in the font it is set in.
    double control = 0.0;     ///< One control after the heading.
    double controlGap = 0.0;  ///< Between two of them.
    std::size_t controls = 0; ///< How many stand after the heading.
    double trail = 0.0;       ///< After the last control: the row's end margin, and the divider.

    [[nodiscard]] constexpr bool operator==(SidebarTitleRow const&) const noexcept = default;
};

/// @param row The title row.
/// @return The narrowest sidebar that holds @p row with its heading whole, in logical pixels.
[[nodiscard]] constexpr double sidebarTitleRowWidth(SidebarTitleRow const& row) noexcept
{
    auto const gaps = row.controls > 0 ? row.controls - 1 : 0;
    return row.lead + row.heading + (static_cast<double>(row.controls) * row.control)
           + (static_cast<double>(gaps) * row.controlGap) + row.trail;
}

/// The sidebar's default width: the row's own, widened to what its title row needs to hold the heading
/// whole -- a wide UI font sets the heading wider than the row's width was chosen against.
/// @param tokens The row's sidebar tokens.
/// @param scale What one unit of them is.
/// @param titleRowPixels What the title row needs (sidebarTitleRowWidth), in logical pixels; 0 where it
///                       has no heading.
/// @return The default width in the row's units, within its bounds.
[[nodiscard]] inline int sidebarDefaultUnits(config::SidebarTokens const& tokens,
                                             SidebarScale scale,
                                             double titleRowPixels) noexcept
{
    auto const needed =
        scale.unitPixels > 0.0 ? static_cast<int>(std::ceil(titleRowPixels / scale.unitPixels)) : 0;
    return clampSidebarUnits(std::max(tokens.width, needed), tokens);
}

/// The sidebar's width in logical pixels.
/// @param visibility Whether the sidebar is shown: a hidden one takes no width.
/// @param units Its width in the row's units, as it was chosen (kept within the row's bounds here).
/// @param tokens The row's sidebar tokens.
/// @param scale What one unit of them is.
/// @return The width in pixels, 0 when hidden: what Chrome::width declares.
[[nodiscard]] inline int sidebarWidthPixels(config::SidebarVisibility visibility,
                                            int units,
                                            config::SidebarTokens const& tokens,
                                            SidebarScale scale) noexcept
{
    switch (visibility)
    {
        case config::SidebarVisibility::Hidden: return 0;
        case config::SidebarVisibility::Shown:
            return static_cast<int>(std::lround(clampSidebarUnits(units, tokens) * scale.unitPixels));
    }
    return 0;
}

/// The width the user last left the sidebar at, on a row that counts in @p unit.
/// @param prefs What the GUI state remembers.
/// @param unit What the row counts its extents in: a width in cells is no width in pixels, so each row reads
///             its own.
/// @return The width in the row's units; none before the user sized it on such a row.
[[nodiscard]] constexpr std::optional<int> rememberedSidebarUnits(config::SidebarPrefs const& prefs,
                                                                  config::LengthUnit unit) noexcept
{
    switch (unit)
    {
        case config::LengthUnit::Pixel: return prefs.widthPx;
        case config::LengthUnit::Cell: return prefs.widthCells;
    }
    return std::nullopt;
}

/// Records @p units as the sidebar's width on a row that counts in @p unit, leaving the width remembered
/// for the other kind of row as it was.
/// @param prefs The preferences to change.
/// @param unit What the row counts its extents in.
/// @param units The width in the row's units.
constexpr void rememberSidebarUnits(config::SidebarPrefs& prefs, config::LengthUnit unit, int units) noexcept
{
    switch (unit)
    {
        case config::LengthUnit::Pixel: prefs.widthPx = units; break;
        case config::LengthUnit::Cell: prefs.widthCells = units; break;
    }
}

} // namespace contour::geometry
