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
#include <text_shaper/font.h>
#include <text_shaper/font_locator.h>
#include <text_shaper/open_shaper.h>

#include <crispy/algorithm.h>
#include <crispy/assert.h>
#include <crispy/indexed.h>
#include <crispy/times.h>

#include <unicode/convert.h>

#include <range/v3/algorithm/any_of.hpp>
#include <range/v3/view/iota.hpp>

// clang-format off
#include <ft2build.h>
#include FT_BITMAP_H
#include FT_ERRORS_H
#include FT_FREETYPE_H
#include FT_LCD_FILTER_H
// clang-format on

#include <fontconfig/fontconfig.h>

#include <harfbuzz/hb-ft.h>
#include <harfbuzz/hb.h>

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
    #define HB_FEATURE_GLOBAL_END ((size_t) -1)
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

} // namespace

namespace std
{
template <>
struct hash<FontPathAndSize>
{
    size_t operator()(FontPathAndSize const& fd) const noexcept
    {
        auto fnv = crispy::FNV<char>();
        return size_t(fnv(fnv(fd.path), to_string(fd.size.pt))); // SSO should kick in.
    }
};
} // namespace std

namespace text
{

using HbBufferPtr = unique_ptr<hb_buffer_t, void (*)(hb_buffer_t*)>;
using HbFontPtr = unique_ptr<hb_font_t, void (*)(hb_font_t*)>;
using FtFacePtr = unique_ptr<FT_FaceRec_, void (*)(FT_FaceRec_*)>;

auto constexpr MissingGlyphId = 0xFFFDu;

struct HbFontInfo
{
    font_source primary;
    font_source_list fallbacks;
    font_size size;
    FtFacePtr ftFace;
    HbFontPtr hbFont;
    std::optional<font_metrics> metrics {};
    font_description description {};
};

namespace
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

    // clang-format off
    static string ftErrorStr(FT_Error _errorCode)
    {
        #undef __FTERRORS_H__
        #define FT_ERROR_START_LIST     switch (_errorCode) {
        #define FT_ERRORDEF(e, v, s)    case e: return s;
        #define FT_ERROR_END_LIST       }
        #include FT_ERRORS_H
        return "(Unknown error)";
    }
    // clang-format on

    constexpr bool glyphMissing(text::glyph_position const& _gp) noexcept
    {
        return _gp.glyph.index.value == 0;
    }

    constexpr int fcWeight(font_weight _weight) noexcept
    {
        switch (_weight)
        {
        case font_weight::thin: return FC_WEIGHT_THIN;
        case font_weight::extra_light: return FC_WEIGHT_EXTRALIGHT;
        case font_weight::light: return FC_WEIGHT_LIGHT;
        case font_weight::demilight:
#if defined(FC_WEIGHT_DEMILIGHT)
            return FC_WEIGHT_DEMILIGHT;
#else
            return FC_WEIGHT_LIGHT; // Is this a good fallback? Maybe.
#endif
        case font_weight::book: return FC_WEIGHT_BOOK;
        case font_weight::normal: return FC_WEIGHT_NORMAL;
        case font_weight::medium: return FC_WEIGHT_MEDIUM;
        case font_weight::demibold: return FC_WEIGHT_DEMIBOLD;
        case font_weight::bold: return FC_WEIGHT_BOLD;
        case font_weight::extra_bold: return FC_WEIGHT_EXTRABOLD;
        case font_weight::black: return FC_WEIGHT_BLACK;
        case font_weight::extra_black: return FC_WEIGHT_EXTRABLACK;
        }
        return FC_WEIGHT_NORMAL;
    }

    constexpr int fcSlant(font_slant _slant) noexcept
    {
        switch (_slant)
        {
        case font_slant::italic: return FC_SLANT_ITALIC;
        case font_slant::oblique: return FC_SLANT_OBLIQUE;
        case font_slant::normal: return FC_SLANT_ROMAN;
        }
        return FC_SLANT_ROMAN;
    }

