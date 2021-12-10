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
#include <text_shaper/font_locator.h>

#include <crispy/assert.h>
#include <crispy/algorithm.h>
#include <crispy/times.h>
#include <crispy/indexed.h>

#include <range/v3/view/iota.hpp>
#include <range/v3/algorithm/any_of.hpp>

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

using ranges::views::iota;
using std::get;
using std::holds_alternative;
using std::invalid_argument;
using std::max;
using std::move;
using std::nullopt;
using std::numeric_limits;
using std::optional;
using std::pair;
using std::runtime_error;
using std::size_t;
using std::string;
using std::string_view;
using std::tuple;
using std::u32string_view;
using std::unique_ptr;
using std::unordered_map;
using std::vector;

using namespace std::string_literals;
using namespace std::string_view_literals;

#if !defined(HB_FEATURE_GLOBAL_END)
// Ubuntu 18.04 has a too old harfbuzz that doesn't provide this definition yet.
#define HB_FEATURE_GLOBAL_END ((size_t)-1)
#endif

namespace
{

struct FontPathAndSize
{
    string path;
    text::font_size size;
};

bool operator==(FontPathAndSize const& a, FontPathAndSize const& b) noexcept
{
    return a.path == b.path && a.size.pt == b.size.pt;
}

}

bool operator!=(FontPathAndSize const& a, FontPathAndSize const& b) noexcept
{
    return !(a == b);
}

namespace std
{
    template<>
    struct hash<FontPathAndSize> {
        size_t operator()(FontPathAndSize const& fd) const noexcept
        {
            auto fnv = crispy::FNV<char>();
            return size_t(fnv(fnv(fd.path), to_string(fd.size.pt))); // SSO should kick in.
        }
    };
}

namespace text {

using HbBufferPtr = unique_ptr<hb_buffer_t, void(*)(hb_buffer_t*)>;
using HbFontPtr = unique_ptr<hb_font_t, void(*)(hb_font_t*)>;
using FtFacePtr = unique_ptr<FT_FaceRec_, void(*)(FT_FaceRec_*)>;

auto constexpr MissingGlyphId = 0xFFFDu;

struct HbFontInfo
{
    font_source primary;
    font_source_list fallbacks;
    font_size size;
    FtFacePtr ftFace;
    HbFontPtr hbFont;
    font_description description{};
};

namespace // {{{ helper
{
    string identifierOf(font_source const& source)
    {
        if (holds_alternative<font_path>(source))
            return get<font_path>(source).value;
        if (holds_alternative<font_memory_ref>(source))
            return get<font_memory_ref>(source).identifier;
        throw invalid_argument("source");
    }

    constexpr string_view fcSpacingStr(int _value) noexcept
    {
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
    }

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

    static optional<tuple<string, vector<string>>> getFontFallbackPaths(font_description const& _fd)
    {
        LOGSTORE(LocatorLog)("Loading font chain for: {}", _fd);
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
#ifdef _WIN32
            // On Windows FontConfig can't find "monospace". We need to use "Consolas" instead.
            if (_fd.familyName == "monospace")
                FcPatternAddString(pat.get(), FC_FAMILY, (FcChar8 const*)"Consolas");
#elif __APPLE__
            // Same for macOS, we use "Menlo" for "monospace".
            if (_fd.familyName == "monospace")
                FcPatternAddString(pat.get(), FC_FAMILY, (FcChar8 const*)"Menlo");
#else
            if (_fd.familyName != "monospace")
                FcPatternAddString(pat.get(), FC_FAMILY, (FcChar8 const*) "monospace");
#endif
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
            //     LOGSTORE(LocatorLog)("Skipping font (contains color). {}", (char const*) file);
            //     continue;
            // }
            #endif

            int spacing = -1; // ignore font if we cannot retrieve spacing information
            FcPatternGetInteger(font, FC_SPACING, 0, &spacing);
            if (_fd.strict_spacing)
            {
                if ((_fd.spacing == font_spacing::proportional && spacing < FC_PROPORTIONAL) ||
                    (_fd.spacing == font_spacing::mono && spacing < FC_MONO))
                {
                    LOGSTORE(LocatorLog)("Skipping font: {} ({} < {}).",
                        (char const*)(file), fcSpacingStr(spacing), fcSpacingStr(FC_DUAL));
                    continue;
                }
            }

            fallbackFonts.emplace_back((char const*)(file));
            // LOGSTORE(LocatorLog)("Found font: {}", fallbackFonts.back());
        }

