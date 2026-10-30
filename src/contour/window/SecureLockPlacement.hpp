// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Where the in-pane lock pill goes: the last place the secure-input lock can be drawn, for a pane that has
/// no header, tab well or band on screen to hold it (window::SecureIndicatorRules). Qt-free.

#include <contour/window/LinkPillPlacement.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace contour::window
{

/// Where the lock pill goes.
enum class SecureLockAnchor : uint8_t
{
    Hidden = 0,   ///< Nowhere: the pane is too small for the pill at all.
    TopRight,     ///< The grid's top right, inset, over blank cells.
    BottomRight,  ///< The grid's bottom right, inset, over blank cells.
    TopRightOver, ///< The last resort: the top right over whatever is there, cells and other pills included.
};

/// How many SecureLockAnchor values there are, derived from the last one.
inline constexpr size_t SecureLockAnchorCount = static_cast<size_t>(SecureLockAnchor::TopRightOver) + 1;

/// Everything the lock pill's place depends on.
struct SecureLockInputs
{
    FindBarPresence findBar = FindBarPresence::Closed; ///< Which places the find bar holds.
    LinkPillAnchor linkPill = LinkPillAnchor::Hidden; ///< Where the link pill is now; Hidden while it is not.
    double paneWidth = 0;                             ///< The pane's width.
    double paneHeight = 0;                            ///< The pane's height.
    double lockWidth = 0;                             ///< The pill's width.
    double lockHeight = 0;                            ///< The pill's height.
    double insetTop = 0;                              ///< Its gap to the pane's top and bottom.
    double insetRight = 0;                            ///< Its gap to the pane's right edge.
    LinkPillGrid grid {};                             ///< The cells it may cover.
};

/// Where the lock pill goes.
struct SecureLockPlacement
{
    SecureLockAnchor anchor = SecureLockAnchor::Hidden; ///< Where.
    double x = 0;                                       ///< Its left edge within the pane.
    double y = 0;                                       ///< Its top edge within the pane.

    [[nodiscard]] bool operator==(SecureLockPlacement const&) const = default;
};

namespace detail
{
    /// Whether a spot needs the cells under the pill to be blank.
    enum class SpotCells : uint8_t
    {
        Blank = 0, ///< Only where every cell under it is blank and none is on an avoided row.
        Any,       ///< Whatever is there.
    };

    /// Whether a spot gives way to the find bar and the link pill that hold it.
    enum class SpotYields : uint8_t
    {
        Yes = 0, ///< It is skipped while @ref SecureLockSpot::heldByFindBar or heldByLinkPill is current.
        No,      ///< It is taken whatever else is there.
    };

    /// The pane edge a spot sits on.
    enum class SpotEdge : uint8_t
    {
        Bottom = 0, ///< The pane's bottom.
        Top,        ///< The pane's top.
    };

    /// One place the lock may go, and what holds it.
    struct SecureLockSpot
    {
        SecureLockAnchor anchor;       ///< The place.
        FindBarPresence heldByFindBar; ///< The find bar presence that holds it.
        LinkPillAnchor heldByLinkPill; ///< The link pill anchor that holds it.
        SpotCells cells;               ///< What it needs of the cells under it.
        SpotYields yields;             ///< Whether the holders above keep it from the lock.
        SpotEdge edge;                 ///< The pane edge it sits on.
    };

    /// The places the lock may go, in order of preference.
    inline constexpr auto SecureLockSpots = std::array {
        SecureLockSpot { SecureLockAnchor::TopRight,
                         FindBarPresence::Top,
                         LinkPillAnchor::TopRight,
                         SpotCells::Blank,
                         SpotYields::Yes,
                         SpotEdge::Top },
        SecureLockSpot { SecureLockAnchor::BottomRight,
                         FindBarPresence::Bottom,
                         LinkPillAnchor::BottomRight,
                         SpotCells::Blank,
                         SpotYields::Yes,
                         SpotEdge::Bottom },
        // The lock must never be invisible while secure input is on: with no blank place free (a
        // full-screen program, the find bar and the link pill holding both) it covers a few cells.
        SecureLockSpot { SecureLockAnchor::TopRightOver,
                         FindBarPresence::Closed,
                         LinkPillAnchor::Hidden,
                         SpotCells::Any,
                         SpotYields::No,
                         SpotEdge::Top },
    };

    /// The anchors' QML names, indexed by SecureLockAnchor.
    inline constexpr auto SecureLockAnchorNames = std::array<std::string_view, SecureLockAnchorCount> {
        "hidden",
        "topRight",
        "bottomRight",
        "topRightOver",
    };
} // namespace detail

/// The QML name of @p anchor.
/// @param anchor The anchor.
/// @return `"hidden"`, `"topRight"`, `"bottomRight"` or `"topRightOver"`.
[[nodiscard]] constexpr std::string_view secureLockAnchorName(SecureLockAnchor anchor) noexcept
{
    return detail::SecureLockAnchorNames[static_cast<size_t>(anchor)];
}

/// Where the lock pill goes, so that it only ever covers blank cells and never the prompt, like the link
/// pill.
///
/// At the grid's top right when every cell under it is blank, none is on an avoided row, and neither the find
/// bar nor the link pill holds the top; else at the bottom right under the same test; else, as a last
/// resort, at the top right over whatever is there. The lock yields to both pills while it has a blank place,
/// so it covers something only when it has none: it is never invisible.
/// @param in What the place depends on.
/// @return The place; SecureLockAnchor::Hidden only where the pane is too small for the pill at all.
[[nodiscard]] inline SecureLockPlacement placeSecureLock(SecureLockInputs const& in) noexcept
{
    auto const width = in.lockWidth;
    if (width <= 0 || in.lockHeight <= 0 || in.paneWidth - (2 * in.insetRight) < width
        || in.grid.cellWidth <= 0 || in.grid.cellHeight <= 0)
        return {};
    auto const x = in.paneWidth - in.insetRight - width;
    auto const firstColumn =
        std::max(0, static_cast<int>(std::floor((x - in.grid.marginLeft) / in.grid.cellWidth)));
    for (auto const& spot: detail::SecureLockSpots)
    {
        if (spot.yields == detail::SpotYields::Yes
            && (in.findBar == spot.heldByFindBar || in.linkPill == spot.heldByLinkPill))
            continue;
        auto const y =
            spot.edge == detail::SpotEdge::Top ? in.insetTop : in.paneHeight - in.insetTop - in.lockHeight;
        if (spot.cells == detail::SpotCells::Blank
            && (y < in.insetTop || !detail::isBandClear(in.grid, y, in.lockHeight, firstColumn)))
            continue;
        return { .anchor = spot.anchor, .x = x, .y = y };
    }
    return {};
}

} // namespace contour::window
