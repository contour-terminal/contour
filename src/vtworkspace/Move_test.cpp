// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <vtworkspace/ModelFixture.hpp>
#include <vtworkspace/Move.hpp>
#include <vtworkspace/SessionModel.hpp>

using namespace vtworkspace;
using namespace vtworkspace::test;

namespace
{
/// Window `w1` holds tab `a` (sessions 1000 | 1001, 1001 active) and tab `b` (session 1002).
/// Window `w2` holds tab `c` (sessions 1003 / 1004, 1004 active).
struct MoveFixture: Fixture
{
    Window* w1 = model.createWindow();
    Window* w2 = model.createWindow();
    Tab* a = model.createTab(w1->id());
    Pane* a1 = model.splitActivePane(a->id(), SplitState::Vertical);
    Tab* b = model.createTab(w1->id());
    Tab* c = model.createTab(w2->id());
    Pane* c1 = model.splitActivePane(c->id(), SplitState::Horizontal);
    WindowId w1Id = w1->id();
    WindowId w2Id = w2->id();
    TabId aId = a->id();
    TabId bId = b->id();
    TabId cId = c->id();

    MoveFixture() { events.log.clear(); }

    /// The sessions on the leaves of @p tab, in tree order.
    [[nodiscard]] static std::vector<uint64_t> sessionsOf(Tab const& tab)
    {
        auto sessions = std::vector<uint64_t> {};
        tab.rootPane()->walkTree([&](Pane& pane) {
            if (pane.isLeaf())
                sessions.push_back(pane.session().value);
        });
        return sessions;
    }

    /// Every session the fixture started with is still on exactly one leaf of the model.
    [[nodiscard]] bool allSessionsPresent() const
    {
        return std::ranges::all_of(std::views::iota(uint64_t { 1000 }, uint64_t { 1005 }), [&](auto session) {
            return std::get<2>(model.findSessionLeaf(SessionId { session })) != nullptr;
        });
    }

    /// Checks the focus contract of a successful move: the result names the moved session as the active
    /// pane of the active tab, and the last focus event is on that tab and leaf (only retitles/zoom
    /// notices may follow it).
    void requireFocus(MoveResult const& result, SessionId moved)
    {
        REQUIRE(result.focused == moved);
        auto const [window, tab, leaf] = model.findSessionLeaf(moved);
        REQUIRE(leaf != nullptr);
        CHECK(window->id() == result.window);
        CHECK(tab->id() == result.tab);
        CHECK(window->activeTab() == tab);
        CHECK(tab->activePane() == leaf);
        CHECK_FALSE(tab->isZoomed());

        auto const lastFocus = std::ranges::find_if(
            events.log | std::views::reverse, [](auto const& e) { return e.starts_with("activePane:"); });
        REQUIRE(lastFocus != events.log.rend());
        CHECK(*lastFocus == std::format("activePane:{}:{}", tab->id().value, leaf->id().value));
        auto const trailing = std::ranges::subrange(events.log.rbegin(), lastFocus);
        CHECK(std::ranges::all_of(
            trailing, [](auto const& e) { return e.starts_with("title:") || e.starts_with("zoom:"); }));
    }

    /// A refusal changes nothing and fires nothing.
    void requireRefused(std::expected<MoveResult, MoveError> const& result, MoveError expected)
    {
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == expected);
        CHECK(events.log.empty());
        CHECK(sessionsOf(*a) == std::vector<uint64_t> { 1000, 1001 });
        CHECK(sessionsOf(*c) == std::vector<uint64_t> { 1003, 1004 });
        CHECK(allSessionsPresent());
    }
};
} // namespace

TEST_CASE("moveNode: pane to edge of a pane in the same tab", "[vtworkspace][model][move]")
{
    MoveFixture f;
    // Tab a is [1000 | 1001]; put 1000 below 1001.
    auto const result = f.model.moveNode(PaneSource { SessionId { 1000 } },
                                         EdgeTarget { SessionId { 1001 }, SplitSide::Bottom });
    REQUIRE(result.has_value());
    CHECK(f.a->rootPane()->splitState() == SplitState::Horizontal);
    CHECK(f.a->rootPane()->ratio() == Catch::Approx(0.5));
    CHECK(MoveFixture::sessionsOf(*f.a) == std::vector<uint64_t> { 1001, 1000 });
    CHECK(f.a->paneCount() == 2);
    CHECK(f.events.countPrefix("paneClosed:") == 0);
    CHECK(f.events.countPrefix("tabClosed:") == 0);
    CHECK(f.allSessionsPresent());
    f.requireFocus(*result, SessionId { 1000 });
}

