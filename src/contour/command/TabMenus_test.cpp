// SPDX-License-Identifier: Apache-2.0
//
// The tab menu and the new-tab menu are pure functions of a snapshot, so every row and every rule about
// when it is greyed or ticked is asserted with no window, no tab bar and no Qt.

#include <contour/command/TabMenus.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <ranges>
#include <string>
#include <variant>
#include <vector>

using namespace contour::command;
namespace actions = contour::actions;
using vtbackend::RGBColor;

namespace
{
[[nodiscard]] std::vector<std::string> titles(std::vector<ContextMenuEntry> const& entries)
{
    auto result = std::vector<std::string> {};
    for (auto const& entry: entries)
        result.push_back(entry.title);
    return result;
}

[[nodiscard]] ContextMenuEntry const& row(std::vector<ContextMenuEntry> const& entries,
                                          std::string_view title)
{
    auto const it = std::ranges::find(entries, title, &ContextMenuEntry::title);
    REQUIRE(it != entries.end());
    return *it;
}

[[nodiscard]] TabContextMenuState threeTabs()
{
    return TabContextMenuState {
        .tabIndex = 0,
        .tabCount = 3,
        .palette = { RGBColor { 255, 0, 0 }, RGBColor { 0, 255, 0 }, RGBColor { 0, 0, 255 } },
        .color = std::nullopt,
        .userColor = UserColorPresence::Absent
    };
}
} // namespace

TEST_CASE("tabMenu.rowsInOrder", "[tabmenu]")
{
    auto const menu = buildTabContextMenu(threeTabs());
    CHECK(titles(menu)
          == std::vector<std::string> { "Rename…",
                                        "Tab Color",
                                        "Move Tab to New Window",
                                        "",
                                        "Close Tab",
                                        "Close Other Tabs",
                                        "Close Tabs to the Right" });
    CHECK(menu[3].kind == ContextMenuEntryKind::Separator);
}

TEST_CASE("tabMenu.emptyPaletteLeavesNoLeadingSeparator", "[tabmenu]")
{
    auto state = threeTabs();
    state.palette.clear();

    auto const menu = buildTabContextMenu(state);
    auto const& color = row(menu, "Tab Color");

    REQUIRE_FALSE(color.children.empty());
    CHECK(color.children.front().kind != ContextMenuEntryKind::Separator);
    CHECK(titles(color.children) == std::vector<std::string> { "Custom…", "Reset" });
}

TEST_CASE("tabMenu.swatchesFollowThePalette", "[tabmenu]")
{
    auto state = threeTabs();
    state.color = state.palette[1];
    auto const menu = buildTabContextMenu(state);
    auto const& colors = row(menu, "Tab Color");
    REQUIRE(colors.kind == ContextMenuEntryKind::Submenu);
    REQUIRE(colors.children.size() == state.palette.size() + 3);

    for (auto const& [swatch, color]: std::views::zip(colors.children, state.palette))
    {
        CHECK(swatch.swatch == color);
        CHECK(swatch.title == vtbackend::formatColor(color));
        auto const* action = std::get_if<actions::SetTabColor>(&swatch.action);
        REQUIRE(action != nullptr);
        CHECK(action->color == color);
        CHECK(swatch.check == (color == *state.color ? RowCheck::Checked : RowCheck::Unchecked));
    }

    auto const n = state.palette.size();
    CHECK(colors.children[n].kind == ContextMenuEntryKind::Separator);
    CHECK(colors.children[n + 1].title == "Custom…");
    CHECK(std::get_if<actions::SetTabColor>(&colors.children[n + 1].action) != nullptr);
    CHECK(std::get<actions::SetTabColor>(colors.children[n + 1].action).color == std::nullopt);
    CHECK(colors.children[n + 2].title == "Reset");
}

TEST_CASE("tabMenu.resetNeedsTheUsersColour", "[tabmenu]")
{
    auto state = threeTabs();
    auto find = [&] {
        auto const menu = buildTabContextMenu(state);
        return row(row(menu, "Tab Color").children, "Reset").availability;
    };
    CHECK(find() == RowAvailability::Disabled);
    state.userColor = UserColorPresence::Present;
    CHECK(find() == RowAvailability::Enabled);
}

TEST_CASE("tabMenu.singleTab", "[tabmenu]")
{
    auto state = threeTabs();
    state.tabCount = 1;
    auto const menu = buildTabContextMenu(state);
    CHECK(row(menu, "Move Tab to New Window").availability == RowAvailability::Disabled);
    CHECK(row(menu, "Close Other Tabs").availability == RowAvailability::Disabled);
    CHECK(row(menu, "Close Tabs to the Right").availability == RowAvailability::Disabled);
    CHECK(row(menu, "Close Tab").availability == RowAvailability::Enabled);
}

TEST_CASE("tabMenu.lastTab", "[tabmenu]")
{
    auto state = threeTabs();
    state.tabIndex = 2;
    auto const menu = buildTabContextMenu(state);
    CHECK(row(menu, "Close Tabs to the Right").availability == RowAvailability::Disabled);
    CHECK(row(menu, "Move Tab to New Window").availability == RowAvailability::Enabled);
    CHECK(row(menu, "Close Other Tabs").availability == RowAvailability::Enabled);
    CHECK(row(menu, "Close Tab").availability == RowAvailability::Enabled);
}

TEST_CASE("newTabMenu.profilesThenSettings", "[newtabmenu]")
{
    auto const menu = buildNewTabMenu({ .profileNames = { "dark", "light" } });
    REQUIRE(menu.size() == 4);
    CHECK(titles(menu) == std::vector<std::string> { "New tab — dark", "New tab — light", "", "Settings" });
    CHECK(std::get<actions::CreateNewTab>(menu[0].action).profileName == "dark");
    CHECK(std::get<actions::CreateNewTab>(menu[1].action).profileName == "light");
    CHECK(menu[2].kind == ContextMenuEntryKind::Separator);
    CHECK(std::holds_alternative<actions::OpenConfiguration>(menu[3].action));

    auto const bare = buildNewTabMenu({});
    CHECK(titles(bare) == std::vector<std::string> { "Settings" });
}
