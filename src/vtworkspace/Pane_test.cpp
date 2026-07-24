// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include <vtworkspace/Pane.h>
#include <vtworkspace/Primitives.h>

using namespace vtworkspace;

namespace
{
// A tiny id factory so tests don't hand-thread ids everywhere.
struct Ids
{
    uint64_t nextPane = 1;
    uint64_t nextSession = 100;
    PaneId pane() { return PaneId { nextPane++ }; }
    SessionId session() { return SessionId { nextSession++ }; }
};
} // namespace

TEST_CASE("Pane: a fresh pane is a leaf carrying its session", "[vtworkspace][pane]")
{
    Ids ids;
    auto const sid = ids.session();
    Pane const pane { ids.pane(), sid };

    CHECK(pane.isLeaf());
    CHECK(pane.splitState() == SplitState::None);
    CHECK(pane.session() == sid);
    CHECK(pane.first() == nullptr);
    CHECK(pane.second() == nullptr);
    CHECK(pane.leafCount() == 1);
}

TEST_CASE("Pane: split promotes the leaf and moves its session into the first child",
          "[vtworkspace][pane][split]")
{
    Ids ids;
    auto const originalId = ids.pane();
    auto const originalSession = ids.session();
    Pane root { originalId, originalSession };

    auto const splitNodeId = ids.pane();
    auto const newLeafId = ids.pane();
    auto const newSession = ids.session();

    auto const [first, second] = root.split(SplitState::Vertical, splitNodeId, newLeafId, newSession, 0.5);

    SECTION("the node becomes a split with two children")
    {
        CHECK_FALSE(root.isLeaf());
        CHECK(root.splitState() == SplitState::Vertical);
        CHECK(root.id() == splitNodeId);
        CHECK(root.first() == first);
        CHECK(root.second() == second);
        CHECK(root.leafCount() == 2);
    }

    SECTION("the original session moves into the first child, keeping the old id")
    {
        CHECK(first->isLeaf());
        CHECK(first->session() == originalSession);
        CHECK(first->id() == originalId); // identity preserved across the split
        CHECK(first->parent() == &root);
    }

    SECTION("the new session lands in the second child")
    {
        CHECK(second->isLeaf());
        CHECK(second->session() == newSession);
        CHECK(second->id() == newLeafId);
        CHECK(second->parent() == &root);
    }
}

TEST_CASE("Pane: closeChild lets the parent absorb the surviving sibling", "[vtworkspace][pane][close]")
{
    Ids ids;
    auto const originalId = ids.pane();
    auto const originalSession = ids.session();
    Pane root { originalId, originalSession };

    auto const splitNodeId = ids.pane();
    auto const newLeafId = ids.pane();
    auto const newSession = ids.session();
    auto const [first, second] = root.split(SplitState::Horizontal, splitNodeId, newLeafId, newSession);

    SECTION("closing the second child leaves the first child's content in the root")
    {
        auto const firstSession = first->session();
        auto const firstId = first->id();
        auto const closed = root.closeChild(second);

        CHECK(closed == newSession); // the closed leaf's session is returned
        CHECK(root.isLeaf());        // no single-child node remains
        CHECK(root.session() == firstSession);
        CHECK(root.id() == firstId); // survivor identity preserved
        CHECK(root.leafCount() == 1);
    }

    SECTION("closing the first child leaves the second child's content in the root")
    {
        auto const secondSession = second->session();
        auto const closed = root.closeChild(first);

        CHECK(closed == originalSession);
        CHECK(root.isLeaf());
        CHECK(root.session() == secondSession);
    }
}

