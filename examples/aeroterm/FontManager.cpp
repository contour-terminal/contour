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
#include "FontManager.h"

#include <harfbuzz/hb.h>
#include <harfbuzz/hb-ft.h>

#include <cctype>

#if defined(__linux__)
#define HAVE_FONTCONFIG
#endif

#if defined(HAVE_FONTCONFIG)
#include <fontconfig/fontconfig.h>
#endif

using namespace std;

static string freetypeErrorString(FT_Error _errorCode)
{
    #undef __FTERRORS_H__
    #define FT_ERROR_START_LIST     switch (_errorCode) {
    #define FT_ERRORDEF(e, v, s)    case e: return s;
    #define FT_ERROR_END_LIST       }
    #include FT_ERRORS_H
    return "(Unknown error)";
}

static bool endsWidthIgnoreCase(string const& _text, string const& _suffix)
{
    if (_text.size() < _suffix.size())
        return false;

    auto const* text = &_text[_text.size() - _suffix.size()];
    for (size_t i = 0; i < _suffix.size(); ++i)
        if (tolower(text[i]) != tolower(_suffix[i]))
            return false;

    return true;
}

static string getFontFilePath([[maybe_unused]] string const& _fontPattern)
{
    if (endsWidthIgnoreCase(_fontPattern, ".ttf") || endsWidthIgnoreCase(_fontPattern, ".otf"))
        return _fontPattern;

    #if defined(HAVE_FONTCONFIG)
    string const pattern = _fontPattern; // TODO: append bold/italic if needed

    FcConfig* fcConfig = FcInitLoadConfigAndFonts();
    FcPattern* fcPattern = FcNameParse((FcChar8 const*) pattern.c_str());

    FcDefaultSubstitute(fcPattern);
    FcConfigSubstitute(fcConfig, fcPattern, FcMatchPattern);

    FcResult fcResult;
    FcPattern* matchedPattern = FcFontMatch(fcConfig, fcPattern, &fcResult);
    auto path = string{};
    if (fcResult == FcResultMatch && matchedPattern)
    {
        char* resultPath{};
        if (FcPatternGetString(matchedPattern, FC_FILE, 0, (FcChar8**) &resultPath) == FcResultMatch)
            path = string{resultPath};
        FcPatternDestroy(matchedPattern);
    }
    FcPatternDestroy(fcPattern);
    FcConfigDestroy(fcConfig);
    return path;
    #endif

    #if defined(_WIN32)
    // TODO: Read https://docs.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-enumfontfamiliesexa
    // This is pretty damn hard coded, and to be properly implemented once the other font related code's done,
    // *OR* being completely deleted when FontConfig's windows build fix released and available via vcpkg.
    if (_fontPattern.find("bold italic") != string::npos)
        return "C:\\Windows\\Fonts\\consolaz.ttf";
    else if (_fontPattern.find("italic") != string::npos)
        return "C:\\Windows\\Fonts\\consolai.ttf";
    else if (_fontPattern.find("bold") != string::npos)
        return "C:\\Windows\\Fonts\\consolab.ttf";
    else
        return "C:\\Windows\\Fonts\\consola.ttf";
    #endif
}

FontManager::FontManager()
{
    if (FT_Init_FreeType(&ft_))
        throw runtime_error{ "Failed to initialize FreeType." };
}

FontManager::~FontManager()
{
    fonts_.clear();
    FT_Done_FreeType(ft_);
}

Font& FontManager::load(string const& _fontPattern, unsigned int _fontSize)
{
    string const filePath = getFontFilePath(_fontPattern);

    if (auto i = fonts_.find(filePath); i != fonts_.end())
        return i->second;

    return fonts_.emplace(make_pair(filePath, Font{ ft_, filePath, _fontSize })).first->second;
}

// -------------------------------------------------------------------------------------------------------

