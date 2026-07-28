// SPDX-License-Identifier: Apache-2.0
//
// Tests for the pane-tree layout solver: the leaf->root ratio walk that computes how large the window
// content area must be so a given leaf receives its requested extent (grid) with all ratios fixed.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <limits>
#include <ranges>
#include <vector>

#include <vtworkspace/PaneLayout.h>

using namespace vtworkspace;
using vtpty::ColumnCount;
using vtpty::LineCount;

namespace
{

constexpr int Handle = DefaultSplitHandleThickness;

/// Mirrors the host layout (PaneNode.qml SplitView) downward: allocates @p content along the path from
/// @p root to @p leaf and returns the extent the leaf actually receives. The solver must never make this
/// smaller than what was requested.
LayoutSize simulateAllocation(Pane& root, Pane const& leaf, LayoutSize content)
{
    // Collect the root->leaf path (the solver walks leaf->root; the allocation goes the other way).
    std::vector<Pane const*> path;
    for (auto const* node = &leaf; node != nullptr; node = node->parent())
        path.push_back(node);
    std::ranges::reverse(path);
    REQUIRE(path.front() == &root);

    auto width = static_cast<double>(content.width);
    auto height = static_cast<double>(content.height);
    for (auto const [parent, child]: std::views::zip(path, path | std::views::drop(1)))
    {
        auto const isFirst = parent->first() == child;
        auto const share = isFirst ? parent->ratio() : 1.0 - parent->ratio();
        auto const handle = isFirst ? 0.0 : static_cast<double>(Handle);
        if (parent->splitState() == SplitState::Vertical)
            width = (width * share) - handle;
        else
            height = (height * share) - handle;
    }
    return { .width = static_cast<int>(width), .height = static_cast<int>(height) };
}

} // namespace

TEST_CASE("PaneLayout: unsplit leaf passes the requirement through", "[vtworkspace][layout]")
{
    auto root = Pane { PaneId { 1 }, SessionId { 1 } };
    CHECK(contentSizeForLeaf(root, { .width = 640, .height = 480 }, Handle, root)
          == LayoutSize { .width = 640, .height = 480 });
}

TEST_CASE("PaneLayout: side-by-side split solves the width axis only", "[vtworkspace][layout]")
{
    auto root = Pane { PaneId { 1 }, SessionId { 1 } };
    auto const [first, second] =
        root.split(SplitState::Vertical, PaneId { 2 }, PaneId { 3 }, SessionId { 2 }, 0.5);

    // First child: exactly parent * ratio, no handle involvement.
    CHECK(contentSizeForLeaf(*first, { .width = 400, .height = 300 }, Handle, root)
          == LayoutSize { .width = 800, .height = 300 });

    // Second child: parent * (1-ratio) - handle => parent = ceil((400 + 6) / 0.5).
    CHECK(contentSizeForLeaf(*second, { .width = 400, .height = 300 }, Handle, root)
          == LayoutSize { .width = 812, .height = 300 });
}

TEST_CASE("PaneLayout: stacked split solves the height axis only", "[vtworkspace][layout]")
{
    auto root = Pane { PaneId { 1 }, SessionId { 1 } };
    auto const [first, second] =
        root.split(SplitState::Horizontal, PaneId { 2 }, PaneId { 3 }, SessionId { 2 }, 0.25);

    CHECK(contentSizeForLeaf(*first, { .width = 640, .height = 100 }, Handle, root)
          == LayoutSize { .width = 640, .height = 400 });

    // share = 0.75: ceil((100 + 6) / 0.75) = ceil(141.33) = 142.
    CHECK(contentSizeForLeaf(*second, { .width = 640, .height = 100 }, Handle, root)
          == LayoutSize { .width = 640, .height = 142 });
}

TEST_CASE("PaneLayout: nested splits compose per traversed level", "[vtworkspace][layout]")
{
    auto root = Pane { PaneId { 1 }, SessionId { 1 } };
    auto const [left, right] =
        root.split(SplitState::Vertical, PaneId { 2 }, PaneId { 3 }, SessionId { 2 }, 0.5);
    auto const [top, bottom] =
        right->split(SplitState::Horizontal, PaneId { 4 }, PaneId { 5 }, SessionId { 3 }, 0.3);
    (void) left;
    (void) top;

    // bottom: height share 0.7 with handle -> ceil((140+6)/0.7) = 209; then width as the RIGHT child of
    // the vertical root (share 0.5, handle) -> ceil((200+6)/0.5) = 412.
    CHECK(contentSizeForLeaf(*bottom, { .width = 200, .height = 140 }, Handle, root)
          == LayoutSize { .width = 412, .height = 209 });
}

