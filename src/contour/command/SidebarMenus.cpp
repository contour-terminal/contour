// SPDX-License-Identifier: Apache-2.0
#include <contour/command/ContextMenuTable.hpp>
#include <contour/command/SidebarMenus.hpp>

#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <ranges>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace contour::command
{

namespace
{
    // {{{ Other daemons' rows
    /// Drops what does not run on another daemon's row (otherDaemonRouteOf()): its command rows, and a
    /// submenu left with none. Own rows keep everything.
    void dropNotOffered(std::vector<ContextMenuEntry>& entries, RowOrigin origin, RowFreshness freshness)
    {
        if (origin == RowOrigin::Own)
            return;
        for (auto& entry: entries)
        {
            if (entry.kind == ContextMenuEntryKind::Submenu)
            {
                dropNotOffered(entry.children, origin, freshness);
                // A submenu with nothing left to pick is disabled as a whole.
                if (std::ranges::none_of(entry.children, [](ContextMenuEntry const& child) {
                        return child.kind == ContextMenuEntryKind::Command
                               && child.availability == RowAvailability::Enabled;
                    }))
                    entry.availability = RowAvailability::Disabled;
            }
            // A last-known row's verbs that would go through its daemon's section find nothing to carry them
            // (ui-ux §6.3): shown, so the menu keeps its shape, but disabled.
            else if (entry.kind == ContextMenuEntryKind::Command && freshness == RowFreshness::LastKnown
                     && sectionNeedOf(entry.action) == SectionNeed::Live)
                entry.availability = RowAvailability::Disabled;
        }
        std::erase_if(entries, [](ContextMenuEntry const& entry) {
            switch (entry.kind)
            {
                case ContextMenuEntryKind::Command:
                    return otherDaemonRouteOf(entry.action) == OtherDaemonRoute::NotOffered;
                case ContextMenuEntryKind::Submenu:
                    return std::ranges::none_of(entry.children, [](ContextMenuEntry const& child) {
                        return child.kind == ContextMenuEntryKind::Command;
                    });
                case ContextMenuEntryKind::Separator:
                case ContextMenuEntryKind::Header: return false;
            }
            return false;
        });
    }
    // }}}

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
    [[nodiscard]] detail::MenuTable<State>::Row openInNewWindowRow()
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

    /// What one daemon row verb's menu row says and runs. A menu row stands alone, so it says the verb in
    /// full where the button beside the daemon's name says one word: "Cancel" there, but "Cancel Connecting"
    /// here, as a menu row reading "Cancel" would read as closing the menu.
    struct DaemonVerbRow
    {
        DaemonRowVerb verb;     ///< The verb.
        std::string_view title; ///< What the row says.
        /// What it says while a jump into the daemon's section is pending, which is then what it ends; empty
        /// for @c title either way.
        std::string_view jumpTitle {};
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
                .verb = DaemonRowVerb::Start, .title = "Start Daemon", .action = actions::StartDaemon {} },
            DaemonVerbRow { .verb = DaemonRowVerb::Cancel,
                            .title = "Cancel Connecting",
                            .jumpTitle = "Cancel Jump",
                            .action = actions::CancelSwitch {} },
            DaemonVerbRow { .verb = DaemonRowVerb::Remove,
                            .title = "Remove from Sidebar",
                            .action = actions::RemoveDaemon {},
                            .section = DaemonVerbSection::Removal },
        };
        static_assert(std::tuple_size_v<std::remove_cvref_t<decltype(rows)>>
                          == std::to_underlying(DaemonRowVerb::Remove) + 1,
                      "a row for every verb");
        return rows;
    }

    /// @return What @p row says for a row whose Cancel ends @p cancels.
    [[nodiscard]] std::string_view titleOf(DaemonVerbRow const& row, DaemonCancelTarget cancels) noexcept
    {
        return cancels == DaemonCancelTarget::Jump && !row.jumpTitle.empty() ? row.jumpTitle : row.title;
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
                    entries.push_back(
                        detail::commandEntry(std::string { titleOf(row, state.cancels) }, row.action));
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

    /// Whether the daemon row's menu offers "Hide from Sidebar".
    bool offersHide(SidebarDaemonMenuState const& state) noexcept
    {
        return state.hide == HideOffer::Offered;
    }

    /// Whether the daemon row's menu offers "New Workspace".
    bool offersNewWorkspace(SidebarDaemonMenuState const& state) noexcept
    {
        return state.newWorkspace == NewWorkspaceOffer::Offered;
    }

    [[nodiscard]] std::span<DaemonTable::Row const> sidebarDaemonMenuTemplate()
    {
        static auto const rows = std::array {
            openInNewWindowRow<SidebarDaemonMenuState>(),
            // Made on the window's own daemon as the tab strip's does; on another, made there and jumped to.
            DaemonTable::command(actions::NewWorkspace {}, "New Workspace").shownWhen(offersNewWorkspace),
            DaemonTable::separator(),
            DaemonTable::rows(buttonRows),
            DaemonTable::separator(),
            DaemonTable::command(actions::HideDaemon {}, "Hide from Sidebar").shownWhen(offersHide),
            DaemonTable::rows(removeRows),
        };
        return rows;
    }
    // }}}

} // namespace

