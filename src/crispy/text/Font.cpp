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
        if (count != 0)
            return maxAdvance / count;

        return 8; // What else would it be.
    }
} // }}}

FT_Face Font::loadFace(ostream* _logger, FT_Library _ft, std::string const& _fontPath, unsigned int _fontSize)
{
    FT_Face face{};
    if (FT_New_Face(_ft, _fontPath.c_str(), 0, &face))
    {
        if (_logger)
            *_logger << fmt::format("Failed to load font: \"{}\"\n", _fontPath);
    }

    FT_Error ec = FT_Select_Charmap(face, FT_ENCODING_UNICODE);
    if (ec && _logger)
        *_logger << fmt::format("FT_Select_Charmap failed. Ignoring; {}\n", freetypeErrorString(ec));

    if (doSetFontSize(_logger, face, _fontSize))
        return face;

    FT_Done_Face(face);
    return nullptr;
}

Font::Font(std::ostream* _logger, FT_Library _ft, FT_Face _face, unsigned int _fontSize, std::string _fontPath) :
    logger_{ _logger },
    ft_{ _ft },
    face_{ _face },
    fontSize_{ _fontSize },
    filePath_{ move(_fontPath) },
    hashCode_{ hash<string>{}(filePath_)}
{
    updateBitmapDimensions();
}

Font::Font(Font&& v) noexcept :
    logger_{ v.logger_ },
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

    logger_ = v.logger_;
    ft_ = v.ft_;
    face_ = v.face_;
    fontSize_ = v.fontSize_;
    maxAdvance_ = v.maxAdvance_;
    bitmapWidth_ = v.bitmapWidth_;
    bitmapHeight_ = v.bitmapHeight_;
    filePath_ = move(v.filePath_);
    hashCode_ = v.hashCode_;

    v.logger_ = nullptr;
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
    {
        auto const missingGlyph = FT_Get_Char_Index(face_, MissingGlyphId);

        if (missingGlyph)
            ec = FT_Load_Glyph(face_, missingGlyph, flags);
        else
            ec = FT_Err_Invalid_Glyph_Index;

        if (ec != FT_Err_Ok)
            throw runtime_error{fmt::format(
                "Error loading glyph index {} for font {}; {}",
                _glyphIndex,
                filePath(),
                freetypeErrorString(ec)
            )};
    }

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

bool Font::doSetFontSize(ostream* _logger, FT_Face _face, unsigned int _fontSize)
{
    if (FT_HAS_COLOR(_face))
    {
        FT_Error const ec = FT_Select_Size(_face, 0); // FIXME i think this one can be omitted?
        if (ec != FT_Err_Ok && _logger)
        {
            *_logger << fmt::format("Failed to FT_Select_Size: {}\n", freetypeErrorString(ec));
            return false;
        }
    }
    else
    {
        FT_Error const ec = FT_Set_Pixel_Sizes(_face, 0, static_cast<FT_UInt>(_fontSize));
        if (ec && _logger)
        {
            *_logger << fmt::format("Failed to FT_Set_Pixel_Sizes: {}\n", freetypeErrorString(ec));
            return false;
        }
    }
    return true;
}

void Font::setFontSize(unsigned int _fontSize)
{
    if (fontSize_ != _fontSize && doSetFontSize(logger_, face_, _fontSize))
    {
        fontSize_ = _fontSize;
        updateBitmapDimensions();

        if (logger_)
        {
            *logger_ << fmt::format(
                "Font({}).setFontSize: {}; bitmap_dim={}x{}; maxAdvance={}\n",
                filePath_,
                fontSize_,
                bitmapWidth_,
                bitmapHeight_,
                maxAdvance_
            );
        }
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
