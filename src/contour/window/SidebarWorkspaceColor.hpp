// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a workspace's colour paints on the sidebar's rows, decided by the configured style
/// (`sidebar.workspace_color`), by the row's kind and by its state. Pure and Qt-free, as its tab-colour
/// sibling (SidebarTabColor.hpp) is: the QML only draws what it is handed.
///
/// `tint` fills the workspace's own row as the tab strip fills a coloured tab; `group` marks the workspace's
/// row and every row beneath it with a rail at the sidebar's leading edge over a faint wash, so its tabs read
/// as one group; `swatch` sets a small square before the workspace's name.

#include <contour/config/TabBarMode.hpp>
#include <contour/window/SidebarTabColor.hpp>
#include <contour/window/SidebarTree.hpp>
#include <contour/window/TabColorScheme.hpp>

#include <vtbackend/core/Color.hpp>

#include <optional>

namespace contour::window
{

/// How much of a workspace's colour the `group` wash keeps; the rest is the sidebar's fill. A third of a
/// resting coloured tab's (tabColorFraction(TabVisualState::Inactive)), so the wash reads as a faint band
/// that groups the rows, never as every row being tinted.
inline constexpr float WorkspaceWashFraction = 0.1F;

/// What a workspace's colour paints on one sidebar row; an empty member paints nothing of that kind.
struct SidebarWorkspaceColorPaint
{
    std::optional<vtbackend::RGBColor>
        fill;                               ///< Tint: the workspace row's fill, replacing its selection fill.
    std::optional<vtbackend::RGBColor> ink; ///< Tint: the ink of everything written on that fill.
    std::optional<vtbackend::RGBColor> rail;   ///< Group: the rail at the sidebar's leading edge.
    std::optional<vtbackend::RGBColor> wash;   ///< Group: a faint fill behind the row.
    std::optional<vtbackend::RGBColor> swatch; ///< Swatch: the square before the name.

    [[nodiscard]] bool operator==(SidebarWorkspaceColorPaint const&) const = default;
};

/// Decides what a workspace's colour paints on one sidebar row.
/// @param style          The configured style.
/// @param kind           The row's kind: the workspace's own row, or a tab or pane row beneath it.
/// @param workspaceColor The colour of the workspace the row belongs to; none paints nothing, in every style.
/// @param rowBackground  The sidebar's fill, the tint and the wash fade toward.
/// @param state          The row's prominence (sidebarTabVisualState).
/// @return Tint: on the workspace row, the faded fill and a black or white ink for it. Group: on every row,
///         the colour as the rail and its WorkspaceWashFraction fade as the wash. Swatch: on the workspace
///         row, the colour as the swatch.
[[nodiscard]] inline SidebarWorkspaceColorPaint sidebarWorkspaceColorPaint(
    config::SidebarWorkspaceColorStyle style,
    SidebarRowKind kind,
    std::optional<vtbackend::RGBColor> workspaceColor,
    vtbackend::RGBColor rowBackground,
    TabVisualState state) noexcept
{
    if (!workspaceColor)
        return {};
    auto const ownRow = kind == SidebarRowKind::Workspace;
    auto paint = SidebarWorkspaceColorPaint {};
    switch (style)
    {
        case config::SidebarWorkspaceColorStyle::Tint:
            if (ownRow)
            {
                paint.fill = tabBackgroundColor(*workspaceColor, rowBackground, state);
                paint.ink = contrastingTextColor(*paint.fill);
            }
            break;
        case config::SidebarWorkspaceColorStyle::Group:
            paint.rail = *workspaceColor;
            // The same faint fade whatever the row's state: the wash marks membership, not prominence.
            paint.wash = vtbackend::mixColor(rowBackground, *workspaceColor, WorkspaceWashFraction);
            break;
        case config::SidebarWorkspaceColorStyle::Swatch:
            if (ownRow)
                paint.swatch = *workspaceColor;
            break;
    }
    return paint;
}

/// The colour a status mark is drawn in on a sidebar row a workspace's colour paints: as on a tinted tab
/// row (sidebarMarkInk above), only the tint's fill can swallow a mark.
/// @param mark  The mark's semantic colour.
/// @param paint What the workspace's colour paints on the row (sidebarWorkspaceColorPaint).
/// @return @p mark, or the tint's ink where @p mark would not stand off the tint.
[[nodiscard]] inline vtbackend::RGBColor sidebarMarkInk(vtbackend::RGBColor mark,
                                                        SidebarWorkspaceColorPaint const& paint) noexcept
{
    return sidebarMarkInk(mark,
                          SidebarTabColorPaint { .fill = paint.fill, .bar = std::nullopt, .ink = paint.ink });
}

} // namespace contour::window
