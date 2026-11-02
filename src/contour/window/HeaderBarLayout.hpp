// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the window's top row holds besides the tabs and the window buttons: the header bar's items (new
/// tab, search, the main menu), the slot each takes under the row's arrangement and tab shape, and the
/// separator between the items and the window buttons. Pure and Qt-free, so the provider, the QML and the
/// tests read the same rows.

#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/window/TitleBandLayout.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string_view>

namespace contour::window
{

/// An item of the header bar, beside the tabs.
enum class HeaderBarItem : uint8_t
{
    NewTab = 0, ///< Opens a new tab.
    Search,     ///< Opens the find bar.
    MainMenu,   ///< Opens the main menu: the menu bar's menus as submenus.
};

/// Number of @c HeaderBarItem enumerators, derived from the last one.
inline constexpr size_t HeaderBarItemCount = static_cast<size_t>(HeaderBarItem::MainMenu) + 1;

/// The menu a header-bar item pops below itself.
enum class HeaderBarMenu : uint8_t
{
    None = 0, ///< None.
    NewTab,   ///< The new-tab menu (the profiles), also on a secondary click.
    Main,     ///< The main menu.
};

/// @param menu A header-bar item's menu.
/// @return Its QML-facing name.
[[nodiscard]] constexpr std::string_view headerBarMenuName(HeaderBarMenu menu) noexcept
{
    switch (menu)
    {
        case HeaderBarMenu::None: return "";
        case HeaderBarMenu::NewTab: return "newTab";
        case HeaderBarMenu::Main: return "main";
    }
    return "";
}

/// How one header-bar item is presented.
struct HeaderBarItemRow
{
    HeaderBarItem item = HeaderBarItem::NewTab; ///< The item this row describes.
    std::string_view objectName {};             ///< The QML object name tests find the button by.
    std::string_view accessibleName {};         ///< What assistive technology announces.
    std::string_view iconName {};               ///< The freedesktop icon name, resolved through the theme.
    std::string_view bundledIcon {};            ///< The bundled icon used when the theme lacks @c iconName.
    HeaderBarMenu menu = HeaderBarMenu::None;   ///< The menu the item pops below itself.
    std::string_view commandName {}; ///< The command whose bound chord the tooltip carries; empty for none.
};

/// The header bar's items, in enumerator order.
inline constexpr auto HeaderBarItems = std::array {
    HeaderBarItemRow { .item = HeaderBarItem::NewTab,
                       .objectName = "headerNewTab",
                       .accessibleName = "New tab",
                       .iconName = "tab-new-symbolic",
                       .bundledIcon = "qrc:/contour/icons/tab-new-symbolic.svg",
                       .menu = HeaderBarMenu::NewTab,
                       .commandName = "CreateNewTab" },
    HeaderBarItemRow { .item = HeaderBarItem::Search,
                       .objectName = "headerSearch",
                       .accessibleName = "Search",
                       .iconName = "edit-find-symbolic",
                       .bundledIcon = "qrc:/contour/icons/edit-find-symbolic.svg",
                       .commandName = "SearchReverse" },
    HeaderBarItemRow { .item = HeaderBarItem::MainMenu,
                       .objectName = "headerMainMenu",
                       .accessibleName = "Main menu",
                       .iconName = "open-menu-symbolic",
                       .bundledIcon = "qrc:/contour/icons/open-menu-symbolic.svg",
                       .menu = HeaderBarMenu::Main },
};

static_assert(HeaderBarItems.size() == HeaderBarItemCount, "one row per HeaderBarItem");
static_assert(std::ranges::all_of(std::views::iota(size_t { 0 }, HeaderBarItemCount),
                                  [](size_t index) {
                                      return HeaderBarItems.at(index).item
                                             == static_cast<HeaderBarItem>(index);
                                  }),
              "HeaderBarItems is in enumerator order");

/// The row of @p item.
/// @param item A header-bar item.
/// @return Its presentation.
[[nodiscard]] constexpr HeaderBarItemRow const& headerBarItemRow(HeaderBarItem item) noexcept
{
    return HeaderBarItems.at(static_cast<size_t>(item));
}

/// Where an item sits in the header bar.
enum class HeaderBarSlot : uint8_t
{
    Hidden = 0, ///< Not in the header bar.
    Leading,    ///< Before the tabs.
    Trailing,   ///< After the tabs.
};

/// Number of @c HeaderBarSlot enumerators, derived from the last one.
inline constexpr size_t HeaderBarSlotCount = static_cast<size_t>(HeaderBarSlot::Trailing) + 1;

/// Whether one side of the header bar holds window buttons.
enum class ControlsPresence : uint8_t
{
    None = 0, ///< No window buttons on that side.
    Present,  ///< At least one window button on that side.
};

/// Whether a separator stands between a side's window buttons and the header items.
enum class ControlsSeparator : uint8_t
{
    None = 0, ///< No separator.
    Shown,    ///< A separator.
};

/// One rule of the header bar: the slots of its items for an arrangement and, optionally, a tab shape.
struct HeaderBarRule
{
    config::HeaderBarArrangement arrangement = config::HeaderBarArrangement::Band; ///< The row's arrangement.
    std::optional<config::TabShape> shape {};       ///< The row's tab shape; @c std::nullopt matches any.
    HeaderBarSlot newTab = HeaderBarSlot::Hidden;   ///< Where the new-tab button goes.
    HeaderBarSlot search = HeaderBarSlot::Hidden;   ///< Where the search button goes.
    HeaderBarSlot mainMenu = HeaderBarSlot::Hidden; ///< Where the main-menu button goes.
};

/// The header bar's rules (R-e); the first matching row wins, no match hides every item.
///
/// Under @c Band a @c Pill row's "+" trails the tabs, a @c Flat row keeps it inside the strip; @c Unified
/// puts it leading, with search and the main menu trailing.
inline constexpr auto HeaderBarRules = std::array {
    HeaderBarRule { .arrangement = config::HeaderBarArrangement::Band,
                    .shape = config::TabShape::Flat,
                    .newTab = HeaderBarSlot::Hidden,
                    .search = HeaderBarSlot::Hidden,
                    .mainMenu = HeaderBarSlot::Hidden },
    HeaderBarRule { .arrangement = config::HeaderBarArrangement::Band,
                    .shape = config::TabShape::Pill,
                    .newTab = HeaderBarSlot::Trailing,
                    .search = HeaderBarSlot::Hidden,
                    .mainMenu = HeaderBarSlot::Hidden },
    HeaderBarRule { .arrangement = config::HeaderBarArrangement::Unified,
                    .shape = std::nullopt,
                    .newTab = HeaderBarSlot::Leading,
                    .search = HeaderBarSlot::Trailing,
                    .mainMenu = HeaderBarSlot::Trailing },
};

/// What the header bar is decided from.
struct HeaderBarInputs
{
    config::HeaderBarArrangement arrangement = config::HeaderBarArrangement::Band; ///< The row's arrangement.
    config::TabShape shape = config::TabShape::Flat;                               ///< The row's tab shape.
    WindowFrame frame = WindowFrame::ClientBand;        ///< Who draws the window's frame.
    ControlsPresence leading = ControlsPresence::None;  ///< Window buttons at the leading edge.
    ControlsPresence trailing = ControlsPresence::None; ///< Window buttons at the trailing edge.
};

/// The header bar: each item's slot and each side's separator.
struct HeaderBar
{
    HeaderBarSlot newTab = HeaderBarSlot::Hidden;                  ///< Where the new-tab button goes.
    HeaderBarSlot search = HeaderBarSlot::Hidden;                  ///< Where the search button goes.
    HeaderBarSlot mainMenu = HeaderBarSlot::Hidden;                ///< Where the main-menu button goes.
    ControlsSeparator leadingSeparator = ControlsSeparator::None;  ///< After the leading window buttons.
    ControlsSeparator trailingSeparator = ControlsSeparator::None; ///< Before the trailing window buttons.

