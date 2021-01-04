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

    int computeMaxAdvance(FT_Face _face)
    {
        if (FT_Load_Char(_face, 'M', FT_LOAD_BITMAP_METRICS_ONLY) == FT_Err_Ok)
            return _face->glyph->advance.x >> 6;

        long long maxAdvance = 0;
        int count = 0;
        for (FT_Long glyphIndex = 0; glyphIndex < _face->num_glyphs; ++glyphIndex)
        {
            if (FT_Load_Glyph(_face, glyphIndex, FT_LOAD_BITMAP_METRICS_ONLY) == FT_Err_Ok)// FT_LOAD_BITMAP_METRICS_ONLY);
            {
                maxAdvance += _face->glyph->advance.x >> 6;
                count++;
            }
        }
        if (count != 0)
            return static_cast<int>(maxAdvance / count);

        return 8; // What else would it be.
    }
} // }}}

FT_Face Font::loadFace(FT_Library _ft, std::string const& _fontPath, int _fontSize)
{
    FT_Face face{};
    if (FT_New_Face(_ft, _fontPath.c_str(), 0, &face))
        debuglog().write("Failed to load font: \"{}\"", _fontPath);

    FT_Error ec = FT_Select_Charmap(face, FT_ENCODING_UNICODE);
    if (ec)
        debuglog().write("FT_Select_Charmap failed. Ignoring; {}", freetypeErrorString(ec));

    if (doSetFontSize(face, _fontSize))
        return face;

    FT_Done_Face(face);
    return nullptr;
}

Font::Font(FT_Library _ft, FT_Face _face, int _fontSize, std::string _fontPath) :
    ft_{ _ft },
    face_{ _face },
    fontSize_{ _fontSize },
    filePath_{ move(_fontPath) },
    hashCode_{ hash<string>{}(filePath_)}
{
    updateBitmapDimensions();
}

Font::Font(Font&& v) noexcept :
    ft_{ v.ft_ },
    face_{ v.face_ },
    fontSize_{ v.fontSize_ },
    bitmapWidth_{ v.bitmapWidth_ },
    bitmapHeight_{ v.bitmapHeight_ },
    maxAdvance_{ v.maxAdvance_ },
    filePath_{ move(v.filePath_) },
    hashCode_{ v.hashCode_ }
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

#define LIBTERMINAL_VIEW_NATURAL_COORDS 1

optional<GlyphBitmap> Font::loadGlyphByIndex(int _glyphIndex)
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

    // NB: colored fonts are bitmap fonts, they do not need rendering
    if (!FT_HAS_COLOR(face_))
        if (FT_Render_Glyph(face_->glyph, FT_RENDER_MODE_NORMAL) != FT_Err_Ok)
            return {GlyphBitmap{}};

    auto const width = static_cast<int>(face_->glyph->bitmap.width);
    auto const height = static_cast<int>(face_->glyph->bitmap.rows);
    auto const buffer = face_->glyph->bitmap.buffer;

    vector<uint8_t> bitmap;
    if (!hasColor())
    {
        auto const pitch = face_->glyph->bitmap.pitch;
        bitmap.resize(height * width);
        for (int i = 0; i < height; ++i)
            for (int j = 0; j < width; ++j)
#if defined(LIBTERMINAL_VIEW_NATURAL_COORDS) && LIBTERMINAL_VIEW_NATURAL_COORDS
                bitmap[i * face_->glyph->bitmap.width + j] = buffer[i * pitch + j];
#else
                bitmap[(height - i - 1) * face_->glyph->bitmap.width + j] = buffer[i * pitch + j];
#endif
    }
    else
    {
        bitmap.resize(height * width * 4);
        auto t = bitmap.begin();
#if defined(LIBTERMINAL_VIEW_NATURAL_COORDS) && LIBTERMINAL_VIEW_NATURAL_COORDS
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
#else
        for (int y = 0; y < height; ++y)
        {
            auto s = buffer + (height - y - 1) * width * 4;
            for (int x = 0; x < width * 4; x += 4)
            {
                // BGRA -> RGBA
                *t++ = s[2];
                *t++ = s[1];
                *t++ = s[0];
                *t++ = s[3];
                s += 4;
            }
        }
#endif
    }

    return {GlyphBitmap{
        width,
        height,
        move(bitmap)
    }};
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
        updateBitmapDimensions();
    }
}

void Font::updateBitmapDimensions()
{
    // update bitmap width/height
    if (FT_IS_SCALABLE(face_))
    {
        bitmapWidth_ = FT_MulFix(face_->bbox.xMax - face_->bbox.xMin, face_->size->metrics.x_scale) >> 6;
        bitmapHeight_ = FT_MulFix(face_->bbox.yMax - face_->bbox.yMin, face_->size->metrics.y_scale) >> 6;
    }
    else
    {
        bitmapWidth_ = (face_->available_sizes[0].width);
        bitmapHeight_ = (face_->available_sizes[0].height);
    }

    maxAdvance_ = computeMaxAdvance(face_);
}

} // end namespace
