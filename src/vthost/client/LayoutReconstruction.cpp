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

    /// @return The session of the leaf @p paneId names in @p node's tree, or 0 when no leaf there has it.
    [[nodiscard]] uint64_t leafSessionOf(proto::WirePane const& node, uint64_t paneId) noexcept
    {
        if (!node.isSplit())
            return node.paneId == paneId ? node.session : 0;
        if (auto const first = leafSessionOf(node.children[0], paneId); first != 0)
            return first;
        return leafSessionOf(node.children[1], paneId);
    }
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

TabPresentation presentationOf(proto::WireTab const& tab, TabActivity activity)
{
    return TabPresentation {
        .activeSession = tab.activePane != 0 ? leafSessionOf(tab.root, tab.activePane) : 0,
        .zoomedSession = tab.zoomedPane != 0 ? leafSessionOf(tab.root, tab.zoomedPane) : 0,
        .title = tab.title.empty() ? std::nullopt : std::optional { tab.title },
        .color = tab.hasColor != 0 ? std::optional { tab.color } : std::nullopt,
        .activity = activity,
    };
}

std::vector<proto::EditTab> tabEditsToReport(TabPresentation const& local,
                                             TabPresentation const& lastFromDaemon)
{
    using proto::TabEditKind;
    auto edits = std::vector<proto::EditTab> {};
    auto const tabWide = [&](TabEditKind kind) {
        edits.push_back(proto::EditTab { .session = local.activeSession, .kind = kind });
    };
    if (local.activity == TabActivity::Active && lastFromDaemon.activity == TabActivity::Inactive)
        tabWide(TabEditKind::ActivateTab);
    if (local.activeSession != lastFromDaemon.activeSession)
        tabWide(TabEditKind::ActivatePane);
    if (local.zoomedSession != lastFromDaemon.zoomedSession)
    {
        if (local.zoomedSession != 0)
            edits.push_back(proto::EditTab { .session = local.zoomedSession, .kind = TabEditKind::Zoom });
        else
            tabWide(TabEditKind::Unzoom);
    }
    if (local.title != lastFromDaemon.title)
    {
        if (local.title)
            edits.push_back(proto::EditTab {
                .session = local.activeSession, .kind = TabEditKind::SetTitle, .title = *local.title });
        else
            tabWide(TabEditKind::ClearTitle);
    }
    if (local.color != lastFromDaemon.color)
    {
        if (local.color)
            edits.push_back(proto::EditTab {
                .session = local.activeSession, .kind = TabEditKind::SetColor, .color = *local.color });
        else
            tabWide(TabEditKind::ClearColor);
    }
    return edits;
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
