// SPDX-License-Identifier: Apache-2.0
#include <vthost/TabMerge.hpp>

#include <algorithm>
#include <array>

#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

namespace vthost
{

namespace
{
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
    auto const [fromWindow, source, sourceLeaf] =
        model.findSessionLeaf(vtworkspace::SessionId { merge.tabSession });
    auto const [toWindow, target, targetLeaf] =
        model.findSessionLeaf(vtworkspace::SessionId { merge.targetSession });
    if (fromWindow == nullptr || source == nullptr || toWindow == nullptr || target == nullptr
        || targetLeaf == nullptr)
        return std::unexpected { TabMergeError::UnknownSession };
    if (model.mergeTabIntoSplit(
            fromWindow->id(), source->id(), toWindow->id(), target->id(), targetLeaf->id(), *side)
        == nullptr)
        return std::unexpected { TabMergeError::Refused };
    return {};
}

} // namespace vthost
