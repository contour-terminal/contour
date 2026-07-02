// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <format>
#include <string>
#include <vector>

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

TEST_CASE("Tab: closing the active leaf of a 3-pane tab reselects the most-recently-used survivor",
          "[vtmux][tab][close][focus]")
{
    // The MRU-driven reselection loop is never run with >2 panes, so its ORDERING is unverified — this is
    // where "closed a pane and focus jumped to the wrong pane" bugs live. Build 3 leaves, seed a known MRU by
    // focusing them in order, then close the active leaf and assert the NEXT most-recently-used survivor
    // (not an arbitrary fallback) becomes active.
    Ids ids;
    Tab tab { TabId { 1 }, ids.pane(), ids.session() };

    // Split twice for three leaves. After split #1: root = [orig | second]; `second` is the active new leaf.
    // Split #2 splits the active `second` into [second | third]; so the tree is [orig | [second | third]]
    // and `third` is active. Collect the three actual LEAVES by walking the tree (splitActivePane's return
    // is the new leaf; the intermediate `second` pane became an internal split node after split #2, so we do
    // NOT hold onto it as a leaf).
    tab.splitActivePane(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());
    tab.splitActivePane(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());
    REQUIRE(tab.paneCount() == 3);

    std::vector<Pane*> leaves;
    tab.rootPane()->walkTree([&](Pane& p) {
        if (p.isLeaf())
            leaves.push_back(&p);
    });
    REQUIRE(leaves.size() == 3);
    // Pre-order leaf visit gives [orig, second-leaf, third]. Name them by position.
    auto* orig = leaves[0];
    auto* mid = leaves[1];
    auto* last = leaves[2];

    // Seed a known MRU by focusing each leaf; end on `last` so the MRU front-to-back is last, mid, orig.
    tab.setActivePane(orig);
    tab.setActivePane(mid);
    tab.setActivePane(last);
    REQUIRE(tab.activePane() == last);
    auto const midId = mid->id();

    // Close the active leaf `last`. Reselection walks the MRU for the next surviving leaf -> `mid` (more
    // recently used than orig), NOT the firstLeaf fallback (which would pick orig).
    tab.closePane(last);
    CHECK(tab.paneCount() == 2);
    CHECK(tab.activePane()->id() == midId);

    // Closing a NON-active leaf leaves the active session unchanged. NB: after the collapse the surviving
    // Pane *object* is replaced (closeChild absorbs the survivor into the parent), so identity is the
    // session, not the Pane pointer — assert the session is preserved.
    auto* activeBefore = tab.activePane();
    auto const activeSessionBefore = activeBefore->session();
    Pane* nonActive = nullptr;
    tab.rootPane()->walkTree([&](Pane& p) {
        if (p.isLeaf() && &p != activeBefore)
            nonActive = &p;
    });
    REQUIRE(nonActive != nullptr);
    tab.closePane(nonActive);
    CHECK(tab.paneCount() == 1);
    CHECK(tab.activePane()->session() == activeSessionBefore);
}

TEST_CASE("Tab: focusDirection has no wrap-around and returns nullptr at every edge", "[vtmux][tab][focus]")
{
    // Per-edge null and non-wrap are the boundary cases users hit constantly; only Left-null was covered.
    // Build root Vertical [ left | right ], then split the active `right` Horizontally -> [ rtTop / rtBottom
    // ].
    Ids ids;
    Tab tab { TabId { 1 }, ids.pane(), ids.session() };
    tab.splitActivePane(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());
    auto* rtBottom = tab.splitActivePane(SplitState::Horizontal, ids.pane(), ids.pane(), ids.session());
    REQUIRE(tab.activePane() == rtBottom);

    // Identify the leaves by pre-order walk: [left, rtTop, rtBottom].
    std::vector<Pane*> leaves;
    tab.rootPane()->walkTree([&](Pane& p) {
        if (p.isLeaf())
            leaves.push_back(&p);
    });
    REQUIRE(leaves.size() == 3);
    auto* left = leaves[0];
    auto* rtTop = leaves[1];
    REQUIRE(leaves[2] == rtBottom);

    // rtBottom edges: no Down (bottom), no Right (rightmost column). Down/Right must NOT wrap to the
    // top/left.
    CHECK(tab.focusDirection(FocusDirection::Down) == nullptr);
    CHECK(tab.activePane() == rtBottom);
    CHECK(tab.focusDirection(FocusDirection::Right) == nullptr);
    CHECK(tab.activePane() == rtBottom);

    // From rtTop: no Up (top edge), no Right (rightmost).
    tab.setActivePane(rtTop);
    CHECK(tab.focusDirection(FocusDirection::Up) == nullptr);
    CHECK(tab.activePane() == rtTop);
    CHECK(tab.focusDirection(FocusDirection::Right) == nullptr);

    // From left: no Left (leftmost), no Up/Down (no horizontal split on its path). Left must NOT wrap right.
    tab.setActivePane(left);
    CHECK(tab.focusDirection(FocusDirection::Left) == nullptr);
    CHECK(tab.activePane() == left);
    CHECK(tab.focusDirection(FocusDirection::Up) == nullptr);
    CHECK(tab.focusDirection(FocusDirection::Down) == nullptr);
}

TEST_CASE("Tab: splitting an already-split tab adds a third leaf and activates it", "[vtmux][tab][split]")
{
    Ids ids;
    Tab tab { TabId { 1 }, ids.pane(), ids.session() };
    tab.splitActivePane(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());
    REQUIRE(tab.paneCount() == 2);
    // Split the active pane again.
    auto* third = tab.splitActivePane(SplitState::Horizontal, ids.pane(), ids.pane(), ids.session());
    REQUIRE(third != nullptr);
    CHECK(tab.paneCount() == 3);
    CHECK(tab.activePane() == third); // the new leaf is active
}
