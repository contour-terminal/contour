// SPDX-License-Identifier: Apache-2.0
#pragma once
/// @file
/// Where a node drag dropped on the sidebar goes. The sidebar is a vertical list, so a row's upper and lower
/// thirds are the gaps between rows and the middle is "into" -- the vertical counterpart of
/// TabBarDropPolicy.hpp. Pure and Qt-free: it reads the rows the view shows and what is dragged, nothing
/// else. Every position it returns is a real one in the directory (SidebarRow::position), so a filter or a
/// fold that hides rows does not shift where a drop lands.
#include <contour/window/DropZonePolicy.hpp>
#include <contour/window/SidebarTree.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <variant>

namespace contour::window
{

/// What is dragged, as the sidebar sees it. The caller derives it from the drag payload and the directory: a
/// workspace payload is a Workspace, or a OneTabWorkspace when it holds exactly one tab; a tab payload, or a
/// pane payload of a tab holding that one pane, is a Tab; any other pane is a Pane.
enum class DragSubject : uint8_t
{
    Workspace = 0,   ///< A workspace of several tabs (or none).
    OneTabWorkspace, ///< A workspace holding exactly one tab: it reorders as a workspace, and joins as its
                     ///< tab.
    Tab,             ///< A whole tab.
    Pane,            ///< One pane of a tab with others.
};

/// Whether the dragged node is all its workspace holds, which decides whether a new workspace for it would
/// change anything.
enum class WorkspaceShare : uint8_t
{
    Part = 0, ///< Its workspace holds more: a new workspace for it is a move.
    Whole,    ///< It is its workspace's only tab: a new workspace for it would only rebuild the one it has.
};

/// What is dragged, with what the policy needs to know of where it comes from.
struct SidebarDragged
{
    DragSubject subject = DragSubject::Pane; ///< What it is.
    /// The tab it is, or is in, or (a OneTabWorkspace) holds; none for a workspace of several tabs. A join
    /// into this tab is refused: it would move nothing anywhere.
    std::optional<TabRef> tab {};
    WorkspaceShare share = WorkspaceShare::Part; ///< Whether it is all its workspace holds.
    HostMoves host = HostMoves::Nodes;           ///< What the window's host can move (SidebarHostMoves).
};

/// Where over the sidebar a node is dropped.
enum class SidebarSpot : uint8_t
{
    WorkspaceRow = 0, ///< A workspace's row.
    TabRow,           ///< A tab's row.
    PaneRow,          ///< A pane's row.
    NewWorkspace,     ///< The tree's empty area below the last row, or the New Workspace control.
};

/// Which band of a row the pointer is in: the upper and lower thirds are the gaps beside the row, the middle
/// is the row itself.
enum class DropBand : uint8_t
{
    Upper = 0, ///< The upper third.
    Middle,    ///< The middle third.
    Lower,     ///< The lower third.
};

/// What a drop does, before it is told where exactly: one per (subject, spot, band) in SidebarDropRules.
enum class SidebarPlacement : uint8_t
{
    Refused = 0,  ///< Nothing.
    Before,       ///< Before the row, among its siblings.
    After,        ///< After the row, among its siblings.
    NearerGap,    ///< Before the row in its upper half, after it in its lower: a row with no "into".
    EndOfAbove,   ///< To the end of the workspace shown above the row; the first row has none: its start.
    End,          ///< To the end of the row's workspace.
    Start,        ///< To the start of the row's workspace.
    Join,         ///< Into the row's tab, as splits beside its active pane.
    NewWorkspace, ///< Into a workspace of its own.
};

/// One row of the drop table.
struct SidebarDropRule
{
    DragSubject subject;        ///< What is dragged.
    SidebarSpot spot;           ///< Where it is dropped.
    DropBand band;              ///< In which band of the spot; ignored for SidebarSpot::NewWorkspace.
    SidebarPlacement placement; ///< What the drop does.
};

/// The drop table: what a drag of each subject does at each spot. A combination no row names is refused, so
/// whatever lands on a pane row is. A workspace reorders among the workspaces; a tab or a pane goes into a
/// workspace as a tab, or into a gap between tabs, or joins a tab in its middle band; a one-tab workspace
/// joins a tab as its tab does; a tab or a pane dropped below the rows gets a workspace of its own.
inline constexpr auto SidebarDropRules = std::array {
    SidebarDropRule { .subject = DragSubject::Workspace,
                      .spot = SidebarSpot::WorkspaceRow,
                      .band = DropBand::Upper,
                      .placement = SidebarPlacement::Before },
    SidebarDropRule { .subject = DragSubject::Workspace,
                      .spot = SidebarSpot::WorkspaceRow,
                      .band = DropBand::Middle,
                      .placement = SidebarPlacement::NearerGap },
    SidebarDropRule { .subject = DragSubject::Workspace,
                      .spot = SidebarSpot::WorkspaceRow,
                      .band = DropBand::Lower,
                      .placement = SidebarPlacement::After },
    SidebarDropRule { .subject = DragSubject::OneTabWorkspace,
                      .spot = SidebarSpot::WorkspaceRow,
                      .band = DropBand::Upper,
                      .placement = SidebarPlacement::Before },
    SidebarDropRule { .subject = DragSubject::OneTabWorkspace,
                      .spot = SidebarSpot::WorkspaceRow,
                      .band = DropBand::Middle,
                      .placement = SidebarPlacement::NearerGap },
    SidebarDropRule { .subject = DragSubject::OneTabWorkspace,
                      .spot = SidebarSpot::WorkspaceRow,
                      .band = DropBand::Lower,
                      .placement = SidebarPlacement::After },
    SidebarDropRule { .subject = DragSubject::OneTabWorkspace,
                      .spot = SidebarSpot::TabRow,
                      .band = DropBand::Middle,
                      .placement = SidebarPlacement::Join },
    SidebarDropRule { .subject = DragSubject::Tab,
                      .spot = SidebarSpot::WorkspaceRow,
                      .band = DropBand::Upper,
                      .placement = SidebarPlacement::EndOfAbove },
    SidebarDropRule { .subject = DragSubject::Tab,
                      .spot = SidebarSpot::WorkspaceRow,
                      .band = DropBand::Middle,
                      .placement = SidebarPlacement::End },
    SidebarDropRule { .subject = DragSubject::Tab,
                      .spot = SidebarSpot::WorkspaceRow,
                      .band = DropBand::Lower,
                      .placement = SidebarPlacement::Start },
    SidebarDropRule { .subject = DragSubject::Tab,
                      .spot = SidebarSpot::TabRow,
                      .band = DropBand::Upper,
                      .placement = SidebarPlacement::Before },
    SidebarDropRule { .subject = DragSubject::Tab,
                      .spot = SidebarSpot::TabRow,
                      .band = DropBand::Middle,
                      .placement = SidebarPlacement::Join },
    SidebarDropRule { .subject = DragSubject::Tab,
                      .spot = SidebarSpot::TabRow,
                      .band = DropBand::Lower,
                      .placement = SidebarPlacement::After },
    SidebarDropRule { .subject = DragSubject::Tab,
                      .spot = SidebarSpot::NewWorkspace,
                      .band = DropBand::Middle,
                      .placement = SidebarPlacement::NewWorkspace },
    SidebarDropRule { .subject = DragSubject::Pane,
                      .spot = SidebarSpot::WorkspaceRow,
                      .band = DropBand::Upper,
                      .placement = SidebarPlacement::EndOfAbove },
    SidebarDropRule { .subject = DragSubject::Pane,
                      .spot = SidebarSpot::WorkspaceRow,
                      .band = DropBand::Middle,
                      .placement = SidebarPlacement::End },
    SidebarDropRule { .subject = DragSubject::Pane,
                      .spot = SidebarSpot::WorkspaceRow,
                      .band = DropBand::Lower,
                      .placement = SidebarPlacement::Start },
    SidebarDropRule { .subject = DragSubject::Pane,
                      .spot = SidebarSpot::TabRow,
                      .band = DropBand::Upper,
                      .placement = SidebarPlacement::Before },
    SidebarDropRule { .subject = DragSubject::Pane,
                      .spot = SidebarSpot::TabRow,
                      .band = DropBand::Middle,
                      .placement = SidebarPlacement::Join },
    SidebarDropRule { .subject = DragSubject::Pane,
                      .spot = SidebarSpot::TabRow,
                      .band = DropBand::Lower,
                      .placement = SidebarPlacement::After },
    SidebarDropRule { .subject = DragSubject::Pane,
                      .spot = SidebarSpot::NewWorkspace,
                      .band = DropBand::Middle,
                      .placement = SidebarPlacement::NewWorkspace },
};

/// What a sidebar drop does to the layout, as a host's move support tells the drops apart.
enum class SidebarLanding : uint8_t
{
    Reorder = 0,  ///< A workspace changes its place among the workspaces; no node moves.
    TabWell,      ///< A pane or a tab lands among a workspace's tabs.
    Join,         ///< A pane, a tab or a one-tab workspace's tab joins a tab as splits.
    NewWorkspace, ///< A pane or a tab gets a workspace of its own.
};

/// A row of the host table: a drop a host short of node moves still takes.
struct SidebarHostMove
{
    HostMoves host;         ///< The host.
    DragSubject subject;    ///< What is dragged.
    SidebarLanding landing; ///< What the drop does.
};

/// What a host that does not move nodes takes from the sidebar: only its own order of workspaces. A daemon
/// older than MoveNode moves no tab between workspaces either, since every sidebar move goes by MoveNode;
/// tmux moves nothing. A host that moves nodes takes every drop; anything a row does not name is refused.
inline constexpr auto SidebarHostMoves = std::array {
    SidebarHostMove {
        .host = HostMoves::Tabs, .subject = DragSubject::Workspace, .landing = SidebarLanding::Reorder },
    SidebarHostMove { .host = HostMoves::Tabs,
                      .subject = DragSubject::OneTabWorkspace,
                      .landing = SidebarLanding::Reorder },
    SidebarHostMove {
        .host = HostMoves::Nothing, .subject = DragSubject::Workspace, .landing = SidebarLanding::Reorder },
    SidebarHostMove { .host = HostMoves::Nothing,
                      .subject = DragSubject::OneTabWorkspace,
                      .landing = SidebarLanding::Reorder },
};

/// @param host What the window's host can move.
/// @param subject What is dragged.
/// @param landing What the drop does.
/// @return Whether @p host carries the drop out (SidebarHostMoves).
[[nodiscard]] constexpr bool sidebarHostTakes(HostMoves host,
                                              DragSubject subject,
                                              SidebarLanding landing) noexcept
{
    if (host == HostMoves::Nodes)
        return true;
    return std::ranges::any_of(SidebarHostMoves, [&](SidebarHostMove const& row) {
        return row.host == host && row.subject == subject && row.landing == landing;
    });
}

/// @param host What a window's host can move.
/// @return Why a move the host does not take is refused, as the tab strip says it (HostRefusals).
[[nodiscard]] constexpr MoveRefusal sidebarRefusalOf(HostMoves host) noexcept
{
    return HostRefusals[static_cast<size_t>(host)].refusal;
}

/// The fraction of a row's height, from each edge, that counts as the gap beside it.
inline constexpr double SidebarGapFraction = 1.0 / 3.0;

/// The band of a row a pointer is in.
/// @param yWithinRow The pointer's position within the row, 0 (top) to 1 (bottom).
[[nodiscard]] constexpr DropBand dropBandOf(double yWithinRow) noexcept
{
    if (yWithinRow < SidebarGapFraction)
        return DropBand::Upper;
    if (yWithinRow > 1.0 - SidebarGapFraction)
        return DropBand::Lower;
    return DropBand::Middle;
}

/// What @p placement does to the layout for @p subject: a place among the workspaces for a workspace, among
/// a workspace's tabs for a tab or a pane.
[[nodiscard]] constexpr SidebarLanding sidebarLandingOf(DragSubject subject,
                                                        SidebarPlacement placement) noexcept
{
    switch (placement)
    {
        case SidebarPlacement::Join: return SidebarLanding::Join;
        case SidebarPlacement::NewWorkspace: return SidebarLanding::NewWorkspace;
        case SidebarPlacement::Refused:
        case SidebarPlacement::Before:
        case SidebarPlacement::After:
        case SidebarPlacement::NearerGap:
        case SidebarPlacement::EndOfAbove:
        case SidebarPlacement::End:
        case SidebarPlacement::Start: break;
    }
    return subject == DragSubject::Workspace || subject == DragSubject::OneTabWorkspace
               ? SidebarLanding::Reorder
               : SidebarLanding::TabWell;
}

/// What dropping @p dragged at @p spot does.
/// @param dragged What is dragged.
/// @param spot Where.
/// @param yWithinRow The pointer's position within the row, 0 (top) to 1 (bottom); 0.5 for a spot with no
///        rows.
/// @param target The tab the spot is the row of, for a TabRow; a join into the dragged node's own tab is
///        refused.
/// @return The placement: the table's (SidebarDropRules), NearerGap resolved by the half the pointer is in,
///         and Refused for a join into its own tab, for a new workspace for what already is all of one, and
///         for what the window's host does not move (sidebarHostTakes()).
[[nodiscard]] constexpr SidebarPlacement sidebarPlacementOf(SidebarDragged const& dragged,
                                                            SidebarSpot spot,
                                                            double yWithinRow,
                                                            std::optional<TabRef> target) noexcept
{
    auto const band = spot == SidebarSpot::NewWorkspace ? DropBand::Middle : dropBandOf(yWithinRow);
    auto const rule = std::ranges::find_if(SidebarDropRules, [&](SidebarDropRule const& candidate) {
        return candidate.subject == dragged.subject && candidate.spot == spot && candidate.band == band;
    });
    if (rule == SidebarDropRules.end())
        return SidebarPlacement::Refused;
    auto const placement = [&] {
        switch (rule->placement)
        {
            case SidebarPlacement::NearerGap:
                return yWithinRow < 0.5 ? SidebarPlacement::Before : SidebarPlacement::After;
            case SidebarPlacement::Join:
                return target && dragged.tab == target ? SidebarPlacement::Refused : SidebarPlacement::Join;
            case SidebarPlacement::NewWorkspace:
                return dragged.share == WorkspaceShare::Whole ? SidebarPlacement::Refused
                                                              : SidebarPlacement::NewWorkspace;
            default: return rule->placement;
        }
    }();
    return sidebarHostTakes(dragged.host, dragged.subject, sidebarLandingOf(dragged.subject, placement))
               ? placement
               : SidebarPlacement::Refused;
}

/// What the sidebar shows while a drag hovers a spot.
enum class DropIndicator : uint8_t
{
    Refused = 0, ///< Nothing; the pointer shows the drop is not taken.
    LineAbove,   ///< A line at the row's upper edge: it lands before the row.
    LineBelow,   ///< A line at the row's lower edge: it lands after the row.
    Highlight,   ///< The row (or the New Workspace area) is highlighted: it lands in it.
};

/// The indicator that shows @p placement.
[[nodiscard]] constexpr DropIndicator dropIndicatorOf(SidebarPlacement placement) noexcept
{
    switch (placement)
    {
        case SidebarPlacement::Refused: return DropIndicator::Refused;
        case SidebarPlacement::Before:
        case SidebarPlacement::EndOfAbove: return DropIndicator::LineAbove;
        case SidebarPlacement::After:
        case SidebarPlacement::Start: return DropIndicator::LineBelow;
        case SidebarPlacement::NearerGap: // resolved by sidebarPlacementOf(); shown as the row it lands in
        case SidebarPlacement::End:
        case SidebarPlacement::Join:
        case SidebarPlacement::NewWorkspace: return DropIndicator::Highlight;
    }
    return DropIndicator::Refused;
}

/// MoveTab::toPosition for "after the last tab".
inline constexpr auto AppendTab = std::numeric_limits<std::size_t>::max();

/// Drop that reorders the workspaces: the dragged one goes to this position.
struct ReorderWorkspace
{
    std::size_t to = 0; ///< The position among all the workspaces to insert before.
};

/// Drop of a tab, or of a pane that becomes one, into a workspace.
struct MoveTab
{
    uint64_t toWorkspace = 0;   ///< The workspace's id.
    std::size_t toPosition = 0; ///< The position among its tabs; AppendTab for the end.
};

/// Drop of a pane, a tab or a one-tab workspace into a tab: its panes join that tab as splits.
struct JoinTab
{
    TabRef tab; ///< The tab joined.
};

/// Drop of a pane or a tab into a workspace of its own.
struct IntoNewWorkspace
{
};

/// The place a dragged item ends at, from the place it is dropped before.
/// @param from Where it is now.
/// @param insertBefore The place it is dropped before (ReorderWorkspace::to), in the list as it is now.
/// @return The place it ends at once it has left its own: one less when it moves down past itself.
[[nodiscard]] constexpr std::size_t placeAfterMove(std::size_t from, std::size_t insertBefore) noexcept
{
    return insertBefore > from ? insertBefore - 1 : insertBefore;
}

/// The place a tab ends at among the tabs of the workspace it is dropped into.
/// @param from Where it is now, when it is dropped into its own workspace; none from another one.
/// @param insertBefore The place it is dropped before (MoveTab::toPosition), AppendTab for the end.
/// @param count The destination's tabs, the dragged one included when it is its own.
/// @return The 0-based place, as TerminalSessionManager::moveTabToWorkspace() takes it.
[[nodiscard]] constexpr std::size_t tabPlaceAfterMove(std::optional<std::size_t> from,
                                                      std::size_t insertBefore,
                                                      std::size_t count) noexcept
{
    // Its own workspace holds it already: the last place is one short of the count.
    auto const end = from ? std::max<std::size_t>(count, 1) - 1 : count;
    if (insertBefore == AppendTab)
        return end;
    return std::min(from ? placeAfterMove(*from, insertBefore) : insertBefore, end);
}

/// The drop is not allowed here.
struct Refuse
{
};

/// What dropping on the sidebar does.
using SidebarDrop = std::variant<ReorderWorkspace, MoveTab, JoinTab, IntoNewWorkspace, Refuse>;

/// What the sidebar moves: a pane by its key, or a whole tab. A one-tab workspace moves as its tab.
using SidebarNode = std::variant<PaneKey, TabRef>;

/// Where a moved node goes: the drops that move a pane or a tab.
using NodeDestination = std::variant<MoveTab, JoinTab, IntoNewWorkspace>;

/// @return What @p destination does to the layout.
[[nodiscard]] constexpr SidebarLanding sidebarLandingOf(NodeDestination const& destination) noexcept
{
    if (std::holds_alternative<JoinTab>(destination))
        return SidebarLanding::Join;
    if (std::holds_alternative<IntoNewWorkspace>(destination))
        return SidebarLanding::NewWorkspace;
    return SidebarLanding::TabWell;
}

/// @return What @p node is to the host table: a tab, or a pane.
[[nodiscard]] constexpr DragSubject sidebarSubjectOf(SidebarNode const& node) noexcept
{
    return std::holds_alternative<TabRef>(node) ? DragSubject::Tab : DragSubject::Pane;
}

/// The spot a row is.
[[nodiscard]] constexpr SidebarSpot sidebarSpotOf(SidebarRow const& row) noexcept
{
    switch (row.key.kind)
    {
        case SidebarRowKind::Workspace: return SidebarSpot::WorkspaceRow;
        case SidebarRowKind::Tab: return SidebarSpot::TabRow;
        case SidebarRowKind::Pane: break;
    }
    return SidebarSpot::PaneRow;
}

/// The tab a drop on @p row would join: its own, for a tab row; none for any other.
[[nodiscard]] constexpr std::optional<TabRef> joinTargetOf(SidebarRow const& row) noexcept
{
    if (row.key.kind != SidebarRowKind::Tab)
        return std::nullopt;
    return TabRef { .workspace = row.key.workspace, .tab = row.key.tab };
}

/// Resolves a drop on a sidebar row.
/// @param rows The rows as shown.
/// @param rowIndex The row the pointer is over; out of range is refused.
/// @param yWithinRow The pointer's position within that row, 0 (top) to 1 (bottom).
/// @param dragged What is dragged.
/// @return What sidebarPlacementOf() decides, at the row's real position in the directory.
[[nodiscard]] constexpr SidebarDrop resolveSidebarDrop(std::span<SidebarRow const> rows,
                                                       int rowIndex,
                                                       double yWithinRow,
                                                       SidebarDragged const& dragged) noexcept
{
    if (rowIndex < 0 || static_cast<std::size_t>(rowIndex) >= rows.size())
        return Refuse {};
    auto const& row = rows[static_cast<std::size_t>(rowIndex)];
    auto const isTab = row.key.kind == SidebarRowKind::Tab;
    switch (sidebarPlacementOf(dragged, sidebarSpotOf(row), yWithinRow, joinTargetOf(row)))
    {
        case SidebarPlacement::Refused:
        case SidebarPlacement::NearerGap: break;
        case SidebarPlacement::Before:
            if (isTab)
                return MoveTab { .toWorkspace = row.key.workspace, .toPosition = row.position };
            return ReorderWorkspace { .to = row.position };
        case SidebarPlacement::After:
            if (isTab)
                return MoveTab { .toWorkspace = row.key.workspace, .toPosition = row.position + 1 };
            return ReorderWorkspace { .to = row.position + 1 };
        case SidebarPlacement::EndOfAbove: {
            auto const above = rows.first(static_cast<std::size_t>(rowIndex)) | std::views::reverse;
            auto const previous = std::ranges::find(
                above, SidebarRowKind::Workspace, [](SidebarRow const& r) { return r.key.kind; });
            if (previous != above.end())
                return MoveTab { .toWorkspace = previous->key.workspace, .toPosition = AppendTab };
            return MoveTab { .toWorkspace = row.key.workspace, .toPosition = 0 };
        }
        case SidebarPlacement::End:
            return MoveTab { .toWorkspace = row.key.workspace, .toPosition = AppendTab };
        case SidebarPlacement::Start: return MoveTab { .toWorkspace = row.key.workspace, .toPosition = 0 };
        case SidebarPlacement::Join:
            return JoinTab { .tab = TabRef { .workspace = row.key.workspace, .tab = row.key.tab } };
        case SidebarPlacement::NewWorkspace: return IntoNewWorkspace {};
    }
    return Refuse {};
}

/// The indicator a drag hovering a sidebar row shows: what resolveSidebarDrop() would do there.
/// @param rows The rows as shown.
/// @param rowIndex The row the pointer is over; out of range shows a refusal.
/// @param yWithinRow The pointer's position within that row, 0 (top) to 1 (bottom).
/// @param dragged What is dragged.
[[nodiscard]] constexpr DropIndicator sidebarDropIndicator(std::span<SidebarRow const> rows,
                                                           int rowIndex,
                                                           double yWithinRow,
                                                           SidebarDragged const& dragged) noexcept
{
    if (rowIndex < 0 || static_cast<std::size_t>(rowIndex) >= rows.size())
        return DropIndicator::Refused;
    auto const& row = rows[static_cast<std::size_t>(rowIndex)];
    return dropIndicatorOf(sidebarPlacementOf(dragged, sidebarSpotOf(row), yWithinRow, joinTargetOf(row)));
}

/// Resolves a drop on the tree's empty area below the last row, or on the New Workspace control.
/// @param dragged What is dragged.
/// @return IntoNewWorkspace for a tab or a pane that is not all its workspace holds; Refuse otherwise.
[[nodiscard]] constexpr SidebarDrop resolveNewWorkspaceDrop(SidebarDragged const& dragged) noexcept
{
    if (sidebarPlacementOf(dragged, SidebarSpot::NewWorkspace, 0.5, std::nullopt)
        == SidebarPlacement::NewWorkspace)
        return IntoNewWorkspace {};
    return Refuse {};
}

} // namespace contour::window
