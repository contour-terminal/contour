// SPDX-License-Identifier: Apache-2.0
#include <muxserver/client/LayoutReconstruction.h>

#include <cstddef>
#include <ranges>
#include <utility>

namespace muxserver::client
{

vtmux::LayoutPane wireToLayoutPane(proto::WirePane const& pane)
{
    auto out = vtmux::LayoutPane {};
    if (pane.split == 0)
        return out; // a leaf: no children, no command — a remote session backs it

    out.orientation = static_cast<vtmux::SplitState>(pane.split);
    // A binary wire split: the first child takes the wire ratio (its share of the
    // whole split); the second is left unset, so ratioForFirst gives it the rest.
    auto first = wireToLayoutPane(pane.children[0]);
    first.ratio = static_cast<double>(pane.ratio) / 10000.0;
    out.children.push_back(std::move(first));
    out.children.push_back(wireToLayoutPane(pane.children[1]));
    return out;
}

namespace
{
    /// Lockstep-walks the converted pane tree and the wire tree, recording each
    /// converted leaf's remote session by the leaf's (now stable) address.
    void mapLeaves(vtmux::LayoutPane const& pane,
                   proto::WirePane const& wire,
                   std::unordered_map<vtmux::LayoutPane const*, uint64_t>& out)
    {
        if (pane.isLeaf())
        {
            out.emplace(&pane, wire.session);
            return;
        }
        mapLeaves(pane.children[0], wire.children[0], out);
        mapLeaves(pane.children[1], wire.children[1], out);
    }
} // namespace

WireLayout wireToLayout(proto::LayoutState const& state)
{
    auto result = WireLayout {};
    result.layout.tabs.reserve(state.tabs.size());
    for (auto const& tab: state.tabs)
        result.layout.tabs.push_back(vtmux::LayoutTab { .root = wireToLayoutPane(tab.root) });

    // Build the leaf → session map only now that the tree is complete: the pane
    // addresses are stable (further pushes would have reallocated the children
    // vectors), and NRVO / a move of the result preserves them.
    for (auto const i: std::views::iota(std::size_t { 0 }, state.tabs.size()))
        mapLeaves(result.layout.tabs[i].root, state.tabs[i].root, result.leafSession);
    return result;
}

} // namespace muxserver::client
