// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/PageSize.hpp>

#include <crispy/assert.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include <vtworkspace/Pane.hpp>

namespace vtworkspace
{

/// A 2D extent in the host's pixel unit. The solver is unit-agnostic; hosts pass logical pixels.
struct LayoutSize
{
    int width = 0;
    int height = 0;

    [[nodiscard]] constexpr bool operator==(LayoutSize const&) const noexcept = default;
};

/// Default thickness of a split divider handle in logical pixels.
///
/// The single source for every consumer of the handle's extent: the GUI's explicit SplitView
/// `handle:` delegate binds its implicit size to this value (surfaced through the session manager),
/// and window-size solving passes it as @p handleThickness to contentSizeForLeaf().
///
/// Solver contract: the value handed to the solver must be an UPPER bound of the rendered handle.
/// The solver may only ever overshoot (surplus becomes leaf padding); undershooting would silently
/// shrink a requested grid.
inline constexpr int DefaultSplitHandleThickness = 6;

/// Thickness, in CELLS, of the divider a split spends between its children.
///
/// The cell-space counterpart of DefaultSplitHandleThickness, and deliberately not derived from it:
/// this is tmux's model, whose `layout_check` verifies `first + divider + second == parent` on every
/// ingested layout string — which only holds for exactly one cell. Single-sourced so a projection
/// (layoutInCells) and any composition of one cannot drift apart.
inline constexpr int CellDividerThickness = 1;

/// Computes the root (content-area) size required so @p leaf receives AT LEAST @p requiredLeafSize,
/// keeping every split ratio fixed.
///
/// Host layout contract (mirrors PaneNode.qml's SplitView): along a split's axis the FIRST child gets
/// exactly `parentExtent * ratio`; the divider handle (@p handleThickness) and the remainder go to the
/// SECOND child, i.e. `second = parentExtent * (1 - ratio) - handle`. The cross axis passes through
/// unchanged. SplitState::Vertical places children side-by-side (splits the width axis);
/// SplitState::Horizontal stacks them (splits the height axis).
///
/// Each upward step CEILS, so the leaf ends up with at least the requested extent — exact in the common
/// case; any surplus is absorbed by the consumer's floor-to-grid semantics as padding. Never undershoots:
/// undershooting would silently shrink the requested grid.
///
/// The walk stops at @p layoutRoot, the node the host gives the whole content area to (Tab::layoutRoot).
/// Splits ABOVE it do not divide the content area, so their ratios must not be solved through. One rule
/// covers all three cases: an unsplit tab and a zoomed pane are both a leaf that IS the layout root, so
/// no ratio applies and the answer is the identity; a tiled tab roots at the tree and solves the lot.
/// @param leaf             The target leaf pane (its parent chain supplies the ratios).
/// @param requiredLeafSize The extent the leaf must receive.
/// @param handleThickness  The divider handle's thickness along a split axis, in the same unit.
/// @param layoutRoot       The node occupying the whole content area (Tab::layoutRoot).
/// @pre @p layoutRoot must be an ancestor of @p leaf, or @p leaf itself — otherwise the leaf is not on
///      screen at all and there is no content size to solve for.
/// @return The required content-area extent; equals @p requiredLeafSize when @p leaf is the layout root.
[[nodiscard]] inline LayoutSize contentSizeForLeaf(Pane const& leaf,
                                                   LayoutSize requiredLeafSize,
                                                   int handleThickness,
                                                   Pane const& layoutRoot) noexcept
{
    Require(layoutRoot.contains(&leaf) && "the leaf must lie inside the layout root it is solved against");

    auto width = static_cast<double>(requiredLeafSize.width);
    auto height = static_cast<double>(requiredLeafSize.height);

    for (auto const* node = &leaf; node != &layoutRoot; node = node->parent())
    {
        auto const* parent = node->parent();
        auto const isFirst = parent->first() == node;
        // Pane::setRatio clamps into (0, 1); the max() below merely guards a hand-built tree.
        auto const share = std::max(isFirst ? parent->ratio() : 1.0 - parent->ratio(), 0.01);
        auto const handle = isFirst ? 0.0 : static_cast<double>(handleThickness);

        if (parent->splitState() == SplitState::Vertical)
            width = std::ceil((width + handle) / share);
        else if (parent->splitState() == SplitState::Horizontal)
            height = std::ceil((height + handle) / share);
    }

    // Saturate before narrowing. Each upward step divides by a share floored at Pane::MinimumRatio,
    // so a handful of nested splits with the dividers dragged to their limits multiplies the required
    // extent past INT_MAX — and `static_cast<int>` of an out-of-range double is UNDEFINED, not
    // saturating: a garbage extent asks the window manager for a nonsense window size, and on the
    // project's default -fsanitize=undefined presets it is a hard abort.
    auto const toExtent = [](double value) noexcept {
        return static_cast<int>(std::min(value, static_cast<double>(std::numeric_limits<int>::max())));
    };
    return { .width = toExtent(width), .height = toExtent(height) };
}

/// One leaf pane's resolved rectangle in CELL space, as produced by layoutInCells().
struct PaneCellRect
{
    /// The leaf this rectangle belongs to.
    PaneId pane;
    /// Leftmost cell column of the pane, 0-based within the laid-out area.
    int x = 0;
    /// Topmost cell row of the pane, 0-based within the laid-out area.
    int y = 0;
    /// Extent in cell columns (>= 1).
    int width = 0;
    /// Extent in cell rows (>= 1).
    int height = 0;

