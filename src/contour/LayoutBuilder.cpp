// SPDX-License-Identifier: Apache-2.0
#include <contour/LayoutBuilder.h>

#include <numeric>

#include <vtmux/Pane.h>
#include <vtmux/Tab.h>

namespace contour
{

config::LayoutPane const& leftmostLeaf(config::LayoutPane const& node)
{
    config::LayoutPane const* current = &node;
    while (!current->isLeaf())
        current = &current->children.front();
    return *current;
}

double ratioForFirst(config::LayoutPane const& splitNode)
{
    if (splitNode.isLeaf() || splitNode.children.empty())
        return 0.5;
    double const total = std::accumulate(
        splitNode.children.begin(), splitNode.children.end(), 0.0, [](double acc, auto const& child) {
            return acc + (child.ratio > 0.0 ? child.ratio : 1.0);
        });
    double const first = splitNode.children.front().ratio > 0.0 ? splitNode.children.front().ratio : 1.0;
    if (total <= 0.0)
        return 0.5;
    return first / total;
}

config::LayoutPane tailGroup(config::LayoutPane const& splitNode)
{
    // children[1..] as a group. With exactly one remaining child, return it directly.
    if (splitNode.children.size() == 2)
        return splitNode.children[1];
    config::LayoutPane group;
    group.orientation = splitNode.orientation;
    group.children.assign(splitNode.children.begin() + 1, splitNode.children.end());
    return group;
}

namespace
{
    // Precondition: model's active leaf is a freshly created leaf seeded with leftmostLeaf(node).
    void realizePane(vtmux::SessionModel& model,
                     vtmux::TabId tab,
                     config::LayoutPane const& node,
                     PaneSeeder const& seed)
    {
        if (node.isLeaf())
            return; // active leaf already carries this command

        // The current active leaf will host children[0]'s subtree; remember its id to return to it.
        auto* active = model.findTab(tab)->activePane();
        auto const firstLeafId = active->id();

        auto const tail = tailGroup(node);
        // Stage the backing session for the NEW pane = leftmost leaf of the tail group.
        seed(leftmostLeaf(tail));
        model.splitActivePane(tab, node.orientation, ratioForFirst(node));

        // Active is now the tail's leftmost leaf: build the tail subtree in place.
        realizePane(model, tab, tail, seed);

        // Return to the first child's slot and build it.
        model.setActivePane(tab, firstLeafId);
        realizePane(model, tab, node.children.front(), seed);
    }
} // namespace

vtmux::Tab* realizeLayoutTab(vtmux::SessionModel& model,
                             vtmux::WindowId window,
                             config::LayoutTab const& tab,
                             PaneSeeder const& seed)
{
    // Seed + create the tab's first pane (the root's leftmost leaf).
    seed(leftmostLeaf(tab.root));
    auto* modelTab = model.createTab(window);
    if (modelTab == nullptr)
        return nullptr;

    if (tab.title)
        model.setTabTitle(modelTab->id(), *tab.title);
    if (tab.color)
        model.setTabColor(modelTab->id(), vtmux::TabColorSource::User, *tab.color);

    realizePane(model, modelTab->id(), tab.root, seed);
    return modelTab;
}

} // namespace contour
