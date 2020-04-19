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
#include <terminal_view/FontManager.h>
#include <terminal/util/times.h>
#include <terminal/util/algorithm.h>
#include <fmt/format.h>

#include <harfbuzz/hb.h>
#include <harfbuzz/hb-ft.h>

#include <stdexcept>
#include <cctype>

#if defined(__linux__) || defined(__APPLE__)
#define HAVE_FONTCONFIG
#endif

#if defined(HAVE_FONTCONFIG)
#include <fontconfig/fontconfig.h>
#endif

using namespace std;

namespace terminal::view {

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

    static bool endsWithIgnoreCase(string const& _text, string const& _suffix)
    {
        if (_text.size() < _suffix.size())
            return false;

        auto const* text = &_text[_text.size() - _suffix.size()];
        for (size_t i = 0; i < _suffix.size(); ++i)
            if (tolower(text[i]) != tolower(_suffix[i]))
                return false;

        return true;
    }

    static vector<string> getFontFilePaths([[maybe_unused]] string const& _fontPattern)
    {
        if (endsWithIgnoreCase(_fontPattern, ".ttf") || endsWithIgnoreCase(_fontPattern, ".otf"))
            return {_fontPattern};

        #if defined(HAVE_FONTCONFIG)
        string const& pattern = _fontPattern; // TODO: append bold/italic if needed

        FcConfig* fcConfig = FcInitLoadConfigAndFonts();
        FcPattern* fcPattern = FcNameParse((FcChar8 const*) pattern.c_str());

        FcDefaultSubstitute(fcPattern);
        FcConfigSubstitute(fcConfig, fcPattern, FcMatchPattern);

        FcResult fcResult = FcResultNoMatch;

        vector<string> paths;

        // find exact match
        FcPattern* matchedPattern = FcFontMatch(fcConfig, fcPattern, &fcResult);
        optional<string> primaryFontPath;
        if (fcResult == FcResultMatch && matchedPattern)
        {
            char* resultPath{};
            if (FcPatternGetString(matchedPattern, FC_FILE, 0, (FcChar8**) &resultPath) == FcResultMatch)
            {
                primaryFontPath = resultPath;
                paths.emplace_back(*primaryFontPath);
            }
            FcPatternDestroy(matchedPattern);
        }

        // find fallback fonts
        FcCharSet* fcCharSet = nullptr;
        FcFontSet* fcFontSet = FcFontSort(fcConfig, fcPattern, /*trim*/FcTrue, &fcCharSet, &fcResult);
        for (int i = 0; i < fcFontSet->nfont; ++i)
        {
            FcChar8* fcFile = nullptr;
            if (FcPatternGetString(fcFontSet->fonts[i], FC_FILE, 0, &fcFile) == FcResultMatch)
            {
                // FcBool fcColor = false;
                // FcPatternGetBool(fcFontSet->fonts[i], FC_COLOR, 0, &fcColor);
                if (fcFile && primaryFontPath && *primaryFontPath != (char const*) fcFile)
                    paths.emplace_back((char const*) fcFile);
            }
        }
        FcFontSetDestroy(fcFontSet);
        FcCharSetDestroy(fcCharSet);

        FcPatternDestroy(fcPattern);
        FcConfigDestroy(fcConfig);
        return paths;
        #endif

        #if defined(_WIN32)
        // TODO: Read https://docs.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-enumfontfamiliesexa
        // This is pretty damn hard coded, and to be properly implemented once the other font related code's done,
        // *OR* being completely deleted when FontConfig's windows build fix released and available via vcpkg.
        if (_fontPattern.find("bold italic") != string::npos)
            return {"C:\\Windows\\Fonts\\consolaz.ttf"};
        else if (_fontPattern.find("italic") != string::npos)
            return {"C:\\Windows\\Fonts\\consolai.ttf"};
        else if (_fontPattern.find("bold") != string::npos)
            return {"C:\\Windows\\Fonts\\consolab.ttf"};
        else
            return {"C:\\Windows\\Fonts\\consola.ttf"};
        #endif
    }

    constexpr bool glyphMissing(Font::GlyphPosition const& _gp)
    {
        return _gp.glyphIndex == 0;
    }
}