    constexpr int ftRenderFlag(render_mode _mode) noexcept
    {
        switch (_mode)
        {
        case render_mode::bitmap: return FT_LOAD_MONOCHROME;
        case render_mode::light: return FT_LOAD_TARGET_LIGHT;
        case render_mode::lcd: return FT_LOAD_TARGET_LCD;
        case render_mode::color: return FT_LOAD_COLOR;
        case render_mode::gray: return FT_LOAD_DEFAULT;
        }
        return FT_LOAD_DEFAULT;
    }

    constexpr FT_Render_Mode ftRenderMode(render_mode _mode) noexcept
    {
        switch (_mode)
        {
        case render_mode::bitmap: return FT_RENDER_MODE_MONO;
        case render_mode::gray: return FT_RENDER_MODE_NORMAL;
        case render_mode::light: return FT_RENDER_MODE_LIGHT;
        case render_mode::lcd: return FT_RENDER_MODE_LCD;
        case render_mode::color: return FT_RENDER_MODE_NORMAL; break;
        }
        return FT_RENDER_MODE_NORMAL;
    }

    constexpr hb_script_t mapScriptToHarfbuzzScript(unicode::Script _script)
    {
        using unicode::Script;
        switch (_script)
        {
        case Script::Latin: return HB_SCRIPT_LATIN;
        case Script::Greek: return HB_SCRIPT_GREEK;
        case Script::Common: return HB_SCRIPT_COMMON;
        default:
            // TODO: make this list complete
            return HB_SCRIPT_INVALID; // hb_buffer_guess_segment_properties() will fill it
        }
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
        for (FT_ULong i = 33; i < 128; i++)
            if (auto ci = FT_Get_Char_Index(_face, i); ci != 0)
                if (FT_Load_Glyph(_face, ci, FT_LOAD_DEFAULT) == FT_Err_Ok)
                    maxAdvance = max(maxAdvance, _face->glyph->metrics.horiAdvance);
        return int(ceil(double(maxAdvance) / 64.0));
    }

    int ftBestStrikeIndex(FT_Face _face, int _fontWidth) noexcept
    {
        int best = 0;
        int diff = numeric_limits<int>::max();
        for (int i = 0; i < _face->num_fixed_sizes; ++i)
        {
            auto const currentWidth = _face->available_sizes[i].width;
            auto const d = currentWidth > _fontWidth ? currentWidth - _fontWidth : _fontWidth - currentWidth;
            if (d < diff)
            {
                diff = d;
                best = i;
            }
        }
        return best;
    }

    optional<FtFacePtr> loadFace(font_source const& _source, font_size _fontSize, DPI _dpi, FT_Library _ft)
    {
        FT_Face ftFace = nullptr;

        if (holds_alternative<font_path>(_source))
        {
            auto const& sourcePath = get<font_path>(_source);
            FT_Error ec = FT_New_Face(_ft, sourcePath.value.c_str(), 0, &ftFace);
            if (!ftFace)
            {
                // clang-format off
                errorlog()("Failed to load font from path {}. {}", sourcePath.value, ftErrorStr(ec));
                // clang-format on
                return nullopt;
            }
        }
        else if (holds_alternative<font_memory_ref>(_source))
        {
            int faceIndex = 0;
            auto const& memory = get<font_memory_ref>(_source);
            FT_Error ec = FT_New_Memory_Face(
                _ft, memory.data.data(), static_cast<FT_Long>(memory.data.size()), faceIndex, &ftFace);
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
            auto const strikeIndex =
                ftBestStrikeIndex(ftFace, int(_fontSize.pt)); // TODO: should be font width (not height)

            FT_Error const ec = FT_Select_Size(ftFace, strikeIndex);
            if (ec != FT_Err_Ok)
                errorlog()("Failed to FT_Select_Size(index={}, source {}): {}",
                           strikeIndex,
                           _source,
                           ftErrorStr(ec));
        }
        else
        {
            auto const size = static_cast<FT_F26Dot6>(ceil(_fontSize.pt * 64.0));

            if (FT_Error const ec = FT_Set_Char_Size(
                    ftFace, 0, size, static_cast<FT_UInt>(_dpi.x), static_cast<FT_UInt>(_dpi.y));
                ec != FT_Err_Ok)
            {
                errorlog()("Failed to FT_Set_Char_Size(size={}, dpi {}, source {}): {}\n",
                           size,
                           _dpi,
                           _source,
                           ftErrorStr(ec));
                // If we cannot set the char-size, this font is most likely unusable for us.
                // Specifically PCF files fail here and I do not know how to deal with them in that
                // case, so do not use this font file at all.
                return nullopt;
            }
        }

        return optional<FtFacePtr> { FtFacePtr(ftFace, [](FT_Face p) { FT_Done_Face(p); }) };
    }