std::span<OtherDaemonRouteRow const> otherDaemonRoutes()
{
    using namespace actions;
    using enum OtherDaemonRoute;
    static auto const rows = std::array {
        // Through the daemon's section, the window staying where it is.
        OtherDaemonRouteRow { .action = RenameWorkspace {}, .route = InPlace, .need = SectionNeed::Live },
        OtherDaemonRouteRow { .action = RenameTab {}, .route = InPlace, .need = SectionNeed::Live },
        OtherDaemonRouteRow { .action = SetWorkspaceColor {}, .route = InPlace, .need = SectionNeed::Live },
        OtherDaemonRouteRow { .action = ResetWorkspaceColor {}, .route = InPlace, .need = SectionNeed::Live },
        OtherDaemonRouteRow { .action = SetTabColor {}, .route = InPlace, .need = SectionNeed::Live },
        OtherDaemonRouteRow { .action = ResetTabColor {}, .route = InPlace, .need = SectionNeed::Live },
        OtherDaemonRouteRow { .action = MarkSeen {}, .route = InPlace, .need = SectionNeed::Live },
        OtherDaemonRouteRow { .action = CloseWorkspace {}, .route = InPlace, .need = SectionNeed::Live },
        OtherDaemonRouteRow { .action = CloseTab {}, .route = InPlace, .need = SectionNeed::Live },
        OtherDaemonRouteRow { .action = ClosePane {}, .route = InPlace, .need = SectionNeed::Live },
        // A jump of its own, in a window of its own: this one stays where it is.
        OtherDaemonRouteRow { .action = OpenInNewWindow {}, .route = InPlace },
        // On what a window shows: the row is jumped to first. Go to Pane is that jump alone.
        OtherDaemonRouteRow { .action = CreateNewTab {}, .route = JumpFirst },
        OtherDaemonRouteRow { .action = SplitVertical {}, .route = JumpFirst },
        OtherDaemonRouteRow { .action = SplitHorizontal {}, .route = JumpFirst },
        OtherDaemonRouteRow { .action = GoToPane {}, .route = JumpFirst },
        // Ids of one daemon do not name places on another, and the bulk closes are the tab strip's.
        OtherDaemonRouteRow { .action = MoveWorkspaceUp {}, .route = NotOffered },
        OtherDaemonRouteRow { .action = MoveWorkspaceDown {}, .route = NotOffered },
        OtherDaemonRouteRow { .action = MoveTabToWorkspace {}, .route = NotOffered },
        OtherDaemonRouteRow { .action = MoveToTab {}, .route = NotOffered },
        OtherDaemonRouteRow { .action = MoveToNewTab {}, .route = NotOffered },
        OtherDaemonRouteRow { .action = MoveToNewWorkspace {}, .route = NotOffered },
        OtherDaemonRouteRow { .action = MoveTabToNewWindow {}, .route = NotOffered },
        OtherDaemonRouteRow { .action = CloseOtherTabs {}, .route = NotOffered },
        OtherDaemonRouteRow { .action = CloseTabsToRight {}, .route = NotOffered },
    };
    static_assert(std::tuple_size_v<std::remove_cvref_t<decltype(rows)>> == 24,
                  "one row per action the workspace, tab and pane menus offer");
    return rows;
}

