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
#include <text_shaper/open_shaper.h>
#include <text_shaper/font.h>

#include <crispy/algorithm.h>
#include <crispy/debuglog.h>
#include <crispy/times.h>
#include <crispy/indexed.h>

#include <ft2build.h>
#include FT_BITMAP_H
#include FT_ERRORS_H
#include FT_FREETYPE_H
#include FT_LCD_FILTER_H

#include <fontconfig/fontconfig.h>
#include <harfbuzz/hb.h>
#include <harfbuzz/hb-ft.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>

using std::max;
using std::move;
using std::nullopt;
using std::optional;
using std::pair;
using std::runtime_error;
using std::string;
using std::string_view;
using std::tuple;
using std::u32string_view;
using std::unique_ptr;
using std::vector;

using namespace std::string_literals;

namespace {
    auto const FontFallbackTag = crispy::debugtag::make("font.fallback", "Logs details about font fallback");
    auto const TextShapingTag = crispy::debugtag::make("font.textshaping", "Logs details about text shaping.");
    auto const GlyphRenderTag = crispy::debugtag::make("font.render", "Logs details about rendering glyphs.");
}

struct FontPathAndSize
{
    string path;
    text::font_size size;
};

bool operator==(FontPathAndSize const& a, FontPathAndSize const& b) noexcept
{
    return a.path == b.path && a.size.pt == b.size.pt;
}

bool operator!=(FontPathAndSize const& a, FontPathAndSize const& b) noexcept
{
    return !(a == b);
}

namespace std
{
    template<>
    struct hash<FontPathAndSize> {
        std::size_t operator()(FontPathAndSize const& fd) const noexcept
        {
            auto fnv = crispy::FNV<char>();
            return size_t(fnv(fnv(fd.path), to_string(fd.size.pt))); // SSO should kick in.
        }
    };
}

namespace text {

using HbBufferPtr = std::unique_ptr<hb_buffer_t, void(*)(hb_buffer_t*)>;
using HbFontPtr = std::unique_ptr<hb_font_t, void(*)(hb_font_t*)>;
using FtFacePtr = std::unique_ptr<FT_FaceRec_, void(*)(FT_FaceRec_*)>;

auto constexpr MissingGlyphId = 0xFFFDu;

namespace // {{{ helper
{
    constexpr string_view fcSpacingStr(int _value) noexcept
    {
        using namespace std::string_view_literals;
        switch (_value)
        {
            case FC_PROPORTIONAL: return "proportional"sv;
            case FC_DUAL: return "dual"sv;
            case FC_MONO: return "mono"sv;
            case FC_CHARCELL: return "charcell"sv;
            default: return "INVALID"sv;
        }
    }

    static string ftErrorStr(FT_Error _errorCode)
    {
        #undef __FTERRORS_H__
        #define FT_ERROR_START_LIST     switch (_errorCode) {
        #define FT_ERRORDEF(e, v, s)    case e: return s;
        #define FT_ERROR_END_LIST       }
        #include FT_ERRORS_H
        return "(Unknown error)";
    }

    constexpr bool glyphMissing(text::glyph_position const& _gp) noexcept
    {
        return _gp.glyph.index.value == 0;
    }

    constexpr int fcWeight(font_weight _weight) noexcept
    {
        switch (_weight)
        {
            case font_weight::thin:
                return FC_WEIGHT_THIN;
            case font_weight::extra_light:
                return FC_WEIGHT_EXTRALIGHT;
            case font_weight::light:
                return FC_WEIGHT_LIGHT;
            case font_weight::demilight:
#if defined(FC_WEIGHT_DEMILIGHT)
                return FC_WEIGHT_DEMILIGHT;
#else
                return FC_WEIGHT_LIGHT; // Is this a good fallback? Maybe.
#endif
            case font_weight::book:
                return FC_WEIGHT_BOOK;
            case font_weight::normal:
                return FC_WEIGHT_NORMAL;
            case font_weight::medium:
                return FC_WEIGHT_MEDIUM;
            case font_weight::demibold:
                return FC_WEIGHT_DEMIBOLD;
            case font_weight::bold:
                return FC_WEIGHT_BOLD;
            case font_weight::extra_bold:
                return FC_WEIGHT_EXTRABOLD;
            case font_weight::black:
                return FC_WEIGHT_BLACK;
            case font_weight::extra_black:
                return FC_WEIGHT_EXTRABLACK;
        }
        return FC_WEIGHT_NORMAL;
    }