    void replaceMissingGlyphs(FT_Face _ftFace, shape_result& _result)
    {
        auto const missingGlyph = FT_Get_Char_Index(_ftFace, MissingGlyphId);

        if (!missingGlyph)
            return;

        for (auto&& [i, gpos]: crispy::indexed(_result))
            if (glyphMissing(gpos))
                gpos.glyph.index = glyph_index { missingGlyph };
    }

    void prepareBuffer(hb_buffer_t* _hbBuf,
                       u32string_view _codepoints,
                       gsl::span<unsigned> _clusters,
                       unicode::Script _script)
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

        hb_shape(_hbFont, _hbBuf, hbFeatures.data(), static_cast<unsigned int>(hbFeatures.size()));
        hb_buffer_normalize_glyphs(_hbBuf); // TODO: lookup again what this one does

        auto const glyphCount = hb_buffer_get_length(_hbBuf);
        hb_glyph_info_t const* info = hb_buffer_get_glyph_infos(_hbBuf, nullptr);
        hb_glyph_position_t const* pos = hb_buffer_get_glyph_positions(_hbBuf, nullptr);

        for (auto const i: iota(0u, glyphCount))
        {
            glyph_position gpos {};
            gpos.glyph = glyph_key { _fontInfo.size, _font, glyph_index { info[i].codepoint } };
#if defined(GLYPH_KEY_DEBUG)
            {
                auto const cluster = info[i].cluster;
                for (size_t k = 0; k < _codepoints.size(); ++k)
                    if (_clusters[k] == cluster)
                        gpos.glyph.text += _codepoints[k];
            }
#endif
            gpos.offset.x =
                static_cast<int>(static_cast<double>(pos[i].x_offset) / 64.0); // gpos.offset.(x,y) ?
            gpos.offset.y = static_cast<int>(static_cast<double>(pos[i].y_offset) / 64.0f);
            gpos.advance.x = static_cast<int>(static_cast<double>(pos[i].x_advance) / 64.0f);
            gpos.advance.y = static_cast<int>(static_cast<double>(pos[i].y_advance) / 64.0f);
            gpos.presentation = _presentation;
            _result.emplace_back(gpos);
        }
        return crispy::none_of(_result, glyphMissing);
    }
} // namespace

