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

    /// Whether the row's menu offers "Open in New Window": not for a row no switch can name.
    template <typename State>
    bool opensInNewWindow(State const& state) noexcept
    {
        return state.open == OpenOffer::Offered;
    }

    /// Every row's "Open in New Window", which opens what the row names in a window of its own.
    template <typename State>
    [[nodiscard]] typename detail::MenuTable<State>::Row openInNewWindowRow()
    {
        return detail::MenuTable<State>::command(actions::OpenInNewWindow {}, "Open in New Window")
            .shownWhen(opensInNewWindow<State>);
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

    bool canColor(WorkspaceMenuState const& state) noexcept
    {
        return state.coloring == MenuColoring::Available;
    }

    bool hasColor(WorkspaceMenuState const& state) noexcept
    {
        return state.color.has_value();
    }

    [[nodiscard]] std::span<WorkspaceTable::Row const> workspaceMenuTemplate()
    {
        using namespace actions;

        static auto const rows = std::array {
            openInNewWindowRow<WorkspaceMenuState>(),

            WorkspaceTable::separator(),

            // As the tab menu's "Tab Color"; its "Custom…" opens the picker over the workspace's row.
            WorkspaceTable::submenu(
                "Workspace Color",
                detail::colorRows<WorkspaceMenuState, SetWorkspaceColor, ResetWorkspaceColor, hasColor>)
                .enabledWhen(canColor),
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
            openInNewWindowRow<SidebarTabMenuState>(),
            TabTable::separator(),
            TabTable::rows(tabStripRows),
            TabTable::separator(),
            // Each splits the tab's active pane.
            TabTable::command(actions::SplitVertical {}, "Split Vertically"),
            TabTable::command(actions::SplitHorizontal {}, "Split Horizontally"),
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
            openInNewWindowRow<SidebarPaneMenuState>(),

            PaneTable::separator(),

            PaneTable::command(MarkSeen {}, "Mark Seen"),
            PaneTable::command(SplitVertical {}, "Split Vertically"),
            PaneTable::command(SplitHorizontal {}, "Split Horizontally"),

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

    // {{{ Daemon rows
    using DaemonTable = detail::MenuTable<SidebarDaemonMenuState>;

    /// Where a verb's row stands in the daemon row's menu.
    enum class DaemonVerbSection : uint8_t
    {
        Buttons = 0, ///< With what the row's buttons do.
        Removal,     ///< Set apart below them: it takes the row away.
    };

    /// What one daemon row verb's menu row says and runs.
    struct DaemonVerbRow
    {
        DaemonRowVerb verb;                                     ///< The verb.
        std::string_view title;                                 ///< What the row says: the button's words.
        actions::Action action {};                              ///< What it runs on the row.
        DaemonVerbSection section = DaemonVerbSection::Buttons; ///< Where its row stands.
    };

    /// One row per verb, in enumerator order; None has no menu row.
    [[nodiscard]] std::span<DaemonVerbRow const> daemonVerbRows()
    {
        static auto const rows = std::array {
            DaemonVerbRow { .verb = DaemonRowVerb::None, .title = {}, .action = {} },
            DaemonVerbRow {
                .verb = DaemonRowVerb::Connect, .title = "Connect", .action = actions::ConnectDaemon {} },
            DaemonVerbRow {
                .verb = DaemonRowVerb::Start, .title = "Start", .action = actions::StartDaemon {} },
            DaemonVerbRow {
                .verb = DaemonRowVerb::Cancel, .title = "Cancel", .action = actions::CancelSwitch {} },
            DaemonVerbRow { .verb = DaemonRowVerb::Remove,
                            .title = "Remove",
                            .action = actions::RemoveDaemon {},
                            .section = DaemonVerbSection::Removal },
        };
        return rows;
    }

    /// The rows of the row's buttons whose verbs stand in @p section, in the buttons' order.
    [[nodiscard]] std::vector<ContextMenuEntry> daemonVerbEntries(SidebarDaemonMenuState const& state,
                                                                  DaemonVerbSection section)
    {
        auto entries = std::vector<ContextMenuEntry> {};
        for (auto const verb: state.verbs)
            if (verb != DaemonRowVerb::None)
            {
                auto const& row = daemonVerbRows()[static_cast<std::size_t>(verb)];
                if (row.section == section)
                    entries.push_back(detail::commandEntry(std::string { row.title }, row.action));
            }
        return entries;
    }

    [[nodiscard]] std::vector<ContextMenuEntry> buttonRows(SidebarDaemonMenuState const& state)
    {
        return daemonVerbEntries(state, DaemonVerbSection::Buttons);
    }

    [[nodiscard]] std::vector<ContextMenuEntry> removeRows(SidebarDaemonMenuState const& state)
    {
        return daemonVerbEntries(state, DaemonVerbSection::Removal);
    }

    [[nodiscard]] std::span<DaemonTable::Row const> sidebarDaemonMenuTemplate()
    {
        static auto const rows = std::array {
            openInNewWindowRow<SidebarDaemonMenuState>(),
            DaemonTable::separator(),
            DaemonTable::rows(buttonRows),
            DaemonTable::separator(),
            DaemonTable::rows(removeRows),
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

std::vector<ContextMenuEntry> buildSidebarDaemonMenu(SidebarDaemonMenuState const& state)
{
    auto entries = DaemonTable::buildRows(sidebarDaemonMenuTemplate(), state);
    detail::dropRedundantSeparators(entries);
    return entries;
}

} // namespace contour::command