    constexpr int fcSlant(font_slant _slant) noexcept
    {
        switch (_slant)
        {
            case font_slant::italic:
                return FC_SLANT_ITALIC;
            case font_slant::oblique:
                return FC_SLANT_OBLIQUE;
            case font_slant::normal:
                return FC_SLANT_ROMAN;
        }
        return FC_SLANT_ROMAN;
    }

    constexpr int ftRenderFlag(render_mode _mode) noexcept
    {
        switch (_mode)
        {
            case render_mode::bitmap:
                return FT_LOAD_MONOCHROME;
            case render_mode::light:
                return FT_LOAD_TARGET_LIGHT;
            case render_mode::lcd:
                return FT_LOAD_TARGET_LCD;
            case render_mode::color:
                return FT_LOAD_COLOR;
            case render_mode::gray:
                return FT_LOAD_DEFAULT;
        }
        return FT_LOAD_DEFAULT;
    }

    constexpr FT_Render_Mode ftRenderMode(render_mode _mode) noexcept
    {
        switch (_mode)
        {
            case render_mode::bitmap: return FT_RENDER_MODE_MONO;
            case render_mode::gray:   return FT_RENDER_MODE_NORMAL;
            case render_mode::light:  return FT_RENDER_MODE_LIGHT;
            case render_mode::lcd:    return FT_RENDER_MODE_LCD;
            case render_mode::color:  return FT_RENDER_MODE_NORMAL;
                break;
        }
        return FT_RENDER_MODE_NORMAL;
    };

    constexpr hb_script_t mapScriptToHarfbuzzScript(unicode::Script _script)
    {
        using unicode::Script;
        switch (_script)
        {
            case Script::Latin:
                return HB_SCRIPT_LATIN;
            case Script::Greek:
                return HB_SCRIPT_GREEK;
            case Script::Common:
                return HB_SCRIPT_COMMON;
            default:
                // TODO: make this list complete
                return HB_SCRIPT_INVALID; // hb_buffer_guess_segment_properties() will fill it
        }
    }

#if 0
    char const* fcWeightStr(int _value)
    {
        switch (_value)
        {
            case FC_WEIGHT_THIN: return "Thin";
            case FC_WEIGHT_EXTRALIGHT: return "ExtraLight";
            case FC_WEIGHT_LIGHT: return "Light";
#if defined(FC_WEIGHT_DEMILIGHT)
            case FC_WEIGHT_DEMILIGHT: return "DemiLight";
#endif
            case FC_WEIGHT_BOOK: return "Book";
            case FC_WEIGHT_REGULAR: return "Regular";
            case FC_WEIGHT_MEDIUM: return "Medium";
            case FC_WEIGHT_DEMIBOLD: return "DemiBold";
            case FC_WEIGHT_BOLD: return "Bold";
            case FC_WEIGHT_EXTRABOLD: return "ExtraBold";
            case FC_WEIGHT_BLACK: return "Black";
            case FC_WEIGHT_EXTRABLACK: return "ExtraBlack";
            default: return "?";
        }
    }

    char const* fcSlantStr(int _value)
    {
        switch (_value)
        {
            case FC_SLANT_ROMAN: return "Roman";
            case FC_SLANT_ITALIC: return "Italic";
            case FC_SLANT_OBLIQUE: return "Oblique";
            default: return "?";
        }
    }

