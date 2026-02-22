// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <text_shaper/font.h>
#include <text_shaper/shaper.h>

#include <memory>

namespace text
{

/**
 * Text shaping and rendering engine using open source technologies,
 * fontconfig + harfbuzz + freetype.
 */
class directwrite_shaper: public shaper
{
  public:
    directwrite_shaper(DPI _dpi, font_locator& _locator);

    void set_dpi(DPI _dpi) override;
    void set_locator(font_locator& _locator) override;
    void clear_cache() override;

    void set_font_fallback_limit(int limit) override;

    std::optional<font_key> load_font(font_description const& _description, font_size _size) override;

    font_metrics metrics(font_key _key) const override;

    void shape(font_key _font,
               std::u32string_view _text,
               gsl::span<unsigned> _clusters,
               unicode::Script _script,
               unicode::PresentationStyle _presentation,
               shape_result& _result) override;

    std::optional<glyph_position> shape(font_key _font, char32_t _codepoint) override;

    std::optional<rasterized_glyph> rasterize(glyph_key _glyph,
                                              render_mode _mode,
                                              float outlineThickness = 0.0f) override;

  private:
    struct Private;
    std::unique_ptr<Private, void (*)(Private*)> d;
};

} // namespace text
