// SPDX-License-Identifier: Apache-2.0
#include <contour/LayoutBuilder.h>

#include <numeric>

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

} // namespace contour