TEST_CASE("moveNode: either pane of a two-pane split to any edge of the other", "[vtworkspace][model][move]")
{
    // Tab a is [1000 | 1001]. Dropping one pane on an edge of its only sibling re-splits the tab along that
    // edge's axis, whether or not the leaf order changes: 1001 onto 1000's bottom keeps the order and
    // changes only the axis.
    struct Row
    {
        uint64_t moved;
        uint64_t onto;
        SplitSide side;
        SplitState split;
        std::vector<uint64_t> sessions;
    };
    auto const rows = std::array {
        Row { .moved = 1001,
              .onto = 1000,
              .side = SplitSide::Top,
              .split = SplitState::Horizontal,
              .sessions = { 1001, 1000 } },
        Row { .moved = 1001,
              .onto = 1000,
              .side = SplitSide::Bottom,
              .split = SplitState::Horizontal,
              .sessions = { 1000, 1001 } },
        Row { .moved = 1001,
              .onto = 1000,
              .side = SplitSide::Left,
              .split = SplitState::Vertical,
              .sessions = { 1001, 1000 } },
        Row { .moved = 1001,
              .onto = 1000,
              .side = SplitSide::Right,
              .split = SplitState::Vertical,
              .sessions = { 1000, 1001 } },
        Row { .moved = 1000,
              .onto = 1001,
              .side = SplitSide::Top,
              .split = SplitState::Horizontal,
              .sessions = { 1000, 1001 } },
        Row { .moved = 1000,
              .onto = 1001,
              .side = SplitSide::Bottom,
              .split = SplitState::Horizontal,
              .sessions = { 1001, 1000 } },
        Row { .moved = 1000,
              .onto = 1001,
              .side = SplitSide::Left,
              .split = SplitState::Vertical,
              .sessions = { 1000, 1001 } },
        Row { .moved = 1000,
              .onto = 1001,
              .side = SplitSide::Right,
              .split = SplitState::Vertical,
              .sessions = { 1001, 1000 } },
    };
    for (auto const& row: rows)
    {
        DYNAMIC_SECTION(
            std::format("{} onto side {} of {}", row.moved, std::to_underlying(row.side), row.onto))
        {
            MoveFixture f;
            auto const result = f.model.moveNode(PaneSource { SessionId { row.moved } },
                                                 EdgeTarget { SessionId { row.onto }, row.side });
            REQUIRE(result.has_value());
            CHECK(f.a->rootPane()->splitState() == row.split);
            CHECK(f.a->rootPane()->ratio() == Catch::Approx(0.5));
            CHECK(MoveFixture::sessionsOf(*f.a) == row.sessions);
            CHECK(f.a->paneCount() == 2);
            CHECK(f.allSessionsPresent());
            f.requireFocus(*result, SessionId { row.moved });
        }
    }
}

TEST_CASE("moveNode: pane to edge of a pane in another tab", "[vtworkspace][model][move]")
{
    MoveFixture f;
    auto const result = f.model.moveNode(PaneSource { SessionId { 1000 } },
                                         EdgeTarget { SessionId { 1002 }, SplitSide::Left });
    REQUIRE(result.has_value());
    CHECK(MoveFixture::sessionsOf(*f.a) == std::vector<uint64_t> { 1001 }); // the sibling took the space
    CHECK(f.a->rootPane()->isLeaf());
    CHECK(MoveFixture::sessionsOf(*f.b) == std::vector<uint64_t> { 1000, 1002 });
    CHECK(f.b->rootPane()->splitState() == SplitState::Vertical);
    CHECK(f.events.countPrefix("paneClosed:") == 0);
    CHECK(f.events.countPrefix("tabClosed:") == 0);
    CHECK(f.allSessionsPresent());
    f.requireFocus(*result, SessionId { 1000 });
}

