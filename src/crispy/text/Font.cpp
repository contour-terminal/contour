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

tuple<Bitmap, float> scale(Bitmap const& _bitmap, int _width, int _height)
{
    assert(_bitmap.format == BitmapFormat::RGBA);
    // assert(_bitmap.width <= _width);
    // assert(_bitmap.height <= _height);

    auto const ratioX = float(_bitmap.width) / float(_width);
    auto const ratioY = float(_bitmap.height) / float(_height);
    auto const ratio = max(ratioX, ratioY);
    auto const factor = int(ceilf(ratio));

    vector<uint8_t> dest;
    dest.resize(_height * _width * 4);

    debuglog().write("scale: from {}x{} to {}x{}, ratio {}x{} ({}), factor {}",
            _bitmap.width, _bitmap.height,
            _width, _height,
            ratioX, ratioY, ratio, factor);

    // std::cerr << fmt::format("scale: from {}x{} to {}x{}, ratio {}x{} ({}), factor {}\n",
    //         _bitmap.width, _bitmap.height,
    //         _width, _height,
    //         ratioX, ratioY, ratio, factor);

#if 1
    uint8_t* d = dest.data();
    for (int i = 0, sr = 0; i < _height; i++, sr += factor)
    {
        for (int j = 0, sc = 0; j < _width; j++, sc += factor, d += 4)
        {
            // calculate area average
            unsigned int r = 0, g = 0, b = 0, a = 0, count = 0;
            for (int y = sr; y < min(sr + factor, _bitmap.height); y++)
            {
                uint8_t const* p = _bitmap.data.data() + (y * _bitmap.width * 4) + sc * 4;
                for (int x = sc; x < min(sc + factor, _bitmap.width); x++, count++)
                {
                    b += *(p++);
                    g += *(p++);
                    r += *(p++);
                    a += *(p++);
                }
            }

            if (count)
            {
                d[0] = b / count;
                d[1] = g / count;
                d[2] = r / count;
                d[3] = a / count;
            }
        }
    }
#else
    dest = _bitmap.data;
#endif

    auto output = Bitmap{};
    output.format = _bitmap.format;
    output.width = _width;
    output.height = _height;
    output.data = move(dest);

    return {output, factor};
}

FT_Face Font::loadFace(FT_Library _ft, std::string const& _fontPath)
{
    FT_Face face{};
    if (FT_New_Face(_ft, _fontPath.c_str(), 0, &face) != FT_Err_Ok)
    {
        debuglog().write("Failed to load font: \"{}\"", _fontPath);
        return nullptr;
    }

    return face;
}

