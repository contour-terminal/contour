// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ChromeTokenGroups.hpp>

#include <vtworkspace/PaneLayout.hpp>

namespace contour::window
{

namespace detail
{
    /// ceil(@p tokens * @p unit) for non-negative inputs, in constexpr form: std::ceil is constexpr only
    /// where the library implements P0533.
    /// @param tokens A token extent, in its row's unit.
    /// @param unit   What one unit is, in logical pixels.
    /// @return The extent in whole logical pixels, never below the exact product.
    [[nodiscard]] constexpr int ceilTokens(int tokens, double unit) noexcept
    {
        auto const exact = static_cast<double>(tokens) * unit;
        auto const truncated = static_cast<int>(exact);
        return exact > static_cast<double>(truncated) ? truncated + 1 : truncated;
    }
} // namespace detail

/// What a split tree spends around its leaves, in logical pixels, for the pane-tree size solver.
///
/// The same tokens PaneNode.qml draws the gap, the header and the inset with, so the solver and the
/// canvas cannot disagree; each is rounded up, so the solver may overshoot but never undershoot.
/// @param tokens     The resolved token row (density and accessibility applied).
/// @param unitWidth  One horizontal token unit, in logical pixels: a cell's width on Terminal, else 1.
/// @param unitHeight One vertical token unit, in logical pixels: a cell's height on Terminal, else 1.
/// @return The gaps per axis, the header height and the root inset.
[[nodiscard]] constexpr vtworkspace::SplitChrome splitChromeOf(config::ChromeTokenRow const& tokens,
                                                               double unitWidth,
                                                               double unitHeight) noexcept
{
    return vtworkspace::SplitChrome {
        .sideBySideGap = detail::ceilTokens(tokens.space.paneGap, unitWidth),
        .stackedGap = detail::ceilTokens(tokens.space.paneGapRows, unitHeight),
        .leafHeader = detail::ceilTokens(tokens.size.paneHeader, unitHeight),
        .rootInset = detail::ceilTokens(tokens.space.paneInset, unitWidth),
    };
}

} // namespace contour::window
