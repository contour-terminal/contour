// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>

namespace vtrasterizer
{

/// Where the curly (wavy) underline's wave sits inside the tile that carries it.
///
/// Coordinates are bottom-origin, matching Pixmap: row 0 is the tile's BOTTOM row. A decoration
/// tile is drawn flush with the cell's bottom edge (@see DecorationRenderer::renderDecoration draws
/// it at `cellBottom - bitmapHeight`), so tile row @c y lands at cell-bottom offset `y + 1` and the
/// tile's height is how far up the decoration reaches.
struct CurlyUnderlineGeometry
{
    /// The tile's height in pixels.
    int tileHeight = 1;

    /// The wave's centre line.
    int centerY = 0;

    /// How far the centre line travels above and below @c centerY.
    int amplitude = 1;

    /// Rows painted above AND below the centre line, so the stroke is `2 * strokeRadius + 1` rows
    /// thick. Applied along y: a near-horizontal cosine thickened along x gains nothing and leaves
    /// holes where the curve is steepest.
    int strokeRadius = 0;
};

/// Sizes and places the curly underline's wave, so that it reads as a wave without colliding with
/// the text above it.
///
/// The wave's topmost painted row is the row a straight underline's top occupies, and it descends
/// from there -- so toggling `SGR 4:1` and `SGR 4:3` keeps the same top edge, and neither can reach
/// the baseline. This is what foot (render.c, `draw_styled_underline`) and ghostty
/// (sprite/draw/special.zig, `underline_curly`) both do.
///
/// The amplitude is a quarter of the room between the underline position and the cell bottom. That
/// bound is kitty's (decorations.c: `half_height = max(1u, max_height / 4u); // 4 so as to be not
/// too large`), and it is the crux of issue #1754: sizing the wave from @c GridMetrics::baseline
/// instead made it as tall as the whole descender. `baseline` is a poor proxy for descender depth
/// because it is `lineHeight - ascender`, which folds in the font's entire line gap -- Nimbus Mono
/// PS declares 200/1000 em of leading, so its 10px "descender" is really 6px of descent plus 4px of
/// leading, and the wave grew to 9px in a 20px cell (see issue #2030).
///
/// @param underlinePosition the straight underline's top edge, in pixels above the cell bottom.
/// @param underlineThickness the font's underline thickness in pixels; values below one are treated
///                           as one, since a stroke must be drawn to be seen.
/// @param cellHeight the grid cell's height in pixels, the hard bound on the tile.
///
/// @return a geometry whose painted band `[centerY - amplitude - strokeRadius,
///         centerY + amplitude + strokeRadius]` always lies within `[0, tileHeight - 1]`, so that
///         nothing is silently clipped by Pixmap::paint.
[[nodiscard]] constexpr CurlyUnderlineGeometry curlyUnderlineGeometry(int underlinePosition,
                                                                      int underlineThickness,
                                                                      int cellHeight) noexcept
{
    auto const cell = std::max(1, cellHeight);

    // The room the wave has to live in: everything between the underline position and the cell
    // bottom. Clamped into the cell, because a font may report an underline position outside it.
    auto const room = std::clamp(underlinePosition, 1, cell);

    // A stroke `2 * radius + 1` rows thick. The minimum wave is a crest row, a trough row and one
    // row between them, so a stroke that leaves no space for those three has to give way first --
    // a fat straight line is a worse answer than a thin curl.
    auto const thickness = std::max(1, underlineThickness);
    auto strokeRadius = (thickness - 1) / 2;
    if ((2 * strokeRadius) + 3 > room)
        strokeRadius = std::max(0, (room - 3) / 2);

    // A quarter of the room, so the wave is a wave and not a ribbon; never below one, or there is
    // no wave at all; and never so large that the trough falls out of the tile.
    auto const headroom = std::max(1, (room - 1 - (2 * strokeRadius)) / 2);
    auto const amplitude = std::clamp(room / 4, 1, headroom);

    // The band is `2 * amplitude + 2 * strokeRadius + 1` rows. It normally fits under the underline
    // position; where it does not the tile grows to hold it, but never past the cell.
    auto const band = (2 * amplitude) + (2 * strokeRadius) + 1;
    auto const tileHeight = std::min(cell, std::max(room, band));

    // Fit the band to the tile that was actually granted. A cell too short to hold even the minimum
    // wave degrades to a flatter, ultimately straight stroke -- which is visible, where a curl
    // clipped away by Pixmap::paint would not be.
    auto const halfBand = (tileHeight - 1) / 2;
    auto const stroke = std::min(strokeRadius, halfBand);
    auto const wave = std::min(amplitude, halfBand - stroke);

    return CurlyUnderlineGeometry { .tileHeight = tileHeight,
                                    .centerY = tileHeight - 1 - wave - stroke,
                                    .amplitude = wave,
                                    .strokeRadius = stroke };
}

} // namespace vtrasterizer
