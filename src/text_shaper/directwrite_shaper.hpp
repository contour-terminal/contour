// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <text_shaper/font.hpp>
#include <text_shaper/shaper.hpp>

#include <memory>

namespace text
{

/**
 * Text shaping and rendering engine using open source technologies,
 * fontconfig + harfbuzz + freetype.
 */
class DirectWriteShaper: public Shaper
{
  public:
    DirectWriteShaper(DPI dpi, FontLocator& locator);

    void setDPI(DPI dpi) override;
    void setLocator(FontLocator& locator) override;
    void clearCache() override;

    void setFontFallbackLimit(int limit) override;

    std::optional<FontKey> loadFont(FontDescription const& description, FontSize size) override;

    FontMetrics metrics(FontKey key) const override;

    void shape(FontKey font,
               std::u32string_view text,
               gsl::span<unsigned> clusters,
               unicode::Script script,
               unicode::PresentationStyle presentation,
               ShapeResult& result) override;

    std::optional<GlyphPosition> shape(FontKey font, char32_t codepoint) override;

    std::optional<RasterizedGlyph> rasterize(GlyphKey glyph,
                                             RenderMode mode,
                                             float outlineThickness = 0.0f) override;

  private:
    struct Private;
    std::unique_ptr<Private, void (*)(Private*)> _d;
};

} // namespace text
