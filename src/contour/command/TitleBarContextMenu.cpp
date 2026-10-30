// SPDX-License-Identifier: Apache-2.0
#include <contour/command/ContextMenuTable.hpp>
#include <contour/command/TitleBarContextMenu.hpp>

#include <array>
#include <ranges>
#include <span>
#include <string>

namespace contour::command
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

    /// The title bar's context menu, as data.
    [[nodiscard]] std::span<Row const> titleBarContextMenuTemplate()
    {
        using namespace actions;

        static auto const rows = std::array {
            Table::command(CreateNewTab {}, "New Tab"),
            Table::submenu("New Tab with Profile", Table::newTabProfileRows).shownWhen(hasProfiles),
            Table::attachToHostSubmenu(),

            Table::separator(),

            Table::command(CloseTab {}, "Close Tab").enabledWhen(hasTabs),
            // Hidden rather than greyed with a single tab: "Close Tab" already does exactly that, so a
            // second row saying the same thing is noise rather than information.
            Table::command(CloseAllTabs {}, "Close All Tabs").shownWhen(hasMultipleTabs),

            Table::separator(),

            Table::submenu("Tab Bar",
                           [](TitleBarContextMenuState const& s) {
                               return buildTabBarMenu(
                                   { .visibility = s.tabBarVisibility, .position = s.tabBarPosition });
                           }),

            Table::separator(),

            // The rows above set a runtime override for THIS window; this is where the choice is made
            // permanent. One click away, rather than a menu that quietly forgets on restart.
            Table::command(OpenConfiguration {}, "Settings…"),
        };

        return rows;
    }

} // namespace

namespace
{
    using TabBarTable = detail::MenuTable<TabBarMenuState>;

    /// One row per tab bar mode, generated from the mode table itself.
    ///
    /// Generated, not listed: a mode added to contour/TabBarMode.h appears here with this file
    /// untouched, and the row reads the words a human should see rather than the configuration token.
    [[nodiscard]] std::vector<ContextMenuEntry> tabBarVisibilityRows(TabBarMenuState const& state)
    {
        return std::ranges::to<std::vector>(config::configEnumValues<config::TabBarVisibility>()
                                            | std::views::transform([&](auto const& info) {
                                                  return detail::commandEntry(
                                                      std::string { info.label },
                                                      actions::SetTabBarVisibility { .mode = info.value },
                                                      info.value == state.visibility ? RowCheck::Checked
                                                                                     : RowCheck::Unchecked);
                                              }));
    }

    /// One row per tab bar placement. @see tabBarVisibilityRows.
    [[nodiscard]] std::vector<ContextMenuEntry> tabBarPositionRows(TabBarMenuState const& state)
    {
        return std::ranges::to<std::vector>(
            config::configEnumValues<config::TabBarPosition>() | std::views::transform([&](auto const& info) {
                return detail::commandEntry(std::string { info.label },
                                            actions::SetTabBarPosition { .position = info.value },
                                            info.value == state.position ? RowCheck::Checked
                                                                         : RowCheck::Unchecked);
            }));
    }

    /// The tab bar menu: the visibility modes, then placement behind one more level.
    [[nodiscard]] std::span<TabBarTable::Row const> tabBarTemplate()
    {
        static auto const rows = std::array {
            TabBarTable::rows(tabBarVisibilityRows),
            TabBarTable::separator(),
            TabBarTable::submenu("Position", tabBarPositionRows),
        };
        return rows;
    }

} // namespace

std::vector<ContextMenuEntry> buildTabBarMenu(TabBarMenuState const& state)
{
    auto entries = TabBarTable::buildRows(tabBarTemplate(), state);
    detail::dropRedundantSeparators(entries);
    return entries;
}

std::vector<ContextMenuEntry> buildTitleBarContextMenu(TitleBarContextMenuState const& state)
{
    auto entries = Table::buildRows(titleBarContextMenuTemplate(), state);
    detail::dropRedundantSeparators(entries);
    return entries;
}

} // namespace contour::command