TEST_CASE("moveNode: pane to edge of a pane in another window", "[vtworkspace][model][move]")
{
    MoveFixture f;
    auto const result = f.model.moveNode(PaneSource { SessionId { 1001 } },
                                         EdgeTarget { SessionId { 1003 }, SplitSide::Right });
    REQUIRE(result.has_value());
    CHECK(result->window == f.w2Id);
    CHECK(MoveFixture::sessionsOf(*f.c) == std::vector<uint64_t> { 1003, 1001, 1004 });
    CHECK(MoveFixture::sessionsOf(*f.a) == std::vector<uint64_t> { 1000 });
    CHECK(f.allSessionsPresent());
    f.requireFocus(*result, SessionId { 1001 });
}

TEST_CASE("moveNode: pane swap in one tab keeps both ratios", "[vtworkspace][model][move]")
{
    MoveFixture f;
    f.a->rootPane()->setRatio(0.3);
    auto* const first = f.a->rootPane()->first();
    auto* const second = f.a->rootPane()->second();
    auto const firstId = first->id();
    auto const secondId = second->id();

    auto const result =
        f.model.moveNode(PaneSource { SessionId { 1000 } }, SwapTarget { SessionId { 1001 } });
    REQUIRE(result.has_value());
    CHECK(MoveFixture::sessionsOf(*f.a) == std::vector<uint64_t> { 1001, 1000 });
    CHECK(f.a->rootPane()->ratio() == Catch::Approx(0.3));
    CHECK(f.a->rootPane()->first()->id() == firstId); // slots keep their ids; sessions trade
    CHECK(f.a->rootPane()->second()->id() == secondId);
    CHECK(f.events.countPrefix("swapped:") == 1);
    CHECK(f.allSessionsPresent());
    f.requireFocus(*result, SessionId { 1000 });
}

TEST_CASE("moveNode: pane swap across windows", "[vtworkspace][model][move]")
{
    MoveFixture f;
    auto const result =
        f.model.moveNode(PaneSource { SessionId { 1001 } }, SwapTarget { SessionId { 1003 } });
    REQUIRE(result.has_value());
    CHECK(result->window == f.w2Id);
    CHECK(MoveFixture::sessionsOf(*f.a) == std::vector<uint64_t> { 1000, 1003 });
    CHECK(MoveFixture::sessionsOf(*f.c) == std::vector<uint64_t> { 1001, 1004 });
    CHECK(f.events.countPrefix("replaced:") == 0);
    CHECK(f.events.countPrefix(std::format("restructured:{}", f.aId.value)) == 1);
    CHECK(f.events.countPrefix(std::format("restructured:{}", f.cId.value)) == 1);
    CHECK(f.allSessionsPresent());
    f.requireFocus(*result, SessionId { 1001 });
}

TEST_CASE("moveNode: pane to a tab well makes a one-pane tab at the index", "[vtworkspace][model][move]")
{
    MoveFixture f;
    auto const result = f.model.moveNode(PaneSource { SessionId { 1000 } }, TabWellTarget { f.w1Id, 0 });
    REQUIRE(result.has_value());
    CHECK(f.w1->tabCount() == 3);
    CHECK(f.w1->tabAt(0)->id() == result->tab);
    CHECK(MoveFixture::sessionsOf(*f.w1->tabAt(0)) == std::vector<uint64_t> { 1000 });
    CHECK(f.w1->tabAt(1) == f.a);
    CHECK(f.w1->tabAt(2) == f.b);
    CHECK(MoveFixture::sessionsOf(*f.a) == std::vector<uint64_t> { 1001 });
    CHECK(f.events.countPrefix("paneClosed:") == 0);
    CHECK(f.events.countPrefix("tabClosed:") == 0);
    CHECK(f.allSessionsPresent());
    f.requireFocus(*result, SessionId { 1000 });
}

TEST_CASE("moveNode: pane to a tab well clamps the index", "[vtworkspace][model][move]")
{
    MoveFixture f;
    auto const result = f.model.moveNode(PaneSource { SessionId { 1000 } }, TabWellTarget { f.w2Id, 99 });
    REQUIRE(result.has_value());
    CHECK(f.w2->tabCount() == 2);
    CHECK(f.w2->tabAt(1)->id() == result->tab);
    f.requireFocus(*result, SessionId { 1000 });
}

