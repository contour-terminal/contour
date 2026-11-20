// SPDX-License-Identifier: Apache-2.0
#include <contour/command/ContextMenuTable.hpp>
#include <contour/command/TabMenus.hpp>

#include <array>
#include <format>
#include <ranges>
#include <span>

namespace contour::command
{

namespace
{
    using Table = detail::MenuTable<TabContextMenuState>;
    using Row = Table::Row;

    // {{{ Predicates
    bool hasMultipleTabs(TabContextMenuState const& state) noexcept
    {
        return state.tabCount > 1;
    }

    bool hasTabsToRight(TabContextMenuState const& state) noexcept
    {
        return state.tabIndex < state.tabCount - 1;
    }

    bool hasUserColor(TabContextMenuState const& state) noexcept
    {
        return state.userColor == UserColorPresence::Present;
    }
    // }}}

    /// The tab menu, as data.
    [[nodiscard]] std::span<Row const> tabContextMenuTemplate()
    {
        using namespace actions;

        static auto const rows = std::array {
            Table::submenu("Tab Color",
                           detail::colorRows<TabContextMenuState, SetTabColor, ResetTabColor, hasUserColor>),
            Table::command(RenameTab {}, "Rename…"),
            Table::command(MoveTabToNewWindow {}, "Move Tab to New Window").enabledWhen(hasMultipleTabs),

            Table::separator(),

            Table::command(CloseTab {}, "Close Tab"),
            Table::command(CloseOtherTabs {}, "Close Other Tabs").enabledWhen(hasMultipleTabs),
            Table::command(CloseTabsToRight {}, "Close Tabs to the Right").enabledWhen(hasTabsToRight),
        };
        return rows;
    }

    using NewTabTable = detail::MenuTable<NewTabMenuState>;

    [[nodiscard]] std::vector<ContextMenuEntry> newTabProfileRows(NewTabMenuState const& state)
    {
        return std::ranges::to<std::vector>(state.profileNames | std::views::transform([](auto const& name) {
                                                return detail::commandEntry(
                                                    std::format("New tab — {}", name),
                                                    actions::CreateNewTab { .profileName = name });
                                            }));
    }

    /// One row per profile, and nothing else: Settings opens from the menu bar's Settings… row.
    [[nodiscard]] std::span<NewTabTable::Row const> newTabMenuTemplate()
    {
        static auto const rows = std::array {
            NewTabTable::rows(newTabProfileRows),
        };
        return rows;
    }

} // namespace

std::vector<ContextMenuEntry> buildTabContextMenu(TabContextMenuState const& state)
{
    auto entries = Table::buildRows(tabContextMenuTemplate(), state);
    detail::dropRedundantSeparators(entries);
    return entries;
}

std::vector<ContextMenuEntry> buildNewTabMenu(NewTabMenuState const& state)
{
    auto entries = NewTabTable::buildRows(newTabMenuTemplate(), state);
    detail::dropRedundantSeparators(entries);
    return entries;
}

} // namespace contour::command
