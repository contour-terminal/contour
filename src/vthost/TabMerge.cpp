// SPDX-License-Identifier: Apache-2.0
#include <vthost/TabMerge.hpp>

#include <algorithm>
#include <utility>
#include <vector>

#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

namespace vthost
{

bool namesWholeTab(vtworkspace::Tab const& tab, std::span<uint64_t const> sessions)
{
    return std::ranges::is_permutation(
        tab.leafSessions(), sessions, {}, [](vtworkspace::SessionId session) { return session.value; });
}

std::optional<vtworkspace::SplitSide> splitSideFrom(uint8_t side) noexcept
{
    auto const known = std::ranges::find_if(
        vtworkspace::AllSplitSides, [side](auto candidate) { return std::to_underlying(candidate) == side; });
    return known != vtworkspace::AllSplitSides.end() ? std::optional { *known } : std::nullopt;
}

std::expected<void, TabMergeError> applyTabMerge(vtworkspace::SessionModel& model,
                                                 proto::MergeTabIntoSplit const& merge)
{
    auto const side = splitSideFrom(merge.side);
    if (!side)
        return std::unexpected { TabMergeError::UnknownSide };
    auto const [fromWindow, source, sourceLeaf] = model.findSessionLeaf(
        vtworkspace::SessionId { merge.tabSessions.empty() ? 0 : merge.tabSessions.front() });
    auto const [toWindow, target, targetLeaf] =
        model.findSessionLeaf(vtworkspace::SessionId { merge.targetSession });
    if (fromWindow == nullptr || source == nullptr || toWindow == nullptr || target == nullptr
        || targetLeaf == nullptr)
        return std::unexpected { TabMergeError::UnknownSession };
    // The merge stays within one window until the clients' reconcilers graft across two.
    if (fromWindow != toWindow)
        return std::unexpected { TabMergeError::CrossWindow };
    // The request names the whole tab it saw. Another client (or this one, dragging twice before a push
    // arrived) may have merged more into it since: moving that would drag panes nobody dragged.
    if (!namesWholeTab(*source, merge.tabSessions))
        return std::unexpected { TabMergeError::StaleTab };
    if (model.mergeTabIntoSplit(
            fromWindow->id(), source->id(), toWindow->id(), target->id(), targetLeaf->id(), *side)
        == nullptr)
        return std::unexpected { TabMergeError::Refused };
    return {};
}

} // namespace vthost