        #if defined(_WIN32)
        #define FONTDIR "C:\\Windows\\Fonts\\"
        if (_fd.familyName == "emoji") {
            fallbackFonts.emplace_back(FONTDIR "seguiemj.ttf");
            fallbackFonts.emplace_back(FONTDIR "seguisym.ttf");
        }
        else if (_fd.weight != font_weight::normal && _fd.slant != font_slant::normal) {
            fallbackFonts.emplace_back(FONTDIR "consolaz.ttf");
            fallbackFonts.emplace_back(FONTDIR "seguisbi.ttf");
        }
        else if (_fd.weight != font_weight::normal) {
            fallbackFonts.emplace_back(FONTDIR "consolab.ttf");
            fallbackFonts.emplace_back(FONTDIR "seguisb.ttf");
        }
        else if (_fd.slant != font_slant::normal) {
            fallbackFonts.emplace_back(FONTDIR "consolai.ttf");
            fallbackFonts.emplace_back(FONTDIR "seguisli.ttf");
        }
        else {
            fallbackFonts.emplace_back(FONTDIR "consola.ttf");
            fallbackFonts.emplace_back(FONTDIR "seguisym.ttf");
        }

        #undef FONTDIR
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
        for (FT_ULong i = 32; i < 128; i++)
        {
            if (auto ci = FT_Get_Char_Index(_face, i); ci != 0)
                if (FT_Load_Glyph(_face, ci, FT_LOAD_DEFAULT) == FT_Err_Ok)
                    maxAdvance = max(maxAdvance, _face->glyph->metrics.horiAdvance);
        }
        return int(ceilf(float(maxAdvance) / 64.0f));
    }

