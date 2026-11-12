// SPDX-License-Identifier: Apache-2.0
#pragma once
/// @file
/// Where a node drag dropped on the sidebar goes. The sidebar is a vertical list, so a row's upper and lower
/// thirds are the gaps between rows and the middle is "into" -- the vertical counterpart of
/// TabBarDropPolicy.hpp. Pure and Qt-free: it reads the rows the view shows and what is dragged, nothing
/// else. Every position it returns is a real one in the directory (SidebarRow::position), so a filter or a
/// fold that hides rows does not shift where a drop lands.
#include <contour/window/SidebarTree.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <variant>

namespace contour::window
{

/// What is dragged, as the sidebar sees it. The caller derives it from the drag payload: a workspace
/// payload is a Workspace, a tab payload or a pane payload of a tab holding that one pane is a Tab, any other
/// pane is a Pane.
enum class DragSubject : uint8_t
{
    Workspace = 0, ///< A whole workspace.
    Tab,           ///< A whole tab.
    Pane,          ///< One pane of a tab with others; the sidebar takes no pane drop.
};

/// The fraction of a row's height, from each edge, that counts as the gap beside it.
inline constexpr double SidebarGapFraction = 1.0 / 3.0;

/// MoveTab::toPosition for "after the last tab".
inline constexpr auto AppendTab = std::numeric_limits<std::size_t>::max();

/// Drop that reorders the workspaces: the dragged one goes to this position.
struct ReorderWorkspace
{
    std::size_t to = 0; ///< The position among all the workspaces to insert before.
};

/// Drop of a tab into a workspace.
struct MoveTab
{
    uint64_t toWorkspace = 0;   ///< The workspace's id.
    std::size_t toPosition = 0; ///< The position among its tabs; AppendTab for the end.
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
using SidebarDrop = std::variant<ReorderWorkspace, MoveTab, Refuse>;

/// Resolves a drop on a sidebar row.
/// @param rows The rows as shown.
/// @param rowIndex The row the pointer is over; out of range is refused.
/// @param yWithinRow The pointer's position within that row, 0 (top) to 1 (bottom).
/// @param subject What is dragged.
/// @return By subject and spot:
///   - Workspace on a workspace row: reorder before it in the upper half, after it in the lower; on any
///     other row, refused.
///   - Tab on a workspace row: in its upper third, to the end of the workspace shown above it (the first
///     row has none: the start of this one); in the middle, to the end of it; in the lower third, to the
///     start of it. On a tab row: before it in the upper half, after it in the lower.
///   - Pane: always refused, as is anything on a pane row.
[[nodiscard]] constexpr SidebarDrop resolveSidebarDrop(std::span<SidebarRow const> rows,
                                                       int rowIndex,
                                                       double yWithinRow,
                                                       DragSubject subject) noexcept
{
    if (rowIndex < 0 || static_cast<std::size_t>(rowIndex) >= rows.size())
        return Refuse {};
    auto const& row = rows[static_cast<std::size_t>(rowIndex)];
    switch (subject)
    {
        case DragSubject::Workspace:
            if (row.key.kind != SidebarRowKind::Workspace)
                return Refuse {};
            return ReorderWorkspace { .to = yWithinRow < 0.5 ? row.position : row.position + 1 };
        case DragSubject::Tab:
            switch (row.key.kind)
            {
                case SidebarRowKind::Workspace: {
                    if (yWithinRow >= SidebarGapFraction && yWithinRow <= 1.0 - SidebarGapFraction)
                        return MoveTab { .toWorkspace = row.key.workspace, .toPosition = AppendTab };
                    if (yWithinRow > 1.0 - SidebarGapFraction)
                        return MoveTab { .toWorkspace = row.key.workspace, .toPosition = 0 };
                    auto const above = rows.first(static_cast<std::size_t>(rowIndex)) | std::views::reverse;
                    auto const previous = std::ranges::find(
                        above, SidebarRowKind::Workspace, [](SidebarRow const& r) { return r.key.kind; });
                    if (previous != above.end())
                        return MoveTab { .toWorkspace = previous->key.workspace, .toPosition = AppendTab };
                    return MoveTab { .toWorkspace = row.key.workspace, .toPosition = 0 };
                }
                case SidebarRowKind::Tab:
                    return MoveTab { .toWorkspace = row.key.workspace,
                                     .toPosition = yWithinRow < 0.5 ? row.position : row.position + 1 };
                case SidebarRowKind::Pane: break;
            }
            break;
        case DragSubject::Pane: break;
    }
    return Refuse {};
}

} // namespace contour::window