TEST_CASE("moveNode: pane to a new window", "[vtworkspace][model][move]")
{
    MoveFixture f;
    auto const windowsBefore = f.model.windowCount();
    auto const result = f.model.moveNode(PaneSource { SessionId { 1000 } }, NewWindowTarget {});
    REQUIRE(result.has_value());
    CHECK(f.model.windowCount() == windowsBefore + 1);
    CHECK(result->window != f.w1Id);
    CHECK(result->window != f.w2Id);
    auto* const created = f.model.window(result->window);
    REQUIRE(created != nullptr);
    CHECK(created->tabCount() == 1);
    CHECK(MoveFixture::sessionsOf(*created->tabAt(0)) == std::vector<uint64_t> { 1000 });
    CHECK(f.allSessionsPresent());
    f.requireFocus(*result, SessionId { 1000 });
}

TEST_CASE("moveNode: multi-pane tab to an edge in another window", "[vtworkspace][model][move]")
{
    MoveFixture f;
    auto const result =
        f.model.moveNode(TabSource { f.w1Id, f.aId }, EdgeTarget { SessionId { 1003 }, SplitSide::Right });
    REQUIRE(result.has_value());
    CHECK(f.w1->tabCount() == 1); // tab a is gone
    CHECK(MoveFixture::sessionsOf(*f.c) == std::vector<uint64_t> { 1003, 1000, 1001, 1004 });
    CHECK(f.events.countPrefix("paneClosed:") == 0);
    CHECK(f.allSessionsPresent());
    f.requireFocus(*result, SessionId { 1001 }); // the dragged tab's active pane
}

TEST_CASE("moveNode: multi-pane tab to a tab well and a new window", "[vtworkspace][model][move]")
{
    MoveFixture f;
    SECTION("tab well of another window")
    {
        auto const result = f.model.moveNode(TabSource { f.w1Id, f.aId }, TabWellTarget { f.w2Id, 0 });
        REQUIRE(result.has_value());
        CHECK(f.w2->tabAt(0)->id() == f.aId);
        CHECK(f.w1->tabCount() == 1);
        CHECK(f.allSessionsPresent());
        f.requireFocus(*result, SessionId { 1001 });
    }
    SECTION("reorder within the window")
    {
        auto const result = f.model.moveNode(TabSource { f.w1Id, f.aId }, TabWellTarget { f.w1Id, 1 });
        REQUIRE(result.has_value());
        CHECK(f.w1->tabAt(1)->id() == f.aId);
        f.requireFocus(*result, SessionId { 1001 });
    }
    SECTION("new window")
    {
        auto const result = f.model.moveNode(TabSource { f.w1Id, f.aId }, NewWindowTarget {});
        REQUIRE(result.has_value());
        CHECK(f.model.window(result->window)->tabAt(0)->id() == f.aId);
        f.requireFocus(*result, SessionId { 1001 });
    }
}

TEST_CASE("moveNode: one-pane tab is treated as a pane (swap)", "[vtworkspace][model][move]")
{
    MoveFixture f;
    auto const result = f.model.moveNode(TabSource { f.w1Id, f.bId }, SwapTarget { SessionId { 1000 } });
    REQUIRE(result.has_value());
    CHECK(f.w1->tabCount() == 2); // tab b stays: the sessions traded
    CHECK(MoveFixture::sessionsOf(*f.b) == std::vector<uint64_t> { 1000 });
    CHECK(MoveFixture::sessionsOf(*f.a) == std::vector<uint64_t> { 1002, 1001 });
    CHECK(f.allSessionsPresent());
    f.requireFocus(*result, SessionId { 1002 });
}

TEST_CASE("moveNode: one-pane tab to an edge merges the tab", "[vtworkspace][model][move]")
{
    MoveFixture f;
    auto const result = f.model.moveNode(PaneSource { SessionId { 1002 } },
                                         EdgeTarget { SessionId { 1003 }, SplitSide::Top });
    REQUIRE(result.has_value());
    CHECK(f.w1->tabCount() == 1);
    CHECK(f.events.countPrefix("paneClosed:") == 0);
    CHECK(MoveFixture::sessionsOf(*f.c) == std::vector<uint64_t> { 1002, 1003, 1004 });
    f.requireFocus(*result, SessionId { 1002 });
}