    static optional<vector<string>> getAvailableFonts()
    {
        FcPattern* pat = FcPatternCreate();
        FcObjectSet* os = FcObjectSetBuild(
#if defined(FC_COLOR)
            FC_COLOR,
#endif
            FC_FAMILY,
            FC_FILE,
            FC_FULLNAME,
            FC_HINTING,
            FC_HINT_STYLE,
            FC_INDEX,
            FC_OUTLINE,
#if defined(FC_POSTSCRIPT_NAME)
            FC_POSTSCRIPT_NAME,
#endif
            FC_SCALABLE,
            FC_SLANT,
            FC_SPACING,
            FC_STYLE,
            FC_WEIGHT,
            FC_WIDTH,
            NULL
        );
        FcFontSet* fs = FcFontList(nullptr, pat, os);

        vector<string> output;

        for (auto i = 0; i < fs->nfont; ++i)
        {
            FcPattern* font = fs->fonts[i];

            FcChar8* filename = nullptr;
            FcPatternGetString(font, FC_FILE, 0, &filename);

            FcChar8* family = nullptr;
            FcPatternGetString(font, FC_FAMILY, 0, &family);

            int spacing = -1; // ignore font if we cannot retrieve spacing information
            FcPatternGetInteger(font, FC_SPACING, 0, &spacing);

            int weight = -1;
            FcPatternGetInteger(font, FC_WEIGHT, 0, &weight);

            int slant = -1;
            FcPatternGetInteger(font, FC_SLANT, 0, &slant);

            if (spacing >= FC_DUAL)
            {
                std::cerr << fmt::format("font({}, {}): {}\n",
                    fcWeightStr(weight),
                    fcSlantStr(slant),
                    (char*) family);
                output.emplace_back((char const*) filename);
            }
        }

        FcObjectSetDestroy(os);
        FcFontSetDestroy(fs);
        FcPatternDestroy(pat);

        return output;
    }
#endif

    static optional<tuple<string, vector<string>>> getFontFallbackPaths(font_description const& _fd)
    {
        debuglog(FontFallbackTag).write("Loading font chain for: {}", _fd);
        auto pat = unique_ptr<FcPattern, void(*)(FcPattern*)>(
            FcPatternCreate(),
            [](auto p) { FcPatternDestroy(p); });

        FcPatternAddBool(pat.get(), FC_OUTLINE, true);
        FcPatternAddBool(pat.get(), FC_SCALABLE, true);
        //FcPatternAddBool(pat.get(), FC_EMBEDDED_BITMAP, false);

        // XXX It should be recommended to turn that on if you are looking for colored fonts,
        //     such as for emoji, but it seems like fontconfig doesn't care, it works either way.
        //
        // bool const _color = true;
        // FcPatternAddBool(pat.get(), FC_COLOR, _color);

        if (!_fd.familyName.empty())
            FcPatternAddString(pat.get(), FC_FAMILY, (FcChar8 const*) _fd.familyName.c_str());

        if (_fd.spacing != font_spacing::proportional)
        {
            if (_fd.familyName != "monospace")
                FcPatternAddString(pat.get(), FC_FAMILY, (FcChar8 const*) "monospace");
            FcPatternAddInteger(pat.get(), FC_SPACING, FC_MONO);
            FcPatternAddInteger(pat.get(), FC_SPACING, FC_DUAL);
        }

        if (_fd.weight != font_weight::normal)
            FcPatternAddInteger(pat.get(), FC_WEIGHT, fcWeight(_fd.weight));
        if (_fd.slant != font_slant::normal)
            FcPatternAddInteger(pat.get(), FC_SLANT, fcSlant(_fd.slant));

        FcConfigSubstitute(nullptr, pat.get(), FcMatchPattern);
        FcDefaultSubstitute(pat.get());

        FcResult result = FcResultNoMatch;
        auto fs = unique_ptr<FcFontSet, void(*)(FcFontSet*)>(
            FcFontSort(nullptr, pat.get(), /*unicode-trim*/FcTrue, /*FcCharSet***/nullptr, &result),
            [](auto p) { FcFontSetDestroy(p); });

        if (!fs || result != FcResultMatch)
            return {};

        vector<string> fallbackFonts;
        for (int i = 0; i < fs->nfont; ++i)
        {
            FcPattern* font = fs->fonts[i];

            FcChar8* file;
            if (FcPatternGetString(font, FC_FILE, 0, &file) != FcResultMatch)
                continue;

            #if defined(FC_COLOR) // Not available on OS/X?
            // FcBool color = FcFalse;
            // FcPatternGetInteger(font, FC_COLOR, 0, &color);
            // if (color && !_color)
            // {
            //     debuglog(FontFallbackTag).write("Skipping font (contains color). {}", (char const*) file);
            //     continue;
            // }
            #endif

            int spacing = -1; // ignore font if we cannot retrieve spacing information
            FcPatternGetInteger(font, FC_SPACING, 0, &spacing);
            if (_fd.force_spacing)
            {
                if ((_fd.spacing == font_spacing::proportional && spacing < FC_PROPORTIONAL) ||
                    (_fd.spacing == font_spacing::mono && spacing < FC_MONO))
                {
                    debuglog(FontFallbackTag).write("Skipping font: {} ({} < {}).",
                        (char const*)(file), fcSpacingStr(spacing), fcSpacingStr(FC_DUAL));
                    continue;
                }
            }

            fallbackFonts.emplace_back((char const*)(file));
            // debuglog(FontFallbackTag).write("Found font: {}", fallbackFonts.back());
        }

        #if defined(_WIN32)
        if (_fd.weight != font_weight::normal && _fd.slant != font_slant::normal)
            fallbackFonts.emplace_back("C:\\Windows\\Fonts\\consolaz.ttf");
        else if (_fd.weight != font_weight::normal)
            fallbackFonts.emplace_back("C:\\Windows\\Fonts\\consolab.ttf");
        else if (_fd.slant != font_slant::normal)
            fallbackFonts.emplace_back("C:\\Windows\\Fonts\\consolai.ttf");
        else
            fallbackFonts.emplace_back("C:\\Windows\\Fonts\\consola.ttf");
        #endif

        if (fallbackFonts.empty())
            return nullopt;

        string primary = fallbackFonts.front();
        fallbackFonts.erase(fallbackFonts.begin());

        return tuple{primary, fallbackFonts};
    }

