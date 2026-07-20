// SPDX-License-Identifier: Apache-2.0
#include <contour/ContextMenuTable.h>
#include <contour/TitleBarContextMenu.h>

#include <array>
#include <span>

namespace contour
{

namespace
{
    using Table = detail::MenuTable<TitleBarContextMenuState>;
    using Row = Table::Row;

    // {{{ Predicates
    bool hasTabs(TitleBarContextMenuState const& state) noexcept
    {
        return state.tabCount > 0;
    }

    bool hasMultipleTabs(TitleBarContextMenuState const& state) noexcept
    {
        return state.tabCount > 1;
    }

    bool hasProfiles(TitleBarContextMenuState const& state) noexcept
    {
        return !state.profileNames.empty();
    }
    // }}}

    /// One "New Tab" row per configured profile.
    ///
    /// Deliberately NOT the pane menu's profileRows(): that one switches the CURRENT pane's profile and
    /// check-marks the active one. Here each row opens a NEW tab under its profile, so nothing is
    /// "current" and nothing is checked — a tick would claim a state this menu does not describe.
    [[nodiscard]] std::vector<ContextMenuEntry> newTabProfileRows(TitleBarContextMenuState const& state)
    {
        auto entries = std::vector<ContextMenuEntry> {};
        entries.reserve(state.profileNames.size());

        for (auto const& name: state.profileNames)
            entries.emplace_back(ContextMenuEntry { .kind = ContextMenuEntryKind::Command,
                                                    .title = name,
                                                    .action = actions::CreateNewTab { .profileName = name },
                                                    .enabled = true,
                                                    .checkable = false,
                                                    .checked = false,
                                                    .children = {} });

        return entries;
    }

    /// One row per tab bar mode, generated from the mode table itself.
    ///
    /// Generated, not listed: a mode added to contour/TabBarMode.h appears here with this file
    /// untouched, and the row reads the words a human should see rather than the configuration token.
    [[nodiscard]] std::vector<ContextMenuEntry> tabBarVisibilityRows(TitleBarContextMenuState const& state)
    {
        auto entries = std::vector<ContextMenuEntry> {};

        for (auto const& info: config::tabBarModes<config::TabBarVisibility>())
            entries.emplace_back(
                ContextMenuEntry { .kind = ContextMenuEntryKind::Command,
                                   .title = std::string { info.label },
                                   .action = actions::SetTabBarVisibility { .mode = info.mode },
                                   .enabled = true,
                                   .checkable = true,
                                   .checked = info.mode == state.tabBarVisibility,
                                   .children = {} });

        return entries;
    }

    /// One row per tab bar placement. @see tabBarVisibilityRows.
    [[nodiscard]] std::vector<ContextMenuEntry> tabBarPositionRows(TitleBarContextMenuState const& state)
    {
        auto entries = std::vector<ContextMenuEntry> {};

        for (auto const& info: config::tabBarModes<config::TabBarPosition>())
            entries.emplace_back(
                ContextMenuEntry { .kind = ContextMenuEntryKind::Command,
                                   .title = std::string { info.label },
                                   .action = actions::SetTabBarPosition { .position = info.mode },
                                   .enabled = true,
                                   .checkable = true,
                                   .checked = info.mode == state.tabBarPosition,
                                   .children = {} });

        return entries;
    }

    [[nodiscard]] std::span<Row const> tabBarTemplate()
    {
        static auto const rows = std::array {
            Table::submenu("Position", tabBarPositionRows),
        };
        return rows;
    }

    /// The "Tab Bar" submenu: the visibility modes, then placement behind one more level.
    [[nodiscard]] std::vector<ContextMenuEntry> tabBarRows(TitleBarContextMenuState const& state)
    {
        auto entries = tabBarVisibilityRows(state);
        entries.emplace_back(ContextMenuEntry { .kind = ContextMenuEntryKind::Separator,
                                                .title = {},
                                                .action = {},
                                                .enabled = true,
                                                .checkable = false,
                                                .checked = false,
                                                .children = {} });
        auto nested = Table::buildRows(tabBarTemplate(), state);
        entries.insert(entries.end(), nested.begin(), nested.end());
        return entries;
    }

    /// The title bar's context menu, as data.
    [[nodiscard]] std::span<Row const> titleBarContextMenuTemplate()
    {
        using namespace actions;

        static auto const rows = std::array {
            Table::command(CreateNewTab {}, "New Tab"),
            Table::submenu("New Tab with Profile", newTabProfileRows).shownWhen(hasProfiles),

            Table::separator(),

            Table::command(CloseTab {}, "Close Tab").enabledWhen(hasTabs),
            // Hidden rather than greyed with a single tab: "Close Tab" already does exactly that, so a
            // second row saying the same thing is noise rather than information.
            Table::command(CloseAllTabs {}, "Close All Tabs").shownWhen(hasMultipleTabs),

            Table::separator(),

            Table::submenu("Tab Bar", tabBarRows),

            Table::separator(),

            // The rows above set a runtime override for THIS window; this is where the choice is made
            // permanent. One click away, rather than a menu that quietly forgets on restart.
            Table::command(OpenConfiguration {}, "Tab Bar Settings…"),
        };

        return rows;
    }

} // namespace

std::vector<ContextMenuEntry> buildTitleBarContextMenu(TitleBarContextMenuState const& state)
{
    auto entries = Table::buildRows(titleBarContextMenuTemplate(), state);
    detail::dropRedundantSeparators(entries);
    return entries;
}

} // namespace contour
