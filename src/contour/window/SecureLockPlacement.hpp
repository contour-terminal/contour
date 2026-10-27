// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Where the in-pane lock pill goes: the last place the secure-input lock can be drawn, for the one tab of
/// one pane that has no header, tab well or band to hold it (window::SecureIndicatorRules). Qt-free.

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
    Hidden = 0,  ///< Nowhere: no blank place is big enough, or the other pills hold them.
    TopRight,    ///< The grid's top right, inset.
    BottomRight, ///< The grid's bottom right, inset.
};

/// How many SecureLockAnchor values there are, derived from the last one.
inline constexpr size_t SecureLockAnchorCount = static_cast<size_t>(SecureLockAnchor::BottomRight) + 1;

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
    /// One place the lock may go, and what holds it.
    struct SecureLockSpot
    {
        SecureLockAnchor anchor;       ///< The place.
        FindBarPresence heldByFindBar; ///< The find bar presence that holds it.
        LinkPillAnchor heldByLinkPill; ///< The link pill anchor that holds it.
    };

    /// The places the lock may go, in order of preference.
    inline constexpr auto SecureLockSpots = std::array {
        SecureLockSpot { SecureLockAnchor::TopRight, FindBarPresence::Top, LinkPillAnchor::TopRight },
        SecureLockSpot {
            SecureLockAnchor::BottomRight, FindBarPresence::Bottom, LinkPillAnchor::BottomRight },
    };

    /// The anchors' QML names, indexed by SecureLockAnchor.
    inline constexpr auto SecureLockAnchorNames = std::array<std::string_view, SecureLockAnchorCount> {
        "hidden",
        "topRight",
        "bottomRight",
    };
} // namespace detail

/// The QML name of @p anchor.
/// @param anchor The anchor.
/// @return `"hidden"`, `"topRight"` or `"bottomRight"`.
[[nodiscard]] constexpr std::string_view secureLockAnchorName(SecureLockAnchor anchor) noexcept
{
    return detail::SecureLockAnchorNames[static_cast<size_t>(anchor)];
}

/// Where the lock pill goes, so that it only ever covers blank cells and never the prompt, like the link
/// pill.
///
/// At the grid's top right when every cell under it is blank, none is on an avoided row, and neither the find
/// bar nor the link pill holds the top; else at the bottom right under the same test; else nowhere. The lock
/// yields to both, so the three never overlap.
/// @param in What the place depends on.
/// @return The place; SecureLockAnchor::Hidden where there is none.
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
        if (in.findBar == spot.heldByFindBar || in.linkPill == spot.heldByLinkPill)
            continue;
        auto const y = spot.anchor == SecureLockAnchor::TopRight
                           ? in.insetTop
                           : in.paneHeight - in.insetTop - in.lockHeight;
        if (y < in.insetTop || !detail::isBandClear(in.grid, y, in.lockHeight, firstColumn))
            continue;
        return { .anchor = spot.anchor, .x = x, .y = y };
    }
    return {};
}

} // namespace contour::window
