// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/ReGISFontRasterizer.h>

#include <text_shaper/font_locator_provider.h>
#include <text_shaper/open_shaper.h>

#include <algorithm>
#include <cmath>

using vtbackend::regis::ReGISGlyphBitmap;

namespace vtrasterizer
{

namespace
{
    /// Extracts the coverage (alpha) byte of the glyph pixel at (@p x, @p y).
    uint8_t coverageAt(text::rasterized_glyph const& glyph, int x, int y) noexcept
    {
        auto const width = unbox<int>(glyph.bitmapSize.width);
        auto const bytesPerPixel = static_cast<int>(text::pixel_size(glyph.format));
        auto const base = static_cast<size_t>(((y * width) + x) * bytesPerPixel);
        if (base >= glyph.bitmap.size())
            return 0;
        // alpha_mask stores coverage directly; rgba keeps it in the last byte.
        return glyph.bitmap[base + static_cast<size_t>(bytesPerPixel - 1)];
    }
} // namespace

ReGISFontRasterizer::ReGISFontRasterizer(text::DPI dpi, text::font_description font):
    _shaper { std::make_unique<text::open_shaper>(dpi, text::font_locator_provider::get().native()) },
    _font { std::move(font) },
    _dpi { dpi }
{
}

std::optional<ReGISGlyphBitmap> ReGISFontRasterizer::rasterize(char32_t codepoint,
                                                               vtpty::ImageSize cellSize) const
{
    auto const cellWidth = unbox<int>(cellSize.width);
    auto const cellHeight = unbox<int>(cellSize.height);
    if (cellWidth <= 0 || cellHeight <= 0)
        return std::nullopt;

    // Size the font so a glyph fits within the ReGIS cell (leaving a small margin), converting the
    // pixel cell height to a point size at the display DPI.
    auto const points = (static_cast<double>(cellHeight) * 72.0 / static_cast<double>(_dpi.y)) * 0.82;

    // The shaper is not thread-safe and is shared across sessions, so serialize the whole shaping path
    // (load/shape/rasterize all mutate its caches) here.
    auto const lock = std::lock_guard { _mutex };

    // Reuse the font key per point size (all glyphs of a string share one) instead of reloading the
    // font -- and rebuilding its fallback chain -- for every character. Quantize the size to a stable
    // integer key so tiny float differences do not defeat the cache.
    auto const sizeKey = static_cast<int>(std::lround(points * 64.0));
    auto cached = _fontKeyByPointSize.find(sizeKey);
    if (cached == _fontKeyByPointSize.end())
    {
        auto const loaded = _shaper->load_font(_font, text::font_size { points });
        if (!loaded)
            return std::nullopt;
        cached = _fontKeyByPointSize.emplace(sizeKey, *loaded).first;
    }
    auto const fontKey = cached->second;

    auto const glyphPosition = _shaper->shape(fontKey, codepoint);
    if (!glyphPosition)
        return std::nullopt;

    auto const glyph = _shaper->rasterize(glyphPosition->glyph, text::render_mode::gray);
    if (!glyph)
        return std::nullopt;

    auto const glyphWidth = unbox<int>(glyph->bitmapSize.width);
    auto const glyphHeight = unbox<int>(glyph->bitmapSize.height);

    // Honour the font baseline so glyphs of differing heights sit on a common line -- just as the
    // grid renderer does in TextRenderer::applyGlyphPositionToPen. Centring each glyph on its own
    // bitmap (offsetY = (cellHeight - glyphHeight) / 2) would place 'a' and 'g' at different heights
    // and make adjacent letters look misaligned.
    auto const metrics = _shaper->metrics(fontKey);
    auto const offsetY =
        regisGlyphBaselineOffsetY(cellHeight, metrics.ascender, metrics.descender, glyph->position.y);

    // Horizontal placement stays centred: ReGIS advances text by a full cell per glyph, so each
    // glyph is centred within its own fixed-width cell.
    auto const offsetX = (cellWidth - glyphWidth) / 2;

    auto bitmap = ReGISGlyphBitmap { .size = cellSize, .coverage = std::vector<uint8_t>(cellSize.area(), 0) };
    for (auto gy = 0; gy < glyphHeight; ++gy)
    {
        auto const ty = offsetY + gy;
        if (ty < 0 || ty >= cellHeight)
            continue;
        for (auto gx = 0; gx < glyphWidth; ++gx)
        {
            auto const tx = offsetX + gx;
            if (tx < 0 || tx >= cellWidth)
                continue;
            bitmap.coverage[static_cast<size_t>((ty * cellWidth) + tx)] = coverageAt(*glyph, gx, gy);
        }
    }
    return bitmap;
}

} // namespace vtrasterizer