FontManager::FontManager(unsigned int _fontSize) :
    ft_{},
    fonts_{},
    fontSize_{_fontSize}
{
    if (FT_Init_FreeType(&ft_))
        throw runtime_error{ "Failed to initialize FreeType." };
}

FontManager::~FontManager()
{
    fonts_.clear();
    FT_Done_FreeType(ft_);
}

void FontManager::clearRenderCache()
{
    for (auto& font : fonts_)
        font.second.clearRenderCache();
}

void FontManager::setFontSize(unsigned int _size)
{
    for (auto& font : fonts_)
        font.second.setFontSize(_size);

    fontSize_ = _size;
}

Font& FontManager::load(string const& _fontPattern)
{
    vector<string> const filePaths = getFontFilePaths(_fontPattern);

    // Load in reverse order so the newly loaded font always knows its fallback.
    Font* next = nullptr;
    for (auto path = filePaths.rbegin(); path != filePaths.rend(); ++path)
        next = &loadFromFilePath(*path, next);

    //cout << fmt::format("Loading '{}' ({} fonts)", _fontPattern, filePaths.size()) << '\n';

    return *next;
}

Font& FontManager::loadFromFilePath(std::string const& _path, Font* _fallback)
{
    if (auto k = fonts_.find(_path); k != fonts_.end())
        return k->second;
    else
        return fonts_.emplace(make_pair(_path, Font(ft_, _path, _fallback, fontSize_))).first->second;
}

void Font::setFontSize(unsigned int _fontSize)
{
    if (fontSize_ != _fontSize)
    {
        if (FT_HAS_COLOR(face_))
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
        loadGlyphByIndex(0);
        clearRenderCache();
    }
}

// -------------------------------------------------------------------------------------------------------

Font::Font(FT_Library _ft, std::string _fontPath, Font* _fallback, unsigned int _fontSize) :
    ft_{ _ft },
    face_{},
    hb_font_{},
    hb_buf_{},
    fontSize_{ 0 },
    filePath_{ move(_fontPath) },
    fallback_{ _fallback }
{
    if (FT_New_Face(ft_, filePath_.c_str(), 0, &face_))
        throw runtime_error{ "Failed to load font." };

    FT_Error ec = FT_Select_Charmap(face_, FT_ENCODING_UNICODE);
    if (ec)
        throw runtime_error{ string{"Failed to set charmap. "} + freetypeErrorString(ec) };

    setFontSize(_fontSize);

    hb_font_ = hb_ft_font_create_referenced(face_);
    hb_buf_ = hb_buffer_create();

    loadGlyphByIndex(0);
    // XXX Woot, needed in order to retrieve maxAdvance()'s field,
    // as max_advance metric seems to be broken on at least FiraCode (Regular),
    // which is twice as large as it should be, but taking
    // a regular face's advance value works.
}

Font::Font(Font&& v) noexcept :
    ft_{ v.ft_ },
    face_{ v.face_ },
    hb_font_{ v.hb_font_ },
    hb_buf_{ v.hb_buf_ },
    fontSize_{ v.fontSize_ },
    filePath_{ move(v.filePath_) },
    fallback_{ v.fallback_ }
{
    v.ft_ = nullptr;
    v.face_ = nullptr;
    v.hb_font_ = nullptr;
    v.hb_buf_ = nullptr;
    v.fontSize_ = 0;
    v.filePath_ = {};
    v.fallback_ = nullptr;
}