    // XXX currently not needed
    // int scaleHorizontal(FT_Face _face, long _value) noexcept
    // {
    //     assert(_face);
    //     return int(ceil(double(FT_MulFix(_value, _face->size->metrics.x_scale)) / 64.0));
    // }

    int scaleVertical(FT_Face _face, long _value) noexcept
    {
        assert(_face);
        return int(ceil(double(FT_MulFix(_value, _face->size->metrics.y_scale)) / 64.0));
    }

    int computeAverageAdvance(FT_Face _face) noexcept
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

    int ftBestStrikeIndex(FT_Face _face, int _fontWidth) noexcept
    {
        int best = 0;
        int diff = std::numeric_limits<int>::max();
        for (int i = 0; i < _face->num_fixed_sizes; ++i)
        {
            auto const currentWidth = _face->available_sizes[i].width;
            auto const d = currentWidth > _fontWidth ? currentWidth - _fontWidth
                                                     : _fontWidth - currentWidth;
            if (d < diff) {
                diff = d;
                best = i;
            }
        }
        return best;
    }

    optional<FtFacePtr> loadFace(string const& _path, font_size _fontSize, crispy::Point _dpi, FT_Library _ft)
    {
        FT_Face ftFace = nullptr;
        auto ftErrorCode = FT_New_Face(_ft, _path.c_str(), 0, &ftFace);
        if (!ftFace)
        {
            debuglog(FontFallbackTag).write("Failed to load font from path {}. {}", _path, ftErrorStr(ftErrorCode));
            return nullopt;
        }

        if (FT_Error const ec = FT_Select_Charmap(ftFace, FT_ENCODING_UNICODE); ec != FT_Err_Ok)
            debuglog(FontFallbackTag).write("FT_Select_Charmap failed. Ignoring; {}", ftErrorStr(ec));

        if (FT_HAS_COLOR(ftFace))
        {
            auto const strikeIndex = ftBestStrikeIndex(ftFace, int(_fontSize.pt)); // TODO: should be font width (not height)

            FT_Error const ec = FT_Select_Size(ftFace, strikeIndex);
            if (ec != FT_Err_Ok)
                debuglog(FontFallbackTag).write("Failed to FT_Select_Size(index={}, file={}): {}", strikeIndex, _path, ftErrorStr(ec));
        }
        else
        {
            auto const size = static_cast<FT_F26Dot6>(ceil(_fontSize.pt * 64.0));

            if (FT_Error const ec = FT_Set_Char_Size(ftFace, size, size, _dpi.x, _dpi.y); ec != FT_Err_Ok)
            {
                debuglog(FontFallbackTag).write("Failed to FT_Set_Char_Size(size={}, dpi={}, file={}): {}\n", size, _dpi, _path, ftErrorStr(ec));
            }
        }

        return optional<FtFacePtr>{FtFacePtr(ftFace, [](FT_Face p) { FT_Done_Face(p); })};
    }