Font::Font(FT_Library _ft, std::string const& _fontPath, unsigned int _fontSize) :
    ft_{ _ft },
    face_{},
    hb_font_{},
    hb_buf_{},
    fontSize_{ _fontSize }
{
    if (FT_New_Face(ft_, _fontPath.c_str(), 0, &face_))
        throw runtime_error{ "Failed to load font." };

    FT_Error ec = FT_Select_Charmap(face_, FT_ENCODING_UNICODE);
    if (ec)
        throw runtime_error{ string{"Failed to set charmap. "} + freetypeErrorString(ec) };

    ec = FT_Set_Pixel_Sizes(face_, 0, static_cast<FT_UInt>(_fontSize));
    if (ec)
        throw runtime_error{ string{"Failed to set font pixel size. "} + freetypeErrorString(ec) };

    hb_font_ = hb_ft_font_create_referenced(face_);
    hb_buf_ = hb_buffer_create();

    loadGlyphByIndex(0);
    // XXX Woot, needed in order to retrieve maxAdvance()'s field,
    // as max_advance metric seems to be broken on at least FiraCode (Regular),
    // which is twice as large as it should be, but taking
    // a regular face's advance value works.
}

Font::Font(Font&& v) :
    ft_{ v.ft_ },
    face_{ v.face_ },
    hb_font_{ v.hb_font_ },
    hb_buf_{ v.hb_buf_ },
    fontSize_{ v.fontSize_ }
{
    v.ft_ = nullptr;
    v.face_ = nullptr;
    v.hb_font_ = nullptr;
    v.hb_buf_ = nullptr;
    v.fontSize_ = 0;
}

Font& Font::operator=(Font&& v)
{
    // TODO: free current resources, if any

    ft_ = v.ft_;
    face_ = v.face_;
    hb_font_ = v.hb_font_;
    hb_buf_ = v.hb_buf_;
    fontSize_ = v.fontSize_;

    v.ft_ = nullptr;
    v.face_ = nullptr;
    v.hb_font_ = nullptr;
    v.hb_buf_ = nullptr;
    v.fontSize_ = 0;

    return *this;
}

Font::~Font()
{
    if (face_)
        FT_Done_Face(face_);

    if (hb_font_)
        hb_font_destroy(hb_font_);

    if (hb_buf_)
        hb_buffer_destroy(hb_buf_);
}

void Font::loadGlyphByIndex(unsigned int _glyphIndex)
{
    FT_Error ec = FT_Load_Glyph(face_, _glyphIndex, FT_LOAD_RENDER);
    if (ec != FT_Err_Ok)
        throw runtime_error{ string{"Error loading glyph. "} + freetypeErrorString(ec) };
}

void Font::render(vector<char32_t> const& _chars, vector<Font::GlyphPosition>& _result)
{
    hb_buffer_clear_contents(hb_buf_);
    hb_buffer_add_utf32(hb_buf_, (uint32_t const*)_chars.data(), _chars.size(), 0, _chars.size());
    hb_buffer_set_direction(hb_buf_, HB_DIRECTION_LTR);
    hb_buffer_guess_segment_properties(hb_buf_);

    hb_shape(hb_font_, hb_buf_, nullptr, 0);

    unsigned const len = hb_buffer_get_length(hb_buf_);
    hb_glyph_info_t* info = hb_buffer_get_glyph_infos(hb_buf_, nullptr);
    hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(hb_buf_, nullptr);

    unsigned int cx = 0;
    unsigned int cy = 0;
    unsigned int advance = 0; // not yet exposed nor needed on caller side
    for (unsigned i = 0; i < len; ++i)
    {
        _result.emplace_back(GlyphPosition{
            cx + (pos[i].x_offset >> 6),
            cy + (pos[i].y_offset >> 6),
            info[i].codepoint
        });

        cx += maxAdvance(), // Ought to be (pos[i].x_advance / 64), but that breaks on some font sizes it seems.
        cy += pos[i].y_advance >> 6;
        advance += pos[i].x_advance >> 6;
    }
}
