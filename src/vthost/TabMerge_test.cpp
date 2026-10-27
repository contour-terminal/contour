// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <utility>

#include <vthost/TabMerge.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

using vthost::applyTabMerge;
using vthost::splitSideFrom;
using vthost::TabMergeError;
using vthost::proto::MergeTabIntoSplit;
using vtworkspace::SplitSide;

namespace
{

/// Counts the tab-level events a merge must fire, and the ones it must not.
struct MergeEvents final: vtworkspace::ModelEvents
{
    int tabsClosed = 0;
    int restructured = 0;
    int panesClosed = 0;

    void tabAdded(vtworkspace::WindowId, vtworkspace::TabId, int) override {}
    void tabClosed(vtworkspace::WindowId, vtworkspace::TabId, int) override { ++tabsClosed; }
    void tabMoved(vtworkspace::WindowId, vtworkspace::TabId, int, int) override {}
    void activeTabChanged(vtworkspace::WindowId, vtworkspace::TabId, int) override {}
    void paneSplit(vtworkspace::TabId, vtworkspace::PaneId, vtworkspace::PaneId) override {}
    void paneClosed(vtworkspace::TabId, vtworkspace::PaneId, vtworkspace::PaneId) override { ++panesClosed; }
    void activePaneChanged(vtworkspace::TabId, vtworkspace::PaneId) override {}
    void paneRatioChanged(vtworkspace::TabId, vtworkspace::PaneId, double) override {}
    void paneZoomChanged(vtworkspace::TabId, std::optional<vtworkspace::PaneId>) override {}
    void tabTitleChanged(vtworkspace::TabId) override {}
    void tabColorChanged(vtworkspace::TabId) override {}
    void paneTreeRestructured(vtworkspace::TabId) override { ++restructured; }
};

/// One window of two tabs: the first holds sessions 1 and 2, the second session 3.
struct TwoTabs
{
    MergeEvents events;
    uint64_t nextSession = 1;
    vtworkspace::SessionModel model { events, [this] { return vtworkspace::SessionId { nextSession++ }; } };
    vtworkspace::WindowId window = model.createWindow()->id();
    vtworkspace::Tab* first = model.createTab(window);
    vtworkspace::Pane* second = model.splitActivePane(first->id(), vtworkspace::SplitState::Horizontal);
    vtworkspace::Tab* dragged = model.createTab(window);
};

} // namespace

TEST_CASE("a wire side is read, never cast", "[vthost][tab-merge]")
{
    for (auto const side: std::array { SplitSide::Left, SplitSide::Right, SplitSide::Top, SplitSide::Bottom })
        CHECK(splitSideFrom(std::to_underlying(side)) == side);
    CHECK_FALSE(splitSideFrom(std::to_underlying(SplitSide::Bottom) + 1).has_value());
    CHECK_FALSE(splitSideFrom(0xEE).has_value());
}

TEST_CASE("merging a tab grafts its session beside the target and keeps every session", "[vthost][tab-merge]")
{
    auto m = TwoTabs {};
    auto const merged =
        applyTabMerge(m.model, MergeTabIntoSplit { .tabSession = 3, .targetSession = 1, .side = 1 });
    REQUIRE(merged.has_value());

    CHECK(m.model.window(m.window)->tabCount() == 1);
    CHECK(m.first->paneCount() == 3);
    CHECK(m.events.tabsClosed == 1);
    CHECK(m.events.restructured == 1);
    // The dragged tab's session was moved, not closed: no pane was reported closed.
    CHECK(m.events.panesClosed == 0);
    auto const [window, tab, leaf] = m.model.findSessionLeaf(vtworkspace::SessionId { 3 });
    CHECK(tab == m.first);
    CHECK(leaf != nullptr);
}

TEST_CASE("a merge that names nothing changes nothing", "[vthost][tab-merge]")
{
    auto m = TwoTabs {};
    SECTION("an unknown tab session")
    {
        CHECK(applyTabMerge(m.model, MergeTabIntoSplit { .tabSession = 99, .targetSession = 1, .side = 1 })
              == std::unexpected { TabMergeError::UnknownSession });
    }
    SECTION("an unknown target session")
    {
        CHECK(applyTabMerge(m.model, MergeTabIntoSplit { .tabSession = 3, .targetSession = 99, .side = 1 })
              == std::unexpected { TabMergeError::UnknownSession });
    }
    SECTION("an unknown side")
    {
        CHECK(applyTabMerge(m.model, MergeTabIntoSplit { .tabSession = 3, .targetSession = 1, .side = 0xEE })
              == std::unexpected { TabMergeError::UnknownSide });
    }
    SECTION("both sessions in one tab")
    {
        CHECK(applyTabMerge(m.model, MergeTabIntoSplit { .tabSession = 2, .targetSession = 1, .side = 1 })
              == std::unexpected { TabMergeError::Refused });
    }
    CHECK(m.model.window(m.window)->tabCount() == 2);
    CHECK(m.events.tabsClosed == 0);
    CHECK(m.events.restructured == 0);
}
