// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace vtrasterizer
{

/// The pen that walks one shaped run of text across the cell grid.
///
/// The grid decides where text goes, not the font. Every glyph names its cluster -- the display column
/// its cell starts at, counted from the run's first (two per character on a double-width line) -- and
/// the first glyph of each cluster is drawn on that column, however far the advances before it would
/// have carried the pen. A fallback glyph two cells wide in its
/// own font but one cell on the grid therefore cannot push the rest of the run to the right (#452), and
/// a proportional font's narrow advance cannot pull it to the left (#1939).
///
/// Only within a cluster does the pen move by advance, and by the font's own, unrounded: the glyphs of
/// one cluster -- a base and its combining marks, or a character that shapes to several glyphs -- are
/// positioned against each other by the font, whose offsets assume the pen moved by exactly that much.
class RunPen
{
  public:
    /// @param cellWidth    The grid cell width, in pixels.
    /// @param advanceScale How many times wider than the font draws it a line is: 2 on a double-width line.
    constexpr RunPen(int cellWidth, int advanceScale) noexcept:
        _cellWidth { cellWidth }, _advanceScale { advanceScale }
    {
    }

    /// Places the next glyph of the run.
    ///
    /// @param cluster  The glyph's cluster: the display column its cell starts at, counted from the run's.
    /// @param advanceX The glyph's horizontal advance, in pixels.
    /// @return Where to draw the glyph, in pixels from the run's first cell.
    [[nodiscard]] constexpr int place(unsigned cluster, int advanceX) noexcept
    {
        // A glyph's advance concerns only a following glyph of the same cluster, so it is applied when
        // one arrives rather than when the glyph is placed -- which spares the common case of one glyph
        // per cell any arithmetic on it.
        if (cluster == _cluster)
            _x += _pendingAdvance * _advanceScale;
        else
        {
            _x = static_cast<int>(cluster) * _cellWidth;
            _cluster = cluster;
        }
        _pendingAdvance = advanceX;
        return _x;
    }

  private:
    int _cellWidth;
    int _advanceScale;
    int _x = 0;
    int _pendingAdvance = 0;
    unsigned _cluster = 0;
};

} // namespace vtrasterizer
