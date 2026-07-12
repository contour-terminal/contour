// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/Config.h>

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <vtmux/SessionModel.h>

namespace contour
{

/// Descends first-children to the leftmost leaf of @p node (returns @p node itself if it is a leaf).
[[nodiscard]] config::LayoutPane const& leftmostLeaf(config::LayoutPane const& node);

/// The share of space in (0, 1) that @p children.front() receives when @p children are laid out
/// side by side in one split. A child's `ratio` is its fraction of the whole split; children
/// without one share what the explicitly-sized ones leave over, equally.
/// @param children The children of one split (or the not-yet-placed tail of one).
/// @return The first child's share, or 0.5 for an empty range.
[[nodiscard]] double ratioForFirst(std::span<config::LayoutPane const> children);

/// The share given to @p splitNode's first child versus its remaining children.
/// @param splitNode A split node (a leaf yields 0.5: it has no children to weigh).
[[nodiscard]] double ratioForFirst(config::LayoutPane const& splitNode);

/// Invoked immediately before each model allocation to stage the backing session for that leaf's
/// command/profile/dir. It must arrange for the model's SessionAllocator to return the id it created.
using PaneSeeder = std::function<void(config::LayoutPane const& leaf)>;

/// Creates a tab in @p window from @p tab: seeds+creates the first pane, applies title/color, then
/// builds the pane tree. @p seed is invoked immediately before each model allocation to stage that
/// pane's backing session (it must make the model's allocator return the id it created).
vtmux::Tab* realizeLayoutTab(vtmux::SessionModel& model,
                             vtmux::WindowId window,
                             config::LayoutTab const& tab,
                             PaneSeeder const& seed);

/// Renders a full `layouts:` YAML document (the exact text later written to `layouts.yml`) from
/// @p layouts, via yaml-cpp's YAML::Emitter — so quoting/escaping of names, commands, arguments and
/// paths is the emitter's job, not ours. Meant to be parsed back by the same config reader that
/// reads `layouts:` from `contour.yml`; layout names are emitted in sorted order so repeated saves
/// produce stable, diff-friendly output.
/// @param layouts The layouts to persist, keyed by name.
/// @return The complete YAML document text.
[[nodiscard]] std::string emitLayoutsYaml(std::unordered_map<std::string, config::Layout> const& layouts);

/// A leaf pane's resolved runtime data, as reported by a LeafResolver for SaveLayout's use.
struct PaneLeafData
{
    std::optional<std::string> command;
    std::vector<std::string> arguments;
    std::optional<std::string> directory;
    std::optional<std::string> profile;
};

/// Resolves a leaf pane's backing session id to its runtime command/arguments/directory, for
/// serializing a live pane tree back into a config::LayoutPane (the inverse of PaneSeeder).
using LeafResolver = std::function<PaneLeafData(vtmux::SessionId)>;

/// Serializes @p pane (leaf or split) into a config::LayoutPane, resolving each leaf's runtime data
/// via @p resolve. A split's orientation and ratio are preserved so the tree can be realized again
/// unchanged via realizeLayoutTab.
[[nodiscard]] config::LayoutPane serializePane(vtmux::Pane const& pane, LeafResolver const& resolve);

/// Serializes @p tab (title/color/pane tree) into a config::LayoutTab, for SaveLayout.
[[nodiscard]] config::LayoutTab serializeTab(vtmux::Tab const& tab, LeafResolver const& resolve);

} // namespace contour
