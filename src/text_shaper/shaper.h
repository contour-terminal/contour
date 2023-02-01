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

#include <crispy/ImageSize.h>
#include <crispy/logstore.h>
#include <crispy/point.h>
#include <crispy/size.h>

#include <unicode/emoji_segmenter.h>
#include <unicode/ucd.h>

#include <fmt/format.h>

#include <gsl/span>
#include <gsl/span_ext>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace text
{

auto const inline RasterizerLog = logstore::Category("font.render", "Logs details about rendering glyphs.");
auto const inline TextShapingLog = logstore::Category("font.textshaping", "Logs details about text shaping.");

// NOLINTBEGIN(readability-identifier-naming)
enum class bitmap_format
{
    alpha_mask,
    rgb,
    rgba
};
// NOLINTEND(readability-identifier-naming)

constexpr size_t pixel_size(bitmap_format format) noexcept
{
    switch (format)
    {
        case bitmap_format::rgba: return 4;
        case bitmap_format::rgb: return 3;
        case bitmap_format::alpha_mask: return 1;
    }
    return 1;
}

struct rasterized_glyph
{
    glyph_index index;
    crispy::ImageSize bitmapSize; // Glyph bitmap size in pixels.
    crispy::Point position;       // top-left position of the bitmap, relative to the basline's origin.
    bitmap_format format;
    std::vector<uint8_t> bitmap;

    [[nodiscard]] bool valid() const
    {
        return bitmap.size()
               == text::pixel_size(format) * unbox<size_t>(bitmapSize.width)
                      * unbox<size_t>(bitmapSize.height);
    }
};

std::tuple<rasterized_glyph, float> scale(rasterized_glyph const& bitmap, crispy::ImageSize boundingBox);

struct glyph_position
{
    glyph_key glyph;
    crispy::Point offset;
    crispy::Point advance;

    unicode::PresentationStyle presentation {};
};

using shape_result = std::vector<glyph_position>;

class font_locator;

/**
 * Platform-independent font loading, text shaping, and glyph rendering API.
 */
class shaper
{
  public:
    virtual ~shaper() = default;

    /**
     * Sets or updates DPI to the given value.
     */
    virtual void set_dpi(DPI dpi) = 0;

    /**
     * Configures the font location API to be used.
     */
    virtual void set_locator(font_locator& locator) = 0;

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
    [[nodiscard]] virtual std::optional<font_key> load_font(font_description const& description,
                                                            font_size size) = 0;

    /**
     * Retrieves global font metrics of font identified by @p key.
     */
    [[nodiscard]] virtual font_metrics metrics(font_key key) const = 0;

    /**
     * Shapes the given text @p text using the font face @p font.
     *
     * @param font     font_key identifying the font to use for text shaping.
     * @param font     the font to use for text shaping.
     * @param text     the sequence of codepoints to shape (must be all of the same script).
     * @param clusters codepoint clusters
     * @param script   the script of the given text.
     * @param presentation the pre-determined presentation style that is being stored in each glyph position.
     * @param result   vector at which the text shaping result will be stored.
     *
     * The call always returns a usable shape result, optionally using font fallback if the given
     * font did not satisfy.
     */
    virtual void shape(font_key font,
                       std::u32string_view text,
                       gsl::span<unsigned> clusters,
                       unicode::Script script,
                       unicode::PresentationStyle presentation,
                       shape_result& result) = 0;

    [[nodiscard]] virtual std::optional<glyph_position> shape(font_key font, char32_t codepoint) = 0;

    /**
     * Rasterizes (renders) the glyph using the given render mode.
     *
     * @param glyph glyph identifier.
     * @param mode  render technique to use.
     */
    [[nodiscard]] virtual std::optional<rasterized_glyph> rasterize(glyph_key glyph, render_mode mode) = 0;
};

} // end namespace text

namespace fmt
{ // {{{
template <>
struct formatter<text::bitmap_format>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(text::bitmap_format value, FormatContext& ctx)
    {
        switch (value)
        {
            case text::bitmap_format::alpha_mask: return fmt::format_to(ctx.out(), "alpha_mask");
            case text::bitmap_format::rgb: return fmt::format_to(ctx.out(), "rgb");
            case text::bitmap_format::rgba: return fmt::format_to(ctx.out(), "rgba");
            default: return fmt::format_to(ctx.out(), "{}", static_cast<unsigned>(value));
        }
    }
};

template <>
struct formatter<text::glyph_position>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(text::glyph_position const& gpos, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(),
                              "({}+{}+{}|{}+{})",
                              gpos.glyph.index.value,
                              gpos.offset.x,
                              gpos.offset.y,
                              gpos.advance.x,
                              gpos.advance.y);
    }
};

template <>
struct formatter<text::rasterized_glyph>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(text::rasterized_glyph const& glyph, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(),
                              "rasterized_glyph({}, {}+{}, {})",
                              glyph.index.value,
                              glyph.bitmapSize,
                              glyph.position,
                              glyph.format);
    }
};
} // namespace fmt
