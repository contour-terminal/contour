// SPDX-License-Identifier: Apache-2.0
#include <contour/command/ContextMenuTable.hpp>
#include <contour/command/SidebarMenus.hpp>

#include <array>
#include <ranges>
#include <span>
#include <variant>

namespace contour::command
{

namespace
{
    // {{{ Workspace rows
    using WorkspaceTable = detail::MenuTable<WorkspaceMenuState>;

    bool canMoveUp(WorkspaceMenuState const& state) noexcept
    {
        return state.position > 0;
    }

    bool canMoveDown(WorkspaceMenuState const& state) noexcept
    {
        return state.position + 1 < state.count;
    }

    /// A window keeps its last workspace, as a daemon keeps its own last one.
    bool canClose(WorkspaceMenuState const& state) noexcept
    {
        return state.count > 1;
    }

    [[nodiscard]] std::span<WorkspaceTable::Row const> workspaceMenuTemplate()
    {
        using namespace actions;

        static auto const rows = std::array {
            WorkspaceTable::command(RenameWorkspace {}, "Rename…"),
            WorkspaceTable::command(MarkSeen {}, "Mark Seen"),
            WorkspaceTable::command(CreateNewTab {}, "New Tab Here"),

            WorkspaceTable::separator(),

            WorkspaceTable::command(MoveWorkspaceUp {}, "Move Up").enabledWhen(canMoveUp),
            WorkspaceTable::command(MoveWorkspaceDown {}, "Move Down").enabledWhen(canMoveDown),

            WorkspaceTable::separator(),

            WorkspaceTable::command(CloseWorkspace {}, "Close Workspace…").enabledWhen(canClose),
        };
        return rows;
    }
    // }}}

    // {{{ Tab rows
    using TabTable = detail::MenuTable<SidebarTabMenuState>;

    /// The tab strip's menu, with its close row ending in an ellipsis: from the sidebar it always asks first.
    [[nodiscard]] std::vector<ContextMenuEntry> tabStripRows(SidebarTabMenuState const& state)
    {
        auto entries = buildTabContextMenu(state.tab);
        for (auto& entry: entries)
            if (std::holds_alternative<actions::CloseTab>(entry.action))
                entry.title = "Close Tab…";
        return entries;
    }

    [[nodiscard]] std::vector<ContextMenuEntry> moveTargetRows(SidebarTabMenuState const& state)
    {
        return std::ranges::to<std::vector>(state.others | std::views::transform([](auto const& choice) {
                                                return detail::commandEntry(
                                                    choice.title,
                                                    actions::MoveTabToWorkspace { .index = choice.position });
                                            }));
    }

    [[nodiscard]] std::span<TabTable::Row const> sidebarTabMenuTemplate()
    {
        static auto const rows = std::array {
            TabTable::rows(tabStripRows),
            TabTable::separator(),
            TabTable::submenu("Move to Workspace", moveTargetRows),
        };
        return rows;
    }
    // }}}

    // {{{ Pane rows
    using PaneTable = detail::MenuTable<SidebarPaneMenuState>;

    [[nodiscard]] std::span<PaneTable::Row const> sidebarPaneMenuTemplate()
    {
        using namespace actions;

        static auto const rows = std::array {
            PaneTable::command(GoToPane {}, "Go to Pane"),
            PaneTable::command(MarkSeen {}, "Mark Seen"),

            PaneTable::separator(),

            PaneTable::command(ClosePane {}, "Close Pane…"),
        };
        return rows;
    }
    // }}}

} // namespace

std::vector<ContextMenuEntry> buildWorkspaceMenu(WorkspaceMenuState const& state)
{
    auto entries = WorkspaceTable::buildRows(workspaceMenuTemplate(), state);
    detail::dropRedundantSeparators(entries);
    return entries;
}

std::vector<ContextMenuEntry> buildSidebarTabMenu(SidebarTabMenuState const& state)
{
    auto entries = TabTable::buildRows(sidebarTabMenuTemplate(), state);
    detail::dropRedundantSeparators(entries);
    return entries;
}

std::vector<ContextMenuEntry> buildSidebarPaneMenu(SidebarPaneMenuState const& state)
{
    auto entries = PaneTable::buildRows(sidebarPaneMenuTemplate(), state);
    detail::dropRedundantSeparators(entries);
    return entries;
}

} // namespace contour::command
