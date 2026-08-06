// SPDX-License-Identifier: Apache-2.0
#include <vthost/client/LayoutReconstruction.hpp>

#include <algorithm>
#include <cstddef>
#include <ranges>

#include <vtworkspace/LayoutConvert.hpp>
#include <vtworkspace/PaneLayout.hpp>

namespace vthost::client
{

namespace
{
    /// Adapts a daemon `WirePane` for the shared layout converter (@ref vtworkspace::convertLayoutPane).
    struct WirePaneAdapter
    {
        [[nodiscard]] bool isSplit(proto::WirePane const& pane) const noexcept { return pane.isSplit(); }
        [[nodiscard]] vtworkspace::SplitState orientation(proto::WirePane const& pane) const noexcept
        {
            return static_cast<vtworkspace::SplitState>(pane.split);
        }
        /// The first child's share.
        [[nodiscard]] double firstRatio(proto::WirePane const& pane) const noexcept
        {
            return proto::fromWireRatio(pane.ratio);
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

bool paneTreeHosts(proto::WirePane const& root, uint64_t session) noexcept
{
    auto const adapter = WirePaneAdapter {};
    if (!adapter.isSplit(root))
        return root.session == session;
    return paneTreeHosts(adapter.first(root), session) || paneTreeHosts(adapter.second(root), session);
}

std::optional<vtpty::PageSize> composeClientArea(proto::WirePane const& root, LeafCellSize const& leafSize)
{
    auto const adapter = WirePaneAdapter {};
    auto const compose = [&](auto const& self,
                             proto::WirePane const& node) -> std::optional<vtpty::PageSize> {
        if (!adapter.isSplit(node))
            return leafSize(node.session);

        auto const first = self(self, adapter.first(node));
        auto const second = self(self, adapter.second(node));
        if (!first || !second)
            return std::nullopt;

        if (adapter.orientation(node) == vtworkspace::SplitState::Vertical)
            return vtpty::PageSize { .lines = std::max(first->lines, second->lines),
                                     .columns = first->columns
                                                + vtpty::ColumnCount(vtworkspace::CellDividerThickness)
                                                + second->columns };
        return vtpty::PageSize { .lines = first->lines + vtpty::LineCount(vtworkspace::CellDividerThickness)
                                          + second->lines,
                                 .columns = std::max(first->columns, second->columns) };
    };
    return compose(compose, root);
}

} // namespace vthost::client
