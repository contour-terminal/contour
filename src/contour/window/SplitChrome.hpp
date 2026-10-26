// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ChromeTokenGroups.hpp>

#include <vtworkspace/PaneLayout.hpp>

namespace contour::window
{

namespace detail
{
    /// ceil(@p value) for non-negative inputs, in constexpr form: std::ceil is constexpr only where the
    /// library implements P0533.
    [[nodiscard]] constexpr int ceilOf(double value) noexcept
    {
        auto const truncated = static_cast<int>(value);
        return value > static_cast<double>(truncated) ? truncated + 1 : truncated;
    }
} // namespace detail

/// A gap or an inset of @p logical pixels as PaneNode.qml draws it at @p devicePixelRatio: floored to whole
/// device pixels, and at least one device pixel when non-zero.
///
/// The C++ half of PaneNode.qml's `snapGapToDevice`, which the two must keep agreeing with (the QML split
/// handle cases check it). Floored so a drawn gap never outgrows what the solver counts for it.
/// @param logical          The extent in logical pixels.
/// @param devicePixelRatio The window's device pixels per logical pixel; positive.
/// @return The drawn extent in logical pixels, on the device-pixel grid.
[[nodiscard]] constexpr double snappedChromePixels(double logical, double devicePixelRatio) noexcept
{
    if (logical <= 0.0)
        return 0.0;
    // Truncation is floor for the non-negative product; the 1e-9 keeps an exact one (6 x 1.5) whole.
    auto const devicePixels = static_cast<int>((logical * devicePixelRatio) + 1e-9);
    return static_cast<double>(devicePixels > 1 ? devicePixels : 1) / devicePixelRatio;
}

/// What a split tree spends around its leaves, in logical pixels, for the pane-tree size solver.
///
/// The same tokens PaneNode.qml draws the gap, the header and the inset with, so the solver and the
/// canvas cannot disagree. The gaps and the inset are counted as drawn -- snapped to the device grid by
/// snappedChromePixels() -- and rounded up to whole logical pixels: never less than the canvas spends, and
/// less than one logical pixel more. The header, which no split draws yet, is its token rounded up.
/// @param tokens           The resolved token row (density and accessibility applied).
/// @param unitWidth        One horizontal token unit, in logical pixels: a cell's width on Terminal, else 1.
/// @param unitHeight       One vertical token unit, in logical pixels: a cell's height on Terminal, else 1.
/// @param devicePixelRatio The window's device pixels per logical pixel; positive.
/// @return The gaps per axis, the header height and the root inset.
[[nodiscard]] constexpr vtworkspace::SplitChrome splitChromeOf(config::ChromeTokenRow const& tokens,
                                                               double unitWidth,
                                                               double unitHeight,
                                                               double devicePixelRatio) noexcept
{
    auto const drawn = [devicePixelRatio](int tokenCount, double unit) noexcept {
        return detail::ceilOf(snappedChromePixels(static_cast<double>(tokenCount) * unit, devicePixelRatio));
    };
    return vtworkspace::SplitChrome {
        .sideBySideGap = drawn(tokens.space.paneGap, unitWidth),
        .stackedGap = drawn(tokens.space.paneGapRows, unitHeight),
        .leafHeader = detail::ceilOf(static_cast<double>(tokens.size.paneHeader) * unitHeight),
        .rootInset = drawn(tokens.space.paneInset, unitWidth),
    };
}

} // namespace contour::window
