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

#include <crispy/reference.h>

#include <fmt/format.h>

#include <cmath>
#include <sstream>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ERRORS_H

#if defined(_MSC_VER)
// XXX purely for IntelliSense
#include <freetype/freetype.h>
#endif

#include <array>
#include <functional>
#include <optional>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace crispy::text {

// TODO: remove FontStyle?
enum class FontStyle {
    Regular = 0,
    Bold = 1,
    Italic = 2,
    BoldItalic = 3,
};

constexpr FontStyle operator|(FontStyle lhs, FontStyle rhs)
{
    return static_cast<FontStyle>(static_cast<unsigned>(lhs) | static_cast<unsigned>(rhs));
}

constexpr FontStyle& operator|=(FontStyle& lhs, FontStyle rhs)
{
    lhs = lhs | rhs;
    return lhs;
}

struct Vec2 {
    int x;
    int y;
};

struct GlobalGlyphMetrics {
    int lineHeight;
    int baseline;
    int maxAdvance;
    int ascender;
    int descender;
};

struct GlyphMetrics {
    Vec2 bitmapSize;        // glyph size in pixels
    Vec2 bearing;           // offset baseline and left to top and left of the glyph's bitmap
    int advance;            // pixels from origin to next glyph's origin (TODO: do i need this?)
};

enum class BitmapFormat {
    Gray,       //!< AA 8-bit alpha channel
    RGBA,       //!< usually colored glyphs (especially emoji)
    LCD,        //!< LCD optimized bitmap for using Subpixel rendering technique
};

struct Bitmap {
    BitmapFormat format;
    int width;
    int height;
    std::vector<uint8_t> data;
};

std::tuple<Bitmap, float> scale(Bitmap const& _bitmap, int _width, int _height);

struct Glyph {
    GlyphMetrics metrics;
    Bitmap bitmap;
};

enum class RenderMode {
    Bitmap, //!< bitmaps are preferred>
    Gray,   //!< gray-scale anti-aliasing
    Light,  //!< gray-scale anti-aliasing for optimized for LCD screens
    LCD,    //!< LCD-optimized anti-aliasing
    Color   //!< embedded color bitmaps are preferred
};

class Font;

/**
 * Represents one Font face along with support for its fallback fonts.
 */
class Font {
  public:
    Font(FT_Library _ft, FT_Face _face, double _fontSize, Vec2 _dpi, std::string _fontPath);
    Font(Font const&) = delete;
    Font& operator=(Font const&) = delete;
    Font(Font&&) noexcept;
    Font& operator=(Font&&) noexcept;
    ~Font();

    std::string const& filePath() const noexcept { return filePath_; }
    std::size_t hashCode() const noexcept { return hashCode_; }

    bool selectSizeForWidth(int _width);

    void setFontSize(double _fontSize);
    double fontSize() const noexcept { return fontSize_; }

    std::string familyName() const
    {
        return face_->family_name;
    }

    FontStyle style() const noexcept
    {
        if (face_->style_flags & (FT_STYLE_FLAG_BOLD | FT_STYLE_FLAG_ITALIC))
            return FontStyle::BoldItalic;
        if (face_->style_flags & FT_STYLE_FLAG_BOLD)
            return FontStyle::Bold;
        if (face_->style_flags & FT_STYLE_FLAG_ITALIC)
            return FontStyle::Italic;
        return FontStyle::Regular;
    }

    bool hasColor() const noexcept { return FT_HAS_COLOR(face_); }

    int bitmapWidth() const noexcept { return bitmapWidth_; }
    int bitmapHeight() const noexcept { return bitmapHeight_; }

    // global metrics
    //

    /// @returns the horizontal gap between two characters.
    int maxAdvance() const noexcept { return maxAdvance_; }

    /// @returns the vertical gap between two baselines.
    int lineHeight() const noexcept;

    /// @returns the basline, i.e. lineHeight - ascender.
    int baseline() const noexcept;

    /// @returns pixels from baseline to bitmap top
    int ascender() const noexcept;

    /// @returns pixels from baseline to bitmap bottom (negative)
    int descender() const noexcept;

    /// @returns pixels of center of underline position, relative to baseline.
    int underlineOffset() const noexcept;
    int underlineThickness() const noexcept;

