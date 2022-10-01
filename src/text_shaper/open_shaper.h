/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
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
    explicit open_shaper(DPI _dpi, font_locator& _locator);

    void set_dpi(DPI _dpi) override;

    void set_locator(font_locator& _locator) override;

    void clear_cache() override;

    [[nodiscard]] std::optional<font_key> load_font(font_description const& _description, font_size _size) override;

    [[nodiscard]] font_metrics metrics(font_key _key) const override;

    void shape(font_key _font,
               std::u32string_view _text,
               gsl::span<unsigned> _clusters,
               unicode::Script _script,
               unicode::PresentationStyle _presentation,
               shape_result& _result) override;

    [[nodiscard]] std::optional<glyph_position> shape(font_key _font, char32_t _codepoint) override;

    [[nodiscard]] std::optional<rasterized_glyph> rasterize(glyph_key _glyph, render_mode _mode) override;

  private:
    struct Private;
    std::unique_ptr<Private, void (*)(Private*)> d;
};

} // namespace text