TEST_CASE("Pane: nested split then collapse keeps the tree minimal", "[vtworkspace][pane][close]")
{
    Ids ids;
    Pane root { ids.pane(), ids.session() };

    // Split twice to build: root(split) -> [A(split) -> [A1, A2], B]
    auto* a = root.split(SplitState::Vertical, ids.pane(), ids.pane(), ids.session()).first;
    a->split(SplitState::Horizontal, ids.pane(), ids.pane(), ids.session());
    REQUIRE(root.leafCount() == 3);

    // Close A2: A absorbs A1 and becomes a leaf again; root stays a 2-leaf split.
    // `a` is the split node (its old id migrated to a1 during the split), so it is still the
    // root's first child.
    auto* aNode = root.first();
    REQUIRE(aNode == a);
    REQUIRE_FALSE(aNode->isLeaf());
    aNode->closeChild(aNode->second()); // close A2

    CHECK(aNode->isLeaf());
    CHECK(root.leafCount() == 2);
    CHECK_FALSE(root.isLeaf());
}

TEST_CASE("Pane: walkTree visits depth-first pre-order", "[vtworkspace][pane][walk]")
{
    Ids ids;
    Pane root { ids.pane(), ids.session() };
    auto* a = root.split(SplitState::Vertical, ids.pane(), ids.pane(), ids.session()).first;
    a->split(SplitState::Horizontal, ids.pane(), ids.pane(), ids.session());

    std::vector<PaneId> visited;
    root.walkTree([&](Pane& p) { visited.push_back(p.id()); });

    // pre-order: root, first subtree (the split node then its two leaves), then second leaf.
    REQUIRE(visited.size() == 5);
    CHECK(visited[0] == root.id());
    CHECK(visited[1] == root.first()->id());
    CHECK(visited[2] == root.first()->first()->id());
    CHECK(visited[3] == root.first()->second()->id());
    CHECK(visited[4] == root.second()->id());
}

TEST_CASE("Pane: findPane and findLeaf locate nodes by id and session", "[vtworkspace][pane][find]")
{
    Ids ids;
    Pane root { ids.pane(), ids.session() };
    auto const newLeafId = ids.pane();
    auto const newSession = ids.session();
    auto [first, second] = root.split(SplitState::Vertical, ids.pane(), newLeafId, newSession);

    CHECK(root.findPane(newLeafId) == second);
    CHECK(root.findLeaf(newSession) == second);
    CHECK(root.findPane(PaneId { 99999 }) == nullptr);
    CHECK(root.findLeaf(SessionId { 99999 }) == nullptr);
}

TEST_CASE("Pane: neighbor finds adjacent leaves across the matching split axis",
          "[vtworkspace][pane][neighbor]")
{
    Ids ids;
    Pane root { ids.pane(), ids.session() };
    // Vertical split: first child on the left, second on the right.
    auto [left, right] = root.split(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());

    CHECK(root.neighbor(left, FocusDirection::Right) == right);
    CHECK(root.neighbor(right, FocusDirection::Left) == left);
    // No vertical neighbor across a vertical split when moving up/down.
    CHECK(root.neighbor(left, FocusDirection::Up) == nullptr);
    CHECK(root.neighbor(right, FocusDirection::Down) == nullptr);
}

TEST_CASE("Pane: neighbor descends into nested splits to the boundary leaf", "[vtworkspace][pane][neighbor]")
{
    Ids ids;
    Pane root { ids.pane(), ids.session() };
    // root: vertical split [left, right]
    auto [left, right] = root.split(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());
    // right: horizontal split [rightTop, rightBottom]
    auto [rightTop, rightBottom] =
        right->split(SplitState::Horizontal, ids.pane(), ids.pane(), ids.session());

    // Moving Right from the left leaf should land on the top-most leaf of the right subtree
    // (the boundary leaf nearest the edge we came from).
    auto* target = root.neighbor(left, FocusDirection::Right);
    CHECK(target == rightTop);
    CHECK(target != rightBottom);
}