    void replaceMissingGlyphs(FT_Face _ftFace, shape_result& _result)
    {
        auto const missingGlyph = FT_Get_Char_Index(_ftFace, MissingGlyphId);

        if (!missingGlyph)
            return;

        for (auto && [i, gpos] : crispy::indexed(_result))
            if (glyphMissing(gpos))
                gpos.glyph.index = glyph_index{ missingGlyph };
    }
} // }}}

struct FontInfo
{
    string path;
    font_size size;
    FtFacePtr ftFace;
    HbFontPtr hbFont;
    font_description description{};
    vector<string> fallbackFonts{};
};

struct open_shaper::Private // {{{
{
    FT_Library ft_;
    crispy::Point dpi_;
    std::unordered_map<font_key, FontInfo> fonts_;  // from font_key to FontInfo struct
    std::unordered_map<FontPathAndSize, font_key> fontPathSizeToKeys;

    // The key (for caching) should be composed out of:
    // (file_path, file_mtime, font_weight, font_slant, pixel_size)

    std::unordered_map<glyph_key, rasterized_glyph> glyphs_;
    HbBufferPtr hb_buf_;
    font_key nextFontKey_;

    font_key create_font_key()
    {
        auto result = nextFontKey_;
        nextFontKey_.value++;
        return result;
    }

    optional<font_key> get_font_key_for(string _path, font_size _fontSize)
    {
        if (auto i = fontPathSizeToKeys.find(FontPathAndSize{_path, _fontSize}); i != fontPathSizeToKeys.end())
            return i->second;

        auto ftFacePtrOpt = loadFace(_path, _fontSize, dpi_, ft_);
        if (!ftFacePtrOpt.has_value())
            return nullopt;

        auto ftFacePtr = move(ftFacePtrOpt.value());
        auto hbFontPtr = HbFontPtr(hb_ft_font_create_referenced(ftFacePtr.get()),
                                   [](auto p) { hb_font_destroy(p); });

        auto fontInfo = FontInfo{_path, _fontSize, move(ftFacePtr), move(hbFontPtr)};

        auto key = create_font_key();
        fonts_.emplace(pair{key, move(fontInfo)});
        debuglog(FontFallbackTag).write("Loading font: key={}, path=\"{}\" size={} dpi={} {}", key, _path, _fontSize, dpi_, metrics(key));
        fontPathSizeToKeys.emplace(pair{FontPathAndSize{move(_path), _fontSize}, key});
        return key;
    }

    font_metrics metrics(font_key _key)
    {
        auto ftFace = fonts_.at(_key).ftFace.get();

        font_metrics output{};

        output.line_height = scaleVertical(ftFace, ftFace->height);
        output.advance = computeAverageAdvance(ftFace);
        output.ascender = scaleVertical(ftFace, ftFace->ascender);
        output.descender = scaleVertical(ftFace, ftFace->descender);
        output.underline_position = scaleVertical(ftFace, ftFace->underline_position);
        output.underline_thickness = scaleVertical(ftFace, ftFace->underline_thickness);

        return output;
    }

    explicit Private(crispy::Point _dpi) :
        ft_{},
        dpi_{ _dpi },
        hb_buf_(hb_buffer_create(), [](auto p) { hb_buffer_destroy(p); }),
        nextFontKey_{}
    {
        FcInit();

        if (auto const ec = FT_Init_FreeType(&ft_); ec != FT_Err_Ok)
            throw runtime_error{ "freetype: Failed to initialize. "s + ftErrorStr(ec)};

#if defined(FT_LCD_FILTER_DEFAULT)
        if (auto const ec = FT_Library_SetLcdFilter(ft_, FT_LCD_FILTER_DEFAULT); ec != FT_Err_Ok)
            debuglog(GlyphRendstring).write("freetype: Failed to set LCD filter. {}", ftErrorStr(ec));
#endif

        //getAvailableFonts();
    }

