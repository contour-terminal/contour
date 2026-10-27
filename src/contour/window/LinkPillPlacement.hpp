// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/window/FindBarPlacement.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string_view>

namespace contour::window
{

/// What hosts the link pill: the pane's grid, or the header above it.
enum class LinkPillHost : uint8_t
{
    Grid = 0,   ///< No inline header: the pill floats over the grid's blank cells.
    PaneHeader, ///< The pane's inline header: the pill sits in its trailing area, over no cells.
};

/// How many LinkPillHost values there are, derived from the last one.
inline constexpr size_t LinkPillHostCount = static_cast<size_t>(LinkPillHost::PaneHeader) + 1;

/// Where the pane's find bar is, as the link pill sees it: which of the pill's places it holds.
enum class FindBarPresence : uint8_t
{
    Closed = 0, ///< No bar: the pill may take any place.
    Top,        ///< In the header row or at the top right: it holds the header's trailing area and the top.
    Bottom,     ///< At the bottom right, out of the current match's way: it holds the bottom.
};

/// How many FindBarPresence values there are, derived from the last one.
inline constexpr size_t FindBarPresenceCount = static_cast<size_t>(FindBarPresence::Bottom) + 1;

/// The presence of a shown find bar at each FindBarAnchor, indexed by it.
inline constexpr auto FindBarPresenceAt = std::array<FindBarPresence, FindBarAnchorCount> {
    FindBarPresence::Top,    // TopRight
    FindBarPresence::Bottom, // BottomRight
    FindBarPresence::Top,    // HeaderRow
};

/// @param anchor Where a shown find bar sits (window::placeFindBar).
/// @return The places it holds against the link pill.
[[nodiscard]] constexpr FindBarPresence findBarPresenceAt(FindBarAnchor anchor) noexcept
{
    return FindBarPresenceAt[static_cast<size_t>(anchor)];
}

/// Where the link pill goes.
enum class LinkPillAnchor : uint8_t
{
    Hidden = 0,     ///< Nowhere: no blank place is big enough.
    HeaderTrailing, ///< The pane header's trailing area.
    TopRight,       ///< The grid's top right, inset.
    BottomRight,    ///< The grid's bottom right, inset.
};

/// How many LinkPillAnchor values there are, derived from the last one.
inline constexpr size_t LinkPillAnchorCount = static_cast<size_t>(LinkPillAnchor::BottomRight) + 1;

/// The grid the pill may cover, as vtbackend::OverlayClearance describes it, in the pane's pixels.
struct LinkPillGrid
{
    double marginLeft = 0;                        ///< The grid's left edge within the pane.
    double marginTop = 0;                         ///< The first drawn row's top within the pane.
    double cellWidth = 0;                         ///< A cell's width.
    double cellHeight = 0;                        ///< A cell's height.
    int columns = 0;                              ///< The page's width in cells.
    std::span<int const> trailingBlankColumns {}; ///< Per drawn row, the blank cells at its right end.
    std::span<int const> avoidRows {};            ///< The drawn rows the pill must never cover.
};

/// Everything the pill's place depends on.
struct LinkPillInputs
{
    LinkPillHost host = LinkPillHost::Grid;            ///< What hosts it.
    FindBarPresence findBar = FindBarPresence::Closed; ///< Which places the find bar holds.
    double paneWidth = 0;                              ///< The pane's width.
    double paneHeight = 0;                             ///< The pane's height.
    double pillWidth = 0;                              ///< The pill's natural width.
    double pillHeight = 0;                             ///< The pill's height.
    double insetTop = 0;                               ///< Its gap to the pane's top and bottom.
    double insetRight = 0;                             ///< Its gap to the pane's right edge.
    LinkPillGrid grid {};                              ///< The cells it may cover.
};

/// Where the pill goes and how wide it is.
struct LinkPillPlacement
{
    LinkPillAnchor anchor = LinkPillAnchor::Hidden; ///< Where.
    double x = 0;                                   ///< Its left edge within the pane (or the header).
    double y = 0;                                   ///< Its top edge within the pane (or the header).
    double width = 0;                               ///< Its width: its natural one, shrunk to fit the pane.