TEST_CASE("Pane: setRatio clamps into the open interval (0, 1)", "[vtworkspace][pane][ratio]")
{
    Ids ids;
    Pane root { ids.pane(), ids.session() };
    root.split(SplitState::Vertical, ids.pane(), ids.pane(), ids.session(), 0.5);

    SECTION("a valid ratio passes through unchanged")
    {
        // 0.25 is within range, so std::clamp returns it bitwise-unchanged (exact compare is safe).
        root.setRatio(0.25);
        CHECK(root.ratio() == 0.25);
    }

    SECTION("a degenerate 0.0 is clamped above zero so the first child stays visible")
    {
        // Regression: an unclamped 0.0/1.0 collapses one child to zero size in the renderer, leaving an
        // invisible, ungrabbable pane the user cannot recover.
        root.setRatio(0.0);
        CHECK(root.ratio() > 0.0);
        CHECK(root.ratio() < 1.0);
    }

    SECTION("a degenerate 1.0 is clamped below one so the second child stays visible")
    {
        root.setRatio(1.0);
        CHECK(root.ratio() > 0.0);
        CHECK(root.ratio() < 1.0);
    }

    SECTION("out-of-range values are clamped into range")
    {
        root.setRatio(-5.0);
        CHECK(root.ratio() > 0.0);
        root.setRatio(5.0);
        CHECK(root.ratio() < 1.0);
    }

    SECTION("clamps to the EXACT boundary values (0.05 / 0.95), not merely inside (0,1)")
    {
        // Pin the actual bounds: a regression that changed MinimumRatio (e.g. to 0.001) would still satisfy
        // the >0 && <1 checks above, so assert the precise clamp targets.
        root.setRatio(0.0);
        CHECK(root.ratio() == Catch::Approx(0.05));
        root.setRatio(1.0);
        CHECK(root.ratio() == Catch::Approx(0.95));
    }
}

TEST_CASE("Pane: closeChild absorbs a split-node survivor and re-parents its grandchildren",
          "[vtworkspace][pane][close]")
{
    // The survivor-is-a-split re-parenting branch is otherwise never exercised (the existing nested test only
    // collapses to a LEAF survivor). A wrong/dangling _parent here breaks every later neighbor()/closePane
    // walk up the tree.
    Ids ids;
    Pane root { ids.pane(), ids.session() };

    // Split so root has: first = leaf A, second = B. Then split B into (B1, B2) so the survivor is a split.
    auto const [a, b] = root.split(SplitState::Vertical, ids.pane(), ids.pane(), ids.session(), 0.5);
    auto const closedSessionOfA = a->session();
    auto const [b1, b2] = b->split(SplitState::Horizontal, ids.pane(), ids.pane(), ids.session(), 0.3);
    auto const b1Id = b1->id();
    auto const b2Id = b2->id();
    auto const bAxis = b->splitState();
    auto const bRatio = b->ratio();

    // Capture B1/B2 sessions before absorption (the Pane objects for A and B are destroyed by closeChild).
    auto const b1Session = b1->session();
    auto const b2Session = b2->session();

    // Close A -> root must BECOME B: take B's axis/ratio/children, and B's grandchildren re-parent to root.
    auto const returned = root.closeChild(a);

    CHECK(returned == closedSessionOfA); // returns the closed leaf's session
    CHECK_FALSE(root.isLeaf());
    CHECK(root.splitState() == bAxis);
    CHECK(root.ratio() == Catch::Approx(bRatio));
    REQUIRE(root.first() != nullptr);
    REQUIRE(root.second() != nullptr);
    CHECK(root.first()->id() == b1Id);
    CHECK(root.second()->id() == b2Id);
    CHECK(root.first()->session() == b1Session);
    CHECK(root.second()->session() == b2Session);
    // CRUCIAL: the absorbed grandchildren now parent to root, not the freed B.
    CHECK(root.first()->parent() == &root);
    CHECK(root.second()->parent() == &root);
    CHECK(root.leafCount() == 2);
}

