// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ChromeTokenGroups.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace contour::window
{

/// Where the find bar sits in its pane.
enum class FindBarAnchor : uint8_t
{
    TopRight = 0, //!< At the pane's top right: under its header where it has one.
    BottomRight,  //!< At the pane's bottom right, out of the current match's way.
    HeaderRow,    //!< In the pane header's own row (the Terminal row).
};

/// Number of @c FindBarAnchor enumerators, derived from the last one.
inline constexpr size_t FindBarAnchorCount = static_cast<size_t>(FindBarAnchor::HeaderRow) + 1;

/// Whether the pane shows an inline header above its terminal.
enum class PaneHeaderPresence : uint8_t
{
    Absent = 0, //!< No header, or one overlaid on the terminal (Minimal while ⌥ is held).
    Present,    //!< A header row above the terminal.
};

/// Number of @c PaneHeaderPresence enumerators, derived from the last one.
inline constexpr size_t PaneHeaderPresenceCount = static_cast<size_t>(PaneHeaderPresence::Present) + 1;

/// A vertical band of the pane: the rows a match covers, in the pane's coordinates.
struct RowBand
{
    double top = 0;    ///< Its top edge.
    double bottom = 0; ///< Its bottom edge, below @ref top.

    bool operator==(RowBand const&) const = default;
};

/// What decides where the find bar goes. Extents are logical pixels, in the coordinates of the item the bar
/// is placed in: the pane's terminal, whose top is already below an inline header.
struct FindBarInputs
{
    config::FindBarHost host = config::FindBarHost::Overlay; ///< Where the style hosts the bar.
    PaneHeaderPresence header = PaneHeaderPresence::Absent;  ///< Whether the pane has an inline header.
    double paneWidth = 0;                                    ///< The pane's width.
    double paneHeight = 0;                                   ///< The pane's height.
    double barWidth = 0;                                     ///< The bar's width, where it fits.
    double barHeight = 0;                                    ///< The bar's height.
    double insetX = 0;                                       ///< Its inset from the pane's right edge.
    double insetY = 0;                      ///< Its inset from the pane's top or bottom edge.
    std::optional<RowBand> currentMatch {}; ///< The current match's rows, if any.
};

/// Where the find bar goes.
struct FindBarPlacement
{
    FindBarAnchor anchor = FindBarAnchor::TopRight; ///< Which place.
    double x = 0;                                   ///< Its left edge.
    double y = 0;                                   ///< Its top edge.
    double width = 0;                               ///< Its width: the bar's, or what the pane leaves of it.

    bool operator==(FindBarPlacement const&) const = default;
};

/// Places the find bar in a pane, by these rules, in order:
///   1. a style that hosts it in the header row, on a pane with a header: in that row, full width;
///   2. otherwise it is the bar's width, shrunk to what the pane leaves inside its insets, at the right;
///   3. at the bottom right, where the current match lies under the top position and the bottom one is below
///      it without overlapping it;
///   4. else at the top right.
/// @param in What decides it.
/// @return The placement.
[[nodiscard]] constexpr FindBarPlacement placeFindBar(FindBarInputs const& in) noexcept
{
    if (in.host == config::FindBarHost::PaneHeaderRow && in.header == PaneHeaderPresence::Present)
        return { .anchor = FindBarAnchor::HeaderRow, .x = 0, .y = 0, .width = in.paneWidth };

    auto const width = std::clamp(in.paneWidth - (2 * in.insetX), 0.0, in.barWidth);
    auto const x = in.paneWidth - in.insetX - width;
    auto const top = in.insetY;
    auto const bottom = in.paneHeight - in.insetY - in.barHeight;

    auto const covers = [&](RowBand band, double y) {
        return band.top < y + in.barHeight && band.bottom > y;
    };
    if (in.currentMatch && covers(*in.currentMatch, top) && !covers(*in.currentMatch, bottom)
        && bottom >= top + in.barHeight)
        return { .anchor = FindBarAnchor::BottomRight, .x = x, .y = bottom, .width = width };
    return { .anchor = FindBarAnchor::TopRight, .x = x, .y = top, .width = width };
}

/// The band rows @p first to @p last take, as a terminal lays its rows out.
/// @param first     The first row.
/// @param last      The last row.
/// @param marginTop The terminal's top margin.
/// @param rowHeight A row's height.
/// @return Their band.
[[nodiscard]] constexpr RowBand rowBandOf(int first, int last, double marginTop, double rowHeight) noexcept
{
    return { .top = marginTop + (first * rowHeight), .bottom = marginTop + ((last + 1) * rowHeight) };
}

/// The name QML compares a placement's anchor with.
/// @param anchor The anchor.
/// @return "topRight", "bottomRight" or "headerRow".
[[nodiscard]] constexpr std::string_view findBarAnchorName(FindBarAnchor anchor) noexcept
{
    switch (anchor)
    {
        case FindBarAnchor::TopRight: return "topRight";
        case FindBarAnchor::BottomRight: return "bottomRight";
        case FindBarAnchor::HeaderRow: return "headerRow";
    }
    return {};
}

} // namespace contour::window
