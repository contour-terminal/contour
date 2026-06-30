// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include <vtmux/Pane.h>

using namespace vtmux;

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

TEST_CASE("Pane: a fresh pane is a leaf carrying its session", "[vtmux][pane]")
{
    Ids ids;
    auto const sid = ids.session();
    Pane pane { ids.pane(), sid };

    CHECK(pane.isLeaf());
    CHECK(pane.splitState() == SplitState::None);
    CHECK(pane.session() == sid);
    CHECK(pane.first() == nullptr);
    CHECK(pane.second() == nullptr);
    CHECK(pane.leafCount() == 1);
}

TEST_CASE("Pane: split promotes the leaf and moves its session into the first child", "[vtmux][pane][split]")
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

TEST_CASE("Pane: closeChild lets the parent absorb the surviving sibling", "[vtmux][pane][close]")
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

TEST_CASE("Pane: nested split then collapse keeps the tree minimal", "[vtmux][pane][close]")
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

TEST_CASE("Pane: walkTree visits depth-first pre-order", "[vtmux][pane][walk]")
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

TEST_CASE("Pane: findPane and findLeaf locate nodes by id and session", "[vtmux][pane][find]")
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

TEST_CASE("Pane: neighbor finds adjacent leaves across the matching split axis", "[vtmux][pane][neighbor]")
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

TEST_CASE("Pane: neighbor descends into nested splits to the boundary leaf", "[vtmux][pane][neighbor]")
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

TEST_CASE("Pane: setRatio clamps into the open interval (0, 1)", "[vtmux][pane][ratio]")
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
}