TEST_CASE("moveNode: last pane of the last tab leaves its window empty", "[vtworkspace][model][move]")
{
    MoveFixture f;
    f.model.closeTab(f.w1Id, f.aId);
    f.model.closeTab(f.w1Id, f.bId);
    auto* const lone = f.model.createTab(f.w1Id);
    auto const loneSession = lone->rootPane()->session();
    f.events.log.clear();

    auto const result =
        f.model.moveNode(PaneSource { loneSession }, EdgeTarget { SessionId { 1003 }, SplitSide::Left });
    REQUIRE(result.has_value());
    CHECK(f.w1->empty());
    REQUIRE(f.model.window(f.w1Id) != nullptr); // the host closes it, not the model
    CHECK(f.events.countPrefix("paneClosed:") == 0);
    CHECK(std::get<2>(f.model.findSessionLeaf(loneSession)) != nullptr);
    f.requireFocus(*result, loneSession);
}

TEST_CASE("moveNode: zoom is cleared on both ends", "[vtworkspace][model][move]")
{
    MoveFixture f;
    f.model.toggleActivePaneZoom(f.aId);
    f.model.toggleActivePaneZoom(f.cId);
    REQUIRE(f.a->isZoomed());
    REQUIRE(f.c->isZoomed());
    f.events.log.clear();

    auto const result = f.model.moveNode(PaneSource { SessionId { 1001 } },
                                         EdgeTarget { SessionId { 1003 }, SplitSide::Right });
    REQUIRE(result.has_value());
    CHECK_FALSE(f.a->isZoomed());
    CHECK_FALSE(f.c->isZoomed());
    CHECK(f.events.sawPrefix(std::format("zoom:{}:-", f.aId.value)));
    CHECK(f.events.sawPrefix(std::format("zoom:{}:-", f.cId.value)));
    f.requireFocus(*result, SessionId { 1001 });
}

TEST_CASE("moveNode: zoom is cleared by a swap on both ends", "[vtworkspace][model][move]")
{
    MoveFixture f;
    f.model.toggleActivePaneZoom(f.aId);
    f.model.toggleActivePaneZoom(f.cId);
    f.events.log.clear();

    auto const result =
        f.model.moveNode(PaneSource { SessionId { 1001 } }, SwapTarget { SessionId { 1003 } });
    REQUIRE(result.has_value());
    CHECK_FALSE(f.a->isZoomed());
    CHECK_FALSE(f.c->isZoomed());
    f.requireFocus(*result, SessionId { 1001 });
}

TEST_CASE("moveNode: refuses an unknown source", "[vtworkspace][model][move]")
{
    MoveFixture f;
    SECTION("a closed session")
    {
        f.events.log.clear();
        f.requireRefused(f.model.moveNode(PaneSource { SessionId { 4242 } }, NewWindowTarget {}),
                         MoveError::UnknownSource);
    }
    SECTION("an unknown tab")
    {
        f.events.log.clear();
        f.requireRefused(f.model.moveNode(TabSource { f.w1Id, TabId { 999 } }, NewWindowTarget {}),
                         MoveError::UnknownSource);
    }
    SECTION("an unknown window")
    {
        f.events.log.clear();
        f.requireRefused(f.model.moveNode(TabSource { WindowId { 999 }, f.aId }, NewWindowTarget {}),
                         MoveError::UnknownSource);
    }
}

TEST_CASE("moveNode: refuses an unknown target", "[vtworkspace][model][move]")
{
    MoveFixture f;
    auto const windowsBefore = f.model.windowCount();

    SECTION("a closed session as edge")
    {
        f.requireRefused(f.model.moveNode(PaneSource { SessionId { 1000 } },
                                          EdgeTarget { SessionId { 4242 }, SplitSide::Left }),
                         MoveError::UnknownTarget);
    }
    SECTION("a closed session as swap")
    {
        f.requireRefused(
            f.model.moveNode(PaneSource { SessionId { 1000 } }, SwapTarget { SessionId { 4242 } }),
            MoveError::UnknownTarget);
    }
    SECTION("an unknown window")
    {
        f.requireRefused(
            f.model.moveNode(PaneSource { SessionId { 1000 } }, TabWellTarget { WindowId { 999 }, 0 }),
            MoveError::UnknownTarget);
    }
    CHECK(f.model.windowCount() == windowsBefore);
}

