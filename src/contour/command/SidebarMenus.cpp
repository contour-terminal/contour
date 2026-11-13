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
    // {{{ Tab choices
    /// One header per workspace, its joinable tabs beneath it, each running MoveToTab on the row.
    [[nodiscard]] std::vector<ContextMenuEntry> joinRows(std::span<TabChoiceGroup const> groups)
    {
        auto entries = std::vector<ContextMenuEntry> {};
        for (auto const& group: groups)
        {
            entries.push_back(ContextMenuEntry { .kind = ContextMenuEntryKind::Header,
                                                 .title = group.workspace,
                                                 .action = {},
                                                 .availability = RowAvailability::Disabled,
                                                 .check = RowCheck::None,
                                                 .role = MenuItemRole::None,
                                                 .swatch = std::nullopt,
                                                 .children = {} });
            for (auto const& choice: group.tabs)
                entries.push_back(detail::commandEntry(
                    choice.title, actions::MoveToTab { .workspace = choice.workspace, .tab = choice.tab }));
        }
        return entries;
    }

    template <typename State>
    [[nodiscard]] std::vector<ContextMenuEntry> joinRowsOf(State const& state)
    {
        return joinRows(state.joinable);
    }

    /// Whether the window's host moves panes and tabs anywhere: a join, a new tab and a new workspace.
    template <typename State>
    bool movesNodes(State const& state) noexcept
    {
        return state.moves == MenuMoves::Nodes;
    }
    // }}}

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
            // Empty, and so not drawn, for a workspace of several tabs.
            WorkspaceTable::submenu("Merge into Tab", joinRowsOf<WorkspaceMenuState>)
                .enabledWhen(movesNodes<WorkspaceMenuState>),

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

    /// A new workspace for a workspace's only tab would only rebuild the one it has.
    bool canLeaveWorkspace(SidebarTabMenuState const& state) noexcept
    {
        return state.workspaceTabs > 1 && movesNodes(state);
    }

    [[nodiscard]] std::span<TabTable::Row const> sidebarTabMenuTemplate()
    {
        static auto const rows = std::array {
            TabTable::rows(tabStripRows),
            TabTable::separator(),
            TabTable::submenu("Move to Workspace", moveTargetRows)
                .enabledWhen(movesNodes<SidebarTabMenuState>),
            TabTable::submenu("Merge into Tab", joinRowsOf<SidebarTabMenuState>)
                .enabledWhen(movesNodes<SidebarTabMenuState>),
            TabTable::command(actions::MoveToNewWorkspace {}, "Move to New Workspace")
                .enabledWhen(canLeaveWorkspace),
        };
        return rows;
    }
    // }}}

    // {{{ Pane rows
    using PaneTable = detail::MenuTable<SidebarPaneMenuState>;

    /// A tab of its own for its tab's only pane would only rebuild the tab it has.
    bool canLeaveTab(SidebarPaneMenuState const& state) noexcept
    {
        return state.tabPanes > 1 && movesNodes(state);
    }

    /// A new workspace for what is all its workspace holds would only rebuild the one it has.
    bool canLeavePaneWorkspace(SidebarPaneMenuState const& state) noexcept
    {
        return (state.tabPanes > 1 || state.workspaceTabs > 1) && movesNodes(state);
    }

    [[nodiscard]] std::span<PaneTable::Row const> sidebarPaneMenuTemplate()
    {
        using namespace actions;

        static auto const rows = std::array {
            PaneTable::command(GoToPane {}, "Go to Pane"),
            PaneTable::command(MarkSeen {}, "Mark Seen"),

            PaneTable::separator(),

            PaneTable::submenu("Move to Tab", joinRowsOf<SidebarPaneMenuState>)
                .enabledWhen(movesNodes<SidebarPaneMenuState>),
            PaneTable::command(MoveToNewTab {}, "Move to New Tab").enabledWhen(canLeaveTab),
            PaneTable::command(MoveToNewWorkspace {}, "Move to New Workspace")
                .enabledWhen(canLeavePaneWorkspace),

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