namespace
{
    /// @return The route table's row of @p action; null for an action it does not name.
    [[nodiscard]] OtherDaemonRouteRow const* routeRowOf(actions::Action const& action) noexcept
    {
        auto const routes = otherDaemonRoutes();
        auto const found = std::ranges::find_if(
            routes, [&](OtherDaemonRouteRow const& row) { return row.action.index() == action.index(); });
        return found != routes.end() ? &*found : nullptr;
    }
} // namespace

OtherDaemonRoute otherDaemonRouteOf(actions::Action const& action) noexcept
{
    auto const* const row = routeRowOf(action);
    return row != nullptr ? row->route : OtherDaemonRoute::NotOffered;
}

SectionNeed sectionNeedOf(actions::Action const& action) noexcept
{
    auto const* const row = routeRowOf(action);
    return row != nullptr ? row->need : SectionNeed::None;
}

std::vector<ContextMenuEntry> buildWorkspaceMenu(WorkspaceMenuState const& state)
{
    auto entries = WorkspaceTable::buildRows(workspaceMenuTemplate(), state);
    dropNotOffered(entries, state.origin, state.freshness);
    detail::dropRedundantSeparators(entries);
    return entries;
}

std::vector<ContextMenuEntry> buildSidebarTabMenu(SidebarTabMenuState const& state)
{
    auto entries = TabTable::buildRows(sidebarTabMenuTemplate(), state);
    dropNotOffered(entries, state.origin, state.freshness);
    if (state.origin == RowOrigin::OtherDaemon)
    {
        // Its bulk closes dropped, "Close Tab…" is left alone in the tab strip's group: it goes last, after a
        // separator, as the workspace and pane menus end with their close.
        auto const close = std::ranges::find_if(entries, [](ContextMenuEntry const& entry) {
            return entry.kind == ContextMenuEntryKind::Command
                   && std::holds_alternative<actions::CloseTab>(entry.action);
        });
        if (close != entries.end())
        {
            auto closing = std::move(*close);
            entries.erase(close);
            entries.push_back(ContextMenuEntry { .kind = ContextMenuEntryKind::Separator });
            entries.push_back(std::move(closing));
        }
    }
    detail::dropRedundantSeparators(entries);
    return entries;
}

std::vector<ContextMenuEntry> buildSidebarPaneMenu(SidebarPaneMenuState const& state)
{
    auto entries = PaneTable::buildRows(sidebarPaneMenuTemplate(), state);
    dropNotOffered(entries, state.origin, state.freshness);
    detail::dropRedundantSeparators(entries);
    return entries;
}

std::string_view daemonRowVerbTitle(DaemonRowVerb verb, DaemonCancelTarget cancels) noexcept
{
    return titleOf(daemonVerbRows()[static_cast<std::size_t>(verb)], cancels);
}

std::vector<ContextMenuEntry> buildSidebarDaemonMenu(SidebarDaemonMenuState const& state)
{
    auto entries = DaemonTable::buildRows(sidebarDaemonMenuTemplate(), state);
    detail::dropRedundantSeparators(entries);
    return entries;
}

std::vector<ContextMenuEntry> buildSidebarHiddenMenu(std::span<std::string const> names)
{
    auto entries = std::vector<ContextMenuEntry> {};
    for (auto const& [index, name]: core::views::enumerate(names))
        entries.push_back(
            detail::commandEntry(std::format("Show {}", name),
                                 actions::ShowHiddenDaemon { .index = static_cast<uint32_t>(index) + 1 }));
    return entries;
}

} // namespace contour::command
