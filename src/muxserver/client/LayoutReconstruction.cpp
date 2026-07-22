// SPDX-License-Identifier: Apache-2.0
#include <muxserver/client/LayoutReconstruction.h>

namespace muxserver::client
{

namespace
{
    /// The leftmost leaf's session of a subtree — the pane that seeded it (the
    /// daemon's split invariant keeps the original session in the first child).
    [[nodiscard]] uint64_t leftmostSession(proto::WirePane const& pane)
    {
        auto const* node = &pane;
        while (node->split != 0 && !node->children.empty())
            node = &node->children.front();
        return node->session;
    }

    void reconstructPane(proto::WirePane const& node, std::vector<ReconstructStep>& steps)
    {
        // A leaf is already placed by the enclosing NewTab/Split; only a split node
        // (exactly two children) produces further work.
        if (node.split == 0 || node.children.size() < 2)
            return;

        auto const& first = node.children[0];
        auto const& second = node.children[1];

        // The active pane is this node's leftmost leaf (== first's leftmost). Split
        // it: the first child keeps its session, the second child takes the right
        // subtree's leftmost session and becomes active.
        steps.push_back(ReconstructStep { .kind = ReconstructStep::Kind::Split,
                                          .session = leftmostSession(second),
                                          .orientation = node.split,
                                          .ratio = node.ratio });
        reconstructPane(second, steps); // build the right subtree (already active)

        // The left child needs building only if it is itself a split: re-activate
        // its leftmost leaf, then recurse. A left leaf is already in place (the
        // split preserved its session in the first child), so no step is needed.
        if (first.split != 0)
        {
            steps.push_back(ReconstructStep { .kind = ReconstructStep::Kind::Activate,
                                              .session = leftmostSession(first) });
            reconstructPane(first, steps);
        }
    }
} // namespace

std::vector<ReconstructStep> planReconstruction(proto::LayoutState const& layout)
{
    auto steps = std::vector<ReconstructStep> {};
    for (auto const& tab: layout.tabs)
    {
        steps.push_back(
            ReconstructStep { .kind = ReconstructStep::Kind::NewTab, .session = leftmostSession(tab.root) });
        reconstructPane(tab.root, steps);
    }
    return steps;
}

} // namespace muxserver::client
