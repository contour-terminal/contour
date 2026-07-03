// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cmath>

#include <vtmux/Pane.h>

namespace vtmux
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
/// @param leaf             The target leaf pane (its parent chain supplies the ratios).
/// @param requiredLeafSize The extent the leaf must receive.
/// @param handleThickness  The divider handle's thickness along a split axis, in the same unit.
/// @return The required root extent; equals @p requiredLeafSize for an unsplit (parentless) leaf.
[[nodiscard]] inline LayoutSize contentSizeForLeaf(Pane const& leaf,
                                                   LayoutSize requiredLeafSize,
                                                   int handleThickness) noexcept
{
    auto width = static_cast<double>(requiredLeafSize.width);
    auto height = static_cast<double>(requiredLeafSize.height);

    auto const* node = &leaf;
    while (auto const* parent = node->parent())
    {
        auto const isFirst = parent->first() == node;
        // Pane::setRatio clamps into (0, 1); the max() below merely guards a hand-built tree.
        auto const share = std::max(isFirst ? parent->ratio() : 1.0 - parent->ratio(), 0.01);
        auto const handle = isFirst ? 0.0 : static_cast<double>(handleThickness);

        if (parent->splitState() == SplitState::Vertical)
            width = std::ceil((width + handle) / share);
        else if (parent->splitState() == SplitState::Horizontal)
            height = std::ceil((height + handle) / share);

        node = parent;
    }

    return { .width = static_cast<int>(width), .height = static_cast<int>(height) };
}

} // namespace vtmux
