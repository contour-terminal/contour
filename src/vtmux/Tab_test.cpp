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
    Tab const tab { TabId { 1 }, rootPaneId, session };

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

    tab.setColor(TabColorSource::User, vtbackend::RGBColor { 0x12, 0x34, 0x56 });
    REQUIRE(tab.color().has_value());
    CHECK(tab.color()->red == 0x12);
    CHECK(tab.color()->green == 0x34);
    CHECK(tab.color()->blue == 0x56);

    tab.resetColor(TabColorSource::User);
    CHECK_FALSE(tab.color().has_value());
}

TEST_CASE("Tab: the user's color outranks the application's, and each survives the other's reset",
          "[vtmux][tab][color]")
{
    // The two sources are independent slots, not one shared cell. This is what lets a terminal reset
    // (RIS -> DECAC reset -> Application source) leave a color the user picked alone, and what makes
    // "set the tab color back to default" fall back to the application's color rather than erase it.
    Ids ids;
    Tab tab { TabId { 1 }, ids.pane(), ids.session() };

    auto constexpr AppColor = vtbackend::RGBColor { 0x11, 0x22, 0x33 };
    auto constexpr UserColor = vtbackend::RGBColor { 0xAA, 0xBB, 0xCC };

    // With only an application color, that is what the tab shows.
    tab.setColor(TabColorSource::Application, AppColor);
    REQUIRE(tab.color() == AppColor);

    // The user's choice outranks it, whichever order the two arrive in.
    tab.setColor(TabColorSource::User, UserColor);
    CHECK(tab.color() == UserColor);
    CHECK(tab.color(TabColorSource::Application) == AppColor); // still there, merely outranked

    // An application reset (DECAC bare form, or RIS) cannot take the user's color away.
    tab.resetColor(TabColorSource::Application);
    CHECK(tab.color() == UserColor);
    CHECK_FALSE(tab.color(TabColorSource::Application).has_value());

    // And the user returning the tab "to default" falls back to the application's color, if it has one.
    tab.setColor(TabColorSource::Application, AppColor);
    tab.resetColor(TabColorSource::User);
    CHECK(tab.color() == AppColor);

    // Only when no source has a color is the tab uncolored, and the host paints its default.
    tab.resetColor(TabColorSource::Application);
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

TEST_CASE("Tab: toggleActivePaneOrientation flips the active pane's parent split", "[vtmux][tab][split]")
{
    Ids ids;
    Tab tab { TabId { 1 }, ids.pane(), ids.session() };

    SECTION("single-pane tab has no split to flip")
    {
        CHECK(tab.toggleActivePaneOrientation() == nullptr);
    }

    SECTION("split tab flips its parent split")
    {
        tab.splitActivePane(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());
        auto* flipped = tab.toggleActivePaneOrientation();
        REQUIRE(flipped == tab.rootPane());
        CHECK(flipped->splitState() == SplitState::Horizontal);
        CHECK(tab.toggleActivePaneOrientation()->splitState() == SplitState::Vertical); // back
    }
}

TEST_CASE("Tab: swapActivePane trades sessions with a neighbor and follows the moved pane",
          "[vtmux][tab][swap]")
{
    Ids ids;
    Tab tab { TabId { 1 }, ids.pane(), ids.session() };
    // left|right; right is active after the split.
    auto* right = tab.splitActivePane(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());
    auto* left = tab.rootPane()->first();
    auto const leftId = left->id();
    auto const rightId = right->id();
    auto const leftSession = left->session();
    auto const rightSession = right->session();
    REQUIRE(tab.activePane() == right);

    SECTION("swapping left trades the two sessions, ids stay, active follows to the left slot")
    {
        auto const [a, b] = tab.swapActivePane(FocusDirection::Left);
        REQUIRE(a == right);
        REQUIRE(b == left);
        CHECK(left->id() == leftId); // ids unchanged
        CHECK(right->id() == rightId);
        CHECK(left->session() == rightSession); // sessions swapped
        CHECK(right->session() == leftSession);
        CHECK(tab.activePane() == left); // focus follows the moved session
    }

    SECTION("no neighbor is a no-op")
    {
        auto const [a, b] = tab.swapActivePane(FocusDirection::Right); // right pane, nothing further right
        CHECK(a == nullptr);
        CHECK(b == nullptr);
        CHECK(tab.activePane() == right);
        CHECK(right->session() == rightSession); // unchanged
    }
}

TEST_CASE("Tab: moveActivePane re-parents the active pane across a non-sibling neighbor",
          "[vtmux][tab][move]")
{
    Ids ids;
    Tab tab { TabId { 1 }, ids.pane(), ids.session() };
    // Build: root(Vertical) = [ left | right(Horizontal) = [ top | bottom ] ], with `bottom` active.
    auto* right = tab.splitActivePane(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());
    tab.setActivePane(right);
    auto* bottom = tab.splitActivePane(SplitState::Horizontal, ids.pane(), ids.pane(), ids.session());
    auto const movedSession = bottom->session();
    auto const movedId = bottom->id();
    REQUIRE(tab.paneCount() == 3);
    REQUIRE(tab.activePane() == bottom);

    SECTION("moving the active pane Left re-parents it beside the left leaf")
    {
        auto* leftLeaf = tab.rootPane()->first();
        auto const leftSession = leftLeaf->session();
        REQUIRE(tab.moveActivePane(FocusDirection::Left, ids.pane()));

        // Leaf count is preserved; the moved session survived with its id and is active.
        CHECK(tab.paneCount() == 3);
        auto* moved = tab.rootPane()->findPane(movedId);
        REQUIRE(moved != nullptr);
        CHECK(moved->isLeaf());
        CHECK(moved->session() == movedSession);
        CHECK(tab.activePane() == moved);
        // The former left leaf's session still exists somewhere in the tree.
        CHECK(tab.rootPane()->findLeaf(leftSession) != nullptr);
        // Tree integrity: every leaf's parent chain reaches the root.
        tab.rootPane()->walkTree([&](Pane& p) {
            if (&p != tab.rootPane())
                CHECK(p.parent() != nullptr);
        });
    }

    SECTION("moving toward a sibling degenerates to a swap, preserving the tree shape")
    {
        // `bottom`'s Up neighbor is `top`, its sibling in the same split -> swap, not re-parent.
        auto* top = right->first();
        auto const topSession = top->session();
        REQUIRE(tab.moveActivePane(FocusDirection::Up, ids.pane()));
        CHECK(tab.paneCount() == 3);
        // Sessions of the two siblings swapped; the tree shape is unchanged (still the same nodes).
        CHECK(top->session() == movedSession);
        CHECK(bottom->session() == topSession);
    }

    SECTION("no neighbor is a no-op")
    {
        // From `bottom`, there is no neighbor Down (bottom of the right split, nothing below).
        CHECK_FALSE(tab.moveActivePane(FocusDirection::Down, ids.pane()));
        CHECK(tab.paneCount() == 3);
    }
}

// {{{ Zoom
//
// The contract under test: zoom always applies to the ACTIVE pane. That single rule is what gives
// "zoom follows focus" for free and what every restructuring operation cancels.

TEST_CASE("Tab: zooming a single-pane tab is a no-op", "[vtmux][tab][zoom]")
{
    Ids ids;
    Tab tab { TabId { 1 }, ids.pane(), ids.session() };

    // There are no siblings to hide, so there is nothing to zoom into: refuse rather than record a
    // zoom the user could not see (and so could not toggle back off by eye).
    CHECK_FALSE(tab.toggleZoom());
    CHECK_FALSE(tab.isZoomed());
    CHECK_FALSE(tab.zoomedLeafId().has_value());
    CHECK(tab.layoutRoot() == tab.rootPane());
}

TEST_CASE("Tab: zoom re-roots the layout at the active pane and back", "[vtmux][tab][zoom]")
{
    Ids ids;
    auto const rootId = ids.pane();
    Tab tab { TabId { 1 }, rootId, ids.session() };
    auto* second = tab.splitActivePane(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());

    // Unzoomed, the host renders the whole tree.
    CHECK(tab.layoutRoot() == tab.rootPane());

    REQUIRE(tab.toggleZoom());
    CHECK(tab.isZoomed());
    // Zoomed, the host renders ONLY the active leaf — that re-rooting is the entire feature.
    CHECK(tab.layoutRoot() == second);
    CHECK(tab.zoomedLeafId() == second->id());
    // The tree itself is untouched: zoom hides panes, it does not close them.
    CHECK(tab.paneCount() == 2);

    REQUIRE(tab.toggleZoom());
    CHECK_FALSE(tab.isZoomed());
    CHECK(tab.layoutRoot() == tab.rootPane());
    CHECK_FALSE(tab.zoomedLeafId().has_value());
}

TEST_CASE("Tab: zoom follows focus", "[vtmux][tab][zoom]")
{
    Ids ids;
    Tab tab { TabId { 1 }, ids.pane(), ids.session() };
    auto* right = tab.splitActivePane(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());
    auto* left = tab.rootPane()->first();

    REQUIRE(tab.activePane() == right);
    REQUIRE(tab.toggleZoom());

    // Moving focus while zoomed stays zoomed and shows the newly focused pane (Windows Terminal
    // semantics). No code implements this: layoutRoot() reads the active leaf, so focus carries it.
    auto* target = tab.focusDirection(FocusDirection::Left);
    REQUIRE(target == left);
    CHECK(tab.isZoomed());
    CHECK(tab.layoutRoot() == left);
    CHECK(tab.zoomedLeafId() == left->id());

    // ...and back.
    REQUIRE(tab.focusDirection(FocusDirection::Right) == right);
    CHECK(tab.isZoomed());
    CHECK(tab.layoutRoot() == right);
}

TEST_CASE("Tab: a focus move with no neighbor leaves the zoom where it was", "[vtmux][tab][zoom]")
{
    Ids ids;
    Tab tab { TabId { 1 }, ids.pane(), ids.session() };
    auto* right = tab.splitActivePane(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());
    REQUIRE(tab.toggleZoom());

    // Nothing to the right of `right`: focus does not move, so neither does the zoom.
    CHECK(tab.focusDirection(FocusDirection::Right) == nullptr);
    CHECK(tab.isZoomed());
    CHECK(tab.layoutRoot() == right);
}

TEST_CASE("Tab: splitting while zoomed unzooms", "[vtmux][tab][zoom]")
{
    Ids ids;
    Tab tab { TabId { 1 }, ids.pane(), ids.session() };
    tab.splitActivePane(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());
    REQUIRE(tab.toggleZoom());
    REQUIRE(tab.isZoomed());

    // The pane a split creates has to be visible, or the user just watched a keypress do nothing.
    tab.splitActivePane(SplitState::Horizontal, ids.pane(), ids.pane(), ids.session());
    CHECK_FALSE(tab.isZoomed());
    CHECK(tab.layoutRoot() == tab.rootPane());
    CHECK(tab.paneCount() == 3);
}

TEST_CASE("Tab: closing a pane while zoomed unzooms without dangling", "[vtmux][tab][zoom]")
{
    Ids ids;
    Tab tab { TabId { 1 }, ids.pane(), ids.session() };
    auto* right = tab.splitActivePane(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());
    REQUIRE(tab.toggleZoom());

    // closePane() absorbs the sibling into the parent, DESTROYING Pane objects. Zoom is a bool tied
    // to the active leaf precisely so there is no pane pointer/id of its own left dangling here.
    tab.closePane(right);

    CHECK_FALSE(tab.isZoomed());
    CHECK(tab.paneCount() == 1);
    CHECK(tab.layoutRoot() == tab.rootPane());
    CHECK(tab.layoutRoot() == tab.activePane());
}

TEST_CASE("Tab: restructuring the tree while zoomed unzooms", "[vtmux][tab][zoom]")
{
    auto const zoomedThreePaneTab = [](Ids& ids, Tab& tab) {
        tab.splitActivePane(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());
        tab.splitActivePane(SplitState::Horizontal, ids.pane(), ids.pane(), ids.session());
        REQUIRE(tab.toggleZoom());
        REQUIRE(tab.isZoomed());
    };

    SECTION("flipping the split orientation")
    {
        Ids ids;
        Tab tab { TabId { 1 }, ids.pane(), ids.session() };
        zoomedThreePaneTab(ids, tab);

        // Flipping an axis that is not on screen would be an invisible keypress.
        CHECK(tab.toggleActivePaneOrientation() != nullptr);
        CHECK_FALSE(tab.isZoomed());
    }

    SECTION("swapping with a neighbor")
    {
        Ids ids;
        Tab tab { TabId { 1 }, ids.pane(), ids.session() };
        zoomedThreePaneTab(ids, tab);

        auto const [a, b] = tab.swapActivePane(FocusDirection::Up);
        REQUIRE(a != nullptr);
        CHECK_FALSE(tab.isZoomed());
    }

    SECTION("moving across a neighbor")
    {
        Ids ids;
        Tab tab { TabId { 1 }, ids.pane(), ids.session() };
        zoomedThreePaneTab(ids, tab);

        REQUIRE(tab.moveActivePane(FocusDirection::Left, ids.pane()));
        CHECK_FALSE(tab.isZoomed());
    }
}

TEST_CASE("Tab: an operation that does nothing leaves the zoom alone", "[vtmux][tab][zoom]")
{
    Ids ids;
    Tab tab { TabId { 1 }, ids.pane(), ids.session() };
    auto* right = tab.splitActivePane(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());
    REQUIRE(tab.toggleZoom());

    // Only operations that actually reshape the tree cancel the zoom. A swap/move that finds no
    // neighbor changes nothing, so cancelling would be a state change the user never asked for.
    CHECK(tab.swapActivePane(FocusDirection::Right).first == nullptr);
    CHECK(tab.isZoomed());

    CHECK_FALSE(tab.moveActivePane(FocusDirection::Right, ids.pane()));
    CHECK(tab.isZoomed());
    CHECK(tab.layoutRoot() == right);
}

TEST_CASE("Tab: a zoomed tab is titled after the pane on screen", "[vtmux][tab][zoom][title]")
{
    Ids ids;
    auto const resolver = makeResolver();
    Tab tab { TabId { 1 }, ids.pane(), ids.session() };
    auto const rightSession = ids.session();
    tab.splitActivePane(SplitState::Vertical, ids.pane(), ids.pane(), rightSession);

    // Tiled, the tab cannot name one pane, so it says so.
    CHECK(tab.title(resolver) == std::string { Tab::MultiplePanesLabel });

    // Zoomed, exactly one pane is on screen — naming it beats the placeholder.
    REQUIRE(tab.toggleZoom());
    CHECK(tab.title(resolver) == std::format("session-{}", rightSession.value));

    // A rename still wins over both (it is the highest-precedence source).
    tab.setRuntimeTitle("pinned");
    CHECK(tab.title(resolver) == "pinned");
}
// }}}

TEST_CASE("Tab: resizing while zoomed unzooms rather than moving an invisible divider", "[vtmux][tab][zoom]")
{
    Ids ids;
    Tab tab { TabId { 1 }, ids.pane(), ids.session() };
    tab.splitActivePane(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());
    REQUIRE(tab.toggleZoom());
    auto const ratioBefore = tab.rootPane()->ratio();

    // Regression: resize used to be the one mutator outside the zoom contract. While zoomed there is
    // no divider on screen, so each press silently rewrote a hidden ratio (clamping it at the extreme
    // after a few presses) and the tiled layout lurched on the next unzoom. It must surface instead.
    auto* split = tab.resizeActivePane(FocusDirection::Left, 0.05);
    REQUIRE(split != nullptr);
    CHECK_FALSE(tab.isZoomed());
    CHECK(tab.layoutRoot() == tab.rootPane());
    CHECK(split->ratio() < ratioBefore); // ...and the resize itself still happened, now visibly
}

TEST_CASE("Tab: resizing with no ancestor split on the axis leaves the zoom alone", "[vtmux][tab][zoom]")
{
    Ids ids;
    Tab tab { TabId { 1 }, ids.pane(), ids.session() };
    // A single Vertical split: there is no Horizontal ancestor, so an Up/Down resize finds nothing.
    tab.splitActivePane(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());
    REQUIRE(tab.toggleZoom());

    CHECK(tab.resizeActivePane(FocusDirection::Up, 0.05) == nullptr);
    CHECK(tab.isZoomed()); // nothing moved, so nothing was surfaced
}

TEST_CASE("Tab: usesMultiplePanesLabel is the one rule behind every consumer's tab label",
          "[vtmux][tab][zoom][title]")
{
    Ids ids;
    Tab tab { TabId { 1 }, ids.pane(), ids.session() };
    CHECK_FALSE(tab.usesMultiplePanesLabel()); // one pane: named after its session

    tab.splitActivePane(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());
    CHECK(tab.usesMultiplePanesLabel()); // tiled and multi-pane: no single pane to name it after

    // Zoomed, exactly one pane is on screen, so the tab is named after THAT pane. Both title() and the
    // host's own label templating read this, so they cannot disagree about what a zoomed tab is called.
    REQUIRE(tab.toggleZoom());
    CHECK_FALSE(tab.usesMultiplePanesLabel());

    REQUIRE(tab.toggleZoom());
    CHECK(tab.usesMultiplePanesLabel());
}