    [[nodiscard]] constexpr bool operator==(PaneCellRect const&) const noexcept = default;
};

/// The smallest cell area @p node's tree can occupy: every leaf keeping one cell, one divider cell
/// between neighbours.
///
/// The bound tmux's `layout_check` implies — along a split's axis the parent must equal
/// `first + 1 + second`, so a split needs its two subtrees' minima plus one, and N leaves stacked
/// along one axis need `2N - 1` cells there. Below this there is NO valid layout string for the
/// tree at all, which is why encodeLayout() solves against this instead of clamping and emitting
/// one that every control-mode consumer rejects.
/// @param node The pane tree (or subtree).
/// @return Its minimum extent in cells; at least 1x1.
[[nodiscard]] inline LayoutSize minimumCellArea(Pane const& node) noexcept
{
    if (node.isLeaf())
        return { .width = 1, .height = 1 };

    auto const first = minimumCellArea(*node.first());
    auto const second = minimumCellArea(*node.second());
    if (node.splitState() == SplitState::Vertical)
        return { .width = first.width + CellDividerThickness + second.width,
                 .height = std::max(first.height, second.height) };
    return { .width = std::max(first.width, second.width),
             .height = first.height + CellDividerThickness + second.height };
}

/// The two children's extents when @p axisExtent cells split at @p ratio with a one-cell divider,
/// each child held at or above its own minimum: the first child receives
/// round(ratio * (extent - 1)) cells clamped into range, the divider takes one, and the second
/// child receives the rest.
///
/// `first + 1 + second == extent` holds EXACTLY whenever the axis can seat both minima
/// (`axisExtent >= firstMinimum + 1 + secondMinimum`) — the arithmetic tmux's `layout_check`
/// verifies. A smaller axis cannot be partitioned at all; the minima then win and the subtree
/// overflows, which is what layoutInCells wants (a pane below one cell is not a pane) and what
/// encodeLayout must not do (@see minimumCellArea).
/// @param axisExtent The parent's extent along the split axis, in cells.
/// @param ratio The first child's share in (0, 1).
/// @param firstMinimum The fewest cells the first child's subtree can occupy along this axis.
/// @param secondMinimum The fewest cells the second child's subtree can occupy along this axis.
/// @return {first, second} extents in cells.
[[nodiscard]] inline std::pair<int, int> splitCellExtents(int axisExtent,
                                                          double ratio,
                                                          int firstMinimum,
                                                          int secondMinimum) noexcept
{
    auto const divisible = axisExtent - CellDividerThickness;
    // max() so the clamp range can never invert (lo > hi is UB) on an axis too small for both.
    auto const highest = std::max(firstMinimum, divisible - secondMinimum);
    auto const first = std::clamp(static_cast<int>(std::lround(ratio * divisible)), firstMinimum, highest);
    return { first, std::max(secondMinimum, divisible - first) };
}

/// The two children's extents when @p axisExtent cells split at @p ratio, each child keeping at
/// least ONE cell — the leaf-vs-leaf case of the overload above, and what a cell PROJECTION wants.
/// @param axisExtent The parent's extent along the split axis, in cells.
/// @param ratio The first child's share in (0, 1).
/// @return {first, second} extents in cells.
[[nodiscard]] inline std::pair<int, int> splitCellExtents(int axisExtent, double ratio) noexcept
{
    return splitCellExtents(axisExtent, ratio, 1, 1);
}

/// Computes every leaf's cell rectangle when @p root is laid out into @p area with a ONE-CELL
/// divider between split children — tmux's model, deliberately NOT the GUI's pixel-thick handle.
///
/// This is a cell-space PROJECTION of the pane tree, never a readout of GUI geometry: along a
/// split's axis the first child receives round(ratio * (extent - 1)) cells (clamped so both
/// children keep at least one cell), the divider takes one, and the second child receives the
/// rest. Hence `first + 1 + second == extent` exactly — the arithmetic tmux's layout_check
/// verifies on every ingested layout string, which is why the divider width must be one cell.
/// SplitState::Vertical places children side-by-side (splits columns); SplitState::Horizontal
/// stacks them (splits lines), matching contentSizeForLeaf's axis contract above.
///
/// Pass Tab::layoutRoot() as @p root so a zoomed tab projects as its zoomed leaf filling the area.
///
/// The exact-sum invariant holds whenever @p area is at least minimumCellArea(root); a smaller area
/// keeps every leaf at one cell and lets the subtree overflow instead, because a zero-cell pane is
/// not a pane. Callers that promise tmux-valid output must therefore not project through this —
/// encodeLayout() solves against minimumCellArea() for exactly that reason.
/// @param root The pane tree (or subtree) occupying the whole area.
/// @param area The content grid to project into.
/// @return One rectangle per leaf, in tree order (first child before second child).
[[nodiscard]] inline std::vector<PaneCellRect> layoutInCells(Pane const& root, vtpty::PageSize area)
{
    auto out = std::vector<PaneCellRect> {};
    out.reserve(static_cast<size_t>(root.leafCount()));

    auto const recurse = [&out](auto const& self, Pane const& node, int x, int y, int width, int height) {
        if (node.isLeaf())
        {
            out.push_back({ .pane = node.id(), .x = x, .y = y, .width = width, .height = height });
            return;
        }

        auto const axisExtent = node.splitState() == SplitState::Vertical ? width : height;
        auto const [first, second] = splitCellExtents(axisExtent, node.ratio());

        if (node.splitState() == SplitState::Vertical)
        {
            self(self, *node.first(), x, y, first, height);
            self(self, *node.second(), x + first + CellDividerThickness, y, second, height);
        }
        else
        {
            self(self, *node.first(), x, y, width, first);
            self(self, *node.second(), x, y + first + CellDividerThickness, width, second);
        }
    };
    recurse(recurse, root, 0, 0, unbox(area.columns), unbox(area.lines));
    return out;
}

} // namespace vtworkspace