TEST_CASE("Pane: split ratio persists across a subsequent child split and is inherited on collapse",
          "[vtworkspace][pane][resize]")
{
    // Ratio PERSISTENCE has no coverage: no test reads ratio() after a split-of-child or a collapse. A
    // layout/restore regression that reset or failed to inherit ratios would pass every other test.
    Ids ids;

    SECTION("outer split ratio survives splitting one of its children")
    {
        Pane root { ids.pane(), ids.session() };
        auto const [first, second] =
            root.split(SplitState::Vertical, ids.pane(), ids.pane(), ids.session(), 0.7);
        CHECK(root.ratio() == Catch::Approx(0.7)); // exact, not just in-range
        // Split the second child; the OUTER root ratio must be untouched.
        second->split(SplitState::Horizontal, ids.pane(), ids.pane(), ids.session(), 0.25);
        CHECK(root.ratio() == Catch::Approx(0.7));
        CHECK(second->ratio() == Catch::Approx(0.25));
    }

    SECTION("survivor's ratio is inherited when a split collapses onto its split survivor")
    {
        Pane root { ids.pane(), ids.session() };
        auto const [a, b] = root.split(SplitState::Vertical, ids.pane(), ids.pane(), ids.session(), 0.5);
        b->split(SplitState::Horizontal, ids.pane(), ids.pane(), ids.session(), 0.3);
        root.closeChild(a); // root becomes B, inheriting B's 0.3 ratio
        CHECK(root.ratio() == Catch::Approx(0.3));
    }

    SECTION("an out-of-range initial split ratio is clamped by split()")
    {
        Pane low { ids.pane(), ids.session() };
        low.split(SplitState::Vertical, ids.pane(), ids.pane(), ids.session(), 0.0);
        CHECK(low.ratio() == Catch::Approx(0.05));

        Pane high { ids.pane(), ids.session() };
        high.split(SplitState::Vertical, ids.pane(), ids.pane(), ids.session(), 1.0);
        CHECK(high.ratio() == Catch::Approx(0.95));
    }
}

TEST_CASE("Pane: firstLeaf returns the depth-first first leaf of a subtree", "[vtworkspace][pane][find]")
{
    Ids ids;
    Pane root { ids.pane(), ids.session() };

    // A leaf is its own first leaf.
    CHECK(root.firstLeaf() == &root);

    // Split, then split the FIRST child so the depth-first first leaf sits one level down.
    auto const [left, right] = root.split(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());
    auto const [leftTop, leftBottom] =
        left->split(SplitState::Horizontal, ids.pane(), ids.pane(), ids.session());

    CHECK(root.firstLeaf() == leftTop);
    // The query is per-subtree, not global: each node answers for its own subtree.
    CHECK(left->firstLeaf() == leftTop);
    CHECK(right->firstLeaf() == right);
    CHECK(leftBottom->firstLeaf() == leftBottom);
}

TEST_CASE("Pane: neighbor descends a same-axis nested split to the nearest column",
          "[vtworkspace][pane][neighbor]")
{
    // descendToEdge's same-axis branch: when the subtree being entered is split along the SAME axis
    // that is being crossed, the walk must keep descending toward the edge nearest the origin —
    // the first child when moving Right/Down, the second child when moving Left/Up. A regression
    // here lands focus on the far column instead of the adjacent one.
    Ids ids;
    Pane root { ids.pane(), ids.session() };

    // Four columns: root Vertical [ A | B ] with A Vertical [ a1 | a2 ] and B Vertical [ b1 | b2 ].
    auto const [a, b] = root.split(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());
    auto const [a1, a2] = a->split(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());
    auto const [b1, b2] = b->split(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());

    // Moving Right out of A enters B, itself vertical: land on B's LEFT-most leaf (b1), not b2.
    CHECK(root.neighbor(a2, FocusDirection::Right) == b1);
    // Moving Left out of B enters A, likewise vertical: land on A's RIGHT-most leaf (a2), not a1.
    CHECK(root.neighbor(b1, FocusDirection::Left) == a2);
    // Within each same-axis subtree the immediate sibling is the neighbor.
    CHECK(root.neighbor(a1, FocusDirection::Right) == a2);
    CHECK(root.neighbor(b2, FocusDirection::Left) == b1);
}