    int ftBestStrikeIndex(FT_Face _face, int _fontWidth) noexcept
    {
        int best = 0;
        int diff = numeric_limits<int>::max();
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

    optional<FtFacePtr> loadFace(font_source const& _source, font_size _fontSize, crispy::Point _dpi, FT_Library _ft)
    {
        FT_Face ftFace = nullptr;

        if (holds_alternative<font_path>(_source))
        {
            auto const& sourcePath = get<font_path>(_source);
            FT_Error ec = FT_New_Face(_ft, sourcePath.value.c_str(), 0, &ftFace);
            if (!ftFace)
            {
                errorlog()("Failed to load font from path {}. {}", sourcePath.value, ftErrorStr(ec));
                return nullopt;
            }
        }
        else if (holds_alternative<font_memory_ref>(_source))
        {
            int faceIndex = 0;
            auto const& memory = get<font_memory_ref>(_source);
            FT_Error ec = FT_New_Memory_Face(_ft,
                                             memory.data.data(),
                                             static_cast<FT_Long>(memory.data.size()),
                                             faceIndex,
                                             &ftFace);
            if (!ftFace)
            {
                errorlog()("Failed to load font from memory. {}", ftErrorStr(ec));
                return nullopt;
            }
        }
        else
        {
            errorlog()("Unsupported font_source type.");
            return nullopt;
        }

        if (FT_Error const ec = FT_Select_Charmap(ftFace, FT_ENCODING_UNICODE); ec != FT_Err_Ok)
            errorlog()("FT_Select_Charmap failed. Ignoring; {}", ftErrorStr(ec));

        if (FT_HAS_COLOR(ftFace))
        {
            auto const strikeIndex = ftBestStrikeIndex(ftFace, int(_fontSize.pt)); // TODO: should be font width (not height)

            FT_Error const ec = FT_Select_Size(ftFace, strikeIndex);
            if (ec != FT_Err_Ok)
                errorlog()("Failed to FT_Select_Size(index={}, source {}): {}", strikeIndex, _source, ftErrorStr(ec));
        }
        else
        {
            auto const size = static_cast<FT_F26Dot6>(ceil(_fontSize.pt * 64.0));

            if (FT_Error const ec = FT_Set_Char_Size(ftFace, size, size,
                                            static_cast<FT_UInt>(_dpi.x),
                                            static_cast<FT_UInt>(_dpi.y)); ec != FT_Err_Ok)
            {
                errorlog()("Failed to FT_Set_Char_Size(size={}, dpi {}, source {}): {}\n", size, _dpi, _source, ftErrorStr(ec));
                // If we cannot set the char-size, this font is most likely unusable for us.
                // Specifically PCF files fail here and I do not know how to deal with them in that
                // case, so do not use this font file at all.
                return nullopt;
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

    void prepareBuffer(hb_buffer_t* _hbBuf, u32string_view _codepoints, gsl::span<unsigned> _clusters, unicode::Script _script)
    {
        hb_buffer_clear_contents(_hbBuf);
        for (auto const i: iota(0u, _codepoints.size()))
            hb_buffer_add(_hbBuf, _codepoints[i], _clusters[i]);

        hb_buffer_set_direction(_hbBuf, HB_DIRECTION_LTR);
        hb_buffer_set_script(_hbBuf, mapScriptToHarfbuzzScript(_script));
        hb_buffer_set_language(_hbBuf, hb_language_get_default());
        hb_buffer_set_content_type(_hbBuf, HB_BUFFER_CONTENT_TYPE_UNICODE);
        hb_buffer_guess_segment_properties(_hbBuf);
    }

    bool tryShape(font_key _font,
                  HbFontInfo& _fontInfo,
                  hb_buffer_t* _hbBuf,
                  hb_font_t* _hbFont,
                  unicode::Script _script,
                  unicode::PresentationStyle _presentation,
                  u32string_view _codepoints,
                  gsl::span<unsigned> _clusters,
                  shape_result& _result)
    {
        assert(_hbFont != nullptr);
        assert(_hbBuf != nullptr);

        prepareBuffer(_hbBuf, _codepoints, _clusters, _script);

        vector<hb_feature_t> hbFeatures;
        for (font_feature const feature: _fontInfo.description.features)
        {
            hb_feature_t hbFeature;
            hbFeature.tag = HB_TAG(feature.name[0], feature.name[1], feature.name[2], feature.name[3]);
            hbFeature.value = 1;
            hbFeature.start = 0;
            hbFeature.end = HB_FEATURE_GLOBAL_END;
            hbFeatures.emplace_back(hbFeature);
        }

        hb_shape(_hbFont, _hbBuf, hbFeatures.data(), hbFeatures.size());
        hb_buffer_normalize_glyphs(_hbBuf);    // TODO: lookup again what this one does

        auto const glyphCount = hb_buffer_get_length(_hbBuf);
        hb_glyph_info_t const* info = hb_buffer_get_glyph_infos(_hbBuf, nullptr);
        hb_glyph_position_t const* pos = hb_buffer_get_glyph_positions(_hbBuf, nullptr);

        for (auto const i: iota(0u, glyphCount))
        {
            glyph_position gpos{};
            gpos.glyph = glyph_key{_font, _fontInfo.size, glyph_index{info[i].codepoint}};
            gpos.offset.x = static_cast<int>(static_cast<double>(pos[i].x_offset) / 64.0); // gpos.offset.(x,y) ?
            gpos.offset.y = static_cast<int>(static_cast<double>(pos[i].y_offset) / 64.0f);
            gpos.advance.x = static_cast<int>(static_cast<double>(pos[i].x_advance) / 64.0f);
            gpos.advance.y = static_cast<int>(static_cast<double>(pos[i].y_advance) / 64.0f);
            gpos.presentation = _presentation;
            _result.emplace_back(gpos);
        }
        return crispy::none_of(_result, glyphMissing);
    }
} // }}}

struct open_shaper::Private // {{{
{
    FT_Library ft_;
    unique_ptr<font_locator> locator_;
    crispy::Point dpi_;
    unordered_map<font_key, HbFontInfo> fonts_;  // from font_key to FontInfo struct
    unordered_map<FontPathAndSize, font_key> fontPathSizeToKeys;

    // Blacklisted font files as we tried them already and failed.
    std::vector<std::string> blacklistedSources;

    // The key (for caching) should be composed out of:
    // (file_path, file_mtime, font_weight, font_slant, pixel_size)

    unordered_map<glyph_key, rasterized_glyph> glyphs_;
    HbBufferPtr hb_buf_;
    font_key nextFontKey_;

    font_key create_font_key()
    {
        auto result = nextFontKey_;
        nextFontKey_.value++;
        return result;
    }

    bool has_color(font_key _font) const noexcept
    {
        return FT_HAS_COLOR(fonts_.at(_font).ftFace.get());
    }

    optional<font_key> get_font_key_for(font_source const& source, font_size _fontSize)
    {
        auto const sourceId = identifierOf(source);
        if (auto i = fontPathSizeToKeys.find(FontPathAndSize{sourceId, _fontSize}); i != fontPathSizeToKeys.end())
            return i->second;

        if (ranges::any_of(blacklistedSources, [&](auto const& a) { return a == sourceId; }))
            return nullopt;

        auto ftFacePtrOpt = loadFace(source, _fontSize, dpi_, ft_);
        if (!ftFacePtrOpt.has_value())
        {
            blacklistedSources.emplace_back(sourceId);
            return nullopt;
        }

        auto ftFacePtr = move(ftFacePtrOpt.value());
        auto hbFontPtr = HbFontPtr(hb_ft_font_create_referenced(ftFacePtr.get()),
                                   [](auto p) { hb_font_destroy(p); });

        auto fontInfo = HbFontInfo{source, {}, _fontSize, move(ftFacePtr), move(hbFontPtr)};

        auto key = create_font_key();
        fonts_.emplace(pair{key, move(fontInfo)});
        LOGSTORE(LocatorLog)("Loading font: key={}, id=\"{}\" size={} dpi {} {}", key, sourceId, _fontSize, dpi_, metrics(key));
        fontPathSizeToKeys.emplace(pair{FontPathAndSize{sourceId, _fontSize}, key});
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

    Private(crispy::Point _dpi,
            unique_ptr<font_locator> _locator) :
        ft_{},
        locator_{ move(_locator) },
        dpi_{ _dpi },
        hb_buf_(hb_buffer_create(), [](auto p) { hb_buffer_destroy(p); }),
        nextFontKey_{}
    {
        if (auto const ec = FT_Init_FreeType(&ft_); ec != FT_Err_Ok)
            throw runtime_error{ "freetype: Failed to initialize. "s + ftErrorStr(ec)};

#if defined(FT_LCD_FILTER_DEFAULT)
        if (auto const ec = FT_Library_SetLcdFilter(ft_, FT_LCD_FILTER_DEFAULT); ec != FT_Err_Ok)
            errorlog()("freetype: Failed to set LCD filter. {}", ftErrorStr(ec));
#endif
    }

    ~Private()
    {
        FT_Done_FreeType(ft_);
    }

    bool tryShapeWithFallback(font_key _font,
                              HbFontInfo& _fontInfo,
                              hb_buffer_t* _hbBuf,
                              hb_font_t* _hbFont,
                              unicode::Script _script,
                              unicode::PresentationStyle _presentation,
                              u32string_view _codepoints,
                              gsl::span<unsigned> _clusters,
                              shape_result& _result)
    {
        auto const initialResultOffset = _result.size();

        if (tryShape(_font, _fontInfo, _hbBuf, _hbFont, _script, _presentation, _codepoints, _clusters, _result))
            return true;

        for (font_source const& fallbackFont: _fontInfo.fallbacks)
        {
            _result.resize(initialResultOffset); // rollback to initial size

            optional<font_key> fallbackKeyOpt = get_font_key_for(fallbackFont, _fontInfo.size);
            if (!fallbackKeyOpt.has_value())
                continue;

            // Skip if main font is monospace but fallbacks font is not.
            if (_fontInfo.description.strict_spacing &&
                _fontInfo.description.spacing != font_spacing::proportional)
            {
                HbFontInfo const& fallbackFontInfo = fonts_.at(fallbackKeyOpt.value());
                bool const fontIsMonospace = fallbackFontInfo.ftFace->face_flags & FT_FACE_FLAG_FIXED_WIDTH;
                if (!fontIsMonospace)
                    continue;
            }

            HbFontInfo& fallbackFontInfo = fonts_.at(fallbackKeyOpt.value());
            LOGSTORE(TextShapingLog)("Try fallbacks font key:{}, source: {}", fallbackKeyOpt.value(), fallbackFontInfo.primary);
            if (tryShape(fallbackKeyOpt.value(), fallbackFontInfo, _hbBuf, fallbackFontInfo.hbFont.get(), _script, _presentation, _codepoints, _clusters, _result))
                return true;
        }

        return false;
    }
}; // }}}

open_shaper::open_shaper(crispy::Point _dpi, unique_ptr<font_locator> _locator):
    d(new Private(_dpi, move(_locator)), [](Private* p) { delete p; })
{
}

void open_shaper::set_dpi(crispy::Point _dpi)
{
    if (_dpi == crispy::Point{})
        return;

    d->dpi_ = _dpi;
}

void open_shaper::set_locator(unique_ptr<font_locator> _locator)
{
    d->locator_ = move(_locator);
}

void open_shaper::clear_cache()
{
    d->fonts_.clear();
    d->fontPathSizeToKeys.clear();
}

optional<font_key> open_shaper::load_font(font_description const& _description, font_size _size)
{
    font_source_list sources = d->locator_->locate(_description);
    if (sources.empty())
        return nullopt;

    optional<font_key> fontKeyOpt = d->get_font_key_for(sources[0], _size);
    if (!fontKeyOpt.has_value())
        return nullopt;

    sources.erase(sources.begin()); // remove primary font from list

    HbFontInfo& fontInfo = d->fonts_.at(fontKeyOpt.value());
    fontInfo.fallbacks = move(sources);
    fontInfo.description = _description;

    return fontKeyOpt;
}

font_metrics open_shaper::metrics(font_key _key) const
{
    return d->metrics(_key);
}

optional<glyph_position> open_shaper::shape(font_key _font,
                                            char32_t _codepoint)
{
    HbFontInfo& fontInfo = d->fonts_.at(_font);

    glyph_index glyphIndex{ FT_Get_Char_Index(fontInfo.ftFace.get(), _codepoint) };
    if (!glyphIndex.value)
    {
        for (font_source const& fallbackFont : fontInfo.fallbacks)
        {
            optional<font_key> fallbackKeyOpt = d->get_font_key_for(fallbackFont, fontInfo.size);
            if (!fallbackKeyOpt.has_value())
                continue;
            HbFontInfo const& fallbackFontInfo = d->fonts_.at(fallbackKeyOpt.value());
            glyphIndex = glyph_index{ FT_Get_Char_Index(fallbackFontInfo.ftFace.get(), _codepoint) };
            if (glyphIndex.value)
                break;
        }
    }
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
                        gsl::span<unsigned> _clusters,
                        unicode::Script _script,
                        unicode::PresentationStyle _presentation,
                        shape_result& _result)
{
    assert(_clusters.size() == _codepoints.size());

    HbFontInfo& fontInfo = d->fonts_.at(_font);
    hb_font_t* hbFont = fontInfo.hbFont.get();
    hb_buffer_t* hbBuf = d->hb_buf_.get();

    if (TextShapingLog)
    {
        auto logMessage = LOGSTORE(TextShapingLog);
        logMessage.append("Shaping codepoints:");
        for (auto [i, codepoint] : crispy::indexed(_codepoints))
            logMessage.append(" {}:U+{:x}", _clusters[i], static_cast<unsigned>(codepoint));
        logMessage.append("\n");
        logMessage.append("Using font: key={}, path=\"{}\"\n", _font, identifierOf(fontInfo.primary));
    }

    if (d->tryShapeWithFallback(_font, fontInfo, hbBuf, hbFont, _script, _presentation, _codepoints, _clusters, _result))
        return;

    LOGSTORE(TextShapingLog)("Shaping failed.");

    // Reshape each cluster individually.
    _result.clear();
    auto cluster = _clusters[0];
    int start = 0;
    for (int i = 1; i < _clusters.size(); ++i)
    {
        if (cluster != _clusters[i])
        {
            int const count = i - start;
            d->tryShapeWithFallback(_font, fontInfo, hbBuf, hbFont,
                                    _script, _presentation,
                                    _codepoints.substr(start, count),
                                    _clusters.subspan(start, count),
                                    _result);
            start = i;
            cluster = _clusters[i];
        }
    }

    // shape last cluster
    auto const end = _clusters.size();
    d->tryShapeWithFallback(_font, fontInfo, hbBuf, hbFont,
                                _script, _presentation,
                                _codepoints.substr(start, end - start),
                                _clusters.subspan(start, end - start),
                                _result);

    // last resort
    replaceMissingGlyphs(fontInfo.ftFace.get(), _result);
}

optional<rasterized_glyph> open_shaper::rasterize(glyph_key _glyph, render_mode _mode)
{
    auto const font = _glyph.font;
    auto ftFace = d->fonts_.at(font).ftFace.get();
    auto const glyphIndex = _glyph.index;
    auto const flags = static_cast<FT_Int32>(ftRenderFlag(_mode) | (FT_HAS_COLOR(ftFace) ? FT_LOAD_COLOR : 0));

    FT_Error ec = FT_Load_Glyph(ftFace, glyphIndex.value, flags);
    if (ec != FT_Err_Ok)
    {
        auto const missingGlyph = FT_Get_Char_Index(ftFace, MissingGlyphId);

        if (missingGlyph)
            ec = FT_Load_Glyph(ftFace, missingGlyph, flags);

        if (ec != FT_Err_Ok)
        {
            if (LocatorLog)
            {
                LOGSTORE(LocatorLog)(
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
    output.size.width = crispy::Width::cast_from(ftFace->glyph->bitmap.width);
    output.size.height = crispy::Height::cast_from(ftFace->glyph->bitmap.rows);
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
            output.bitmap.resize(height.as<size_t>() * width.as<size_t>()); // 8-bit channel (with values 0 or 255)

            auto const pitch = static_cast<unsigned>(ftBitmap.pitch);
            for (auto const i: iota(0u, ftBitmap.rows))
                for (auto const j: iota(0u, ftBitmap.width))
                    output.bitmap[i * width.as<size_t>() + j] = ftBitmap.buffer[(height.as<size_t>() - 1 - i) * pitch + j] * 255;

            FT_Bitmap_Done(d->ft_, &ftBitmap);
            break;
        }
        case FT_PIXEL_MODE_GRAY:
        {
            output.format = bitmap_format::alpha_mask;
            output.bitmap.resize(unbox<size_t>(output.size.height) * unbox<size_t>(output.size.width));

            auto const pitch = static_cast<unsigned>(ftFace->glyph->bitmap.pitch);
            auto const s = ftFace->glyph->bitmap.buffer;
            for (auto const i: iota(0u, *output.size.height))
                for (auto const j: iota(0u, *output.size.width))
                    output.bitmap[i * *output.size.width + j] = s[(*output.size.height - 1 - i) * pitch + j];
            break;
        }
        case FT_PIXEL_MODE_LCD:
        {
            auto const width = static_cast<size_t>(ftFace->glyph->bitmap.width);
            auto const height = static_cast<size_t>(ftFace->glyph->bitmap.rows);

            output.format = bitmap_format::rgb; // LCD
            output.bitmap.resize(width * height);
            output.size.width /= crispy::Width(3);

            auto const pitch = static_cast<unsigned>(ftFace->glyph->bitmap.pitch);
            auto const s = ftFace->glyph->bitmap.buffer;
            for (auto const i: iota(0u, ftFace->glyph->bitmap.rows))
                for (auto const j: iota(0u, ftFace->glyph->bitmap.width))
                    output.bitmap[i * width + j] = s[(height - 1 - i) * pitch + j];
            break;
        }
        case FT_PIXEL_MODE_BGRA:
        {
            auto const width = output.size.width;
            auto const height = output.size.height;

            output.format = bitmap_format::rgba;
            output.bitmap.resize(height.as<size_t>() * width.as<size_t>() * 4);
            auto t = output.bitmap.begin();

            auto const pitch = static_cast<unsigned>(ftFace->glyph->bitmap.pitch);
            for (auto const i: iota(0u, height.as<size_t>()))
            {
                for (auto const j: iota(0u, width.as<size_t>()))
                {
                    auto const s = &ftFace->glyph->bitmap.buffer[
                        static_cast<size_t>(height.as<size_t>() - i - 1u) * pitch +
                        static_cast<size_t>(j) * 4u
                    ];

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
            LOGSTORE(RasterizerLog)("Glyph requested that has an unsupported pixel_mode:{}", ftFace->glyph->bitmap.pixel_mode);
            return nullopt;
    }

    Ensure(output.valid());

    return output;
}

} // end namespace
