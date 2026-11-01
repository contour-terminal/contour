// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/window/DropZonePolicy.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace contour::window
{

/// How long a drag must rest on another tab before that tab opens under it.
inline constexpr auto TabDwell = std::chrono::milliseconds { 600 };

/// Logical pixels each side of a tab boundary that count as the gap between the tabs.
inline constexpr double TabGapHalfWidth = 10.0;

/// A tab's horizontal extent in the tab strip, in logical pixels.
struct TabSpan
{
    double left = 0.0;  ///< Its left edge.
    double right = 0.0; ///< Its right edge.
};

/// What a pointer over the tab strip is on.
enum class TabBarSpot : uint8_t
{
    None = 0, //!< Nothing.
    OnTab,    //!< Inside a tab.
    Gap,      //!< In the gap between two tabs, or before the first or after the last.
};

/// The result of hit-testing the tab strip.
struct TabBarHit
{
    TabBarSpot spot = TabBarSpot::None; ///< What the pointer is on.
    int index = 0;                      ///< OnTab: the tab's index; Gap: the insertion index.
};

/// How far each side of @p tab's boundaries counts as a gap: TabGapHalfWidth, but never more than a quarter
/// of the tab, so that a narrow tab keeps a middle to be dropped on.
/// @param tab The tab's extent.
/// @return The half-width of the gaps at its edges.
[[nodiscard]] constexpr double tabGapHalfWidthOf(TabSpan tab) noexcept
{
    return std::min(TabGapHalfWidth, (tab.right - tab.left) / 4.0);
}

/// Hit-tests the tab strip: within tabGapHalfWidthOf() a boundary is a gap, inside a tab otherwise.
/// @param tabs The tabs' extents, left to right.
/// @param x The pointer's position along the strip.
/// @return The hit; a gap at 0 for an empty strip, and a gap at @c tabs.size() beyond the last tab.
[[nodiscard]] constexpr TabBarHit resolveTabBarHit(std::span<TabSpan const> tabs, double x) noexcept
{
    if (tabs.empty())
        return { .spot = TabBarSpot::Gap, .index = 0 };

    auto index = 0;
    for (auto const& tab: tabs)
    {
        auto const halfGap = tabGapHalfWidthOf(tab);
        if (x < tab.left + halfGap)
            return { .spot = TabBarSpot::Gap, .index = index };
        if (x <= tab.right - halfGap)
            return { .spot = TabBarSpot::OnTab, .index = index };
        ++index;
        // Past this tab's right gap edge: the next tab's left edge (if it abuts) is covered by the
        // next iteration, so a pointer within the right half-gap belongs to the boundary after it.
        if (x < tab.right + halfGap)
            return { .spot = TabBarSpot::Gap, .index = index };
    }
    return { .spot = TabBarSpot::Gap, .index = index };
}

/// What a drag hovers a tab as.
enum class DwellSubject : uint8_t
{
    OtherTab = 0, //!< A tab other than the dragged one and the one it came from.
    DraggedTab,   //!< The tab being dragged.
    SourceTab,    //!< The tab the drag came from.
};

/// Whether dwelling opens the hovered tab.
enum class DwellOpens : uint8_t
{
    No = 0, //!< Leave the active tab alone.
    Yes,    //!< Open the hovered tab.
};

/// Decides whether resting on a tab springs it open.
/// @param subject Which tab is hovered.
/// @param refusal Why the drag could not be moved into this window, if it cannot.
/// @return Yes only for another tab when nothing refuses the move.
[[nodiscard]] constexpr DwellOpens shouldSpringOpen(DwellSubject subject, MoveRefusal refusal) noexcept
{
    return subject == DwellSubject::OtherTab && refusal == MoveRefusal::None ? DwellOpens::Yes
                                                                             : DwellOpens::No;
}

} // namespace contour::window