TEST_CASE("PaneLayout: solved content never allocates the leaf below its requirement",
          "[vtworkspace][layout]")
{
    // Adversarial ratios at the clamp extremes, three levels deep.
    auto root = Pane { PaneId { 1 }, SessionId { 1 } };
    auto const [l1a, l1b] =
        root.split(SplitState::Vertical, PaneId { 2 }, PaneId { 3 }, SessionId { 2 }, 0.05);
    (void) l1a;
    auto const [l2a, l2b] =
        l1b->split(SplitState::Horizontal, PaneId { 4 }, PaneId { 5 }, SessionId { 3 }, 0.95);
    (void) l2b;
    auto const [l3a, l3b] =
        l2a->split(SplitState::Vertical, PaneId { 6 }, PaneId { 7 }, SessionId { 4 }, 0.33);

    auto const required = LayoutSize { .width = 331, .height = 173 };
    for (auto const* leaf: { l3a, l3b })
    {
        auto const content = contentSizeForLeaf(*leaf, required, Handle, root);
        auto const got = simulateAllocation(root, *leaf, content);
        CAPTURE(leaf->id().value, content.width, content.height, got.width, got.height);
        CHECK(got.width >= required.width);
        CHECK(got.height >= required.height);
    }
}

TEST_CASE("PaneLayout: the layout root bounds the ratio walk", "[vtworkspace][layout][zoom]")
{
    // A three-leaf tree: root splits side-by-side, its second child splits top/bottom.
    auto root = Pane { PaneId { 1 }, SessionId { 1 } };
    auto* right = root.split(SplitState::Vertical, PaneId { 2 }, PaneId { 3 }, SessionId { 2 }).second;
    auto* rightBottom =
        right->split(SplitState::Horizontal, PaneId { 4 }, PaneId { 5 }, SessionId { 3 }).second;

    auto const required = LayoutSize { .width = 640, .height = 480 };

    SECTION("rooting at the tree root solves the whole parent chain")
    {
        // Both splits shrink the leaf, so the content area must be bigger than the leaf on both axes.
        auto const content = contentSizeForLeaf(*rightBottom, required, Handle, root);
        CHECK(content.width > required.width);
        CHECK(content.height > required.height);
        // ...and the host allocation gives the leaf at least what was asked for (never undershoot).
        auto const got = simulateAllocation(root, *rightBottom, content);
        CHECK(got.width >= required.width);
        CHECK(got.height >= required.height);
    }

    SECTION("a zoomed leaf is its own layout root, so it owns the content area outright")
    {
        // This is what makes a content-driven resize correct while zoomed: the leaf fills the tab, so
        // no split ratio above it applies and the answer is the identity.
        CHECK(contentSizeForLeaf(*rightBottom, required, Handle, /*layoutRoot*/ *rightBottom) == required);
    }

    SECTION("an intermediate layout root stops the walk there")
    {
        // Solving only up to `right` accounts for the top/bottom split but NOT the outer side-by-side
        // one, so the height grows while the width passes through untouched.
        auto const content = contentSizeForLeaf(*rightBottom, required, Handle, /*layoutRoot*/ *right);
        CHECK(content.width == required.width);
        CHECK(content.height > required.height);
    }
}

TEST_CASE("layoutInCells: a single leaf fills the whole area", "[vtworkspace][layout][cells]")
{
    auto root = Pane { PaneId { 1 }, SessionId { 1 } };
    auto const rects = layoutInCells(root, { .lines = LineCount(24), .columns = ColumnCount(80) });
    REQUIRE(rects.size() == 1);
    CHECK(rects[0] == PaneCellRect { .pane = PaneId { 1 }, .x = 0, .y = 0, .width = 80, .height = 24 });
}

TEST_CASE("layoutInCells: a side-by-side split spends one column on the divider",
          "[vtworkspace][layout][cells]")
{
    auto root = Pane { PaneId { 1 }, SessionId { 1 } };
    auto const [first, second] =
        root.split(SplitState::Vertical, PaneId { 2 }, PaneId { 3 }, SessionId { 2 }, 0.5);

    auto const rects = layoutInCells(root, { .lines = LineCount(24), .columns = ColumnCount(80) });
    REQUIRE(rects.size() == 2);

    // 79 divisible columns: first gets round(0.5 * 79) = 40, divider 1, second the remaining 39 —
    // exactly the sum(child + 1) - 1 == parent arithmetic tmux's layout_check verifies.
    CHECK(rects[0] == PaneCellRect { .pane = first->id(), .x = 0, .y = 0, .width = 40, .height = 24 });
    CHECK(rects[1] == PaneCellRect { .pane = second->id(), .x = 41, .y = 0, .width = 39, .height = 24 });
    CHECK(rects[0].width + 1 + rects[1].width == 80);
}