struct open_shaper::Private // {{{
{
    crispy::finally ftCleanup_;
    FT_Library ft_;
    unique_ptr<font_locator> locator_;
    DPI dpi_;
    unordered_map<FontPathAndSize, font_key> fontPathAndSizeToKeyMapping;
    unordered_map<font_key, HbFontInfo> fontKeyToHbFontInfoMapping; // from font_key to FontInfo struct

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
        return FT_HAS_COLOR(fontKeyToHbFontInfoMapping.at(_font).ftFace.get());
    }

    optional<font_key> getOrCreateKeyForFont(font_source const& source, font_size _fontSize)
    {
        auto const sourceId = identifierOf(source);
        if (auto i = fontPathAndSizeToKeyMapping.find(FontPathAndSize { sourceId, _fontSize });
            i != fontPathAndSizeToKeyMapping.end())
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
        auto hbFontPtr =
            HbFontPtr(hb_ft_font_create_referenced(ftFacePtr.get()), [](auto p) { hb_font_destroy(p); });

        auto fontInfo = HbFontInfo { source, {}, _fontSize, move(ftFacePtr), move(hbFontPtr) };

        auto key = create_font_key();
        fontPathAndSizeToKeyMapping.emplace(pair { FontPathAndSize { sourceId, _fontSize }, key });
        fontKeyToHbFontInfoMapping.emplace(pair { key, move(fontInfo) });
        LocatorLog()("Loading font: key={}, id=\"{}\" size={} dpi {} {}",
                     key,
                     sourceId,
                     _fontSize,
                     dpi_,
                     metrics(key));
        return key;
    }

    font_metrics metrics(font_key _key)
    {
        Require(fontKeyToHbFontInfoMapping.count(_key) == 1);
        auto ftFace = fontKeyToHbFontInfoMapping.at(_key).ftFace.get();

        font_metrics output {};

        output.line_height = scaleVertical(ftFace, ftFace->height);
        output.advance = computeAverageAdvance(ftFace);
        if (!output.advance)
            output.advance = int(double(output.line_height) * 2.0 / 3.0);
        output.ascender = scaleVertical(ftFace, ftFace->ascender);
        output.descender = scaleVertical(ftFace, ftFace->descender);
        output.underline_position = scaleVertical(ftFace, ftFace->underline_position);
        output.underline_thickness = scaleVertical(ftFace, ftFace->underline_thickness);

        return output;
    }

    Private(DPI _dpi, unique_ptr<font_locator> _locator):
        ftCleanup_ { [this]() {
            FT_Done_FreeType(ft_);
        } },
        ft_ {},
        locator_ { move(_locator) },
        dpi_ { _dpi },
        hb_buf_(hb_buffer_create(), [](auto p) { hb_buffer_destroy(p); }),
        nextFontKey_ {}
    {
        if (auto const ec = FT_Init_FreeType(&ft_); ec != FT_Err_Ok)
            throw runtime_error { "freetype: Failed to initialize. "s + ftErrorStr(ec) };

#if defined(FT_LCD_FILTER_DEFAULT)
        if (auto const ec = FT_Library_SetLcdFilter(ft_, FT_LCD_FILTER_DEFAULT); ec != FT_Err_Ok)
            errorlog()("freetype: Failed to set LCD filter. {}", ftErrorStr(ec));
#endif
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

        if (tryShape(
                _font, _fontInfo, _hbBuf, _hbFont, _script, _presentation, _codepoints, _clusters, _result))
            return true;

        for (font_source const& fallbackFont: _fontInfo.fallbacks)
        {
            _result.resize(initialResultOffset); // rollback to initial size

            optional<font_key> fallbackKeyOpt = getOrCreateKeyForFont(fallbackFont, _fontInfo.size);
            if (!fallbackKeyOpt.has_value())
                continue;

            // Skip if main font is monospace but fallbacks font is not.
            if (_fontInfo.description.strict_spacing
                && _fontInfo.description.spacing != font_spacing::proportional)
            {
                Require(fontKeyToHbFontInfoMapping.count(fallbackKeyOpt.value()) == 1);
                HbFontInfo const& fallbackFontInfo = fontKeyToHbFontInfoMapping.at(fallbackKeyOpt.value());
                bool const fontIsMonospace = fallbackFontInfo.ftFace->face_flags & FT_FACE_FLAG_FIXED_WIDTH;
                if (!fontIsMonospace)
                    continue;
            }

            Require(fontKeyToHbFontInfoMapping.count(fallbackKeyOpt.value()) == 1);
            HbFontInfo& fallbackFontInfo = fontKeyToHbFontInfoMapping.at(fallbackKeyOpt.value());
            // clang-format off
            TextShapingLog()("Try fallbacks font key:{}, source: {}",
                             fallbackKeyOpt.value(),
                             fallbackFontInfo.primary);
            // clang-format on
            if (tryShape(fallbackKeyOpt.value(),
                         fallbackFontInfo,
                         _hbBuf,
                         fallbackFontInfo.hbFont.get(),
                         _script,
                         _presentation,
                         _codepoints,
                         _clusters,
                         _result))
                return true;
        }

        return false;
    }
}; // }}}

