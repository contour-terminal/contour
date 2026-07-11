// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/Config.h>

namespace contour
{

/// Descends first-children to the leftmost leaf of @p node (returns @p node itself if it is a leaf).
[[nodiscard]] config::LayoutPane const& leftmostLeaf(config::LayoutPane const& node);

/// The share of space (0,1) given to children[0] versus the remaining children, from their ratios.
[[nodiscard]] double ratioForFirst(config::LayoutPane const& splitNode);

/// A node holding children[1..] under the same orientation. If exactly one child remains, that child
/// is returned directly (so a binary split collapses cleanly).
[[nodiscard]] config::LayoutPane tailGroup(config::LayoutPane const& splitNode);

} // namespace contour