    [[nodiscard]] bool operator==(HeaderBar const&) const = default;
};

/// The header bar for @p inputs.
///
/// The slots are the first matching rule's (HeaderBarRules). A side's separator is shown when the row is
/// @c Unified, the chrome draws the frame, that side has window buttons, and some item sits on that side.
/// @param inputs The row and the window's side of the decision.
/// @return The header bar.
[[nodiscard]] constexpr HeaderBar resolveHeaderBar(HeaderBarInputs const& inputs) noexcept
{
    auto const rule = std::ranges::find_if(HeaderBarRules, [&](HeaderBarRule const& candidate) {
        return candidate.arrangement == inputs.arrangement
               && (!candidate.shape.has_value() || *candidate.shape == inputs.shape);
    });
    if (rule == HeaderBarRules.end())
        return HeaderBar {};

    auto const separatorOn = [&](ControlsPresence controls, HeaderBarSlot side) {
        auto const anyItemOnSide = rule->newTab == side || rule->search == side || rule->mainMenu == side;
        auto const shown = inputs.arrangement == config::HeaderBarArrangement::Unified
                           && inputs.frame == WindowFrame::ClientBand && controls == ControlsPresence::Present
                           && anyItemOnSide;
        return shown ? ControlsSeparator::Shown : ControlsSeparator::None;
    };

    return HeaderBar { .newTab = rule->newTab,
                       .search = rule->search,
                       .mainMenu = rule->mainMenu,
                       .leadingSeparator = separatorOn(inputs.leading, HeaderBarSlot::Leading),
                       .trailingSeparator = separatorOn(inputs.trailing, HeaderBarSlot::Trailing) };
}

/// Where @p item sits in @p bar.
/// @param bar  The header bar.
/// @param item A header-bar item.
/// @return Its slot.
[[nodiscard]] constexpr HeaderBarSlot headerBarSlotOf(HeaderBar const& bar, HeaderBarItem item) noexcept
{
    switch (item)
    {
        case HeaderBarItem::NewTab: return bar.newTab;
        case HeaderBarItem::Search: return bar.search;
        case HeaderBarItem::MainMenu: return bar.mainMenu;
    }
    return HeaderBarSlot::Hidden;
}

/// The name QML reads a header-bar slot by.
/// @param slot A slot.
/// @return "hidden", "leading" or "trailing".
[[nodiscard]] constexpr std::string_view headerBarSlotName(HeaderBarSlot slot) noexcept
{
    switch (slot)
    {
        case HeaderBarSlot::Hidden: return "hidden";
        case HeaderBarSlot::Leading: return "leading";
        case HeaderBarSlot::Trailing: return "trailing";
    }
    return "hidden";
}

} // namespace contour::window