TEST_CASE("Primitives: crossingSplitFor and pointsTowardSecondChild map directions to axes",
          "[vtworkspace][primitives]")
{
    // Left/Right cross a Vertical divider (children side by side); Up/Down cross a Horizontal one.
    CHECK(crossingSplitFor(FocusDirection::Left) == SplitState::Vertical);
    CHECK(crossingSplitFor(FocusDirection::Right) == SplitState::Vertical);
    CHECK(crossingSplitFor(FocusDirection::Up) == SplitState::Horizontal);
    CHECK(crossingSplitFor(FocusDirection::Down) == SplitState::Horizontal);

    CHECK(pointsTowardSecondChild(FocusDirection::Right));
    CHECK(pointsTowardSecondChild(FocusDirection::Down));
    CHECK_FALSE(pointsTowardSecondChild(FocusDirection::Left));
    CHECK_FALSE(pointsTowardSecondChild(FocusDirection::Up));
}

TEST_CASE("Pane: neighbor walks up through mismatched-axis ancestors and is null at a deep tree edge",
          "[vtworkspace][pane][focus]")
{
    // neighbor() is the core of directional focus. Build a tree where the nearest matching-axis ancestor is
    // several levels up through an opposite-axis split, so the ancestor-walk (skip same-axis ancestor when
    // the leaf is on the wrong side, keep walking up) is exercised — and assert null at a deep edge.
    Ids ids;
    Pane root { ids.pane(), ids.session() };

    // root: Vertical split -> [ left | right ]. right is a Horizontal split -> [ rtTop / rtBottom ].
    auto const [left, right] = root.split(SplitState::Vertical, ids.pane(), ids.pane(), ids.session(), 0.5);
    auto const leftId = left->id();
    auto const [rtTop, rtBottom] =
        right->split(SplitState::Horizontal, ids.pane(), ids.pane(), ids.session(), 0.5);

    // From rtTop, moving Left must cross the VERTICAL root split (skipping the horizontal parent) to the
    // left leaf — the multi-level walk-up.
    auto* leftNeighborOfTop = root.neighbor(rtTop, FocusDirection::Left);
    REQUIRE(leftNeighborOfTop != nullptr);
    CHECK(leftNeighborOfTop->id() == leftId);
    // rtBottom's Left neighbor is likewise the left leaf.
    auto* leftNeighborOfBottom = root.neighbor(rtBottom, FocusDirection::Left);
    REQUIRE(leftNeighborOfBottom != nullptr);
    CHECK(leftNeighborOfBottom->id() == leftId);

    // Up/Down WITHIN the right horizontal split: rtTop's Down neighbor is rtBottom and vice versa.
    CHECK(root.neighbor(rtTop, FocusDirection::Down) == rtBottom);
    CHECK(root.neighbor(rtBottom, FocusDirection::Up) == rtTop);

    // Edges: the left leaf has no Left neighbor; rtTop has no Up neighbor (top of the tree); rtBottom no
    // Down.
    CHECK(root.neighbor(left, FocusDirection::Left) == nullptr);
    CHECK(root.neighbor(rtTop, FocusDirection::Up) == nullptr);
    CHECK(root.neighbor(rtBottom, FocusDirection::Down) == nullptr);
    // The left leaf has no Up/Down neighbor at all (no horizontal split on its path).
    CHECK(root.neighbor(left, FocusDirection::Up) == nullptr);
    CHECK(root.neighbor(left, FocusDirection::Down) == nullptr);
}

TEST_CASE("Pane: toggleOrientation flips a split node's axis, leaving children and ratio",
          "[vtworkspace][pane]")
{
    Ids ids;
    Pane root { ids.pane(), ids.session() };
    auto const [first, second] = root.split(SplitState::Vertical, ids.pane(), ids.pane(), ids.session(), 0.3);
    auto const* firstBefore = first;
    auto const* secondBefore = second;

    root.toggleOrientation();
    CHECK(root.splitState() == SplitState::Horizontal);
    CHECK(root.first() == firstBefore); // children untouched
    CHECK(root.second() == secondBefore);
    CHECK(root.ratio() == Catch::Approx(0.3)); // ratio untouched

    root.toggleOrientation();
    CHECK(root.splitState() == SplitState::Vertical); // flips back
}

