// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cmath>

namespace contour::window
{

/// What the command palette is placed from: the window and the row's palette tokens, in logical pixels.
struct PaletteInputs
{
    double windowWidth = 0;  ///< The window's width.
    double windowHeight = 0; ///< The window's height.
    double width = 0;        ///< The palette's width (`size.paletteWidth`).
    double maxHeight = 0;    ///< The palette's greatest height (`size.paletteMaxHeight`).
    double marginX = 0;      ///< The least gap to the window's sides.
    double marginY = 0;      ///< The least gap to the window's bottom.
    int topPercent = 0;    ///< The top edge, in percent of the window's height (`layout.paletteTopPercent`).
    double rowQuantum = 1; ///< What the top edge snaps down to: a cell's height on the Terminal row, else 1.
};

/// Where the command palette goes.
struct PalettePlacement
{
    double x = 0;         ///< Left edge, centring the palette.
    double y = 0;         ///< Top edge.
    double width = 0;     ///< Width.
    double maxHeight = 0; ///< The height the palette may grow to; it fits its content below that.

    [[nodiscard]] bool operator==(PalettePlacement const&) const = default;
};

/// Places the command palette: centred, its top a fixed share of the way down, as wide as the window
/// allows up to its token width and never past the window's bottom margin.
///
/// Not constexpr: std::floor is not constexpr in every standard library CI builds with.
/// @param in The window and the palette's tokens.
/// @return The placement; no extent is negative, however small the window.
[[nodiscard]] inline PalettePlacement placePalette(PaletteInputs const& in) noexcept
{
    auto const width = std::clamp(in.windowWidth - (2 * in.marginX), 0.0, in.width);
    auto const quantum = in.rowQuantum > 0 ? in.rowQuantum : 1.0;
    auto const y = std::floor(in.windowHeight * in.topPercent / 100.0 / quantum) * quantum;
    return PalettePlacement {
        .x = (in.windowWidth - width) / 2,
        .y = y,
        .width = width,
        .maxHeight = std::clamp(in.windowHeight - y - in.marginY, 0.0, in.maxHeight),
    };
}

} // namespace contour::window
