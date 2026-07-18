// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/regis/ReGISTextRasterizer.h>

#include <algorithm>
#include <cmath>

namespace vtbackend::regis
{

namespace
{
    /// Bilinearly samples the 8x8 glyph's coverage at continuous source coordinates.
    /// @return coverage in [0, 1].
    double sampleGlyph(Glyph const& glyph, double sourceX, double sourceY) noexcept
    {
        auto const bitAt = [&glyph](int column, int row) -> double {
            if (column < 0 || column >= GlyphBaseSize || row < 0 || row >= GlyphBaseSize)
                return 0.0;
            return ((glyph.rows[static_cast<size_t>(row)] >> column) & 1u) ? 1.0 : 0.0;
        };
        auto const x0 = static_cast<int>(std::floor(sourceX));
        auto const y0 = static_cast<int>(std::floor(sourceY));
        auto const dx = sourceX - x0;
        auto const dy = sourceY - y0;
        return (bitAt(x0, y0) * (1.0 - dx) * (1.0 - dy)) + (bitAt(x0 + 1, y0) * dx * (1.0 - dy))
               + (bitAt(x0, y0 + 1) * (1.0 - dx) * dy) + (bitAt(x0 + 1, y0 + 1) * dx * dy);
    }
} // namespace

std::optional<ReGISGlyphBitmap> EmbeddedReGISTextRasterizer::rasterize(char32_t codepoint,
                                                                       vtpty::ImageSize cellSize) const
{
    auto const width = unbox<int>(cellSize.width);
    auto const height = unbox<int>(cellSize.height);
    if (width <= 0 || height <= 0)
        return std::nullopt;

    // The embedded font only covers 7-bit ASCII; higher code points fall back to the blank glyph.
    auto const& glyph = _font.glyphOf(codepoint <= 0x7F ? static_cast<char>(codepoint) : ' ');

    auto bitmap = ReGISGlyphBitmap { .size = cellSize, .coverage = std::vector<uint8_t>(cellSize.area(), 0) };
    for (auto ty = 0; ty < height; ++ty)
    {
        // Map the target pixel centre back into the 8x8 glyph, then bilinearly sample for smooth,
        // anti-aliased edges rather than a blocky nearest-neighbour upscale.
        auto const sourceY = (((ty + 0.5) / height) * GlyphBaseSize) - 0.5;
        for (auto tx = 0; tx < width; ++tx)
        {
            auto const sourceX = (((tx + 0.5) / width) * GlyphBaseSize) - 0.5;
            auto const coverage = sampleGlyph(glyph, sourceX, sourceY);
            bitmap.coverage[static_cast<size_t>((ty * width) + tx)] =
                static_cast<uint8_t>(std::lround(std::clamp(coverage, 0.0, 1.0) * 255.0));
        }
    }
    return bitmap;
}

} // namespace vtbackend::regis