Font& Font::operator=(Font&& v) noexcept
{
    // TODO: free current resources, if any

    ft_ = v.ft_;
    face_ = v.face_;
    hb_font_ = v.hb_font_;
    hb_buf_ = v.hb_buf_;
    fontSize_ = v.fontSize_;
    filePath_ = move(v.filePath_);
    fallback_ = v.fallback_;

    v.ft_ = nullptr;
    v.face_ = nullptr;
    v.hb_font_ = nullptr;
    v.hb_buf_ = nullptr;
    v.fontSize_ = 0;
    v.filePath_ = {};
    v.fallback_ = nullptr;

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

void Font::clearRenderCache()
{
#if defined(LIBTERMINAL_VIEW_FONT_RENDER_CACHE) && LIBTERMINAL_VIEW_FONT_RENDER_CACHE
    renderCache_.clear();
#endif
}

Font::Glyph Font::loadGlyphByIndex(unsigned int _glyphIndex)
{
    return loadGlyphByIndex(0, _glyphIndex);
}

Font::Glyph Font::loadGlyphByIndex(unsigned int _faceIndex, unsigned int _glyphIndex)
{
    if (_faceIndex && fallback_)
        return fallback_->loadGlyphByIndex(_faceIndex - 1, _glyphIndex);

    FT_Error ec = FT_Load_Glyph(face_, _glyphIndex, FT_LOAD_RENDER);
    if (ec != FT_Err_Ok)
        throw runtime_error{ string{"Error loading glyph. "} + freetypeErrorString(ec) };

    auto const width = face_->glyph->bitmap.width;
    auto const height = face_->glyph->bitmap.rows;
    auto const buffer = face_->glyph->bitmap.buffer;

    return Glyph{
        width,
        height,
        vector<uint8_t>(buffer, buffer + width * height)
    };
}

bool Font::render(CharSequence const& _chars, GlyphPositionList& _result, unsigned _attempt)
{
#if defined(LIBTERMINAL_VIEW_FONT_RENDER_CACHE) && LIBTERMINAL_VIEW_FONT_RENDER_CACHE
    if (auto i = renderCache_.find(_chars); i != renderCache_.end())
    {
        _result = i->second;
        return true;
    }
#endif

    hb_buffer_clear_contents(hb_buf_);
    hb_buffer_add_utf32(
        hb_buf_,
        reinterpret_cast<uint32_t const*>(_chars.data()), // text data
        static_cast<int>(_chars.size()),                  // text length
        0,                                                // item offset TODO: optimize fallback by making use of this here
        static_cast<int>(_chars.size())                   // item length
    );
    hb_buffer_set_direction(hb_buf_, HB_DIRECTION_LTR);
    hb_buffer_set_script(hb_buf_, HB_SCRIPT_COMMON);
    hb_buffer_set_language(hb_buf_, hb_language_get_default());
    hb_buffer_guess_segment_properties(hb_buf_);

    hb_shape(hb_font_, hb_buf_, nullptr, 0);

    unsigned const glyphCount = hb_buffer_get_length(hb_buf_);
    hb_glyph_info_t* info = hb_buffer_get_glyph_infos(hb_buf_, nullptr);
    hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(hb_buf_, nullptr);

    _result.clear();
    _result.reserve(glyphCount);

    unsigned int cx = 0;
    unsigned int cy = 0;
    unsigned int advance = 0; // not yet exposed nor needed on caller side
    for (unsigned const i : support::times(glyphCount))
    {
        _result.emplace_back(GlyphPosition{
            *this,
            cx + (pos[i].x_offset >> 6),
            cy + (pos[i].y_offset >> 6),
            info[i].codepoint
        });

        cx += maxAdvance(), // Ought to be (pos[i].x_advance / 64), but that breaks on some font sizes it seems.
        cy += pos[i].y_advance >> 6;
        advance += pos[i].x_advance >> 6;
    }

    if (!support::any_of(_result, glyphMissing))
    {
#if defined(LIBTERMINAL_VIEW_FONT_RENDER_CACHE) && LIBTERMINAL_VIEW_FONT_RENDER_CACHE
        renderCache_[_chars] = _result;
#endif
        // if (_attempt > 0)
        //     cout << fmt::format("Glyph rendering succeed after {} attempts: {} CPs: {}\n",
        //             _attempt, _chars.size(), filePath_);
        return true;
    }
    else if (fallback_)
    {
        _result.clear();
        return fallback_->render(_chars, _result, _attempt + 1);
    }
    else
    {
        auto constexpr missingGlyphId = 0xFFFDu;
        auto const missingGlyph = FT_Get_Char_Index(face_, missingGlyphId);
        if (missingGlyph)
        {
            for (auto i : support::times(glyphCount))
                if (glyphMissing(_result[i]))
                    _result[i].glyphIndex = missingGlyph;
        }
        return false;
    }
}

} // namespace terminal::view
