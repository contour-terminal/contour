// SPDX-License-Identifier: Apache-2.0
#include <vtmux/LayoutTree.h>

#include <algorithm>
#include <numeric>
#include <tuple>

#include <vtmux/Pane.h>
#include <vtmux/Tab.h>

namespace vtmux
{

LayoutPane const& leftmostLeaf(LayoutPane const& node)
{
    LayoutPane const* current = &node;
    while (!current->isLeaf())
        current = &current->children.front();
    return *current;
}

double ratioForFirst(std::span<LayoutPane const> children)
{
    if (children.empty())
        return 0.5;

    // A specified `ratio` is that child's fraction of the whole split; children without one share
    // the remaining space equally (so `ratio: 0.6` next to an unspecified sibling means 60/40).
    // MinimumShare keeps degenerate inputs (over-committed or non-positive fractions) from
    // collapsing a pane to zero; the final division re-normalizes whatever the shares add up to.
    auto constexpr MinimumShare = 0.01;
    auto const specifiedTotal =
        std::accumulate(children.begin(), children.end(), 0.0, [](double acc, auto const& child) {
            return acc + child.ratio.value_or(0.0);
        });
    auto const unspecifiedCount =
        std::ranges::count_if(children, [](auto const& child) { return !child.ratio.has_value(); });
    auto const defaultShare =
        unspecifiedCount > 0
            ? std::max((1.0 - specifiedTotal) / static_cast<double>(unspecifiedCount), MinimumShare)
            : MinimumShare;
    auto const share = [&](LayoutPane const& child) {
        return std::max(child.ratio.value_or(defaultShare), MinimumShare);
    };
    auto const total =
        std::accumulate(children.begin(), children.end(), 0.0, [&](double acc, auto const& child) {
            return acc + share(child);
        });
    return share(children.front()) / total;
}

double ratioForFirst(LayoutPane const& splitNode)
{
    return ratioForFirst(std::span<LayoutPane const> { splitNode.children });
}

namespace
{
    [[nodiscard]] bool realizePane(SessionModel& model,
                                   TabId tab,
                                   LayoutPane const& node,
                                   PaneSeeder const& seed);

    /// Realizes @p children as the siblings of one split, splitting the model's active leaf once
    /// per child. Walks a span rather than materializing a "tail group" node at each level: the
    /// tail is the rest of the SAME children vector, so no subtree is ever copied.
    /// Precondition: the model's active leaf is a freshly created leaf seeded with
    /// leftmostLeaf(children.front()).
    /// @return False if a seed failed (a backing session was refused): realization stops so no pane
    ///         is ever allocated without one.
    [[nodiscard]] bool realizeSiblings(SessionModel& model,
                                       TabId tab,
                                       SplitState orientation,
                                       std::span<LayoutPane const> children,
                                       PaneSeeder const& seed)
    {
        if (children.size() == 1)
        {
            // No sibling left to split against: the active leaf hosts this child's subtree.
            return realizePane(model, tab, children.front(), seed);
        }

        // The current active leaf will host children[0]'s subtree; remember its id to return to it.
        auto const firstLeafId = model.findTab(tab)->activePane()->id();

        auto const tail = children.subspan(1);
        // Stage the backing session for the NEW pane = leftmost leaf of the tail group. If it is
        // refused, stop BEFORE splitActivePane allocates an unbacked pane.
        if (!seed(leftmostLeaf(tail.front())))
            return false;
        model.splitActivePane(tab, orientation, ratioForFirst(children));

        // Active is now the tail's leftmost leaf: build the remaining siblings in place.
        if (!realizeSiblings(model, tab, orientation, tail, seed))
            return false;

        // Return to the first child's slot and build it.
        model.setActivePane(tab, firstLeafId);
        return realizePane(model, tab, children.front(), seed);
    }

    // Precondition: model's active leaf is a freshly created leaf seeded with leftmostLeaf(node).
    [[nodiscard]] bool realizePane(SessionModel& model,
                                   TabId tab,
                                   LayoutPane const& node,
                                   PaneSeeder const& seed)
    {
        if (node.isLeaf())
            return true; // active leaf already carries this command

        return realizeSiblings(model, tab, node.orientation, node.children, seed);
    }
} // namespace

Tab* realizeLayoutTab(SessionModel& model, WindowId window, LayoutTab const& tab, PaneSeeder const& seed)
{
    // Refuse before creating: seeding spawns a real backing session, so an unknown window must be
    // rejected BEFORE the first seed() runs — otherwise that session is orphaned when createTab
    // fails (mirrors the GUI's refuse-before-create guard).
    if (model.window(window) == nullptr)
        return nullptr;

    // Seed + create the tab's first pane (the root's leftmost leaf). A refused first seed yields no
    // tab at all (nothing was allocated to back).
    if (!seed(leftmostLeaf(tab.root)))
        return nullptr;
    auto* modelTab = model.createTab(window);
    if (modelTab == nullptr)
        return nullptr;

    if (tab.title)
        model.setTabTitle(modelTab->id(), *tab.title);
    if (tab.color)
        model.setTabColor(modelTab->id(), TabColorSource::User, *tab.color);

    // A refused seed mid-tree leaves the tab with the panes realized so far — every one backed, none
    // blank — which is a better failure than allocating panes with no session behind them.
    std::ignore = realizePane(model, modelTab->id(), tab.root, seed);
    return modelTab;
}

LayoutPane serializePane(Pane const& pane, LeafResolver const& resolve)
{
    LayoutPane out;
    if (pane.isLeaf())
    {
        auto data = resolve(pane.session());
        out.command = std::move(data.command);
        out.arguments = std::move(data.arguments);
        out.directory = std::move(data.directory);
        out.profile = std::move(data.profile);
        return out;
    }
    out.orientation = pane.splitState();
    out.children.push_back(serializePane(*pane.first(), resolve));
    out.children.push_back(serializePane(*pane.second(), resolve));
    // Preserve the split position as the first child's fraction (and its complement for the second).
    out.children.front().ratio = pane.ratio();
    out.children.back().ratio = 1.0 - pane.ratio();
    return out;
}

LayoutTab serializeTab(Tab const& tab, LeafResolver const& resolve)
{
    LayoutTab out;
    if (tab.runtimeTitle())
        out.title = *tab.runtimeTitle();
    if (auto const color = tab.color(TabColorSource::User))
        out.color = *color;
    out.root = serializePane(*tab.rootPane(), resolve);
    return out;
}

} // namespace vtmux