Font::Font(FT_Library _ft, FT_Face _face, double _fontSize, Vec2 _dpi, std::string _fontPath) :
    hashCode_{ hash<string>{}(_fontPath)},
    filePath_{ move(_fontPath) },
    ft_{ _ft },
    face_{ _face },
    strikeIndex_{ 0 },
    fontSize_{ 0 },
    dpi_{ _dpi }
{
    if (FT_Error const ec = FT_Select_Charmap(face_, FT_ENCODING_UNICODE); ec != FT_Err_Ok)
        debuglog().write("FT_Select_Charmap failed. Ignoring; {}", freetypeErrorString(ec));

    setFontSize(_fontSize);
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
            bitmap.width = width;
            bitmap.height = height;
            bitmap.data.resize(height * width); // 8-bit channel (with values 0 or 255)

            auto const pitch = abs(ftBitmap.pitch);
            for (auto const i : crispy::times(ftBitmap.rows))
                for (auto const j : crispy::times(ftBitmap.width))
                    bitmap.data[i * width + j] = ftBitmap.buffer[(height - 1 - i) * pitch + j] * 255;

            FT_Bitmap_Done(ft_, &ftBitmap);
            break;
        }
        case FT_PIXEL_MODE_GRAY:
        {
            auto const width = metrics.bitmapSize.x;
            auto const height = metrics.bitmapSize.y;

            bitmap.format = BitmapFormat::Gray;
            bitmap.width = width;
            bitmap.height = height;
            bitmap.data.resize(height * width);

            auto const pitch = face_->glyph->bitmap.pitch;
            auto const s = face_->glyph->bitmap.buffer;
            for (auto const i : crispy::times(height))
                for (auto const j : crispy::times(width))
                    bitmap.data[i * width + j] = s[(height - 1 - i) * pitch + j];
            break;
        }
        case FT_PIXEL_MODE_LCD:
        {
#if 1
            auto const width = face_->glyph->bitmap.width;
            auto const height = face_->glyph->bitmap.rows;
            assert(width == unsigned(metrics.bitmapSize.x));

            bitmap.format = BitmapFormat::LCD;
            bitmap.width = int(width / 3);
            bitmap.height = height;
            bitmap.data.resize(height * width);
            metrics.bitmapSize.x /= 3;

            auto const pitch = face_->glyph->bitmap.pitch;
            auto s = face_->glyph->bitmap.buffer;
            // for (auto const [i, j] : crispy::times2D(height, width))
            for (auto const i : crispy::times(height))
                for (auto const j : crispy::times(width))
                    bitmap.data[i * width + j] = s[(height - 1 - i) * pitch + j];
#else
            // This code path converts the LCD RGB image into an RGBA image
            auto const width = face_->glyph->bitmap.width / 3;
            auto const height = face_->glyph->bitmap.rows;

            bitmap.format = BitmapFormat::RGBA;
            bitmap.width = width;
            bitmap.height = height;
            bitmap.data.resize(height * width * 4);

            // that is interesting, that FT_PIXEL_MODE_LCD's width accounts for each color component
            metrics.bitmapSize.x /= 3;

            auto const pitch = face_->glyph->bitmap.pitch;
            auto s = face_->glyph->bitmap.buffer;
            auto t = bitmap.data.begin();

            for (auto i = 0u; i < height; ++i)
            {
                for (auto j = 0u; j < width; ++j)
                {
                    auto const [r, g, b] = tuple{s[j * 3 + 0], s[j * 3 + 1], s[j * 3 + 2]};
                    auto const a = int(ceil(r + g + b) / 3.0);
                    *t++ = r;
                    *t++ = g;
                    *t++ = b;
                    *t++ = a;
                }
                s += pitch;
            }
#endif
            break;
        }
        case FT_PIXEL_MODE_BGRA:
        {
            auto const width = metrics.bitmapSize.x;
            auto const height = metrics.bitmapSize.y;
            assert(unsigned(width) == face_->glyph->bitmap.width);
            assert(unsigned(height) == face_->glyph->bitmap.rows);

            bitmap.format = BitmapFormat::RGBA;
            bitmap.width = width;
            bitmap.height = height;
            bitmap.data.resize(height * width * 4);
            auto t = bitmap.data.begin();

            auto const pitch = face_->glyph->bitmap.pitch;
            for (auto const i : crispy::times(height))
            {
                for (auto const j : crispy::times(width))
                {
                    auto const s = &face_->glyph->bitmap.buffer[(height - i - 1) * pitch + j * 4];

                    // BGRA -> RGBA
                    *t++ = s[2];
                    *t++ = s[1];
                    *t++ = s[0];
                    *t++ = s[3];
                }
            }

            break;
        }
        default:
            debuglog().write("Glyph requested that has an unsupported pixel_mode:{}", face_->glyph->bitmap.pixel_mode);
            return nullopt;
    }

    return {Glyph{metrics, move(bitmap)}};
}

bool Font::selectSizeForWidth(int _width) // or call it: selectStrikeIndexForWidth(int _width))
{
    int best = 0, diff = std::numeric_limits<int>::max();
    for (int i = 0; i < face_->num_fixed_sizes; ++i)
    {
        auto const width = face_->available_sizes[i].width;
        auto const d = width > _width ? width - _width
                                      : _width - width;
        if (d < diff) {
            diff = d;
            best = i;
        }
    }

    strikeIndex_ = best;

    if (face_->num_fixed_sizes > 1)
        debuglog().write("set strike index to {} (total: {}) for font {}",
                         best, face_->num_fixed_sizes, filePath_);

    FT_Error const ec = FT_Select_Size(face_, best);
    if (ec != FT_Err_Ok)
        debuglog().write("Failed to FT_Select_Size: {}", freetypeErrorString(ec));

    return ec == FT_Err_Ok;
}

void Font::setFontSize(double _fontSize)
{
    if (fontSize_ == _fontSize)
        return;

    if (FT_HAS_COLOR(face_))
    {
        if (FT_Error const ec = FT_Select_Size(face_, strikeIndex_); ec != FT_Err_Ok)
        {
            debuglog().write("Failed to FT_Select_Size: {}", freetypeErrorString(ec));
        }
    }
    else
    {
        auto const size = static_cast<FT_F26Dot6>(ceil(_fontSize * 64.0));
        if (FT_Error const ec = FT_Set_Char_Size(face_, size, size, dpi_.x, dpi_.y); ec != FT_Err_Ok)
        {
            debuglog().write("Failed to FT_Set_Pixel_Sizes: {}\n", freetypeErrorString(ec));
        }
    }

    fontSize_ = _fontSize;

    // recalculate metrics

    // update bitmap width/height
    if (FT_IS_SCALABLE(face_))
    {
        bitmapWidth_ = scaleHorizontal(face_->bbox.xMax - face_->bbox.xMin);
        bitmapHeight_ = scaleVertical(face_->bbox.yMax - face_->bbox.yMin);
    }
    else
    {
        bitmapWidth_ = face_->available_sizes[strikeIndex_].width;
        bitmapHeight_ = face_->available_sizes[strikeIndex_].height;
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
