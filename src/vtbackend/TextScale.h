// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstdint>

namespace vtbackend
{

/// How one cell's glyph is sized and placed, as the kitty text sizing protocol (`OSC 66`) states it.
///
/// The block's extent in cells comes from @c scale alone; the remaining fields change only how large
/// the glyph is DRAWN inside that block and where it sits. That split is the protocol's, and keeping
/// it here means the layout arithmetic never has to look at the fraction.
struct CellScale
{
    /// `s` -- the block is @c scale cells tall. 1 for ordinary text.
    uint8_t scale = 1;

    /// `n` / `d` -- the glyph is drawn at @c numerator / @c denominator of @c scale.
    /// Both zero (the default) means no fraction was asked for.
    uint8_t numerator = 0;
    uint8_t denominator = 0;

    /// `v` / `h` -- where a fractionally-scaled glyph sits in the cells it was given.
    /// 0 = top/left, 1 = bottom/right, 2 = centered. Meaningless without a fraction.
    uint8_t verticalAlignment = 0;
    uint8_t horizontalAlignment = 0;

    /// @return whether a proper fraction was asked for. An improper one (`n >= d`) is not a
    ///         shrink and is ignored, matching kitty's `effective_scale`.
    [[nodiscard]] constexpr bool hasFraction() const noexcept
    {
        return numerator != 0 && denominator != 0 && numerator < denominator;
    }

    /// @return how large the glyph is drawn, relative to one ordinary cell.
    ///
    /// `scale` alone for ordinary and whole-scaled text; `scale * n/d` when a fraction applies. This
    /// is kitty's `effective_scale` (fonts.c), and it is the only number the renderer needs -- the
    /// cell block is sized from @c scale regardless.
    [[nodiscard]] constexpr double drawFactor() const noexcept
    {
        auto const base = static_cast<double>(scale != 0 ? scale : 1);
        return hasFraction() ? base * static_cast<double>(numerator) / static_cast<double>(denominator)
                             : base;
    }

    /// @return whether this cell is drawn exactly as ordinary text -- the overwhelmingly common case,
    ///         which must cost nothing anywhere it is asked.
    [[nodiscard]] constexpr bool isOrdinary() const noexcept
    {
        return scale <= 1 && !hasFraction() && verticalAlignment == 0 && horizontalAlignment == 0;
    }

    constexpr bool operator==(CellScale const&) const noexcept = default;
};

/// The fraction and alignment packed into one 16-bit word, for the cold per-cell column.
///
/// `scale` is deliberately NOT packed here: it is read on every erase, selection and render and lives
/// in its own hot array, while these four are zero for all but a vanishing fraction of cells.
///
/// Layout: `n:4 | d:4 | v:2 | h:2` -- 12 bits used, 0 meaning "ordinary" so a zeroed line is correct.
[[nodiscard]] constexpr uint16_t packTextScaleExtras(CellScale const& cellScale) noexcept
{
    return static_cast<uint16_t>((cellScale.numerator & 0x0F) | ((cellScale.denominator & 0x0F) << 4)
                                 | ((cellScale.verticalAlignment & 0x03) << 8)
                                 | ((cellScale.horizontalAlignment & 0x03) << 10));
}

/// Rebuilds a CellScale from the hot @p scale and the cold @p extras. @see packTextScaleExtras.
[[nodiscard]] constexpr CellScale unpackTextScale(uint8_t scale, uint16_t extras) noexcept
{
    return CellScale { .scale = scale,
                       .numerator = static_cast<uint8_t>(extras & 0x0F),
                       .denominator = static_cast<uint8_t>((extras >> 4) & 0x0F),
                       .verticalAlignment = static_cast<uint8_t>((extras >> 8) & 0x03),
                       .horizontalAlignment = static_cast<uint8_t>((extras >> 10) & 0x03) };
}

/// A cell's sizing together with WHICH row of its block this cell draws.
///
/// A block `scale` cells tall is rasterized once and then drawn one row at a time, so every row can
/// clip to its own cell and -- crucially -- can draw itself. When only the head cell drew, a block
/// whose head scrolled above the viewport vanished entirely instead of being clipped.
///
/// kitty carries the same datum as `multicell_y` on its RunFont, and compares it in
/// run_fonts_are_equal() so each row of a block becomes its own shaping run.
struct GlyphSizing
{
    CellScale scale {};

    /// Row within the block; 0 is the head row, which is where the text lives.
    uint8_t band = 0;

    /// How many cells the block spans horizontally -- `scale * width`, as the backend claimed them.
    ///
    /// The renderer would otherwise have to infer it from the shaper's advances, which cannot be
    /// done: a Devanagari conjunct such as `क्नि` shapes into several glyphs that HarfBuzz reorders
    /// and gives advances of their own, so counting them yields more cells than the one the block
    /// occupies. The backend already knows the answer; carrying it is what keeps a cluster whole.
    uint8_t columns = 1;

    /// @return How many grid columns this cell occupies; never zero.
    ///
    /// Every reader wants the clamped value -- a span of zero would give two cells the same cluster and
    /// draw them on top of each other -- so the floor lives here rather than being restated at each of
    /// them.
    [[nodiscard]] constexpr uint8_t columnSpan() const noexcept { return std::max<uint8_t>(1, columns); }

    constexpr bool operator==(GlyphSizing const&) const noexcept = default;
};

} // namespace vtbackend