TEST_CASE("layoutInCells: a stacked split spends one line on the divider", "[vtworkspace][layout][cells]")
{
    auto root = Pane { PaneId { 1 }, SessionId { 1 } };
    auto const [top, bottom] =
        root.split(SplitState::Horizontal, PaneId { 2 }, PaneId { 3 }, SessionId { 2 }, 0.5);

    auto const rects = layoutInCells(root, { .lines = LineCount(24), .columns = ColumnCount(80) });
    REQUIRE(rects.size() == 2);
    CHECK(rects[0] == PaneCellRect { .pane = top->id(), .x = 0, .y = 0, .width = 80, .height = 12 });
    CHECK(rects[1] == PaneCellRect { .pane = bottom->id(), .x = 0, .y = 13, .width = 80, .height = 11 });
    CHECK(rects[0].height + 1 + rects[1].height == 24);
}

TEST_CASE("layoutInCells: an asymmetric ratio rounds and the remainder goes to the second child",
          "[vtworkspace][layout][cells]")
{
    auto root = Pane { PaneId { 1 }, SessionId { 1 } };
    root.split(SplitState::Vertical, PaneId { 2 }, PaneId { 3 }, SessionId { 2 }, 0.7);

    auto const rects = layoutInCells(root, { .lines = LineCount(24), .columns = ColumnCount(100) });
    REQUIRE(rects.size() == 2);
    // 99 divisible columns: round(0.7 * 99) = 69 for the first child, the remaining 30 for the second.
    CHECK(rects[0].width == 69);
    CHECK(rects[1].width == 30);
    CHECK(rects[1].x == 70);
    CHECK(rects[0].width + 1 + rects[1].width == 100);
}

TEST_CASE("layoutInCells: nested splits keep the cell arithmetic exact per level",
          "[vtworkspace][layout][cells]")
{
    auto root = Pane { PaneId { 1 }, SessionId { 1 } };
    auto const [left, right] =
        root.split(SplitState::Vertical, PaneId { 2 }, PaneId { 3 }, SessionId { 2 }, 0.5);
    auto const [top, bottom] =
        right->split(SplitState::Horizontal, PaneId { 4 }, PaneId { 5 }, SessionId { 3 }, 0.3);

    auto const rects = layoutInCells(root, { .lines = LineCount(50), .columns = ColumnCount(160) });
    REQUIRE(rects.size() == 3);

    // Outer: 159 divisible -> 80 | 79. Inner (within the right child's 79 x 50): 49 divisible
    // lines -> round(0.3 * 49) = 15 on top, the remaining 34 below.
    CHECK(rects[0] == PaneCellRect { .pane = left->id(), .x = 0, .y = 0, .width = 80, .height = 50 });
    CHECK(rects[1] == PaneCellRect { .pane = top->id(), .x = 81, .y = 0, .width = 79, .height = 15 });
    CHECK(rects[2] == PaneCellRect { .pane = bottom->id(), .x = 81, .y = 16, .width = 79, .height = 34 });

    CHECK(rects[0].width + 1 + rects[1].width == 160);
    CHECK(rects[1].height + 1 + rects[2].height == 50);
}

TEST_CASE("layoutInCells: an extreme ratio still leaves the small child one cell",
          "[vtworkspace][layout][cells]")
{
    auto root = Pane { PaneId { 1 }, SessionId { 1 } };
    // Pane::setRatio itself clamps into [0.05, 0.95], so 0.999 arrives as 0.95. In a 4-column area
    // that still rounds to the FULL divisible extent (round(0.95 * 3) = 3), which the solver must
    // pull back so the second child keeps its one guaranteed cell.
    root.split(SplitState::Vertical, PaneId { 2 }, PaneId { 3 }, SessionId { 2 }, 0.999);

    auto const rects = layoutInCells(root, { .lines = LineCount(24), .columns = ColumnCount(4) });
    REQUIRE(rects.size() == 2);
    CHECK(rects[0].width == 2);
    CHECK(rects[1].width == 1);
    CHECK(rects[0].width + 1 + rects[1].width == 4);
}

TEST_CASE("layoutInCells: an area too small for a split degrades without crashing",
          "[vtworkspace][layout][cells]")
{
    auto root = Pane { PaneId { 1 }, SessionId { 1 } };
    root.split(SplitState::Vertical, PaneId { 2 }, PaneId { 3 }, SessionId { 2 }, 0.5);

    // Two columns cannot host `first + divider + second`: both children clamp to one cell and the
    // subtree overflows the area (documented; the server's resize policy refuses such grids).
    auto const rects = layoutInCells(root, { .lines = LineCount(24), .columns = ColumnCount(2) });
    REQUIRE(rects.size() == 2);
    CHECK(rects[0].width == 1);
    CHECK(rects[1].width == 1);
}

