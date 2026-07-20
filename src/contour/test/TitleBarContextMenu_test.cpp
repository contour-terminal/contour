// SPDX-License-Identifier: Apache-2.0
//
// The title bar's context menu is a pure function of a POD snapshot, so every row it offers and every
// rule about when a row is shown, greyed or ticked is decided — and asserted — with no window, no
// terminal and no Qt behind it.

#include <contour/TitleBarContextMenu.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <ranges>
#include <utility>

using contour::buildTitleBarContextMenu;
using contour::ContextMenuEntry;
using contour::ContextMenuEntryKind;
using contour::TitleBarContextMenuState;
namespace actions = contour::actions;

namespace
{
/// The entry titled @p title, or nullptr.
[[nodiscard]] ContextMenuEntry const* find(std::vector<ContextMenuEntry> const& entries,
                                           std::string_view title)
{
    auto const it = std::ranges::find_if(entries, [title](auto const& e) { return e.title == title; });
    return it != entries.end() ? &*it : nullptr;
}

/// A state with a couple of tabs and profiles, i.e. the ordinary case.
[[nodiscard]] TitleBarContextMenuState populatedState()
{
    return TitleBarContextMenuState { .tabCount = 3,
                                      .tabBarVisibility = contour::config::TabBarVisibility::Always,
                                      .tabBarPosition = contour::config::TabBarPosition::Top,
                                      .activeProfile = "main",
                                      .profileNames = { "main", "dark" } };
}
} // namespace

TEST_CASE("titleBarContextMenu: opening a tab is always offered", "[contour][titlebar][menu]")
{
    auto const entries = buildTitleBarContextMenu(populatedState());

    auto const* newTab = find(entries, "New Tab");
    REQUIRE(newTab != nullptr);
    CHECK(newTab->enabled);
    CHECK(std::holds_alternative<actions::CreateNewTab>(newTab->action));
}

TEST_CASE("titleBarContextMenu: each profile row opens a tab UNDER that profile", "[contour][titlebar][menu]")
{
    auto const entries = buildTitleBarContextMenu(populatedState());

    auto const* submenu = find(entries, "New Tab with Profile");
    REQUIRE(submenu != nullptr);
    REQUIRE(submenu->kind == ContextMenuEntryKind::Submenu);
    REQUIRE(submenu->children.size() == 2);

    // The trap this pins: the pane menu's profile rows run ChangeProfile, which switches the CURRENT
    // pane. Reusing them here would have re-profiled the tab the user is on instead of opening a new one.
    for (auto const& child: submenu->children)
    {
        auto const* create = std::get_if<actions::CreateNewTab>(&child.action);
        REQUIRE(create != nullptr);
        REQUIRE(create->profileName.has_value());
        CHECK(*create->profileName == child.title);
        // Nothing is "current" among these, so nothing may claim to be.
        CHECK_FALSE(child.checkable);
    }
}

TEST_CASE("titleBarContextMenu: the profile submenu vanishes when there are no profiles",
          "[contour][titlebar][menu]")
{
    auto state = populatedState();
    state.profileNames.clear();

    auto const entries = buildTitleBarContextMenu(state);
    CHECK(find(entries, "New Tab with Profile") == nullptr);
    // ...and the plain "New Tab" survives, so the menu is never left with nothing to open.
    CHECK(find(entries, "New Tab") != nullptr);
}

TEST_CASE("titleBarContextMenu: closing rows track how many tabs there are", "[contour][titlebar][menu]")
{
    SECTION("several tabs: close one, or close them all")
    {
        auto const entries = buildTitleBarContextMenu(populatedState());

        auto const* closeTab = find(entries, "Close Tab");
        REQUIRE(closeTab != nullptr);
        CHECK(closeTab->enabled);

        auto const* closeAll = find(entries, "Close All Tabs");
        REQUIRE(closeAll != nullptr);
        CHECK(std::holds_alternative<actions::CloseAllTabs>(closeAll->action));
    }

    SECTION("one tab: 'Close All Tabs' would say the same as 'Close Tab', so it is not offered")
    {
        auto state = populatedState();
        state.tabCount = 1;

        auto const entries = buildTitleBarContextMenu(state);
        CHECK(find(entries, "Close Tab") != nullptr);
        CHECK(find(entries, "Close All Tabs") == nullptr);
    }

    SECTION("no tabs: there is nothing to close")
    {
        auto state = populatedState();
        state.tabCount = 0;

        auto const entries = buildTitleBarContextMenu(state);
        auto const* closeTab = find(entries, "Close Tab");
        REQUIRE(closeTab != nullptr);
        CHECK_FALSE(closeTab->enabled);
    }
}

