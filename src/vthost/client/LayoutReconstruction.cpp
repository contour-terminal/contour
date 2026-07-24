// SPDX-License-Identifier: Apache-2.0
#include <vthost/client/LayoutReconstruction.h>

#include <cstddef>
#include <ranges>

#include <vtworkspace/LayoutConvert.h>

namespace vthost::client
{

namespace
{
    /// Adapts a daemon `WirePane` for the shared layout converter (@ref vtworkspace::convertLayoutPane).
    struct WirePaneAdapter
    {
        /// A split needs its two children. The decoder already rejects an under-populated split
        /// (decodePane cross-checks split against child count); guarding here too keeps the converter
        /// from indexing children[0]/[1] out of bounds on any WirePane not built through the decoder —
        /// a malformed split collapses to a leaf.
        [[nodiscard]] bool isSplit(proto::WirePane const& pane) const noexcept
        {
            return pane.split != 0 && pane.children.size() >= 2;
        }
        [[nodiscard]] vtworkspace::SplitState orientation(proto::WirePane const& pane) const noexcept
        {
            return static_cast<vtworkspace::SplitState>(pane.split);
        }
        /// The first child's share; the wire ratio is in units of 1/10000.
        [[nodiscard]] double firstRatio(proto::WirePane const& pane) const noexcept
        {
            return static_cast<double>(pane.ratio) / 10000.0;
        }
        [[nodiscard]] proto::WirePane const& first(proto::WirePane const& pane) const noexcept
        {
            return pane.children[0];
        }
        [[nodiscard]] proto::WirePane const& second(proto::WirePane const& pane) const noexcept
        {
            return pane.children[1];
        }
        [[nodiscard]] uint64_t leafId(proto::WirePane const& pane) const noexcept { return pane.session; }
    };
} // namespace

vtworkspace::LayoutPane wireToLayoutPane(proto::WirePane const& pane)
{
    return vtworkspace::convertLayoutPane(pane, WirePaneAdapter {});
}

WireLayout wireToLayout(proto::LayoutState const& state)
{
    auto result = WireLayout {};
    auto const adapter = WirePaneAdapter {};
    result.layout.tabs.reserve(state.tabs.size());
    for (auto const& tab: state.tabs)
        result.layout.tabs.push_back(
            vtworkspace::LayoutTab { .root = vtworkspace::convertLayoutPane(tab.root, adapter) });

    // Build the leaf → session map only now that the tree is complete AND in its final location: the
    // pane addresses are stable (further pushes would have reallocated the children vectors), and
    // NRVO / a move of the result preserves them.
    for (auto const i: std::views::iota(std::size_t { 0 }, state.tabs.size()))
        vtworkspace::mapLayoutLeaves(
            result.layout.tabs[i].root, state.tabs[i].root, adapter, result.leafSession);
    return result;
}

} // namespace vthost::client
