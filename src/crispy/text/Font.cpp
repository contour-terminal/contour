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
#include <crispy/text/Font.h>
#include <crispy/times.h>
#include <crispy/algorithm.h>
#include <crispy/logger.h>

#include <fmt/format.h>

#include <cctype>
#include <iostream>
#include <map>
#include <stdexcept>

#if defined(__linux__) || defined(__APPLE__)
#define HAVE_FONTCONFIG
#endif

#if defined(HAVE_FONTCONFIG)
#include <fontconfig/fontconfig.h>
#endif

using namespace std;

auto constexpr MissingGlyphId = 0xFFFDu;

namespace crispy::text {

namespace { // {{{ helper functions
    static string freetypeErrorString(FT_Error _errorCode)
    {
        #undef __FTERRORS_H__
        #define FT_ERROR_START_LIST     switch (_errorCode) {
        #define FT_ERRORDEF(e, v, s)    case e: return s;
        #define FT_ERROR_END_LIST       }
        #include FT_ERRORS_H
        return "(Unknown error)";
    }

    /// Computes the maximum horizontal advance for standard 7-bit text.
    int computeMaxAdvance(FT_Face _face)
    {
        FT_Pos maxAdvance = 0;
        for (int i = 32; i < 128; i++)
        {
            if (auto ci = FT_Get_Char_Index(_face, i); ci == FT_Err_Ok)
                if (FT_Load_Glyph(_face, ci, FT_LOAD_DEFAULT) == FT_Err_Ok)
                    maxAdvance = max(maxAdvance, _face->glyph->metrics.horiAdvance);
        }
        return int(ceilf(float(maxAdvance) / 64.0f));
    }
} // }}}

FT_Face Font::loadFace(FT_Library _ft, std::string const& _fontPath, int _fontSize)
{
    FT_Face face{};
    if (FT_New_Face(_ft, _fontPath.c_str(), 0, &face) != FT_Err_Ok)
    {
        debuglog().write("Failed to load font: \"{}\"", _fontPath);
        return nullptr;
    }

    FT_Error ec = FT_Select_Charmap(face, FT_ENCODING_UNICODE);
    if (ec)
        debuglog().write("FT_Select_Charmap failed. Ignoring; {}", freetypeErrorString(ec));

    if (doSetFontSize(face, _fontSize))
        return face;

    FT_Done_Face(face);
    return nullptr;
}

Font::Font(FT_Library _ft, FT_Face _face, int _fontSize, std::string _fontPath) :
    hashCode_{ hash<string>{}(_fontPath)},
    filePath_{ move(_fontPath) },
    ft_{ _ft },
    face_{ _face },
    fontSize_{ _fontSize }
{
    recalculateMetrics();
}

Font::Font(Font&& v) noexcept :
    hashCode_{ v.hashCode_ },
    filePath_{ move(v.filePath_) },
    ft_{ v.ft_ },
    face_{ v.face_ },
    fontSize_{ v.fontSize_ },
    bitmapWidth_{ v.bitmapWidth_ },
    bitmapHeight_{ v.bitmapHeight_ },
    maxAdvance_{ v.maxAdvance_ }
{
    v.ft_ = nullptr;
    v.face_ = nullptr;
    v.fontSize_ = 0;
    v.bitmapWidth_ = 0;
    v.bitmapHeight_ = 0;
    v.filePath_ = {};
    v.hashCode_ = 0;
}

Font& Font::operator=(Font&& v) noexcept
{
    // TODO: free current resources, if any

    ft_ = v.ft_;
    face_ = v.face_;
    fontSize_ = v.fontSize_;
    maxAdvance_ = v.maxAdvance_;
    bitmapWidth_ = v.bitmapWidth_;
    bitmapHeight_ = v.bitmapHeight_;
    filePath_ = move(v.filePath_);
    hashCode_ = v.hashCode_;

    v.ft_ = nullptr;
    v.face_ = nullptr;
    v.fontSize_ = 0;
    v.bitmapWidth_ = 0;
    v.bitmapHeight_ = 0;
    v.filePath_ = {};
    v.hashCode_ = 0;

    return *this;
}

Font::~Font()
{
    if (face_)
        FT_Done_Face(face_);
}

optional<Glyph> Font::loadGlyphByIndex(unsigned _glyphIndex)
{
    FT_Int32 flags = FT_LOAD_DEFAULT;
    if (FT_HAS_COLOR(face_))
        flags |= FT_LOAD_COLOR;

    FT_Error ec = FT_Load_Glyph(face_, _glyphIndex, flags);
    if (ec != FT_Err_Ok)
    {
        auto const missingGlyph = FT_Get_Char_Index(face_, MissingGlyphId);

        if (missingGlyph)
            ec = FT_Load_Glyph(face_, missingGlyph, flags);
        else
            ec = FT_Err_Invalid_Glyph_Index;

        if (ec != FT_Err_Ok)
        {
            if (crispy::logging_sink::for_debug().enabled())
            {
                debuglog().write(
                    "Error loading glyph index {} for font {}; {}",
                    _glyphIndex,
                    filePath(),
                    freetypeErrorString(ec)
                );
            }
            return nullopt;
        }
    }

    auto metrics = GlyphMetrics{};
    metrics.bitmapSize.x = static_cast<int>(face_->glyph->bitmap.width);
    metrics.bitmapSize.y = static_cast<int>(face_->glyph->bitmap.rows);
    metrics.bearing.x = face_->glyph->bitmap_left;
    metrics.bearing.y = face_->glyph->bitmap_top;
    metrics.advance = scaleHorizontal(face_->glyph->advance.x);

    // NB: colored fonts are bitmap fonts, they do not need rendering
    if (!FT_HAS_COLOR(face_))
        if (FT_Render_Glyph(face_->glyph, FT_RENDER_MODE_NORMAL) != FT_Err_Ok)
            return nullopt; // {Glyph{}}; // TODO: why not nullopt?

    auto const width = metrics.bitmapSize.x;
    auto const height = metrics.bitmapSize.y;
    auto const buffer = face_->glyph->bitmap.buffer;

    Bitmap bitmap;
    if (!hasColor())
    {
        assert(face_->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY);
        auto const pitch = face_->glyph->bitmap.pitch;
        bitmap.data.resize(height * width); // 8-bit antialiased alpha channel
        for (int i = 0; i < height; ++i)
            for (int j = 0; j < width; ++j)
                bitmap.data[i * face_->glyph->bitmap.width + j] = buffer[i * pitch + j];
    }
    else
    {
        assert(face_->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA);
        bitmap.data.resize(height * width * 4); // RGBA
        auto t = bitmap.data.begin();

        auto s = buffer;
        for (int i = 0; i < width * height; ++i)
        {
            // BGRA -> RGBA
            *t++ = s[2];
            *t++ = s[1];
            *t++ = s[0];
            *t++ = s[3];
            s += 4;
        }
    }

    return {Glyph{metrics, move(bitmap)}};
}

bool Font::doSetFontSize(FT_Face _face, int _fontSize)
{
    if (FT_HAS_COLOR(_face))
    {
        FT_Error const ec = FT_Select_Size(_face, 0); // FIXME i think this one can be omitted?
        if (ec != FT_Err_Ok)
        {
            debuglog().write("Failed to FT_Select_Size: {}", freetypeErrorString(ec));
            return false;
        }
    }
    else
    {
        FT_Error const ec = FT_Set_Pixel_Sizes(_face, 0, static_cast<FT_UInt>(_fontSize));
        if (ec)
        {
            debuglog().write("Failed to FT_Set_Pixel_Sizes: {}\n", freetypeErrorString(ec));
            return false;
        }
    }
    return true;
}

void Font::setFontSize(int _fontSize)
{
    if (fontSize_ != _fontSize && doSetFontSize(face_, _fontSize))
    {
        fontSize_ = _fontSize;
        recalculateMetrics();
    }
}

void Font::recalculateMetrics()
{
    // update bitmap width/height
    if (FT_IS_SCALABLE(face_))
    {
        bitmapWidth_ = scaleHorizontal(face_->bbox.xMax - face_->bbox.xMin);
        bitmapHeight_ = scaleVertical(face_->bbox.yMax - face_->bbox.yMin);
    }
    else
    {
        bitmapWidth_ = face_->available_sizes[0].width;
        bitmapHeight_ = face_->available_sizes[0].height;
    }

    maxAdvance_ = computeMaxAdvance(face_);
}

int Font::lineHeight() const noexcept
{
    return scaleVertical(face_->height);
}

int Font::baseline() const noexcept
{
    return scaleVertical(face_->height - face_->ascender);
}

int Font::ascender() const noexcept
{
    return scaleVertical(face_->ascender);
}

int Font::descender() const noexcept
{
    return scaleVertical(face_->descender);
}

int Font::underlineOffset() const noexcept
{
    return scaleVertical(face_->underline_position);
}

int Font::underlineThickness() const noexcept
{
    return scaleVertical(face_->underline_thickness);
}

int Font::scaleHorizontal(long _value) const noexcept
{
    return int(ceil(double(FT_MulFix(_value, face_->size->metrics.x_scale)) / 64.0));
}

int Font::scaleVertical(long _value) const noexcept
{
    return int(ceil(double(FT_MulFix(_value, face_->size->metrics.y_scale)) / 64.0));
}

} // end namespace