    bool isFixedWidth() const noexcept { return face_->face_flags & FT_FACE_FLAG_FIXED_WIDTH; }

    // ------------------------------------------------------------------------
    unsigned glyphIndexOfChar(char32_t _char) const noexcept { return FT_Get_Char_Index(face_, _char); }

    std::optional<Glyph> loadGlyphByIndex(unsigned _glyphIndex, RenderMode _renderMode);

    FT_Face face() noexcept { return face_; }

    static FT_Face loadFace(FT_Library _ft, std::string const& _fontPath);

    int scaleHorizontal(long _value) const noexcept;
    int scaleVertical(long _value) const noexcept;

  private:
    void recalculateMetrics();

    // private data
    //
    std::size_t hashCode_;
    std::string filePath_;

    FT_Library ft_;
    FT_Face face_;
    int strikeIndex_;
    double fontSize_;
    Vec2 dpi_;

    int bitmapWidth_ = 0;
    int bitmapHeight_ = 0;
    int maxAdvance_;
};

using FontRef = std::reference_wrapper<Font>;
using FontFallbackList = std::vector<FontRef>;
using FontList = std::pair<FontRef, FontFallbackList>;

} // end namespace

namespace std { // {{{
    template<>
    struct hash<crispy::text::Font> {
        std::size_t operator()(crispy::text::Font const& _font) const noexcept
        {
            return _font.hashCode();
        }
    };
} // }}}

namespace fmt { // {{{
    template <>
    struct formatter<crispy::text::FontStyle> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(crispy::text::FontStyle const& _value, FormatContext& ctx)
        {
            switch (_value)
            {
                case crispy::text::FontStyle::Regular:
                    return format_to(ctx.out(), "regular");
                case crispy::text::FontStyle::Bold:
                    return format_to(ctx.out(), "bold");
                case crispy::text::FontStyle::Italic:
                    return format_to(ctx.out(), "italic");
                case crispy::text::FontStyle::BoldItalic:
                    return format_to(ctx.out(), "bold italic");
            }
            return format_to(ctx.out(), "({})", unsigned(_value));
        }
    };

    template <>
    struct formatter<crispy::text::RenderMode> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(crispy::text::RenderMode const& _mode, FormatContext& ctx)
        {
            switch (_mode)
            {
                case crispy::text::RenderMode::Bitmap:
                    return format_to(ctx.out(), "Bitmap");
                case crispy::text::RenderMode::Gray:
                    return format_to(ctx.out(), "Gray");
                case crispy::text::RenderMode::Light:
                    return format_to(ctx.out(), "RGBA");
                case crispy::text::RenderMode::LCD:
                    return format_to(ctx.out(), "LCD");
                case crispy::text::RenderMode::Color:
                    return format_to(ctx.out(), "Color");
            }
            return format_to(ctx.out(), "({})", unsigned(_mode));
        }
    };

    template <>
    struct formatter<crispy::text::BitmapFormat> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(crispy::text::BitmapFormat const& _format, FormatContext& ctx)
        {
            switch (_format)
            {
                case crispy::text::BitmapFormat::Gray:
                    return format_to(ctx.out(), "Gray");
                case crispy::text::BitmapFormat::RGBA:
                    return format_to(ctx.out(), "RGBA");
                case crispy::text::BitmapFormat::LCD:
                    return format_to(ctx.out(), "LCD");
            }
            return format_to(ctx.out(), "Unknown({})", unsigned(_format));
        }
    };

    template <>
    struct formatter<crispy::text::Vec2> {
        using Vec2 = crispy::text::Vec2;
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(Vec2 const& _v2, FormatContext& ctx)
        {
            return format_to(ctx.out(), "{},{}", _v2.x, _v2.y);
        }
    };

    template <>
    struct formatter<crispy::text::GlyphMetrics> {
        using GlyphMetrics = crispy::text::GlyphMetrics;
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(GlyphMetrics const& _gm, FormatContext& ctx)
        {
            return format_to(ctx.out(), "bitmapSize:{}, bearing:{}, advance:{}",
                                        _gm.bitmapSize,
                                        _gm.bearing,
                                        _gm.advance);
        }
    };
} // }}}