open_shaper::open_shaper(DPI _dpi, unique_ptr<font_locator> _locator):
    d(new Private(_dpi, move(_locator)), [](Private* p) { delete p; })
{
}

void open_shaper::set_dpi(DPI _dpi)
{
    if (!_dpi)
        return;

    d->dpi_ = _dpi;
}

void open_shaper::set_locator(unique_ptr<font_locator> _locator)
{
    d->locator_ = move(_locator);
}

void open_shaper::clear_cache()
{
    LocatorLog()("Clearing cache ({} keys, {} font infos).",
                 d->fontPathAndSizeToKeyMapping.size(),
                 d->fontKeyToHbFontInfoMapping.size());
    d->fontPathAndSizeToKeyMapping.clear();
    d->fontKeyToHbFontInfoMapping.clear();
}

optional<font_key> open_shaper::load_font(font_description const& _description, font_size _size)
{
    font_source_list sources = d->locator_->locate(_description);
    if (sources.empty())
        return nullopt;

    optional<font_key> fontKeyOpt = d->getOrCreateKeyForFont(sources[0], _size);
    if (!fontKeyOpt.has_value())
        return nullopt;

    sources.erase(sources.begin()); // remove primary font from list

    HbFontInfo& fontInfo = d->fontKeyToHbFontInfoMapping.at(*fontKeyOpt);
    fontInfo.fallbacks = move(sources);
    fontInfo.description = _description;

    return fontKeyOpt;
}

font_metrics open_shaper::metrics(font_key _key) const
{
    Require(d->fontKeyToHbFontInfoMapping.count(_key) == 1);
    HbFontInfo& fontInfo = d->fontKeyToHbFontInfoMapping.at(_key);
    if (fontInfo.metrics.has_value())
        return fontInfo.metrics.value();

    fontInfo.metrics = d->metrics(_key);
    LocatorLog()("Calculating font metrics for {}: {}", fontInfo.description, *fontInfo.metrics);
    return fontInfo.metrics.value();
}

