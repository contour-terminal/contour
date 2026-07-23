// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Generic binary-layout-tree conversion into a `vtmux::LayoutPane` tree, single-sourced so the
/// native-attach (`muxserver::client`) and tmux-mirror (`contour`) paths share ONE implementation of
/// the "first child carries the ratio; the second is left unset so `ratioForFirst` gives it the rest"
/// convention plus the lockstep leaf→id map. Each caller supplies a small adapter exposing its own
/// node type's accessors, so a change to the convention (or the map) is made in one place instead of
/// diverging between the two clones.

#include <cstdint>
#include <unordered_map>
#include <utility>

#include <vtmux/LayoutTree.h>

namespace vtmux
{

/// Converts one node of an arbitrary binary layout tree into a `LayoutPane` subtree. A leaf (the
/// adapter reports `!isSplit`) becomes an empty, command-less pane; a split records its orientation
/// and gives the FIRST child the node's ratio (the second stays unset, so `ratioForFirst` gives it
/// the rest).
/// @tparam Node The caller's tree node type.
/// @tparam Adapter Exposes `isSplit(Node)->bool`, `orientation(Node)->SplitState`,
///         `firstRatio(Node)->double`, `first(Node)->Node const&`, `second(Node)->Node const&`.
/// @param node The subtree root.
/// @param adapter The node accessors.
/// @return The converted pane subtree.
template <typename Node, typename Adapter>
[[nodiscard]] LayoutPane convertLayoutPane(Node const& node, Adapter const& adapter)
{
    auto out = LayoutPane {};
    if (!adapter.isSplit(node))
        return out; // a leaf: a backing session fills it
    out.orientation = adapter.orientation(node);
    auto first = convertLayoutPane(adapter.first(node), adapter);
    first.ratio = adapter.firstRatio(node);
    out.children.push_back(std::move(first));
    out.children.push_back(convertLayoutPane(adapter.second(node), adapter));
    return out;
}

/// Lockstep-walks @p pane (a tree built by @ref convertLayoutPane) and its source @p node, recording
/// each converted leaf's id — `adapter.leafId(Node)` — keyed by the leaf's (now stable) address. Build
/// the map only once @p pane sits in its FINAL storage, so the recorded addresses do not later move.
/// @tparam Node The caller's tree node type.
/// @tparam Adapter Additionally exposes `leafId(Node)->uint64_t` (plus the accessors convertLayoutPane
///         needs, since a non-leaf recurses into `first`/`second`).
/// @param pane The converted pane subtree.
/// @param node The source node it was converted from.
/// @param adapter The node accessors.
/// @param out Receives leaf-address → id entries.
template <typename Node, typename Adapter>
void mapLayoutLeaves(LayoutPane const& pane,
                     Node const& node,
                     Adapter const& adapter,
                     std::unordered_map<LayoutPane const*, uint64_t>& out)
{
    if (pane.isLeaf())
    {
        out.emplace(&pane, adapter.leafId(node));
        return;
    }
    // A converted non-leaf only exists where adapter.isSplit(node) held, so first()/second() are valid.
    mapLayoutLeaves(pane.children[0], adapter.first(node), adapter, out);
    mapLayoutLeaves(pane.children[1], adapter.second(node), adapter, out);
}

} // namespace vtmux
