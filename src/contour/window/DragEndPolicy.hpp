// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/window/DropZonePolicy.hpp>

#include <algorithm>
#include <cstdint>

namespace contour::window
{

/// Logical pixels inside a window's edge within which the last sighting of a drag that then left the window
/// counts as having crossed that edge.
inline constexpr double DragExitEdgeBand = 24.0;

/// Whether the pointer is over one of Contour's windows when a drag ends: Qt reports a cancel (Escape) like a
/// drop outside every window, so only a drag that ended over none opens a window of its own.
enum class PointerOverContour : uint8_t
{
    No = 0, //!< Over no Contour window.
    Yes,    //!< Over a Contour window.
};

/// Whether, and how, a tracked drag was last seen over one of Contour's windows.
enum class DragPresence : uint8_t
{
    Unseen = 0, //!< Never seen over a window: nothing to go by.
    Over,       //!< Over a window when it ended.
    Left,       //!< It left the window it was last seen over.
};

/// The last two sightings of a drag over the window it was last seen over, in that window's coordinates.
struct DragSighting
{
    DropPoint at {};      ///< The last position.
    DropPoint before {};  ///< The position before it; the same as @c at when there was none.
    DropExtent window {}; ///< The window's size.
};

/// What is known of a drag where its windows track it, as they do on Wayland: no global pointer position
/// exists there, so where it ended is read from where it was last seen.
struct TrackedDrag
{
    DragPresence presence = DragPresence::Unseen; ///< Whether it was last over a window.
    DragSighting last {};                         ///< Where, when it was seen at all.
};

namespace detail
{
    /// @return Whether @p at lies inside @p window, edges excluded.
    [[nodiscard]] constexpr bool inside(DropPoint at, DropExtent window) noexcept
    {
        return at.x >= 0.0 && at.y >= 0.0 && at.x < window.width && at.y < window.height;
    }
} // namespace detail

/// Reads where a tracked drag ended: outside every window only when it left one across that window's edge --
/// last seen within DragExitEdgeBand of it, or moving fast enough that one more step of the same motion is
/// outside the window. Anything else -- still over a window, never seen, or gone from deep inside, as a
/// compositor ends an Escape-cancelled drag -- reads as over a window, so that a doubt opens nothing.
/// @param drag What was tracked.
/// @return No only for a drag that left across an edge.
[[nodiscard]] constexpr PointerOverContour classifyTrackedDrag(TrackedDrag const& drag) noexcept
{
    if (drag.presence != DragPresence::Left)
        return PointerOverContour::Yes;
    auto const [at, before, window] = drag.last;
    auto const nearestEdge = std::min({ at.x, at.y, window.width - at.x, window.height - at.y });
    auto const next = DropPoint { .x = at.x + (at.x - before.x), .y = at.y + (at.y - before.y) };
    return nearestEdge <= DragExitEdgeBand || !detail::inside(next, window) ? PointerOverContour::No
                                                                            : PointerOverContour::Yes;
}

} // namespace contour::window
