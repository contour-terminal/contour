// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/primitives.h>

#include <text_shaper/font.h>

#include <crispy/logstore.h>
#include <crispy/point.h>
#include <crispy/size.h>

#include <libunicode/emoji_segmenter.h>
#include <libunicode/ucd.h>

#include <gsl/span>
#include <gsl/span_ext>

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace text
{

auto const inline rasterizerLog = logstore::category("font.render", "Logs details about rendering glyphs.");
auto const inline textShapingLog = logstore::category("font.textshaping", "Logs details about text shaping.");

// NOLINTBEGIN(readability-identifier-naming)
enum class bitmap_format : uint8_t
{
    alpha_mask, ///< 1 byte/pixel (R = coverage)
    rgb,        ///< 3 bytes/pixel (LCD subpixel)
    rgba,       ///< 4 bytes/pixel (color emoji/images)
    outlined,   ///< 4 bytes/pixel RGBA (R=fill alpha, G=outline alpha, B=0, A=max)
};
// NOLINTEND(readability-identifier-naming)

constexpr size_t pixel_size(bitmap_format format) noexcept
{
    switch (format)
    {
        case bitmap_format::rgba: return 4;
        case bitmap_format::outlined: return 4;
        case bitmap_format::rgb: return 3;
        case bitmap_format::alpha_mask: return 1;
    }
    return 1;
}

struct rasterized_glyph
{
    glyph_index index {};               // Glyph index.
    vtbackend::ImageSize bitmapSize {}; // Glyph bitmap size in pixels.
    crispy::point position {};          // top-left position of the bitmap, relative to the baseline's origin.
    bitmap_format format {};            // Bitmap pixel format.
    std::vector<uint8_t> bitmap {};     // Raw bitmap data.

    [[nodiscard]] bool valid() const
    {
        return bitmap.size()
               == text::pixel_size(format) * unbox<size_t>(bitmapSize.width)
                      * unbox<size_t>(bitmapSize.height);
    }
};

std::tuple<rasterized_glyph, float> scale(rasterized_glyph const& bitmap, vtbackend::ImageSize boundingBox);

struct glyph_position
{
    glyph_key glyph;
    crispy::point offset;
    crispy::point advance;

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

    /// Sets the maximum number of fallback fonts to consider per font key.
    /// @param limit  -1 for unlimited, 0 to disable fallbacks, positive for a cap.
    virtual void set_font_fallback_limit(int limit) = 0;

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
     * @param glyph             glyph identifier.
     * @param mode              render technique to use.
     * @param outlineThickness  outline thickness in pixel units (0 = no outline).
     */
    [[nodiscard]] virtual std::optional<rasterized_glyph> rasterize(glyph_key glyph,
                                                                    render_mode mode,
                                                                    float outlineThickness = 0.0f) = 0;
};

} // end namespace text

// {{{ fmtlib support
template <>
struct std::formatter<text::bitmap_format>: std::formatter<std::string_view>
{
    auto format(text::bitmap_format value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case text::bitmap_format::alpha_mask: name = "alpha_mask"; break;
            case text::bitmap_format::rgb: name = "rgb"; break;
            case text::bitmap_format::rgba: name = "rgba"; break;
            case text::bitmap_format::outlined: name = "outlined"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<text::glyph_position>: std::formatter<std::string>
{
    auto format(text::glyph_position const& gpos, auto& ctx) const
    {
        return formatter<std::string>::format(std::format("({}+{}+{}|{}+{})",
                                                          gpos.glyph.index.value,
                                                          gpos.offset.x,
                                                          gpos.offset.y,
                                                          gpos.advance.x,
                                                          gpos.advance.y),
                                              ctx);
    }
};

template <>
struct std::formatter<text::rasterized_glyph>: std::formatter<std::string>
{
    auto format(text::rasterized_glyph const& glyph, auto& ctx) const
    {
        return formatter<std::string>::format(std::format("rasterized_glyph({}, {}+{}, {})",
                                                          glyph.index.value,
                                                          glyph.bitmapSize,
                                                          glyph.position,
                                                          glyph.format),
                                              ctx);
    }
};
// }}}
