// SPDX-License-Identifier: Apache-2.0
#include <vthost/TabMerge.hpp>

#include <algorithm>
#include <array>
#include <vector>

#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

namespace vthost
{

namespace
{
    /// @return The session of every leaf of @p tab.
    std::vector<uint64_t> sessionsOf(vtworkspace::Tab& tab)
    {
        auto sessions = std::vector<uint64_t> {};
        tab.rootPane()->walkTree([&](vtworkspace::Pane& pane) {
            if (pane.isLeaf())
                sessions.push_back(pane.session().value);
        });
        return sessions;
    }

    /// Every side the wire may name.
    constexpr auto KnownSides = std::array { vtworkspace::SplitSide::Left,
                                             vtworkspace::SplitSide::Right,
                                             vtworkspace::SplitSide::Top,
                                             vtworkspace::SplitSide::Bottom };
} // namespace

std::optional<vtworkspace::SplitSide> splitSideFrom(uint8_t side) noexcept
{
    auto const known = std::ranges::find_if(
        KnownSides, [side](auto candidate) { return std::to_underlying(candidate) == side; });
    return known != KnownSides.end() ? std::optional { *known } : std::nullopt;
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
    if (!std::ranges::is_permutation(sessionsOf(*source), merge.tabSessions))
        return std::unexpected { TabMergeError::StaleTab };
    if (model.mergeTabIntoSplit(
            fromWindow->id(), source->id(), toWindow->id(), target->id(), targetLeaf->id(), *side)
        == nullptr)
        return std::unexpected { TabMergeError::Refused };
    return {};
}

} // namespace vthost
