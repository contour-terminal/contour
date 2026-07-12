// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>

namespace vtrasterizer
{

/// Converts a shaped glyph's horizontal advance into a whole number of grid cells.
///
/// A terminal paints on a fixed cell grid, so the pen may only ever move by whole cells and a glyph's
/// advance has to be quantized. A zero advance stays zero: combining marks, and the non-final glyphs of
/// a ligature, deliberately do not move the pen. Any non-zero advance, however, moves the pen by at
/// least one cell. A proportional fallback font can report an advance well below half a cell -- a 7px
/// non-breaking space against a 15px cell, say -- and rounding that to nearest collapses it to zero,
/// which draws every following glyph of the run one cell too far left.
///
/// @param advanceX  The glyph's horizontal advance, in pixels.
/// @param cellWidth The grid cell width, in pixels. A non-positive width yields 0.
/// @return The number of cells to advance the pen by; 0 exactly when @p advanceX is 0.
[[nodiscard]] constexpr int advanceToCells(int advanceX, int cellWidth) noexcept
{
    if (advanceX == 0 || cellWidth <= 0)
        return 0;

    auto const magnitude = advanceX < 0 ? -advanceX : advanceX;
    auto const cells = std::max(1, (magnitude + (cellWidth / 2)) / cellWidth);

    return advanceX < 0 ? -cells : cells;
}

} // namespace vtrasterizer
