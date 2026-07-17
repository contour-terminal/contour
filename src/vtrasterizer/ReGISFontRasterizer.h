// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/regis/ReGISTextRasterizer.h>

#include <text_shaper/font.h>
#include <text_shaper/shaper.h>

#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace vtrasterizer
{

/// Computes the vertical offset, measured from the top of the ReGIS text cell, at which a glyph
/// bitmap's first (top) row must be placed so that every glyph shares the font baseline -- exactly
/// as the grid renderer does in @ref TextRenderer::applyGlyphPositionToPen.
///
/// The font's em box (@p ascender .. @p descender) is centred vertically within the cell, which
/// puts the baseline @p ascender below the em box top. A glyph whose bitmap top rises
/// @p glyphTopBearing above the baseline then has its top row placed at `baseline - glyphTopBearing`,
/// so short glyphs (e.g. 'a') and glyphs with descenders (e.g. 'g') hang from the same baseline
/// instead of each being centred on its own bitmap.
///
/// @param cellHeight      ReGIS text cell height in pixels (> 0).
/// @param ascender        Font ascender in pixels, positive, measured up from the baseline.
/// @param descender       Font descender in pixels, negative, measured down from the baseline.
/// @param glyphTopBearing Glyph bitmap top above the baseline in pixels, positive up
///                        (@c text::rasterized_glyph::position.y).
/// @return Row offset from the cell top for the glyph bitmap's top row; may be negative when the
///         glyph is taller than the cell (the caller clips out-of-cell rows).
[[nodiscard]] constexpr int regisGlyphBaselineOffsetY(int cellHeight,
                                                      int ascender,
                                                      int descender,
                                                      int glyphTopBearing) noexcept
{
    auto const emHeight = ascender - descender;
    auto const baselineFromTop = ((cellHeight - emHeight) / 2) + ascender;
    return baselineFromTop - glyphTopBearing;
}

/// A ReGIS text rasterizer backed by the real text-shaping / glyph-rasterization engine.
///
/// This is the display-layer implementation of vtbackend's @ref vtbackend::regis::ReGISTextRasterizer
/// dependency-injection seam: the terminal engine defines the interface and ships an embedded-font
/// default, and the display injects this so ReGIS text renders through the same font engine as normal
/// terminal text -- without vtbackend ever depending on the font stack.
///
/// It owns a dedicated @ref text::shaper so it can be called from the terminal's parser thread
/// without racing the render thread's shaper. A single instance is shared across every session bound
/// to a display, so @ref rasterize serializes access to the shaper with @ref _mutex -- two sessions
/// may drive ReGIS text on their own parser threads at the same time.
class ReGISFontRasterizer final: public vtbackend::regis::ReGISTextRasterizer
{
  public:
    /// @param dpi The display DPI, used to size fonts for a requested pixel cell.
    /// @param font The font to shape ReGIS text with (typically the profile's regular font).
    ReGISFontRasterizer(text::DPI dpi, text::font_description font);

    [[nodiscard]] std::optional<vtbackend::regis::ReGISGlyphBitmap> rasterize(
        char32_t codepoint, vtpty::ImageSize cellSize) const override;

  private:
    std::unique_ptr<text::shaper> _shaper;
    text::font_description _font;
    text::DPI _dpi;

    /// Serializes access to @ref _shaper (which is not thread-safe) and @ref _cachedFont across the
    /// parser threads of the sessions sharing this rasterizer.
    mutable std::mutex _mutex;

    /// Font keys cached by quantized point size. A whole ReGIS string uses one cell size (hence one
    /// point size), so this avoids reloading the font -- and rebuilding its fallback chain -- for every
    /// glyph. Keying by size rather than holding a single entry keeps sessions that render at different
    /// sizes from evicting each other on every glyph. Guarded by @ref _mutex.
    mutable std::unordered_map<int, text::font_key> _fontKeyByPointSize;
};

} // namespace vtrasterizer
