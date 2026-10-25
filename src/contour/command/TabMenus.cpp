// SPDX-License-Identifier: Apache-2.0
#include <contour/command/ContextMenuTable.hpp>
#include <contour/command/TabMenus.hpp>

#include <array>
#include <format>
#include <iterator>
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

    /// What follows the swatches in "Tab Color".
    [[nodiscard]] std::span<Row const> tabColorTail()
    {
        static auto const rows = std::array {
            Table::separator(),
            Table::command(actions::SetTabColor {}, "Custom…"),
            Table::command(actions::ResetTabColor {}, "Reset").enabledWhen(hasUserColor),
        };
        return rows;
    }

    /// The "Tab Color" submenu: one swatch per palette colour, ticked on the tab's colour, then the tail.
    [[nodiscard]] std::vector<ContextMenuEntry> tabColorRows(TabContextMenuState const& state)
    {
        auto entries = std::vector<ContextMenuEntry> {};
        entries.reserve(state.palette.size() + tabColorTail().size());

        for (auto const& color: state.palette)
            entries.emplace_back(
                ContextMenuEntry { .kind = ContextMenuEntryKind::Command,
                                   .title = vtbackend::formatColor(color),
                                   .action = actions::SetTabColor { .color = color },
                                   .availability = RowAvailability::Enabled,
                                   .check = (state.color == color) ? RowCheck::Checked : RowCheck::Unchecked,
                                   .role = MenuItemRole::None,
                                   .swatch = color,
                                   .children = {} });

        auto tail = Table::buildRows(tabColorTail(), state);
        entries.insert(
            entries.end(), std::make_move_iterator(tail.begin()), std::make_move_iterator(tail.end()));
        return entries;
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
        auto entries = std::vector<ContextMenuEntry> {};
        entries.reserve(state.profileNames.size());

        for (auto const& name: state.profileNames)
            entries.emplace_back(ContextMenuEntry { .kind = ContextMenuEntryKind::Command,
                                                    .title = std::format("New tab — {}", name),
                                                    .action = actions::CreateNewTab { .profileName = name },
                                                    .availability = RowAvailability::Enabled,
                                                    .check = RowCheck::None,
                                                    .role = MenuItemRole::None,
                                                    .swatch = std::nullopt,
                                                    .children = {} });
        return entries;
    }

    /// One row per profile, a divider, and "Settings" (to go once the settings live elsewhere).
    [[nodiscard]] std::vector<ContextMenuEntry> newTabMenuRows(NewTabMenuState const& state)
    {
        auto entries = newTabProfileRows(state);
        entries.emplace_back(ContextMenuEntry { .kind = ContextMenuEntryKind::Separator,
                                                .title = {},
                                                .action = {},
                                                .availability = RowAvailability::Enabled,
                                                .check = RowCheck::None,
                                                .role = MenuItemRole::None,
                                                .swatch = std::nullopt,
                                                .children = {} });
        return entries;
    }

    [[nodiscard]] std::span<NewTabTable::Row const> newTabSettingsTemplate()
    {
        static auto const rows = std::array {
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
    auto entries = newTabMenuRows(state);
    auto settings = NewTabTable::buildRows(newTabSettingsTemplate(), state);
    entries.insert(
        entries.end(), std::make_move_iterator(settings.begin()), std::make_move_iterator(settings.end()));
    detail::dropRedundantSeparators(entries);
    return entries;
}

} // namespace contour::command