TEST_CASE("titleBarContextMenu: the tab bar submenu offers every mode and ticks the live one",
          "[contour][titlebar][menu]")
{
    using contour::config::TabBarPosition;
    using contour::config::TabBarVisibility;

    for (auto const& live: contour::config::tabBarModes<TabBarVisibility>())
    {
        auto state = populatedState();
        state.tabBarVisibility = live.mode;

        auto const entries = buildTitleBarContextMenu(state);
        auto const* tabBar = find(entries, "Tab Bar");
        REQUIRE(tabBar != nullptr);
        REQUIRE(tabBar->kind == ContextMenuEntryKind::Submenu);

        // Every mode is offered. Asserted against the TABLE rather than a second literal list, which is
        // the point of generating the rows: a mode added there shows up here with neither file touched.
        auto const modeRows =
            std::ranges::count_if(tabBar->children, [](auto const& e) { return e.checkable; });
        CHECK(std::cmp_equal(modeRows, contour::config::tabBarModes<TabBarVisibility>().size()));

        // Exactly one is ticked, and it is the one in force.
        auto ticked = std::vector<std::string> {};
        for (auto const& child: tabBar->children)
            if (child.checked)
                ticked.push_back(child.title);
        REQUIRE(ticked.size() == 1);
        CHECK(ticked.front() == live.label);

        // The rows read as words a human recognises, not as configuration tokens.
        CHECK(find(tabBar->children, "Always Visible") != nullptr);
        CHECK(find(tabBar->children, "Auto-Hide") != nullptr);
        CHECK(find(tabBar->children, "Hidden") != nullptr);
    }

    SECTION("placement lives one level deeper, and ticks the live one too")
    {
        auto state = populatedState();
        state.tabBarPosition = TabBarPosition::Bottom;

        auto const entries = buildTitleBarContextMenu(state);
        auto const* tabBar = find(entries, "Tab Bar");
        REQUIRE(tabBar != nullptr);

        auto const* position = find(tabBar->children, "Position");
        REQUIRE(position != nullptr);
        REQUIRE(position->kind == ContextMenuEntryKind::Submenu);
        CHECK(
            std::cmp_equal(position->children.size(), contour::config::tabBarModes<TabBarPosition>().size()));

        auto const* bottom = find(position->children, "Bottom");
        REQUIRE(bottom != nullptr);
        CHECK(bottom->checked);
        auto const* top = find(position->children, "Top");
        REQUIRE(top != nullptr);
        CHECK_FALSE(top->checked);
    }
}

TEST_CASE("titleBarContextMenu: the permanent home for these choices is one click away",
          "[contour][titlebar][menu]")
{
    // The rows above are a runtime override for one window. Without this the menu would be a setting
    // that quietly forgets on restart, with nothing saying where to make it stick.
    auto const entries = buildTitleBarContextMenu(populatedState());
    auto const* settings = find(entries, "Tab Bar Settings…");
    REQUIRE(settings != nullptr);
    CHECK(std::holds_alternative<actions::OpenConfiguration>(settings->action));
}

TEST_CASE("titleBarContextMenu: no separator is ever left stranded", "[contour][titlebar][menu]")
{
    using contour::config::TabBarPosition;
    using contour::config::TabBarVisibility;

    // Hiding rows is what strands separators, and the rows here hide on two independent axes. Sweep
    // both rather than trusting the one arrangement that happens to be the common case.
    for (auto const tabCount: { 0, 1, 2, 7 })
        for (auto const profileCount: { 0, 1, 3 })
            for (auto const& visibility: contour::config::tabBarModes<TabBarVisibility>())
            {
                auto state = TitleBarContextMenuState { .tabCount = tabCount,
                                                        .tabBarVisibility = visibility.mode,
                                                        .tabBarPosition = TabBarPosition::Top,
                                                        .activeProfile = "main",
                                                        .profileNames = {} };
                for (auto const i: std::views::iota(0, profileCount))
                    state.profileNames.push_back("p" + std::to_string(i));

                auto const entries = buildTitleBarContextMenu(state);
                INFO("tabs=" << tabCount << " profiles=" << profileCount);

                REQUIRE_FALSE(entries.empty());
                CHECK(entries.front().kind != ContextMenuEntryKind::Separator);
                CHECK(entries.back().kind != ContextMenuEntryKind::Separator);

                auto previousWasSeparator = false;
                for (auto const& entry: entries)
                {
                    auto const isSeparator = entry.kind == ContextMenuEntryKind::Separator;
                    CHECK_FALSE((isSeparator && previousWasSeparator));
                    previousWasSeparator = isSeparator;
                }
            }
}