TEST_CASE("moveNode: refuses to move a node onto itself", "[vtworkspace][model][move]")
{
    MoveFixture f;

    SECTION("a pane onto itself, as edge")
    {
        f.requireRefused(f.model.moveNode(PaneSource { SessionId { 1000 } },
                                          EdgeTarget { SessionId { 1000 }, SplitSide::Left }),
                         MoveError::SelfTarget);
    }
    SECTION("a pane onto itself, as swap")
    {
        f.requireRefused(
            f.model.moveNode(PaneSource { SessionId { 1000 } }, SwapTarget { SessionId { 1000 } }),
            MoveError::SelfTarget);
    }
    SECTION("a tab onto a leaf of itself")
    {
        f.requireRefused(f.model.moveNode(TabSource { f.w1Id, f.aId },
                                          EdgeTarget { SessionId { 1001 }, SplitSide::Right }),
                         MoveError::SelfTarget);
    }
    SECTION("a one-pane tab onto its own pane")
    {
        f.requireRefused(f.model.moveNode(TabSource { f.w1Id, f.bId },
                                          EdgeTarget { SessionId { 1002 }, SplitSide::Right }),
                         MoveError::SelfTarget);
    }
}

TEST_CASE("moveNode: refuses to swap a multi-pane tab with a pane", "[vtworkspace][model][move]")
{
    MoveFixture f;
    f.requireRefused(f.model.moveNode(TabSource { f.w1Id, f.aId }, SwapTarget { SessionId { 1003 } }),
                     MoveError::TabIntoSwap);
}

TEST_CASE("moveNode: refuses a drop that changes nothing", "[vtworkspace][model][move]")
{
    MoveFixture f;

    SECTION("a one-pane tab onto its own well index")
    {
        f.requireRefused(f.model.moveNode(TabSource { f.w1Id, f.bId }, TabWellTarget { f.w1Id, 1 }),
                         MoveError::NoOp);
    }
    SECTION("the last pane of a tab onto its own well index, clamped")
    {
        f.requireRefused(f.model.moveNode(PaneSource { SessionId { 1002 } }, TabWellTarget { f.w1Id, 99 }),
                         MoveError::NoOp);
    }
    SECTION("a multi-pane tab onto its own well index")
    {
        f.requireRefused(f.model.moveNode(TabSource { f.w1Id, f.aId }, TabWellTarget { f.w1Id, 0 }),
                         MoveError::NoOp);
    }
}

TEST_CASE("describe(MoveError) names every error", "[vtworkspace][model][move]")
{
    for (auto const error: { MoveError::UnknownSource,
                             MoveError::UnknownTarget,
                             MoveError::SelfTarget,
                             MoveError::TabIntoSwap,
                             MoveError::NoOp })
        CHECK_FALSE(describe(error).empty());
}

TEST_CASE("moveNode: a swap across tabs of one window restructures both tabs", "[vtworkspace][model][move]")
{
    MoveFixture f;
    auto const result =
        f.model.moveNode(PaneSource { SessionId { 1000 } }, SwapTarget { SessionId { 1002 } });
    REQUIRE(result.has_value());
    CHECK(f.events.countPrefix("replaced:") == 0);
    CHECK(f.events.countPrefix(std::format("restructured:{}", f.aId.value)) == 1);
    CHECK(f.events.countPrefix(std::format("restructured:{}", f.bId.value)) == 1);
}

TEST_CASE("moveNode: a plain tab reorder fires no retitle", "[vtworkspace][model][move]")
{
    MoveFixture f;
    auto const result = f.model.moveNode(TabSource { f.w1Id, f.aId }, TabWellTarget { f.w1Id, 1 });
    REQUIRE(result.has_value());
    CHECK(f.events.countPrefix("title:") == 0);
}

