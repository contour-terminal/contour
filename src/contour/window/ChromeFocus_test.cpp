// SPDX-License-Identifier: Apache-2.0
//
// Ctrl+F6 walks terminal -> tabs -> toolbar and back, skipping chrome that is not shown.

#include <contour/window/ChromeFocus.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace contour::window;

namespace
{
constexpr auto Both = ChromeAreaPresence { .tabs = AreaPresence::Present, .toolbar = AreaPresence::Present };
constexpr auto NoTabs = ChromeAreaPresence { .tabs = AreaPresence::Absent, .toolbar = AreaPresence::Present };
constexpr auto NoToolbar =
    ChromeAreaPresence { .tabs = AreaPresence::Present, .toolbar = AreaPresence::Absent };
constexpr auto Neither = ChromeAreaPresence {};
} // namespace

TEST_CASE("chromeFocusTarget cycles through every area", "[chromefocus]")
{
    CHECK(chromeFocusTarget(ChromeFocusAction::Next, ChromeArea::Terminal, Both) == ChromeArea::Tabs);
    CHECK(chromeFocusTarget(ChromeFocusAction::Next, ChromeArea::Tabs, Both) == ChromeArea::Toolbar);
    CHECK(chromeFocusTarget(ChromeFocusAction::Next, ChromeArea::Toolbar, Both) == ChromeArea::Terminal);

    CHECK(chromeFocusTarget(ChromeFocusAction::Previous, ChromeArea::Terminal, Both) == ChromeArea::Toolbar);
    CHECK(chromeFocusTarget(ChromeFocusAction::Previous, ChromeArea::Toolbar, Both) == ChromeArea::Tabs);
    CHECK(chromeFocusTarget(ChromeFocusAction::Previous, ChromeArea::Tabs, Both) == ChromeArea::Terminal);
}

TEST_CASE("chromeFocusTarget skips what is not shown", "[chromefocus]")
{
    // One tab under tab_bar: multiple has no tab well (Review Focus 2).
    CHECK(chromeFocusTarget(ChromeFocusAction::Next, ChromeArea::Terminal, NoTabs) == ChromeArea::Toolbar);
    CHECK(chromeFocusTarget(ChromeFocusAction::Next, ChromeArea::Toolbar, NoTabs) == ChromeArea::Terminal);
    CHECK(chromeFocusTarget(ChromeFocusAction::Previous, ChromeArea::Terminal, NoTabs)
          == ChromeArea::Toolbar);
    CHECK(chromeFocusTarget(ChromeFocusAction::Next, ChromeArea::Tabs, NoToolbar) == ChromeArea::Terminal);
    CHECK(chromeFocusTarget(ChromeFocusAction::Previous, ChromeArea::Terminal, NoToolbar)
          == ChromeArea::Tabs);
    CHECK(chromeFocusTarget(ChromeFocusAction::Next, ChromeArea::Terminal, Neither) == ChromeArea::Terminal);
    CHECK(chromeFocusTarget(ChromeFocusAction::Previous, ChromeArea::Terminal, Neither)
          == ChromeArea::Terminal);
}

TEST_CASE("chromeFocusTarget Toolbar goes to the toolbar or stays", "[chromefocus]")
{
    CHECK(chromeFocusTarget(ChromeFocusAction::Toolbar, ChromeArea::Terminal, Both) == ChromeArea::Toolbar);
    CHECK(chromeFocusTarget(ChromeFocusAction::Toolbar, ChromeArea::Tabs, NoToolbar) == ChromeArea::Tabs);
    CHECK(chromeFocusTarget(ChromeFocusAction::Toolbar, ChromeArea::Terminal, Neither)
          == ChromeArea::Terminal);
}

TEST_CASE("chromeFocusTarget always lands on a present area", "[chromefocus]")
{
    for (auto const presence: { Both, NoTabs, NoToolbar, Neither })
        for (auto const current: ChromeAreaCycle)
            for (auto const action:
                 { ChromeFocusAction::Next, ChromeFocusAction::Previous, ChromeFocusAction::Toolbar })
            {
                // A focus that is already in an absent area (it just vanished) must also come out present
                // for the cycling actions; Toolbar stays put by design.
                auto const target = chromeFocusTarget(action, current, presence);
                if (action == ChromeFocusAction::Toolbar && !isPresent(current, presence))
                    continue;
                CHECK(isPresent(target, presence));
            }
}
