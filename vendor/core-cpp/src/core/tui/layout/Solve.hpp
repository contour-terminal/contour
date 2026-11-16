// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file Solve.hpp
/// @brief The flexbox layout as a pure function: a tree of styles and a rectangle in, one rectangle per node
/// out.

#include <core/tui/Rect.hpp>
#include <core/tui/layout/Style.hpp>

#include <vector>

namespace core::tui::layout
{

/// @brief Lays a tree out in integer cells.
///
/// The root takes all of @p available. Along each line a container's `gap` is subtracted first; a
/// leftover is shared among the growing children in proportion to `grow`, each taking the whole part
/// of its share and the remaining cells going one at a time to the earliest growing children; a
/// deficit is taken from the shrinking children in proportion to `shrink` times the basis, the
/// remaining cells one at a time from the latest. A child pushed past its `minSize` or `maxSize` is
/// frozen there and the rest is shared again among the others, until no child is pushed past a
/// limit or none is left to take it. What still does not fit is clipped: every rectangle lies inside
/// its parent's, from the last child backwards, so a size is never negative. A node's children past
/// `MaxChildren` are not laid out: they and their subtrees get zero-size rectangles at the parent's origin.
///
/// No floating point, no allocation but the result's and the per-container scratch, nothing thrown:
/// equal input gives equal output, on every platform.
///
/// @param root The tree to lay out.
/// @param available The rectangle the root fills. A negative width or height counts as 0, and position and
///        size are capped to `MaxCells` (a position to `-MaxCells` below), so the root's rectangle is
///        @p available only within those bounds.
/// @return One rectangle per node, in depth-first pre-order: the root first, then each child's subtree.
[[nodiscard]] std::vector<Rect> solve(Node const& root, Rect available);

} // namespace core::tui::layout
