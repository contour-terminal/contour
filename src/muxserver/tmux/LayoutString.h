// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// tmux layout strings: the checksummed geometry grammar `csum,WxH,X,Y{...}`
/// that control-mode clients receive in %layout-change and may send back via
/// select-layout.
///
/// Grammar and checksum are byte-verified against tmux's layout-custom.c (and
/// iTerm2's independent re-implementation). tmux containers are N-ARY while
/// vtworkspace's pane tree is BINARY: emission is always exact (nested binary output
/// is valid tmux), but ingest must accept n-ary containers — so round trips
/// compare TREES, never strings.

#include <vtpty/PageSize.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <vtworkspace/Pane.h>

namespace muxserver::tmux
{

/// One node of a parsed (n-ary) tmux layout.
struct ParsedLayout
{
    /// Container orientation, mapping to vtworkspace::SplitState on collapse.
    enum class Kind : std::uint8_t
    {
        Leaf,       ///< A pane: carries paneId.
        SideBySide, ///< `{...}`: children left to right (splits columns).
        Stacked,    ///< `[...]`: children top to bottom (splits lines).
    };

    Kind kind = Kind::Leaf;
    int width = 0;                       ///< Extent in cell columns.
    int height = 0;                      ///< Extent in cell lines.
    int x = 0;                           ///< Leftmost cell column, 0-based.
    int y = 0;                           ///< Topmost cell row, 0-based.
    std::optional<std::uint64_t> paneId; ///< Set for leaves (when the string carries ids).
    std::vector<ParsedLayout> children;  ///< Two or more for containers.

    [[nodiscard]] bool operator==(ParsedLayout const&) const = default;
};

/// tmux's rotate-add checksum over the layout body (everything after `csum,`).
/// @param body The layout string without its checksum prefix.
/// @return The 16-bit checksum.
[[nodiscard]] std::uint16_t layoutChecksum(std::string_view body) noexcept;

/// Serializes @p root projected into @p area as a checksummed tmux layout
/// string. Pane ids are the vtworkspace PaneId values.
/// @param root The pane (sub)tree occupying the whole area (Tab::layoutRoot()).
/// @param area The content grid, in cells.
/// @return The layout string, e.g. `b25f,160x50,0,0{80x50,0,0,1,79x50,81,0,2}`.
[[nodiscard]] std::string encodeLayout(vtworkspace::Pane const& root, vtpty::PageSize area);

/// Parses and validates a checksummed tmux layout string.
///
/// Validation mirrors tmux's layout_check: within a container the children's
/// split-axis extents must satisfy `sum(extent + 1) - 1 == parent extent` and
/// their cross-axis extents must equal the parent's. The pane-id backtracking
/// quirk is honoured: after `W x H , X , Y`, a `,N` is a pane id only if @c N
/// is not followed by `x` (else it starts the next sibling's geometry).
/// @param text The layout string including its checksum prefix.
/// @return The parsed n-ary tree, or a diagnostic message.
[[nodiscard]] std::expected<ParsedLayout, std::string> parseLayout(std::string_view text);

/// A binary split tree recovered from a parsed layout, ready to apply onto a
/// vtworkspace pane tree: n-ary containers collapse into right-leaning chains, with
/// each split's ratio derived from the children's cell extents.
struct BinaryLayout
{
    std::optional<std::uint64_t> paneId;                     ///< Set for leaves.
    vtworkspace::SplitState orientation = vtworkspace::SplitState::None; ///< None for leaves.
    double ratio = 0.5;                                      ///< First child's share (splits only).
    std::unique_ptr<BinaryLayout> first {};                  ///< Splits only.
    std::unique_ptr<BinaryLayout> second {};                 ///< Splits only.

    /// @return The number of leaves in this subtree.
    [[nodiscard]] int leafCount() const noexcept;
};

/// Collapses a parsed n-ary layout into a binary split chain (right-leaning:
/// `{a,b,c}` becomes `a | (b | c)`), deriving ratios from cell extents.
/// @param layout The parsed layout to collapse.
/// @return The equivalent binary tree.
[[nodiscard]] BinaryLayout collapseToBinary(ParsedLayout const& layout);

} // namespace muxserver::tmux
