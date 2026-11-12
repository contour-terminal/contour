// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Where a popup opened from a control goes: a dropdown hangs below the control that opened it, its leading
/// edge on the control's, and stays on the screen -- flipped above the control where there is no room below,
/// pushed in from an edge it would cross. Pure and Qt-free, so the rule is a table test; the application only
/// moves the window there.

#include <algorithm>

namespace contour::window
{

/// A rectangle in screen coordinates (logical pixels).
struct PopupRect
{
    int x = 0;      ///< Left edge.
    int y = 0;      ///< Top edge.
    int width = 0;  ///< Width.
    int height = 0; ///< Height.

    [[nodiscard]] bool operator==(PopupRect const&) const = default;
};

/// A point in screen coordinates (logical pixels).
struct PopupPoint
{
    int x = 0; ///< Horizontal.
    int y = 0; ///< Vertical.

    [[nodiscard]] bool operator==(PopupPoint const&) const = default;
};

/// Where a popup of @p width × @p height opened from @p control goes on @p screen.
/// @param control The control that opened it, as the screen sees it.
/// @param width   The popup's width.
/// @param height  The popup's height.
/// @param gap     The space between the control and the popup.
/// @param screen  The screen's available area.
/// @return The popup's top-left: below the control and aligned with its leading edge; above it where the
///         screen has no room below but has room above; never past the screen's right or left edge.
[[nodiscard]] constexpr PopupPoint popupBelow(
    PopupRect const& control, int width, int height, int gap, PopupRect const& screen) noexcept
{
    auto const screenRight = screen.x + screen.width;
    auto const screenBottom = screen.y + screen.height;
    auto const below = control.y + control.height + gap;
    auto const above = control.y - gap - height;
    auto const fitsBelow = below + height <= screenBottom;
    auto const fitsAbove = above >= screen.y;
    auto const y = fitsBelow || !fitsAbove ? below : above;
    auto const x = std::max(screen.x, std::min(control.x, screenRight - width));
    return PopupPoint { .x = x, .y = std::max(screen.y, std::min(y, screenBottom - height)) };
}

} // namespace contour::window