TEST_CASE("contentSizeForLeaf saturates instead of overflowing the int cast",
          "[vtworkspace][layout][overflow]")
{
    // Each upward step divides by a share floored at Pane::MinimumRatio (0.05), so every nested
    // split multiplies the requirement by up to 20. Six of them from a modest leaf lands past
    // INT_MAX — and `static_cast<int>` of an out-of-range double is UNDEFINED, not saturating: a
    // garbage (often negative) extent reaches the window sizing, and the project's default
    // -fsanitize=undefined presets abort outright.
    auto root = Pane { PaneId { 1 }, SessionId { 1 } };
    auto* node = &root;
    // Always descend into the SECOND child at the maximum ratio: its share is 1 - 0.95 = 0.05, the
    // worst case the clamp allows.
    for (auto const level: std::views::iota(std::uint64_t { 0 }, std::uint64_t { 6 }))
    {
        auto const id = 2 + (2 * level);
        node = node->split(SplitState::Vertical, PaneId { id }, PaneId { id + 1 }, SessionId { id }, 0.95)
                   .second;
    }

    auto const content = contentSizeForLeaf(*node, { .width = 700, .height = 480 }, Handle, root);
    CHECK(content.width == std::numeric_limits<int>::max());
    CHECK(content.height == 480); // the cross axis is untouched by a side-by-side split
}

TEST_CASE("minimumCellArea counts one cell per leaf plus one per divider", "[vtworkspace][layout][cells]")
{
    auto root = Pane { PaneId { 1 }, SessionId { 1 } };
    CHECK(minimumCellArea(root) == LayoutSize { .width = 1, .height = 1 });

    // Side-by-side: 1 + divider + 1 columns, and the taller child decides the rows.
    auto const [left, right] =
        root.split(SplitState::Vertical, PaneId { 2 }, PaneId { 3 }, SessionId { 2 }, 0.5);
    CHECK(minimumCellArea(root) == LayoutSize { .width = 3, .height = 1 });

    // Stacking inside the right child adds a row there, not a column.
    right->split(SplitState::Horizontal, PaneId { 4 }, PaneId { 5 }, SessionId { 3 }, 0.5);
    CHECK(minimumCellArea(root) == LayoutSize { .width = 3, .height = 3 });

    // ...and N leaves stacked along one axis need 2N - 1 cells on it.
    auto* node = left;
    for (auto const level: std::views::iota(std::uint64_t { 0 }, std::uint64_t { 3 }))
        node = node->split(SplitState::Vertical,
                           PaneId { 10 + level },
                           PaneId { 20 + level },
                           SessionId { 10 + level },
                           0.5)
                   .second;
    CHECK(minimumCellArea(*left) == LayoutSize { .width = 7, .height = 1 }); // 4 leaves -> 2*4 - 1
}

TEST_CASE("splitCellExtents partitions exactly once both minima fit", "[vtworkspace][layout][cells]")
{
    // The arithmetic tmux's layout_check verifies: first + divider + second == parent, with each
    // child at or above the extent its own subtree needs. A flat one-cell minimum is not enough —
    // squeezing a child that itself splits below ITS minimum is what made the emitted layout string
    // fail the check.
    for (auto const extent: std::views::iota(9, 40))
    {
        auto const [first, second] = splitCellExtents(extent,
                                                      0.5,
                                                      /*firstMinimum*/ 3,
                                                      /*secondMinimum*/ 5);
        CAPTURE(extent, first, second);
        CHECK(first >= 3);
        CHECK(second >= 5);
        CHECK(first + CellDividerThickness + second == extent);
    }

    // The extremes of the ratio clamp still respect both minima and still partition exactly.
    for (auto const ratio: { 0.05, 0.5, 0.95 })
    {
        auto const [first, second] = splitCellExtents(20, ratio, 4, 7);
        CAPTURE(ratio, first, second);
        CHECK(first >= 4);
        CHECK(second >= 7);
        CHECK(first + CellDividerThickness + second == 20);
    }
}

TEST_CASE("splitCellExtents keeps a valid clamp range on an impossible axis", "[vtworkspace][layout][cells]")
{
    // Below firstMinimum + divider + secondMinimum there is no partition at all. The minima must
    // still win (a zero-cell pane is not a pane) and, crucially, the clamp range must not invert —
    // std::clamp with lo > hi is undefined behaviour.
    auto const [first, second] = splitCellExtents(2, 0.5, 3, 5);
    CHECK(first == 3);
    CHECK(second == 5);
}
