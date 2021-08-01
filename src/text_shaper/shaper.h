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

#include <unicode/ucd.h>
#include <unicode/emoji_segmenter.h>

#include <text_shaper/font.h>

#include <crispy/point.h>
#include <crispy/size.h>
#include <crispy/ImageSize.h>
#include <crispy/span.h>
#include <crispy/debuglog.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace text {

auto const inline FontLoaderTag = crispy::debugtag::make("font.loader", "Logs font loads and loading errors.");
auto const inline TextShapingTag = crispy::debugtag::make("font.textshaping", "Logs details about text shaping.");
auto const inline GlyphRenderTag = crispy::debugtag::make("font.render", "Logs details about rendering glyphs.");

enum class bitmap_format { alpha_mask, rgb, rgba };

constexpr int pixel_size(bitmap_format _format) noexcept
{
    switch (_format)
    {
        case bitmap_format::rgba:
            return 4;
        case bitmap_format::rgb:
            return 3;
        case bitmap_format::alpha_mask:
            return 1;
    }
    return 1;
}

struct rasterized_glyph
{
    glyph_index index;
    crispy::ImageSize size; // Glyph bitmap size in pixels.
    crispy::Point position; // top-left position of the bitmap, relative to the basline's origin.
    bitmap_format format;
    std::vector<uint8_t> bitmap;
};

std::tuple<rasterized_glyph, float> scale(rasterized_glyph const& _bitmap, crispy::ImageSize _newSize);

struct glyph_position
{
    glyph_key glyph;
    crispy::Point offset;
    crispy::Point advance;

    unicode::PresentationStyle presentation;
};

using shape_result = std::vector<glyph_position>;

class font_locator;

/**
 * Platform-independent font loading, text shaping, and glyph rendering API.
 */
class shaper {
  public:
    virtual ~shaper() = default;

    /**
     * Sets or updates DPI to the given value.
     */
    virtual void set_dpi(crispy::Point _dpi) = 0;

    /**
     * Configures the font location API to be used.
     */
    virtual void set_locator(std::unique_ptr<font_locator> _locator) = 0;

    /**
     * Clears internal caches (if any).
     */
    virtual void clear_cache() = 0;

    /**
     * Returns a font matching the given font description.
     *
     * On Linux this font will be using Freetype, whereas
     * on Windows it will be a DirectWrite font,
     * and on Apple it will be using CoreText (but for now it'll be freetype, too).
     */
    virtual std::optional<font_key> load_font(font_description const& _description, font_size _size) = 0;

    /**
     * Retrieves global font metrics of font identified by @p _key.
     */
    virtual font_metrics metrics(font_key _key) const = 0;

    /**
     * Shapes the given text @p _text using the font face @p _font.
     *
     * @param _font     font_key identifying the font to use for text shaping.
     * @param _font     the font to use for text shaping.
     * @param _text     the sequence of codepoints to shape (must be all of the same script).
     * @param _clusters codepoint clusters
     * @param _script   the script of the given text.
     * @param _presentation the pre-determined presentation style that is being stored in each glyph position.
     * @param _result   vector at which the text shaping result will be stored.
     *
     * The call always returns a usable shape result, optionally using font fallback if the given
     * font did not satisfy.
     */
    virtual void shape(font_key _font,
                       std::u32string_view _text,
                       crispy::span<unsigned> _clusters,
                       unicode::Script _script,
                       unicode::PresentationStyle _presentation,
                       shape_result& _result) = 0;

    virtual std::optional<glyph_position> shape(font_key _font,
                                                char32_t _codepoint) = 0;

    /**
     * Rasterizes (renders) the glyph using the given render mode.
     *
     * @param _glyph glyph identifier.
     * @param _mode  render technique to use.
     */
    virtual std::optional<rasterized_glyph> rasterize(glyph_key _glyph, render_mode _mode) = 0;
};

} // end namespace text


namespace fmt { // {{{
    template <>
    struct formatter<text::glyph_position> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(text::glyph_position const& _gpos, FormatContext& ctx)
        {
            return format_to(
                ctx.out(),
                "({}+{}+{}|{}+{})",
                _gpos.glyph.index.value,
                _gpos.offset.x,
                _gpos.offset.y,
                _gpos.advance.x,
                _gpos.advance.y);
        }
    };

    template <>
    struct formatter<text::rasterized_glyph> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(text::rasterized_glyph const& _glyph, FormatContext& ctx)
        {
            return format_to(
                ctx.out(),
                "rasterized_glyph({}, {}+{}, {})",
                _glyph.index.value,
                _glyph.size,
                _glyph.position,
                _glyph.format
            );
        }
    };
} // }}}
