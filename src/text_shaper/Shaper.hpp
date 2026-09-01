// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>

#include <text_shaper/Font.hpp>

#include <crispy/LogStore.hpp>
#include <crispy/Point.hpp>
#include <crispy/Size.hpp>

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

auto inline const rasterizerLog = logstore::Category("font.render", "Logs details about rendering glyphs.");
auto inline const textShapingLog = logstore::Category("font.textshaping", "Logs details about text shaping.");

enum class BitmapFormat : uint8_t
{
    AlphaMask, ///< 1 byte/pixel (R = coverage)
    RGB,       ///< 3 bytes/pixel (LCD subpixel)
    RGBA,      ///< 4 bytes/pixel (color emoji/images)
    Outlined,  ///< 4 bytes/pixel RGBA (R=fill alpha, G=outline alpha, B=0, A=max)
};

constexpr size_t pixelSize(BitmapFormat format) noexcept
{
    switch (format)
    {
        case BitmapFormat::RGBA: return 4;
        case BitmapFormat::Outlined: return 4;
        case BitmapFormat::RGB: return 3;
        case BitmapFormat::AlphaMask: return 1;
    }
    return 1;
}

struct RasterizedGlyph
{
    GlyphIndex index {};                // Glyph index.
    vtbackend::ImageSize bitmapSize {}; // Glyph bitmap size in pixels.
    crispy::Point position {};          // top-left position of the bitmap, relative to the baseline's origin.
    BitmapFormat format {};             // Bitmap pixel format.
    std::vector<uint8_t> bitmap {};     // Raw bitmap data.

    [[nodiscard]] bool valid() const
    {
        return bitmap.size()
               == text::pixelSize(format) * unbox<size_t>(bitmapSize.width)
                      * unbox<size_t>(bitmapSize.height);
    }
};

std::tuple<RasterizedGlyph, float> scale(RasterizedGlyph const& bitmap, vtbackend::ImageSize boundingBox);

struct GlyphPosition
{
    GlyphKey glyph;
    crispy::Point offset;
    crispy::Point advance;

    unicode::PresentationStyle presentation {};
};

using ShapeResult = std::vector<GlyphPosition>;

class FontLocator;

/**
 * Platform-independent font loading, text shaping, and glyph rendering API.
 */
class Shaper
{
  public:
    virtual ~Shaper() = default;

    /**
     * Sets or updates DPI to the given value.
     */
    virtual void setDPI(DPI dpi) = 0;

    /**
     * Configures the font location API to be used.
     */
    virtual void setLocator(FontLocator& locator) = 0;

    /**
     * Clears internal caches (if any).
     */
    virtual void clearCache() = 0;

    /// Sets the maximum number of fallback fonts to consider per font key.
    /// @param limit  -1 for unlimited, 0 to disable fallbacks, positive for a cap.
    virtual void setFontFallbackLimit(int limit) = 0;

    /**
     * Returns a font matching the given font description.
     *
     * On Linux this font will be using Freetype, whereas
     * on Windows it will be a DirectWrite font,
     * and on Apple it will be using CoreText (but for now it'll be freetype, too).
     */
    [[nodiscard]] virtual std::optional<FontKey> loadFont(FontDescription const& description,
                                                          FontSize size) = 0;

    /**
     * Retrieves global font metrics of font identified by @p key.
     */
    [[nodiscard]] virtual FontMetrics metrics(FontKey key) const = 0;

    /**
     * Returns the SAME face as @p key, loaded at @p size.
     *
     * A FontKey already encodes its size -- loadFont() takes one -- so a caller that wants a glyph
     * rasterized larger cannot get there by editing GlyphKey::size, which the rasterizer ignores.
     * This is the way to ask for it, and it is what makes scaled text (`OSC 66`) crisp rather than
     * magnified.
     *
     * The default returns @p key unchanged, which degrades to rasterizing at the original size --
     * correct, merely not crisp -- on backends that have not implemented it.
     */
    [[nodiscard]] virtual FontKey resizeFont(FontKey key, FontSize /*size*/) { return key; }

    /**
     * Shapes the given text @p text using the font face @p font.
     *
     * @param font     FontKey identifying the font to use for text shaping.
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
    virtual void shape(FontKey font,
                       std::u32string_view text,
                       gsl::span<unsigned> clusters,
                       unicode::Script script,
                       unicode::PresentationStyle presentation,
                       ShapeResult& result) = 0;

    [[nodiscard]] virtual std::optional<GlyphPosition> shape(FontKey font, char32_t codepoint) = 0;

    /**
     * Rasterizes (renders) the glyph using the given render mode.
     *
     * @param glyph             glyph identifier.
     * @param mode              render technique to use.
     * @param outlineThickness  outline thickness in pixel units (0 = no outline).
     */
    [[nodiscard]] virtual std::optional<RasterizedGlyph> rasterize(GlyphKey glyph,
                                                                   RenderMode mode,
                                                                   float outlineThickness = 0.0f) = 0;
};

} // end namespace text

// {{{ fmt support
template <>
struct std::formatter<text::BitmapFormat>: std::formatter<std::string_view>
{
    auto format(text::BitmapFormat value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case text::BitmapFormat::AlphaMask: name = "alpha_mask"; break;
            case text::BitmapFormat::RGB: name = "rgb"; break;
            case text::BitmapFormat::RGBA: name = "rgba"; break;
            case text::BitmapFormat::Outlined: name = "outlined"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<text::GlyphPosition>: std::formatter<std::string>
{
    auto format(text::GlyphPosition const& gpos, auto& ctx) const
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
struct std::formatter<text::RasterizedGlyph>: std::formatter<std::string>
{
    auto format(text::RasterizedGlyph const& glyph, auto& ctx) const
    {
        return formatter<std::string>::format(std::format("RasterizedGlyph({}, {}+{}, {})",
                                                          glyph.index.value,
                                                          glyph.bitmapSize,
                                                          glyph.position,
                                                          glyph.format),
                                              ctx);
    }
};
// }}}