TEST_CASE("moveNode: refuses a new window for what already fills its window", "[vtworkspace][model][move]")
{
    MoveFixture f;
    // Window w2 holds tab c alone, with two panes.
    auto const windowsBefore = f.model.windowCount();

    SECTION("the only tab of a window")
    {
        CHECK(f.model.fillsItsWindow(TabSource { f.w2Id, f.cId }));
        f.requireRefused(f.model.moveNode(TabSource { f.w2Id, f.cId }, NewWindowTarget {}), MoveError::NoOp);
    }
    SECTION("the only pane of a window's only tab")
    {
        f.model.closeTab(f.w2Id, f.cId);
        auto* const lone = f.model.createTab(f.w2Id);
        auto const loneSession = lone->rootPane()->session();
        f.events.log.clear();
        CHECK(f.model.fillsItsWindow(PaneSource { loneSession }));
        auto const result = f.model.moveNode(PaneSource { loneSession }, NewWindowTarget {});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == MoveError::NoOp);
        CHECK(f.events.log.empty());
        CHECK(f.model.windowCount() == windowsBefore);
    }
    CHECK(f.model.windowCount() == windowsBefore);
    // Not all of its window: a tab beside others, and a pane beside others.
    CHECK_FALSE(f.model.fillsItsWindow(PaneSource { SessionId { 1000 } }));
    CHECK_FALSE(f.model.fillsItsWindow(PaneSource { SessionId { 4242 } }));
}

namespace
{
/// Records every model event and, as each fires, whether the session it watches is on a leaf of the model.
/// A notice of a change about to happen (tabAboutToBeAdded and its kin) is not one an observer reads the
/// model on, so only the notices of a change made are probed.
struct ProbingEvents: RecordingEvents
{
    SessionModel const* model = nullptr;
    SessionId watched {};
    std::vector<std::string> lostAt; ///< The events at which the watched session was in no tab.

    void probe()
    {
        if (model != nullptr && std::get<2>(model->findSessionLeaf(watched)) == nullptr)
            lostAt.push_back(log.back());
    }

    void tabAdded(WindowId w, TabId t, int index) override
    {
        RecordingEvents::tabAdded(w, t, index);
        probe();
    }
    void tabClosed(WindowId w, TabId t, int index) override
    {
        RecordingEvents::tabClosed(w, t, index);
        probe();
    }
    void activeTabChanged(WindowId w, TabId t, int index) override
    {
        RecordingEvents::activeTabChanged(w, t, index);
        probe();
    }
    void activePaneChanged(TabId t, PaneId leaf) override
    {
        RecordingEvents::activePaneChanged(t, leaf);
        probe();
    }
    void paneTreeRestructured(TabId t) override
    {
        RecordingEvents::paneTreeRestructured(t);
        probe();
    }
    void tabTitleChanged(TabId t) override
    {
        RecordingEvents::tabTitleChanged(t);
        probe();
    }
    void paneZoomChanged(TabId t, std::optional<PaneId> zoomedLeaf) override
    {
        RecordingEvents::paneZoomChanged(t, zoomedLeaf);
        probe();
    }
    void tabMovedToWindow(WindowId from, TabId t, int fromIdx, WindowId to, int toIdx) override
    {
        RecordingEvents::tabMovedToWindow(from, t, fromIdx, to, toIdx);
        probe();
    }
};
} // namespace

TEST_CASE("moveNode: a pane taken into a tab of its own is in a tab at every event",
          "[vtworkspace][model][move]")
{
    auto events = ProbingEvents {};
    auto nextSession = uint64_t { 1000 };
    auto model = SessionModel { events, [&nextSession] { return SessionId { nextSession++ }; } };
    auto* const window = model.createWindow();
    auto* const other = model.createWindow();
    auto* const tab = model.createTab(window->id());
    auto const moved = model.splitActivePane(tab->id(), SplitState::Vertical)->session();
    std::ignore = model.createTab(other->id());
    events.model = &model;
    events.watched = moved;
    events.log.clear();

    SECTION("into its own window's tab well")
    {
        REQUIRE(model.moveNode(PaneSource { moved }, TabWellTarget { window->id(), 1 }).has_value());
    }
    SECTION("into another window's tab well")
    {
        REQUIRE(model.moveNode(PaneSource { moved }, TabWellTarget { other->id(), 0 }).has_value());
    }
    SECTION("into a new window")
    {
        REQUIRE(model.moveNode(PaneSource { moved }, NewWindowTarget {}).has_value());
    }
    CHECK(events.lostAt.empty());
    // The tab holding it is announced before the tab it left is.
    auto const added =
        std::ranges::find_if(events.log, [](auto const& e) { return e.starts_with("tabAdded:"); });
    auto const shrunk = std::ranges::find(events.log, std::format("restructured:{}", tab->id().value));
    REQUIRE(added != events.log.end());
    REQUIRE(shrunk != events.log.end());
    CHECK(added < shrunk);
}