    ~Private()
    {
        FT_Done_FreeType(ft_);

        FcFini();
    }
}; // }}}

open_shaper::open_shaper(crispy::Point _dpi) : d(new Private(_dpi), [](Private* p) { delete p; })
{
}

void open_shaper::set_dpi(crispy::Point _dpi)
{
    std::cout << fmt::format("open_shaper.set_dpi! {}\n", _dpi);
    if (_dpi == crispy::Point{})
        return;

    d->dpi_ = _dpi;
}

void open_shaper::clear_cache()
{
    std::cout << fmt::format("open_shaper.clear_cache\n");
    d->fonts_.clear();
    d->fontPathSizeToKeys.clear();
}

optional<font_key> open_shaper::load_font(font_description const& _description, font_size _size)
{
    auto fontPathsOpt = getFontFallbackPaths(_description);
    if (!fontPathsOpt.has_value())
        return nullopt;

    auto& [primaryFont, fallbackFonts] = fontPathsOpt.value();

    optional<font_key> fontKeyOpt = d->get_font_key_for(move(primaryFont), _size);
    if (!fontKeyOpt.has_value())
        return nullopt;

    FontInfo& fontInfo = d->fonts_.at(fontKeyOpt.value());
    fontInfo.fallbackFonts = fallbackFonts;
    fontInfo.description = _description;

    return fontKeyOpt;
}

font_metrics open_shaper::metrics(font_key _key) const
{
    return d->metrics(_key);
}

bool open_shaper::has_color(font_key _font) const
{
    return FT_HAS_COLOR(d->fonts_.at(_font).ftFace.get());
}

void prepareBuffer(hb_buffer_t* _hbBuf, u32string_view _codepoints, crispy::span<int> _clusters, unicode::Script _script)
{
    hb_buffer_clear_contents(_hbBuf);
    for (auto const i : crispy::times(_codepoints.size()))
        hb_buffer_add(_hbBuf, _codepoints[i], _clusters[i]);

    hb_buffer_set_direction(_hbBuf, HB_DIRECTION_LTR);
    hb_buffer_set_script(_hbBuf, mapScriptToHarfbuzzScript(_script));
    hb_buffer_set_language(_hbBuf, hb_language_get_default());
    hb_buffer_set_content_type(_hbBuf, HB_BUFFER_CONTENT_TYPE_UNICODE);
    hb_buffer_guess_segment_properties(_hbBuf);
}

bool tryShape(font_key _font,
              FontInfo& _fontInfo,
              hb_buffer_t* _hbBuf,
              hb_font_t* _hbFont,
              unicode::Script _script,
              u32string_view _codepoints,
              crispy::span<int> _clusters,
              shape_result& _result)
{
    assert(_hbFont != nullptr);
    assert(_hbBuf != nullptr);

    prepareBuffer(_hbBuf, _codepoints, _clusters, _script);

    hb_shape(_hbFont, _hbBuf, nullptr, 0); // TODO: support font features
    hb_buffer_normalize_glyphs(_hbBuf);    // TODO: lookup again what this one does

    auto const glyphCount = static_cast<int>(hb_buffer_get_length(_hbBuf));
    hb_glyph_info_t const* info = hb_buffer_get_glyph_infos(_hbBuf, nullptr);
    hb_glyph_position_t const* pos = hb_buffer_get_glyph_positions(_hbBuf, nullptr);

    _result.clear();
    _result.reserve(glyphCount);

    for (auto const i : crispy::times(glyphCount))
    {
        glyph_position gpos{};
        gpos.glyph = glyph_key{_font, _fontInfo.size, glyph_index{info[i].codepoint}};
        gpos.offset.x = int(pos[i].x_offset / 64.0f); // gpos.offset.(x,y) ?
        gpos.offset.y = int(pos[i].y_offset / 64.0f);
        gpos.advance.x = int(pos[i].x_advance / 64.0f);
        gpos.advance.y = int(pos[i].y_advance / 64.0f);
        _result.emplace_back(gpos);
    }
    return crispy::none_of(_result, glyphMissing);
}

optional<glyph_position> open_shaper::shape(font_key _font,
                                            char32_t _codepoint)
{
    FontInfo& fontInfo = d->fonts_.at(_font);

    glyph_index glyphIndex{ FT_Get_Char_Index(fontInfo.ftFace.get(), _codepoint) };
    if (!glyphIndex.value)
        return nullopt;

    glyph_position gpos{};
    gpos.glyph = glyph_key{_font, fontInfo.size, glyphIndex};
    gpos.advance.x = this->metrics(_font).advance;
    gpos.offset = crispy::Point{}; // TODO (load from glyph metrics. needed?)

    return gpos;
}

void open_shaper::shape(font_key _font,
                        u32string_view _codepoints,
                        crispy::span<int> _clusters,
                        unicode::Script _script,
                        shape_result& _result)
{
    FontInfo& fontInfo = d->fonts_.at(_font);
    hb_font_t* hbFont = fontInfo.hbFont.get();
    hb_buffer_t* hbBuf = d->hb_buf_.get();

    if (crispy::logging_sink::for_debug().enabled())
    {
        auto logMessage = debuglog(TextShapingTag);
        logMessage.write("Shaping codepoints:");
        for (auto [i, codepoint] : crispy::indexed(_codepoints))
            logMessage.write(" {}:U+{:x}", _clusters[i], static_cast<unsigned>(codepoint));
        logMessage.write("\n");
        logMessage.write("Using font: key={}, path=\"{}\"\n", _font, fontInfo.path);
    }

    if (tryShape(_font, fontInfo, hbBuf, hbFont, _script, _codepoints, _clusters, _result))
        return;

    for (auto const& fallbackFont : fontInfo.fallbackFonts)
    {
        optional<font_key> fallbackKeyOpt = d->get_font_key_for(fallbackFont, fontInfo.size);
        if (!fallbackKeyOpt.has_value())
            continue;

        // Skip if main font is monospace but fallback font is not.
        if (fontInfo.description.spacing != font_spacing::proportional)
        {
            FontInfo const& fallbackFontInfo = d->fonts_.at(fallbackKeyOpt.value());
            bool const fontIsMonospace = fallbackFontInfo.ftFace->face_flags & FT_FACE_FLAG_FIXED_WIDTH;
            if (!fontIsMonospace)
                continue;
        }

        FontInfo& fallbackFontInfo = d->fonts_.at(fallbackKeyOpt.value());
        debuglog(FontFallbackTag).write("Try fallback font: key={}, path=\"{}\"\n", fallbackKeyOpt.value(), fallbackFontInfo.path);
        if (tryShape(fallbackKeyOpt.value(), fallbackFontInfo, hbBuf, fallbackFontInfo.hbFont.get(), _script, _codepoints, _clusters, _result))
            return;
    }
    debuglog(FontFallbackTag).write("Shaping failed.");

    // reshape with primary font
    tryShape(_font, fontInfo, hbBuf, hbFont, _script, _codepoints, _clusters, _result);
    replaceMissingGlyphs(fontInfo.ftFace.get(), _result);
}

optional<rasterized_glyph> open_shaper::rasterize(glyph_key _glyph, render_mode _mode)
{
    auto const font = _glyph.font;
    auto ftFace = d->fonts_.at(font).ftFace.get();
    auto const glyphIndex = _glyph.index;
    FT_Int32 const flags = ftRenderFlag(_mode) | (has_color(font) ? FT_LOAD_COLOR : 0);

    FT_Error ec = FT_Load_Glyph(ftFace, glyphIndex.value, flags);
    if (ec != FT_Err_Ok)
    {
        auto const missingGlyph = FT_Get_Char_Index(ftFace, MissingGlyphId);

        if (missingGlyph)
            ec = FT_Load_Glyph(ftFace, missingGlyph, flags);

        if (ec != FT_Err_Ok)
        {
            if (crispy::logging_sink::for_debug().enabled())
            {
                debuglog(FontFallbackTag).write(
                    "Error loading glyph index {} for font {} {}. {}",
                    glyphIndex.value,
                    ftFace->family_name,
                    ftFace->style_name,
                    ftErrorStr(ec)
                );
            }
            return nullopt;
        }
    }

    // NB: colored fonts are bitmap fonts, they do not need rendering
    if (!FT_HAS_COLOR(ftFace))
    {
        if (FT_Render_Glyph(ftFace->glyph, ftRenderMode(_mode)) != FT_Err_Ok)
            return nullopt;
    }

    rasterized_glyph output{};
    output.size.width = static_cast<int>(ftFace->glyph->bitmap.width);
    output.size.height = static_cast<int>(ftFace->glyph->bitmap.rows);
    output.position.x = ftFace->glyph->bitmap_left;
    output.position.y = ftFace->glyph->bitmap_top;

    switch (ftFace->glyph->bitmap.pixel_mode)
    {
        case FT_PIXEL_MODE_MONO:
        {
            auto const width = output.size.width;
            auto const height = output.size.height;

            // convert mono to gray
            FT_Bitmap ftBitmap;
            FT_Bitmap_Init(&ftBitmap);

            auto const ec = FT_Bitmap_Convert(d->ft_, &ftFace->glyph->bitmap, &ftBitmap, 1);
            if (ec != FT_Err_Ok)
                return nullopt;

            ftBitmap.num_grays = 256;

            output.format = bitmap_format::alpha_mask;
            output.bitmap.resize(height * width); // 8-bit channel (with values 0 or 255)

            auto const pitch = abs(ftBitmap.pitch);
            for (auto i = 0; i < int(ftBitmap.rows); ++i)
                for (auto j = 0; j < int(ftBitmap.width); ++j)
                    output.bitmap[i * width + j] = ftBitmap.buffer[(height - 1 - i) * pitch + j] * 255;

            FT_Bitmap_Done(d->ft_, &ftBitmap);
            break;
        }
        case FT_PIXEL_MODE_GRAY:
        {
            output.format = bitmap_format::alpha_mask;
            output.bitmap.resize(output.size.height * output.size.width);

            auto const pitch = ftFace->glyph->bitmap.pitch;
            auto const s = ftFace->glyph->bitmap.buffer;
            for (auto i = 0; i < output.size.height; ++i)
                for (auto j = 0; j < output.size.width; ++j)
                    output.bitmap[i * output.size.width + j] = s[(output.size.height - 1 - i) * pitch + j];
            break;
        }
        case FT_PIXEL_MODE_LCD:
        {
            auto const width = ftFace->glyph->bitmap.width;
            auto const height = ftFace->glyph->bitmap.rows;

            output.format = bitmap_format::rgb; // LCD
            output.bitmap.resize(width * height);
            output.size.width /= 3;

            auto const pitch = ftFace->glyph->bitmap.pitch;
            auto s = ftFace->glyph->bitmap.buffer;
            for (auto const i : crispy::times(ftFace->glyph->bitmap.rows))
                for (auto const j : crispy::times(ftFace->glyph->bitmap.width))
                    output.bitmap[i * width + j] = s[(height - 1 - i) * pitch + j];
            break;
        }
        case FT_PIXEL_MODE_BGRA:
        {
            auto const width = output.size.width;
            auto const height = output.size.height;

            output.format = bitmap_format::rgba;
            output.bitmap.resize(height * width * 4);
            auto t = output.bitmap.begin();

            auto const pitch = ftFace->glyph->bitmap.pitch;
            for (auto const i : crispy::times(height))
            {
                for (auto const j : crispy::times(width))
                {
                    auto const s = &ftFace->glyph->bitmap.buffer[(height - i - 1) * pitch + j * 4];

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
            debuglog(GlyphRenderTag).write("Glyph requested that has an unsupported pixel_mode:{}", ftFace->glyph->bitmap.pixel_mode);
            return nullopt;
    }

    return output;
}

} // end namespace