TEST_CASE("Pane: swapLeafPayload trades sessions but keeps both PaneIds", "[vtworkspace][pane]")
{
    Ids ids;
    auto const idA = ids.pane();
    auto const sessA = ids.session();
    auto const idB = ids.pane();
    auto const sessB = ids.session();
    Pane a { idA, sessA };
    Pane b { idB, sessB };

    a.swapLeafPayload(&b);

    CHECK(a.session() == sessB); // sessions traded
    CHECK(b.session() == sessA);
    CHECK(a.id() == idA); // ids stayed put (the stable identity)
    CHECK(b.id() == idB);
}

TEST_CASE("Pane: swapChildren flips the two subtrees wholesale", "[vtworkspace][pane]")
{
    Ids ids;
    Pane root { ids.pane(), ids.session() };
    auto const firstId = root.id(); // migrates to the first child on split
    auto const newLeafId = ids.pane();
    auto const [first, second] = root.split(SplitState::Vertical, ids.pane(), newLeafId, ids.session());
    auto const firstSession = first->session();
    auto const secondSession = second->session();

    root.swapChildren();

    // The nodes swapped slots; each id still carries its own session (no id/session separation).
    CHECK(root.first()->id() == newLeafId);
    CHECK(root.first()->session() == secondSession);
    CHECK(root.second()->id() == firstId);
    CHECK(root.second()->session() == firstSession);
    CHECK(root.first()->parent() == &root); // back-pointers intact
    CHECK(root.second()->parent() == &root);
}

TEST_CASE("Pane: ancestorSplitOnAxis finds the nearest split on the given axis", "[vtworkspace][pane]")
{
    Ids ids;
    // root (Vertical) -> [left leaf | right (Horizontal) -> [top | bottom]]
    Pane root { ids.pane(), ids.session() };
    root.split(SplitState::Vertical, ids.pane(), ids.pane(), ids.session());
    auto* right = root.second();
    right->split(SplitState::Horizontal, ids.pane(), ids.pane(), ids.session());
    auto* top = right->first();
    auto* bottom = right->second();

    // From `top`: nearest Horizontal ancestor is `right`; nearest Vertical ancestor is `root`.
    CHECK(Pane::ancestorSplitOnAxis(top, SplitState::Horizontal) == right);
    CHECK(Pane::ancestorSplitOnAxis(top, SplitState::Vertical) == &root);
    CHECK(Pane::ancestorSplitOnAxis(bottom, SplitState::Horizontal) == right);

    // From the left leaf: only a Vertical ancestor (root) exists; no Horizontal one.
    auto* left = root.first();
    CHECK(Pane::ancestorSplitOnAxis(left, SplitState::Vertical) == &root);
    CHECK(Pane::ancestorSplitOnAxis(left, SplitState::Horizontal) == nullptr);
}

TEST_CASE("Pane: contains() reports subtree membership, counting a node as its own ancestor",
          "[vtworkspace][pane]")
{
    Ids ids;
    auto root = Pane { ids.pane(), ids.session() };
    auto* right = root.split(SplitState::Vertical, ids.pane(), ids.pane(), ids.session()).second;
    auto* left = root.first();
    auto* rightBottom = right->split(SplitState::Horizontal, ids.pane(), ids.pane(), ids.session()).second;
    auto* rightTop = right->first();

    // The tree root contains everything, itself included.
    CHECK(root.contains(&root));
    CHECK(root.contains(left));
    CHECK(root.contains(right));
    CHECK(root.contains(rightBottom));

    // A subtree contains its own descendants and itself, but not its siblings or its ancestors.
    CHECK(right->contains(right));
    CHECK(right->contains(rightTop));
    CHECK(right->contains(rightBottom));
    CHECK_FALSE(right->contains(left));
    CHECK_FALSE(right->contains(&root));

    // A leaf contains only itself — which is what makes a zoomed pane its own layout root.
    CHECK(rightBottom->contains(rightBottom));
    CHECK_FALSE(rightBottom->contains(rightTop));

    CHECK_FALSE(root.contains(nullptr));
}
