// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <format>
#include <string>

#include <vtmux/Tab.h>

using namespace vtmux;

namespace
{
struct Ids
{
    uint64_t nextPane = 1;
    uint64_t nextSession = 100;
    PaneId pane() { return PaneId { nextPane++ }; }
    SessionId session() { return SessionId { nextSession++ }; }
};

// A resolver that maps a session id to a recognizable title string.
Tab::SessionTitleResolver makeResolver()
{
    return [](SessionId s) {
        return std::format("session-{}", s.value);
    };
}
} // namespace

TEST_CASE("Tab: a new tab has one pane and derives its title from the session", "[vtmux][tab]")
{
    Ids ids;
    auto const rootPaneId = ids.pane();
    auto const session = ids.session();
    Tab tab { TabId { 1 }, rootPaneId, session };

    CHECK(tab.paneCount() == 1);
    CHECK_FALSE(tab.hasMultiplePanes());
    CHECK(tab.activePane()->id() == rootPaneId);
    CHECK(tab.title(makeResolver()) == std::format("session-{}", session.value));
}

TEST_CASE("Tab: title precedence is runtime > MultiplePanes > active-leaf", "[vtmux][tab][title]")
{
    Ids ids;
    Tab tab { TabId { 1 }, ids.pane(), ids.session() };
    auto const resolver = makeResolver();

    SECTION("single pane uses the active-leaf session title")
    {
        CHECK(tab.title(resolver).rfind("session-", 0) == 0);
    }

    SECTION("a split with no runtime title yields the MultiplePanes label")
    {
        tab.splitActivePane(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());
        CHECK(tab.hasMultiplePanes());
        CHECK(tab.title(resolver) == std::string { Tab::MultiplePanesLabel });
    }

    SECTION("a runtime title overrides everything")
    {
        tab.splitActivePane(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());
        tab.setRuntimeTitle("my tab");
        CHECK(tab.title(resolver) == "my tab");

        tab.setRuntimeTitle(std::nullopt); // clearing falls back to MultiplePanes
        CHECK(tab.title(resolver) == std::string { Tab::MultiplePanesLabel });
    }
}

TEST_CASE("Tab: splitting makes the new pane active", "[vtmux][tab][split]")
{
    Ids ids;
    Tab tab { TabId { 1 }, ids.pane(), ids.session() };

    auto const newLeafId = ids.pane();
    auto* newLeaf = tab.splitActivePane(SplitState::Horizontal, ids.pane(), newLeafId, ids.session());

    REQUIRE(newLeaf != nullptr);
    CHECK(newLeaf->id() == newLeafId);
    CHECK(tab.activePane() == newLeaf);
    CHECK(tab.paneCount() == 2);
}

TEST_CASE("Tab: closing a pane absorbs the sibling and updates the active pane", "[vtmux][tab][close]")
{
    Ids ids;
    auto const rootId = ids.pane();
    Tab tab { TabId { 1 }, rootId, ids.session() };

    auto* second = tab.splitActivePane(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());
    REQUIRE(tab.paneCount() == 2);
    REQUIRE(tab.activePane() == second);
    CHECK_FALSE(tab.isLastPane(second));

    auto const secondSession = second->session(); // capture before the node is absorbed away
    auto const closedSession = tab.closePane(second);
    CHECK(closedSession == secondSession);

    CHECK(tab.paneCount() == 1);
    CHECK(tab.activePane()->isLeaf());
    CHECK(tab.isLastPane(tab.activePane()));
}

TEST_CASE("Tab: closing the non-active sibling keeps the absorbed active leaf", "[vtmux][tab][close]")
{
    // Regression for the dangling-pointer read in closePane(): with the active leaf being the sibling
    // that closeChild() absorbs (and destroys the old Pane object of), the active-leaf reselection must
    // not depend on comparing against the freed sibling pointer. Scenario: split (right active), focus
    // Left (left active), then close the NON-active right pane -- the active leaf is the absorbed one.
    Ids ids;
    Tab tab { TabId { 1 }, ids.pane(), ids.session() };

    auto* right = tab.splitActivePane(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());
    auto* left = tab.rootPane()->first();
    REQUIRE(tab.focusDirection(FocusDirection::Left) == left);
    REQUIRE(tab.activePane() == left); // the sibling that will be absorbed by closing `right`

    auto const leftSession = left->session();   // capture before the node is absorbed away
    auto const rightSession = right->session(); // capture before `right` is destroyed by the close
    auto const closedSession = tab.closePane(right);
    CHECK(closedSession == rightSession);

    // The tab collapses to the single surviving leaf, which carries the previously-active session, and
    // it is the active + last pane. Reaching here without a use-after-free is the point of the test.
    CHECK(tab.paneCount() == 1);
    REQUIRE(tab.activePane() != nullptr);
    CHECK(tab.activePane()->isLeaf());
    CHECK(tab.activePane()->session() == leftSession);
    CHECK(tab.isLastPane(tab.activePane()));
}

TEST_CASE("Tab: isLastPane is true only for a single-pane tab", "[vtmux][tab][close]")
{
    Ids ids;
    Tab tab { TabId { 1 }, ids.pane(), ids.session() };
    CHECK(tab.isLastPane(tab.activePane()));

    tab.splitActivePane(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());
    CHECK_FALSE(tab.isLastPane(tab.activePane()));
}

TEST_CASE("Tab: directional focus moves between panes", "[vtmux][tab][focus]")
{
    Ids ids;
    Tab tab { TabId { 1 }, ids.pane(), ids.session() };
    // After the split the second (right) pane is active.
    auto* right = tab.splitActivePane(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());
    auto* left = tab.rootPane()->first();

    REQUIRE(tab.activePane() == right);

    auto* moved = tab.focusDirection(FocusDirection::Left);
    CHECK(moved == left);
    CHECK(tab.activePane() == left);

    // No neighbor further left.
    CHECK(tab.focusDirection(FocusDirection::Left) == nullptr);
    CHECK(tab.activePane() == left); // unchanged
}

TEST_CASE("Tab: color override can be set and reset", "[vtmux][tab][color]")
{
    Ids ids;
    Tab tab { TabId { 1 }, ids.pane(), ids.session() };
    CHECK_FALSE(tab.color().has_value());

    tab.setColor(vtbackend::RGBColor { 0x12, 0x34, 0x56 });
    REQUIRE(tab.color().has_value());
    CHECK(tab.color()->red == 0x12);
    CHECK(tab.color()->green == 0x34);
    CHECK(tab.color()->blue == 0x56);

    tab.resetColor();
    CHECK_FALSE(tab.color().has_value());
}