    [[nodiscard]] bool operator==(LinkPillPlacement const&) const = default;
};

namespace detail
{
    /// How far short of a row's bottom edge a pill's bottom may end and still not touch the next row.
    inline constexpr double RowEdgeEpsilon = 1e-9;

    /// The anchors' QML names, indexed by LinkPillAnchor.
    inline constexpr auto LinkPillAnchorNames = std::array<std::string_view, LinkPillAnchorCount> {
        "hidden",
        "headerTrailing",
        "topRight",
        "bottomRight",
    };

    /// Whether a pill of @p height at @p top, over the columns from @p firstColumn, covers only blank cells
    /// of rows it may cover. The rows outside the grid (its margins) cover no cell.
    [[nodiscard]] inline bool isBandClear(LinkPillGrid const& grid,
                                          double top,
                                          double height,
                                          int firstColumn) noexcept
    {
        auto const needed = grid.columns - firstColumn;
        auto const rows = static_cast<int>(grid.trailingBlankColumns.size());
        auto const first =
            std::max(0, static_cast<int>(std::floor((top - grid.marginTop) / grid.cellHeight)));
        auto const last =
            std::min(rows - 1,
                     static_cast<int>(
                         std::floor(((top + height - grid.marginTop) / grid.cellHeight) - RowEdgeEpsilon)));
        return std::ranges::none_of(std::views::iota(first, std::max(first, last + 1)), [&](int row) {
            return grid.trailingBlankColumns[static_cast<size_t>(row)] < needed
                   || std::ranges::contains(grid.avoidRows, row);
        });
    }
} // namespace detail

/// Where the link pill goes (spec R-g), so that it only ever covers blank cells and never the prompt.
///
/// In order: no pill, nowhere. In a pane header, its trailing area, unless the find bar holds the top.
/// Otherwise at the grid's top right when every cell under it is blank and none is on an avoided row, unless
/// the find bar holds the top; else at the bottom right under the same test, unless the find bar holds the
/// bottom; else nowhere. So the pill and the find bar never overlap. The pill is shrunk to the
/// pane's width less its insets, and never moves to dodge the link: a link's cells are not blank.
/// @param in What the place depends on.
/// @return The place; LinkPillAnchor::Hidden with no extent where there is none.
[[nodiscard]] inline LinkPillPlacement placeLinkPill(LinkPillInputs const& in) noexcept
{
    if (in.pillWidth <= 0 || in.pillHeight <= 0)
        return {};
    if (in.host == LinkPillHost::PaneHeader && in.findBar != FindBarPresence::Top)
        return { .anchor = LinkPillAnchor::HeaderTrailing, .x = 0, .y = 0, .width = in.pillWidth };

    auto const width = std::min(in.pillWidth, in.paneWidth - (2 * in.insetRight));
    if (width <= 0 || in.grid.cellWidth <= 0 || in.grid.cellHeight <= 0)
        return {};
    auto const x = in.paneWidth - in.insetRight - width;
    auto const firstColumn =
        std::max(0, static_cast<int>(std::floor((x - in.grid.marginLeft) / in.grid.cellWidth)));

    if (in.findBar != FindBarPresence::Top
        && detail::isBandClear(in.grid, in.insetTop, in.pillHeight, firstColumn))
        return { .anchor = LinkPillAnchor::TopRight, .x = x, .y = in.insetTop, .width = width };

    auto const bottom = in.paneHeight - in.insetTop - in.pillHeight;
    if (in.findBar != FindBarPresence::Bottom && bottom > in.insetTop
        && detail::isBandClear(in.grid, bottom, in.pillHeight, firstColumn))
        return { .anchor = LinkPillAnchor::BottomRight, .x = x, .y = bottom, .width = width };
    return {};
}

/// The QML name of @p anchor.
/// @param anchor The anchor.
/// @return `"hidden"`, `"headerTrailing"`, `"topRight"` or `"bottomRight"`.
[[nodiscard]] constexpr std::string_view linkPillAnchorName(LinkPillAnchor anchor) noexcept
{
    return detail::LinkPillAnchorNames[static_cast<size_t>(anchor)];
}

} // namespace contour::window
