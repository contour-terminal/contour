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
    DirectWriteShaper(DPI _dpi, FontLocator& _locator);

    void set_dpi(DPI _dpi) override;
    void set_locator(FontLocator& _locator) override;
    void clear_cache() override;

    void set_font_fallback_limit(int limit) override;

    std::optional<FontKey> load_font(FontDescription const& _description, FontSize _size) override;

    FontMetrics metrics(FontKey _key) const override;

    void shape(FontKey _font,
               std::u32string_view _text,
               gsl::span<unsigned> _clusters,
               unicode::Script _script,
               unicode::PresentationStyle _presentation,
               ShapeResult& _result) override;

    std::optional<GlyphPosition> shape(FontKey _font, char32_t _codepoint) override;

    std::optional<RasterizedGlyph> rasterize(GlyphKey _glyph,
                                             RenderMode _mode,
                                             float outlineThickness = 0.0f) override;

  private:
    struct Private;
    std::unique_ptr<Private, void (*)(Private*)> d;
};

} // namespace text