optional<glyph_position> open_shaper::shape(font_key _font, char32_t _codepoint)
{
    Require(d->fontKeyToHbFontInfoMapping.count(_font) == 1);
    HbFontInfo& fontInfo = d->fontKeyToHbFontInfoMapping.at(_font);

    glyph_index glyphIndex { FT_Get_Char_Index(fontInfo.ftFace.get(), _codepoint) };
    if (!glyphIndex.value)
    {
        for (font_source const& fallbackFont: fontInfo.fallbacks)
        {
            optional<font_key> fallbackKeyOpt = d->getOrCreateKeyForFont(fallbackFont, fontInfo.size);
            if (!fallbackKeyOpt.has_value())
                continue;
            Require(d->fontKeyToHbFontInfoMapping.count(fallbackKeyOpt.value()) == 1);
            HbFontInfo const& fallbackFontInfo = d->fontKeyToHbFontInfoMapping.at(fallbackKeyOpt.value());
            glyphIndex = glyph_index { FT_Get_Char_Index(fallbackFontInfo.ftFace.get(), _codepoint) };
            if (glyphIndex.value)
                break;
        }
    }
    if (!glyphIndex.value)
        return nullopt;

    glyph_position gpos {};
    gpos.glyph = glyph_key { fontInfo.size, _font, glyphIndex };
#if defined(GLYPH_KEY_DEBUG)
    gpos.glyph.text = std::u32string(1, _codepoint);
#endif
    gpos.advance.x = this->metrics(_font).advance;
    gpos.offset = crispy::Point {}; // TODO (load from glyph metrics. Is this needed?)

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
    TextShapingLog()(
        "Shaping using font key: {}, text: \"{}\"", _font, unicode::convert_to<char>(_codepoints));
    if (!d->fontKeyToHbFontInfoMapping.count(_font))
        TextShapingLog()("Font not found? {}", _font);

    Require(d->fontKeyToHbFontInfoMapping.count(_font) == 1);
    HbFontInfo& fontInfo = d->fontKeyToHbFontInfoMapping.at(_font);
    hb_font_t* hbFont = fontInfo.hbFont.get();
    hb_buffer_t* hbBuf = d->hb_buf_.get();

    if (TextShapingLog)
    {
        auto logMessage = TextShapingLog();
        logMessage.append("Shaping codepoints:");
        for (auto [i, codepoint]: crispy::indexed(_codepoints))
            logMessage.append(" {}:U+{:x}", _clusters[i], static_cast<unsigned>(codepoint));
        logMessage.append("\n");
        logMessage.append("Using font: key={}, path=\"{}\"\n", _font, identifierOf(fontInfo.primary));
    }

    if (d->tryShapeWithFallback(
            _font, fontInfo, hbBuf, hbFont, _script, _presentation, _codepoints, _clusters, _result))
        return;

    TextShapingLog()("Shaping failed.");

    // Reshape each cluster individually.
    _result.clear();
    auto cluster = _clusters[0];
    size_t start = 0;
    for (size_t i = 1; i < _clusters.size(); ++i)
    {
        if (cluster != _clusters[i])
        {
            size_t const count = i - start;
            d->tryShapeWithFallback(_font,
                                    fontInfo,
                                    hbBuf,
                                    hbFont,
                                    _script,
                                    _presentation,
                                    _codepoints.substr(start, count),
                                    _clusters.subspan(start, count),
                                    _result);
            start = i;
            cluster = _clusters[i];
        }
    }

    // shape last cluster
    auto const end = _clusters.size();
    d->tryShapeWithFallback(_font,
                            fontInfo,
                            hbBuf,
                            hbFont,
                            _script,
                            _presentation,
                            _codepoints.substr(start, end - start),
                            _clusters.subspan(start, end - start),
                            _result);

    // last resort
    replaceMissingGlyphs(fontInfo.ftFace.get(), _result);
}

