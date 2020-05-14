/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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

namespace crispy::text {

namespace {
    static string freetypeErrorString(FT_Error _errorCode)
    {
        #undef __FTERRORS_H__
        #define FT_ERROR_START_LIST     switch (_errorCode) {
        #define FT_ERRORDEF(e, v, s)    case e: return s;
        #define FT_ERROR_END_LIST       }
        #include FT_ERRORS_H
        return "(Unknown error)";
    }

    unsigned computeMaxAdvance(FT_Face _face)
    {
        if (FT_Load_Char(_face, 'M', FT_LOAD_BITMAP_METRICS_ONLY) == FT_Err_Ok)
            return _face->glyph->advance.x >> 6;

        unsigned long long maxAdvance = 0;
        unsigned count = 0;
        for (unsigned glyphIndex = 0; glyphIndex < _face->num_glyphs; ++glyphIndex)
        {
            if (FT_Load_Glyph(_face, glyphIndex, FT_LOAD_BITMAP_METRICS_ONLY) == FT_Err_Ok)// FT_LOAD_BITMAP_METRICS_ONLY);
            {
                maxAdvance += _face->glyph->advance.x >> 6;
                count++;
            }
        }
        return maxAdvance / count;
    }
}

Font::Font(FT_Library _ft, std::string _fontPath, unsigned int _fontSize) :
    ft_{ _ft },
    face_{},
    fontSize_{ 0 },
    filePath_{ move(_fontPath) },
    hashCode_{ hash<string>{}(filePath_)}
{
    if (FT_New_Face(ft_, filePath_.c_str(), 0, &face_))
        throw runtime_error{ "Failed to load font." };

    FT_Error ec = FT_Select_Charmap(face_, FT_ENCODING_UNICODE);
    if (ec)
        throw runtime_error{ string{"Failed to set charmap. "} + freetypeErrorString(ec) };

    setFontSize(_fontSize);

    loadGlyphByIndex(0);
    // XXX Woot, needed in order to retrieve maxAdvance()'s field,
    // as max_advance metric seems to be broken on at least FiraCode (Regular),
    // which is twice as large as it should be, but taking
    // a regular face's advance value works.
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

GlyphBitmap Font::loadGlyphByIndex(unsigned int _glyphIndex)
{
    FT_Int32 flags = FT_LOAD_DEFAULT;
    if (FT_HAS_COLOR(face_))
        flags |= FT_LOAD_COLOR;

    FT_Error ec = FT_Load_Glyph(face_, _glyphIndex, flags);
    if (ec != FT_Err_Ok)
        throw runtime_error{ string{"Error loading glyph. "} + freetypeErrorString(ec) };

    // NB: colored fonts are bitmap fonts, they do not need rendering
    if (!FT_HAS_COLOR(face_))
        if (FT_Render_Glyph(face_->glyph, FT_RENDER_MODE_NORMAL) != FT_Err_Ok)
            return GlyphBitmap{};

    auto const width = face_->glyph->bitmap.width;
    auto const height = face_->glyph->bitmap.rows;
    auto const buffer = face_->glyph->bitmap.buffer;

    vector<uint8_t> bitmap;
    if (!hasColor())
    {
        auto const pitch = face_->glyph->bitmap.pitch;
        bitmap.resize(height * width);
        for (unsigned i = 0; i < height; ++i)
            for (unsigned j = 0; j < face_->glyph->bitmap.width; ++j)
                bitmap[i * face_->glyph->bitmap.width + j] = buffer[i * pitch + j];
    }
    else
    {
        bitmap.resize(height * width * 4);
        copy(
            buffer,
            buffer + height * width * 4,
            bitmap.begin()
        );
    }

    return GlyphBitmap{
        width,
        height,
        move(bitmap)
    };
}

void Font::setFontSize(unsigned int _fontSize)
{
    if (fontSize_ != _fontSize)
    {
        if (hasColor())
        {
            // FIXME i think this one can be omitted?
            FT_Error const ec = FT_Select_Size(face_, 0);
            if (ec != FT_Err_Ok)
                throw runtime_error{fmt::format("Failed to FT_Select_Size. {}", freetypeErrorString(ec))};
        }
        else
        {
            FT_Error const ec = FT_Set_Pixel_Sizes(face_, 0, static_cast<FT_UInt>(_fontSize));
            if (ec)
                throw runtime_error{ string{"Failed to set font pixel size. "} + freetypeErrorString(ec) };
        }

        fontSize_ = _fontSize;

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

        loadGlyphByIndex(0);
    }
}

} // end namespace
