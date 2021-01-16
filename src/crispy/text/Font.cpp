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

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_LCD_FILTER_H
#include FT_BITMAP_H

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

FT_Face Font::loadFace(FT_Library _ft, std::string const& _fontPath, double _fontSize, Vec2 _dpi)
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

    if (doSetFontSize(face, _fontSize, _dpi))
        return face;

    FT_Done_Face(face);
    return nullptr;
}

Font::Font(FT_Library _ft, FT_Face _face, double _fontSize, Vec2 _dpi, std::string _fontPath) :
    hashCode_{ hash<string>{}(_fontPath)},
    filePath_{ move(_fontPath) },
    ft_{ _ft },
    face_{ _face },
    fontSize_{ _fontSize },
    dpi_{ _dpi }
{
    recalculateMetrics();
}

Font::Font(Font&& v) noexcept :
    hashCode_{ v.hashCode_ },
    filePath_{ move(v.filePath_) },
    ft_{ v.ft_ },
    face_{ v.face_ },
    fontSize_{ v.fontSize_ },
    dpi_{ v.dpi_ },
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
    dpi_ = v.dpi_;
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

optional<Glyph> Font::loadGlyphByIndex(unsigned _glyphIndex, RenderMode _renderMode)
{
    FT_Int32 flags = 0;
    switch (_renderMode)
    {
        case RenderMode::Bitmap:
            flags |= FT_LOAD_MONOCHROME;
            break;
        case RenderMode::Gray:
            flags |= FT_LOAD_DEFAULT;
            break;
        case RenderMode::Light:
            flags |= FT_LOAD_TARGET_LIGHT;
            break;
        case RenderMode::LCD:
            flags |= FT_LOAD_TARGET_LCD;
            break;
        case RenderMode::Color:
            if (FT_HAS_COLOR(face_))
                flags = FT_LOAD_COLOR;
            break;
    }

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
    {
        auto ftRenderMode = [&]() -> FT_Render_Mode {
            switch (_renderMode)
            {
                case RenderMode::Bitmap: return FT_RENDER_MODE_MONO;
                case RenderMode::Gray:   return FT_RENDER_MODE_NORMAL;
                case RenderMode::Light:  return FT_RENDER_MODE_LIGHT;
                case RenderMode::LCD:    return FT_RENDER_MODE_LCD;
                case RenderMode::Color:  return FT_RENDER_MODE_NORMAL;
                    break;
            }
            return FT_RENDER_MODE_NORMAL;
        }();
        if (FT_Render_Glyph(face_->glyph, ftRenderMode) != FT_Err_Ok)
            return nullopt;
    }

    auto bitmap = Bitmap{};
    switch (face_->glyph->bitmap.pixel_mode)
    {
        case FT_PIXEL_MODE_MONO:
        {
            auto const width = metrics.bitmapSize.x;
            auto const height = metrics.bitmapSize.y;

            // convert mono to gray
            FT_Bitmap ftBitmap;
            FT_Bitmap_Init(&ftBitmap);

            auto const ec = FT_Bitmap_Convert(ft_, &face_->glyph->bitmap, &ftBitmap, 1);
            if (ec != FT_Err_Ok)
                return nullopt;

            ftBitmap.num_grays = 256;

            bitmap.format = BitmapFormat::Gray;
            bitmap.data.resize(height * width); // 8-bit channel (with values 0 or 255)

            auto const pitch = abs(ftBitmap.pitch);
            for (auto const i : crispy::times(ftBitmap.rows))
                for (auto const j : crispy::times(ftBitmap.width))
                    bitmap.data[i * width + j] = ftBitmap.buffer[i * pitch + j] * 255;

            FT_Bitmap_Done(ft_, &ftBitmap);
            break;
        }
        case FT_PIXEL_MODE_GRAY:
        {
            auto const width = metrics.bitmapSize.x;
            auto const height = metrics.bitmapSize.y;

            bitmap.format = BitmapFormat::Gray;
            bitmap.data.resize(height * width);

            auto const pitch = face_->glyph->bitmap.pitch;
            auto const s = face_->glyph->bitmap.buffer;
            for (auto const i : crispy::times(height))
                for (auto const j : crispy::times(width))
                    bitmap.data[i * width + j] = s[i * pitch + j];
            break;
        }
        case FT_PIXEL_MODE_LCD:
        {
            auto const width = face_->glyph->bitmap.width;
            auto const height = face_->glyph->bitmap.rows;
            assert(width == unsigned(metrics.bitmapSize.x));

            bitmap.format = BitmapFormat::LCD;
            bitmap.data.resize(height * width);
            metrics.bitmapSize.x /= 3;

            auto const pitch = face_->glyph->bitmap.pitch;
            auto s = face_->glyph->bitmap.buffer;
            // for (auto const [i, j] : crispy::times2D(height, width))
            for (auto const i : crispy::times(height))
                for (auto const j : crispy::times(width))
                    bitmap.data[i * width + j] = s[i * pitch + j];
            break;
        }
        case FT_PIXEL_MODE_BGRA:
        {
            auto const width = metrics.bitmapSize.x;
            auto const height = metrics.bitmapSize.y;

            bitmap.format = BitmapFormat::RGBA;
            bitmap.data.resize(height * width * 4);
            auto t = bitmap.data.begin();
            auto s = face_->glyph->bitmap.buffer;

            crispy::for_each(crispy::times(0, width * height, 4), [&](auto) {
                // BGRA -> RGBA
                *t++ = s[2];
                *t++ = s[1];
                *t++ = s[0];
                *t++ = s[3];
                s += 4;
            });
            break;
        }
        default:
            debuglog().write("Glyph requested that has an unsupported pixel_mode:{}", face_->glyph->bitmap.pixel_mode);
            return nullopt;
    }

    return {Glyph{metrics, move(bitmap)}};
}

bool Font::doSetFontSize(FT_Face _face, double _fontSize, Vec2 _dpi)
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
        auto const size = static_cast<FT_F26Dot6>(ceil(_fontSize * 64.0));
        FT_Error const ec = FT_Set_Char_Size(_face, size, size, _dpi.x, _dpi.y);
        if (ec)
        {
            debuglog().write("Failed to FT_Set_Pixel_Sizes: {}\n", freetypeErrorString(ec));
            return false;
        }
    }
    return true;
}

void Font::setFontSize(double _fontSize)
{
    if (fontSize_ != _fontSize && doSetFontSize(face_, _fontSize, dpi_))
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
