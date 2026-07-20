// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Color.h>

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <vtmux/SessionModel.h>

namespace vtmux
{

/// A node in a layout tab's pane tree. It is EITHER a leaf (a terminal to open) OR a split (an
/// orientation plus two or more children): an empty @c children means leaf.
struct LayoutPane
{
    /// Program to run in this pane, replacing the profile's shell. Unset runs that shell.
    std::optional<std::string> command;
    /// Arguments passed to @c command.
    std::vector<std::string> arguments;
    /// Working directory the pane's shell starts in. Unset uses the profile's.
    std::optional<std::string> directory;
    /// Per-pane profile override (terminal-level settings only — not window ones).
    std::optional<std::string> profile;
    /// Fraction (0, 1] of the parent split this pane occupies. Unset means "share whatever the
    /// explicitly-sized siblings leave over, equally" (see vtmux::ratioForFirst).
    std::optional<double> ratio;

    /// How this pane's children are arranged (splits only).
    SplitState orientation = SplitState::Vertical;
    /// The child panes of a split, in layout order. Empty for a leaf.
    std::vector<LayoutPane> children;

    /// @return true when this node is a terminal pane rather than a split.
    [[nodiscard]] bool isLeaf() const noexcept { return children.empty(); }
};

/// One tab of a layout: its name and color, plus the pane tree it opens with.
struct LayoutTab
{
    /// Seeds the tab's name.
    std::optional<std::string> title;
    /// Sets the tab's User color.
    std::optional<vtbackend::RGBColor> color;
    /// Default profile for this tab's panes; a pane's own @c profile still wins.
    std::optional<std::string> profile;
    /// The tab's pane tree: a leaf for a plain single-pane tab, a split node otherwise.
    LayoutPane root;
};

/// A named layout: the ordered tabs that a launch-layout action opens together.
struct Layout
{
    /// The tabs to open, in order.
    std::vector<LayoutTab> tabs;
};

/// Descends first-children to the leftmost leaf of @p node (returns @p node itself if it is a leaf).
[[nodiscard]] LayoutPane const& leftmostLeaf(LayoutPane const& node);

/// The share of space in (0, 1) that @p children.front() receives when @p children are laid out
/// side by side in one split. A child's `ratio` is its fraction of the whole split; children
/// without one share what the explicitly-sized ones leave over, equally.
/// @param children The children of one split (or the not-yet-placed tail of one).
/// @return The first child's share, or 0.5 for an empty range.
[[nodiscard]] double ratioForFirst(std::span<LayoutPane const> children);

/// The share given to @p splitNode's first child versus its remaining children.
/// @param splitNode A split node (a leaf yields 0.5: it has no children to weigh).
[[nodiscard]] double ratioForFirst(LayoutPane const& splitNode);

/// Invoked immediately before each model allocation to stage the backing session for that leaf's
/// command/profile/dir. It must arrange for the model's SessionAllocator to return the id it created.
using PaneSeeder = std::function<void(LayoutPane const& leaf)>;

/// Creates a tab in @p window from @p tab: seeds+creates the first pane, applies title/color, then
/// builds the pane tree. @p seed is invoked immediately before each model allocation to stage that
/// pane's backing session (it must make the model's allocator return the id it created).
Tab* realizeLayoutTab(SessionModel& model, WindowId window, LayoutTab const& tab, PaneSeeder const& seed);

/// A leaf pane's resolved runtime data, as reported by a LeafResolver for serializing live panes.
struct PaneLeafData
{
    std::optional<std::string> command;
    std::vector<std::string> arguments;
    std::optional<std::string> directory;
    std::optional<std::string> profile;
};

/// Resolves a leaf pane's backing session id to its runtime command/arguments/directory, for
/// serializing a live pane tree back into a LayoutPane (the inverse of PaneSeeder).
using LeafResolver = std::function<PaneLeafData(SessionId)>;

/// Serializes @p pane (leaf or split) into a LayoutPane, resolving each leaf's runtime data
/// via @p resolve. A split's orientation and ratio are preserved so the tree can be realized again
/// unchanged via realizeLayoutTab.
[[nodiscard]] LayoutPane serializePane(Pane const& pane, LeafResolver const& resolve);

/// Serializes @p tab (title/color/pane tree) into a LayoutTab, for save-layout use.
[[nodiscard]] LayoutTab serializeTab(Tab const& tab, LeafResolver const& resolve);

} // namespace vtmux
