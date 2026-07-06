// SPDX-License-Identifier: Apache-2.0
//
// Tests for the pane-tree layout solver: the leaf->root ratio walk that computes how large the window
// content area must be so a given leaf receives its requested extent (grid) with all ratios fixed.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <ranges>
#include <vector>

#include <vtmux/PaneLayout.h>

using namespace vtmux;

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

TEST_CASE("PaneLayout: unsplit leaf passes the requirement through", "[vtmux][layout]")
{
    auto root = Pane { PaneId { 1 }, SessionId { 1 } };
    CHECK(contentSizeForLeaf(root, { .width = 640, .height = 480 }, Handle)
          == LayoutSize { .width = 640, .height = 480 });
}

TEST_CASE("PaneLayout: side-by-side split solves the width axis only", "[vtmux][layout]")
{
    auto root = Pane { PaneId { 1 }, SessionId { 1 } };
    auto const [first, second] =
        root.split(SplitState::Vertical, PaneId { 2 }, PaneId { 3 }, SessionId { 2 }, 0.5);

    // First child: exactly parent * ratio, no handle involvement.
    CHECK(contentSizeForLeaf(*first, { .width = 400, .height = 300 }, Handle)
          == LayoutSize { .width = 800, .height = 300 });

    // Second child: parent * (1-ratio) - handle => parent = ceil((400 + 6) / 0.5).
    CHECK(contentSizeForLeaf(*second, { .width = 400, .height = 300 }, Handle)
          == LayoutSize { .width = 812, .height = 300 });
}

TEST_CASE("PaneLayout: stacked split solves the height axis only", "[vtmux][layout]")
{
    auto root = Pane { PaneId { 1 }, SessionId { 1 } };
    auto const [first, second] =
        root.split(SplitState::Horizontal, PaneId { 2 }, PaneId { 3 }, SessionId { 2 }, 0.25);

    CHECK(contentSizeForLeaf(*first, { .width = 640, .height = 100 }, Handle)
          == LayoutSize { .width = 640, .height = 400 });

    // share = 0.75: ceil((100 + 6) / 0.75) = ceil(141.33) = 142.
    CHECK(contentSizeForLeaf(*second, { .width = 640, .height = 100 }, Handle)
          == LayoutSize { .width = 640, .height = 142 });
}

TEST_CASE("PaneLayout: nested splits compose per traversed level", "[vtmux][layout]")
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
    CHECK(contentSizeForLeaf(*bottom, { .width = 200, .height = 140 }, Handle)
          == LayoutSize { .width = 412, .height = 209 });
}

TEST_CASE("PaneLayout: solved content never allocates the leaf below its requirement", "[vtmux][layout]")
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
        auto const content = contentSizeForLeaf(*leaf, required, Handle);
        auto const got = simulateAllocation(root, *leaf, content);
        CAPTURE(leaf->id().value, content.width, content.height, got.width, got.height);
        CHECK(got.width >= required.width);
        CHECK(got.height >= required.height);
    }
}
