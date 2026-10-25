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

    /// One swatch per palette colour, ticked on the tab's colour.
    [[nodiscard]] std::vector<ContextMenuEntry> swatchRows(TabContextMenuState const& state)
    {
        return std::ranges::to<std::vector>(
            state.palette | std::views::transform([&](auto const& color) {
                return detail::commandEntry(vtbackend::formatColor(color),
                                            actions::SetTabColor { .color = color },
                                            state.color == color ? RowCheck::Checked : RowCheck::Unchecked,
                                            color);
            }));
    }

    /// The "Tab Color" submenu: the swatches, then a custom colour and a reset.
    [[nodiscard]] std::span<Row const> tabColorTemplate()
    {
        static auto const rows = std::array {
            Table::rows(swatchRows),
            Table::separator(),
            Table::command(actions::SetTabColor {}, "Custom…"),
            Table::command(actions::ResetTabColor {}, "Reset").enabledWhen(hasUserColor),
        };
        return rows;
    }

    [[nodiscard]] std::vector<ContextMenuEntry> tabColorRows(TabContextMenuState const& state)
    {
        return Table::buildRows(tabColorTemplate(), state);
    }

    /// The tab menu, as data.
    [[nodiscard]] std::span<Row const> tabContextMenuTemplate()
    {
        using namespace actions;

        static auto const rows = std::array {
            Table::command(SetTabTitle {}, "Rename…"),
            Table::submenu("Tab Color", tabColorRows),
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

    /// One row per profile, a divider, and "Settings" (to go once the settings live elsewhere).
    [[nodiscard]] std::span<NewTabTable::Row const> newTabMenuTemplate()
    {
        static auto const rows = std::array {
            NewTabTable::rows(newTabProfileRows),
            NewTabTable::separator(),
            NewTabTable::command(actions::OpenConfiguration {}, "Settings"),
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
