// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <text_shaper/font.h>
#include <text_shaper/shaper.h>

#include <memory>

namespace text
{

class font_locator;

/**
 * Text shaping and rendering engine using open source technologies,
 * fontconfig + harfbuzz + freetype.
 */
class open_shaper: public shaper
{
  public:
    explicit open_shaper(DPI dpi, font_locator& locator);

    void set_dpi(DPI dpi) override;

    void set_locator(font_locator& locator) override;

    void clear_cache() override;

    void set_font_fallback_limit(int limit) override;

    [[nodiscard]] std::optional<font_key> load_font(font_description const& description,
                                                    font_size size) override;

    [[nodiscard]] font_metrics metrics(font_key key) const override;

    void shape(font_key font,
               std::u32string_view codepoints,
               gsl::span<unsigned> clusters,
               unicode::Script script,
               unicode::PresentationStyle presentation,
               shape_result& result) override;

    [[nodiscard]] std::optional<glyph_position> shape(font_key font, char32_t codepoint) override;

    [[nodiscard]] std::optional<rasterized_glyph> rasterize(glyph_key glyph,
                                                            render_mode mode,
                                                            float outlineThickness = 0.0f) override;

  private:
    struct private_open_shaper;
    std::unique_ptr<private_open_shaper, void (*)(private_open_shaper*)> _d;
};

} // namespace text