optional<rasterized_glyph> open_shaper::rasterize(glyph_key _glyph, render_mode _mode)
{
    auto const font = _glyph.font;
    auto ftFace = d->fontKeyToHbFontInfoMapping.at(font).ftFace.get();
    auto const glyphIndex = _glyph.index;
    auto const flags =
        static_cast<FT_Int32>(ftRenderFlag(_mode) | (FT_HAS_COLOR(ftFace) ? FT_LOAD_COLOR : 0));

    FT_Error ec = FT_Load_Glyph(ftFace, glyphIndex.value, flags);
    if (ec != FT_Err_Ok)
    {
        auto const missingGlyph = FT_Get_Char_Index(ftFace, MissingGlyphId);

        if (missingGlyph)
            ec = FT_Load_Glyph(ftFace, missingGlyph, flags);

        if (ec != FT_Err_Ok)
        {
            if (LocatorLog)
                LocatorLog()("Error loading glyph index {} for font {} {}. {}",
                             glyphIndex.value,
                             ftFace->family_name,
                             ftFace->style_name,
                             ftErrorStr(ec));
            return nullopt;
        }
    }

    // NB: colored fonts are bitmap fonts, they do not need rendering
    if (!FT_HAS_COLOR(ftFace))
    {
        if (FT_Render_Glyph(ftFace->glyph, ftRenderMode(_mode)) != FT_Err_Ok)
            return nullopt;
    }

    rasterized_glyph output {};
    output.bitmapSize.width = crispy::Width::cast_from(ftFace->glyph->bitmap.width);
    output.bitmapSize.height = crispy::Height::cast_from(ftFace->glyph->bitmap.rows);
    output.position.x = ftFace->glyph->bitmap_left;
    output.position.y = ftFace->glyph->bitmap_top;

    switch (ftFace->glyph->bitmap.pixel_mode)
    {
    case FT_PIXEL_MODE_MONO: {
        auto const width = output.bitmapSize.width;
        auto const height = output.bitmapSize.height;

        // convert mono to gray
        FT_Bitmap ftBitmap;
        FT_Bitmap_Init(&ftBitmap);

        auto const ec = FT_Bitmap_Convert(d->ft_, &ftFace->glyph->bitmap, &ftBitmap, 1);
        if (ec != FT_Err_Ok)
            return nullopt;

        ftBitmap.num_grays = 256;

        output.format = bitmap_format::alpha_mask;
        output.bitmap.resize(height.as<size_t>()
                             * width.as<size_t>()); // 8-bit channel (with values 0 or 255)

        auto const pitch = static_cast<unsigned>(ftBitmap.pitch);
        for (auto const i: iota(0u, ftBitmap.rows))
            for (auto const j: iota(0u, ftBitmap.width))
                output.bitmap[i * width.as<size_t>() + j] =
                    ftBitmap.buffer[(height.as<size_t>() - 1 - i) * pitch + j] * 255;

        FT_Bitmap_Done(d->ft_, &ftBitmap);
        break;
    }
    case FT_PIXEL_MODE_GRAY: {
        output.format = bitmap_format::alpha_mask;
        output.bitmap.resize(unbox<size_t>(output.bitmapSize.height)
                             * unbox<size_t>(output.bitmapSize.width));

        auto const pitch = static_cast<unsigned>(ftFace->glyph->bitmap.pitch);
        auto const s = ftFace->glyph->bitmap.buffer;
        for (auto const i: iota(0u, *output.bitmapSize.height))
            for (auto const j: iota(0u, *output.bitmapSize.width))
                output.bitmap[i * *output.bitmapSize.width + j] =
                    s[(*output.bitmapSize.height - 1 - i) * pitch + j];
        break;
    }
    case FT_PIXEL_MODE_LCD: {
        auto const width = static_cast<size_t>(ftFace->glyph->bitmap.width);
        auto const height = static_cast<size_t>(ftFace->glyph->bitmap.rows);

        output.format = bitmap_format::rgb; // LCD
        output.bitmap.resize(width * height);
        output.bitmapSize.width /= crispy::Width(3);

        auto const pitch = static_cast<unsigned>(ftFace->glyph->bitmap.pitch);
        auto const s = ftFace->glyph->bitmap.buffer;
        for (auto const i: iota(0u, ftFace->glyph->bitmap.rows))
            for (auto const j: iota(0u, ftFace->glyph->bitmap.width))
                output.bitmap[i * width + j] = s[(height - 1 - i) * pitch + j];
        break;
    }
    case FT_PIXEL_MODE_BGRA: {
        auto const width = output.bitmapSize.width;
        auto const height = output.bitmapSize.height;

        output.format = bitmap_format::rgba;
        output.bitmap.resize(height.as<size_t>() * width.as<size_t>() * 4);
        auto t = output.bitmap.begin();

        auto const pitch = static_cast<unsigned>(ftFace->glyph->bitmap.pitch);
        for (auto const i: iota(0u, height.as<size_t>()))
        {
            for (auto const j: iota(0u, width.as<size_t>()))
            {
                auto const s =
                    &ftFace->glyph->bitmap.buffer[static_cast<size_t>(height.as<size_t>() - i - 1u) * pitch
                                                  + static_cast<size_t>(j) * 4u];

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
        RasterizerLog()("Glyph requested that has an unsupported pixel_mode:{}",
                        ftFace->glyph->bitmap.pixel_mode);
        return nullopt;
    }

    Ensures(output.valid());

    if (RasterizerLog)
        RasterizerLog()("rasterize {} to {}", _glyph, output);

    return output;
}

} // namespace text
