// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/font.h>
#include <text_shaper/font_locator.h>
#include <text_shaper/open_shaper.h>

#include <crispy/algorithm.h>
#include <crispy/assert.h>
#include <crispy/indexed.h>
#include <crispy/times.h>

#include <range/v3/algorithm/any_of.hpp>
#include <range/v3/view/iota.hpp>

#include <limits>
#include <optional>

#include <libunicode/convert.h>
#include <libunicode/ucd_fmt.h>

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
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>

using ranges::views::iota;
using std::get;
using std::holds_alternative;
using std::invalid_argument;
using std::max;
using std::min;
using std::move;
using std::nullopt;
using std::numeric_limits;
using std::optional;
using std::ostringstream;
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

namespace
{

struct FontPathAndSize // NOLINT(readability-identifier-naming)
{
    string path;
    text::font_size size;
};

[[maybe_unused]] bool operator==(FontPathAndSize const& a, FontPathAndSize const& b) noexcept
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
        auto fnv = crispy::fnv<char>();
        return size_t(fnv(fnv(fd.path), to_string(fd.size.pt))); // SSO should kick in.
    }
};
} // namespace std

namespace text
{

using hb_buffer_ptr = unique_ptr<hb_buffer_t, void (*)(hb_buffer_t*)>;
using hb_font_ptr = unique_ptr<hb_font_t, void (*)(hb_font_t*)>;
using ft_face_ptr = unique_ptr<FT_FaceRec_, void (*)(FT_FaceRec_*)>;

auto constexpr MissingGlyphId = 0xFFFDu;

struct HbFontInfo // NOLINT(readability-identifier-naming)
{
    font_source primary;
    font_source_list fallbacks;
    font_size size;
    ft_face_ptr ftFace;
    hb_font_ptr hbFont;
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

    // clang-format off
    string ftErrorStr(FT_Error errorCode)
    {
        #undef __FTERRORS_H__
        #define FT_ERROR_START_LIST     switch (errorCode) {
        #define FT_ERRORDEF(e, v, s)    case e: return s;
        #define FT_ERROR_END_LIST       }
        #include FT_ERRORS_H
        return "(Unknown error)";
    }
    // clang-format on

    constexpr bool glyphMissing(text::glyph_position const& gp) noexcept
    {
        return gp.glyph.index.value == 0;
    }

    constexpr int ftRenderFlag(render_mode mode) noexcept
    {
        switch (mode)
        {
            case render_mode::bitmap: return FT_LOAD_MONOCHROME;
            case render_mode::light: return FT_LOAD_TARGET_LIGHT;
            case render_mode::lcd: return FT_LOAD_TARGET_LCD;
            case render_mode::color: return FT_LOAD_COLOR;
            case render_mode::gray: return FT_LOAD_DEFAULT;
        }
        return FT_LOAD_DEFAULT;
    }

    constexpr FT_Render_Mode ftRenderMode(render_mode mode) noexcept
    {
        switch (mode)
        {
            case render_mode::bitmap: return FT_RENDER_MODE_MONO;
            case render_mode::gray: return FT_RENDER_MODE_NORMAL;
            case render_mode::light: return FT_RENDER_MODE_LIGHT;
            case render_mode::lcd: return FT_RENDER_MODE_LCD;
            case render_mode::color: return FT_RENDER_MODE_NORMAL; break;
        }
        return FT_RENDER_MODE_NORMAL;
    }

    constexpr hb_script_t mapScriptToHarfbuzzScript(unicode::Script script)
    {
        using unicode::Script;
        switch (script)
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
    // int scaleHorizontal(FT_Face face, long value) noexcept
    // {
    //     assert(face);
    //     return int(ceil(double(FT_MulFix(value, face->size->metrics.x_scale)) / 64.0));
    // }

    int scaleVertical(FT_Face face, long value) noexcept
    {
        assert(face);
        return int(ceil(double(FT_MulFix(value, face->size->metrics.y_scale)) / 64.0));
    }

    int computeAverageAdvance(FT_Face face) noexcept
    {
        FT_Pos maxAdvance = 0;
        for (FT_ULong i = 33; i < 128; i++)
            if (auto ci = FT_Get_Char_Index(face, i); ci != 0)
                if (FT_Load_Glyph(face, ci, FT_LOAD_DEFAULT) == FT_Err_Ok)
                    maxAdvance = max(maxAdvance, face->glyph->metrics.horiAdvance);
        return int(ceil(double(maxAdvance) / 64.0));
    }

    std::optional<int> ftBestStrikeIndex(FT_Face face, double pt, DPI dpi) noexcept
    {
        auto const targetLength = static_cast<int>(pt * dpi.y / 72.0);
        int bestIndex = 0;
        if (face->num_fixed_sizes == 0)
            return nullopt;

        int bestDiff = std::abs(int(face->available_sizes[0].width) - targetLength);
        for (int i = 1; i < face->num_fixed_sizes; ++i)
        {
            auto const diff = std::abs(int(face->available_sizes[i].width) - targetLength);
            if (diff < bestDiff)
            {
                bestDiff = diff;
                bestIndex = i;
            }
        }
        return bestIndex;
    }

    optional<ft_face_ptr> loadFace(font_source const& source, font_size fontSize, DPI dpi, FT_Library ft)
    {
        FT_Face ftFace = nullptr;

        if (holds_alternative<font_path>(source))
        {
            auto const& sourcePath = get<font_path>(source);
            FT_Error ec = FT_New_Face(ft, sourcePath.value.c_str(), sourcePath.collectionIndex, &ftFace);
            if (!ftFace)
            {
                // clang-format off
                errorLog()("Failed to load font from path {}. {}", sourcePath.value, ftErrorStr(ec));
                // clang-format on
                return nullopt;
            }
        }
        else if (holds_alternative<font_memory_ref>(source))
        {
            int faceIndex = 0;
            auto const& memory = get<font_memory_ref>(source);
            FT_Error ec = FT_New_Memory_Face(
                ft, memory.data.data(), static_cast<FT_Long>(memory.data.size()), faceIndex, &ftFace);
            if (!ftFace)
            {
                errorLog()("Failed to load font from memory. {}", ftErrorStr(ec));
                return nullopt;
            }
        }
        else
        {
            errorLog()("Unsupported font_source type.");
            return nullopt;
        }

        if (FT_Error const ec = FT_Select_Charmap(ftFace, FT_ENCODING_UNICODE); ec != FT_Err_Ok)
            errorLog()("FT_Select_Charmap failed. Ignoring; {}", ftErrorStr(ec));

        if (FT_HAS_COLOR(ftFace))
        {
            auto const strikeIndexOpt = ftBestStrikeIndex(ftFace, fontSize.pt, dpi);
            if (!strikeIndexOpt.has_value())
            {
                FT_Done_Face(ftFace);
                return nullopt;
            }

            auto const strikeIndex = strikeIndexOpt.value();
            FT_Error const ec = FT_Select_Size(ftFace, strikeIndex);
            if (ec != FT_Err_Ok)
                errorLog()(
                    "Failed to FT_Select_Size(index={}, source {}): {}", strikeIndex, source, ftErrorStr(ec));
            else
                rasterizerLog()("Picked color font's strike index {} ({}x{}) from {}\n",
                                strikeIndex,
                                ftFace->available_sizes[strikeIndex].width,
                                ftFace->available_sizes[strikeIndex].height,
                                source);
        }
        else
        {
            auto const size = static_cast<FT_F26Dot6>(ceil(fontSize.pt * 64.0));

            if (FT_Error const ec = FT_Set_Char_Size(
                    ftFace, size, 0, static_cast<FT_UInt>(dpi.x), static_cast<FT_UInt>(dpi.y));
                ec != FT_Err_Ok)
            {
                errorLog()("Failed to FT_Set_Char_Size(size={}, dpi {}, source {}): {}\n",
                           size,
                           dpi,
                           source,
                           ftErrorStr(ec));
                // If we cannot set the char-size, this font is most likely unusable for us.
                // Specifically PCF files fail here and I do not know how to deal with them in that
                // case, so do not use this font file at all.
                return nullopt;
            }
        }

        return optional<ft_face_ptr> { ft_face_ptr(ftFace, [](FT_Face p) { FT_Done_Face(p); }) };
    }

    void replaceMissingGlyphs(FT_Face ftFace, shape_result& result)
    {
        auto const missingGlyph = FT_Get_Char_Index(ftFace, MissingGlyphId);

        if (!missingGlyph)
            return;

        for (auto&& [i, gpos]: crispy::indexed(result))
            if (glyphMissing(gpos))
                gpos.glyph.index = glyph_index { missingGlyph };
    }

    void prepareBuffer(hb_buffer_t* hbBuf,
                       u32string_view codepoints,
                       gsl::span<unsigned> clusters,
                       unicode::Script script)
    {
        hb_buffer_clear_contents(hbBuf);
        for (auto const i: iota(0u, codepoints.size()))
            hb_buffer_add(hbBuf, codepoints[i], clusters[i]);

        hb_buffer_set_direction(hbBuf, HB_DIRECTION_LTR);
        hb_buffer_set_script(hbBuf, mapScriptToHarfbuzzScript(script));
        hb_buffer_set_language(hbBuf, hb_language_get_default());
        hb_buffer_set_content_type(hbBuf, HB_BUFFER_CONTENT_TYPE_UNICODE);
        hb_buffer_guess_segment_properties(hbBuf);
    }

    bool tryShape(font_key font,
                  HbFontInfo& fontInfo,
                  hb_buffer_t* hbBuf,
                  hb_font_t* hbFont,
                  unicode::Script script,
                  unicode::PresentationStyle presentation,
                  u32string_view codepoints,
                  gsl::span<unsigned> clusters,
                  shape_result& result)
    {
        assert(hbFont != nullptr);
        assert(hbBuf != nullptr);

        prepareBuffer(hbBuf, codepoints, clusters, script);

        vector<hb_feature_t> hbFeatures;
        for (font_feature const feature: fontInfo.description.features)
        {
            hb_feature_t hbFeature;
            hbFeature.tag = HB_TAG(feature.name[0], feature.name[1], feature.name[2], feature.name[3]);
            hbFeature.value = feature.enabled ? 1 : 0;
            hbFeature.start = 0;
            hbFeature.end = std::numeric_limits<decltype(hbFeature.end)>::max();
            hbFeatures.emplace_back(hbFeature);
        }

        hb_shape(hbFont, hbBuf, hbFeatures.data(), static_cast<unsigned int>(hbFeatures.size()));
        hb_buffer_normalize_glyphs(hbBuf); // TODO: lookup again what this one does

        auto const glyphCount = hb_buffer_get_length(hbBuf);
        hb_glyph_info_t const* info = hb_buffer_get_glyph_infos(hbBuf, nullptr);
        hb_glyph_position_t const* pos = hb_buffer_get_glyph_positions(hbBuf, nullptr);

        for (auto const i: iota(0u, glyphCount))
        {
            glyph_position gpos {};
            gpos.glyph = glyph_key { fontInfo.size, font, glyph_index { info[i].codepoint } };
#if defined(GLYPH_KEY_DEBUG)
            {
                auto const cluster = info[i].cluster;
                for (size_t k = 0; k < codepoints.size(); ++k)
                    if (clusters[k] == cluster)
                        gpos.glyph.text += codepoints[k];
            }
#endif
            gpos.offset.x =
                static_cast<int>(static_cast<double>(pos[i].x_offset) / 64.0); // gpos.offset.(x,y) ?
            gpos.offset.y = static_cast<int>(static_cast<double>(pos[i].y_offset) / 64.0f);
            gpos.advance.x = static_cast<int>(static_cast<double>(pos[i].x_advance) / 64.0f);
            gpos.advance.y = static_cast<int>(static_cast<double>(pos[i].y_advance) / 64.0f);
            gpos.presentation = presentation;
            result.emplace_back(gpos);
        }
        return crispy::none_of(result, glyphMissing);
    }
} // namespace

struct open_shaper::Private // {{{
{
    crispy::finally ftCleanup;
    FT_Library ft {};
    font_locator* locator = nullptr;
    DPI dpi;
    unordered_map<FontPathAndSize, font_key> fontPathAndSizeToKeyMapping;
    unordered_map<font_key, HbFontInfo> fontKeyToHbFontInfoMapping; // from font_key to FontInfo struct

    // Blacklisted font files as we tried them already and failed.
    std::vector<std::string> blacklistedSources;

    // The key (for caching) should be composed out of:
    // (file_path, file_mtime, font_weight, font_slant, pixel_size)

    unordered_map<glyph_key, rasterized_glyph> glyphs;
    hb_buffer_ptr hb_buf;
    font_key nextFontKey;

    font_key create_font_key()
    {
        auto result = nextFontKey;
        nextFontKey.value++;
        return result;
    }

    [[nodiscard]] bool has_color(font_key font) const noexcept
    {
        return FT_HAS_COLOR(fontKeyToHbFontInfoMapping.at(font).ftFace.get());
    }

    optional<font_key> getOrCreateKeyForFont(font_source const& source, font_size fontSize)
    {
        auto const sourceId = identifierOf(source);
        if (auto i = fontPathAndSizeToKeyMapping.find(FontPathAndSize { sourceId, fontSize });
            i != fontPathAndSizeToKeyMapping.end())
            return i->second;

        if (ranges::any_of(blacklistedSources, [&](auto const& a) { return a == sourceId; }))
            return nullopt;

        auto ftFacePtrOpt = loadFace(source, fontSize, dpi, ft);
        if (!ftFacePtrOpt.has_value())
        {
            blacklistedSources.emplace_back(sourceId);
            return nullopt;
        }

        auto ftFacePtr = std::move(ftFacePtrOpt.value());
        auto hbFontPtr =
            hb_font_ptr(hb_ft_font_create_referenced(ftFacePtr.get()), [](auto p) { hb_font_destroy(p); });

        auto fontInfo = HbFontInfo { source, {}, fontSize, std::move(ftFacePtr), std::move(hbFontPtr) };

        auto key = create_font_key();
        fontPathAndSizeToKeyMapping.emplace(pair { FontPathAndSize { sourceId, fontSize }, key });
        fontKeyToHbFontInfoMapping.emplace(pair { key, std::move(fontInfo) });
        locatorLog()(
            "Loading font: key={}, id=\"{}\" size={} dpi {} {}", key, sourceId, fontSize, dpi, metrics(key));
        return key;
    }

    font_metrics metrics(font_key key)
    {
        Require(fontKeyToHbFontInfoMapping.count(key) == 1);
        auto* ftFace = fontKeyToHbFontInfoMapping.at(key).ftFace.get();

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

    Private(DPI dpi, font_locator& locator):
        ftCleanup { [this]() {
            FT_Done_FreeType(ft);
        } },
        locator { &locator },
        dpi { dpi },
        hb_buf(hb_buffer_create(), [](auto p) { hb_buffer_destroy(p); }),
        nextFontKey {}
    {
        if (auto const ec = FT_Init_FreeType(&ft); ec != FT_Err_Ok)
            throw runtime_error { "freetype: Failed to initialize. "s + ftErrorStr(ec) };

        if (auto const ec = FT_Library_SetLcdFilter(ft, FT_LCD_FILTER_DEFAULT); ec != FT_Err_Ok)
            errorLog()("freetype: Failed to set LCD filter. {}", ftErrorStr(ec));
    }

    bool tryShapeWithFallback(font_key font,
                              HbFontInfo& fontInfo,
                              hb_buffer_t* hbBuf,
                              hb_font_t* hbFont,
                              unicode::Script script,
                              unicode::PresentationStyle presentation,
                              u32string_view codepoints,
                              gsl::span<unsigned> clusters,
                              shape_result& result)
    {
        auto const initialResultOffset = result.size();

        if (tryShape(font, fontInfo, hbBuf, hbFont, script, presentation, codepoints, clusters, result))
            return true;

        for (font_source const& fallbackFont: fontInfo.fallbacks)
        {
            result.resize(initialResultOffset); // rollback to initial size

            optional<font_key> fallbackKeyOpt = getOrCreateKeyForFont(fallbackFont, fontInfo.size);
            if (!fallbackKeyOpt.has_value())
                continue;

            // Skip if main font is monospace but fallbacks font is not.
            if (fontInfo.description.strict_spacing
                && fontInfo.description.spacing != font_spacing::proportional)
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
            textShapingLog()("Try fallbacks font key:{}, source: {}",
                             fallbackKeyOpt.value(),
                             fallbackFontInfo.primary);
            // clang-format on
            if (tryShape(fallbackKeyOpt.value(),
                         fallbackFontInfo,
                         hbBuf,
                         fallbackFontInfo.hbFont.get(),
                         script,
                         presentation,
                         codepoints,
                         clusters,
                         result))
                return true;
        }

        return false;
    }
}; // }}}

open_shaper::open_shaper(DPI dpi, font_locator& locator):
    _d(new Private(dpi, locator), [](Private* p) { delete p; })
{
}

void open_shaper::set_dpi(DPI dpi)
{
    if (!dpi)
        return;

    _d->dpi = dpi;
}

void open_shaper::set_locator(font_locator& locator)
{
    _d->locator = &locator;
}

void open_shaper::clear_cache()
{
    locatorLog()("Clearing cache ({} keys, {} font infos).",
                 _d->fontPathAndSizeToKeyMapping.size(),
                 _d->fontKeyToHbFontInfoMapping.size());
    _d->fontPathAndSizeToKeyMapping.clear();
    _d->fontKeyToHbFontInfoMapping.clear();
}

optional<font_key> open_shaper::load_font(font_description const& description, font_size size)
{
    font_source_list sources = _d->locator->locate(description);
    if (sources.empty())
        return nullopt;

    optional<font_key> fontKeyOpt = _d->getOrCreateKeyForFont(sources[0], size);
    if (!fontKeyOpt.has_value())
        return nullopt;

    sources.erase(sources.begin()); // remove primary font from list

    HbFontInfo& fontInfo = _d->fontKeyToHbFontInfoMapping.at(*fontKeyOpt);
    fontInfo.fallbacks = std::move(sources);
    fontInfo.description = description;

    return fontKeyOpt;
}

font_metrics open_shaper::metrics(font_key key) const
{
    Require(_d->fontKeyToHbFontInfoMapping.count(key) == 1);
    HbFontInfo& fontInfo = _d->fontKeyToHbFontInfoMapping.at(key);
    if (fontInfo.metrics.has_value())
        return fontInfo.metrics.value();

    fontInfo.metrics = _d->metrics(key);
    locatorLog()("Calculating font metrics for {}: {}", fontInfo.description, *fontInfo.metrics);
    return fontInfo.metrics.value();
}

optional<glyph_position> open_shaper::shape(font_key font, char32_t codepoint)
{
    Require(_d->fontKeyToHbFontInfoMapping.count(font) == 1);
    HbFontInfo& fontInfo = _d->fontKeyToHbFontInfoMapping.at(font);

    glyph_index glyphIndex { FT_Get_Char_Index(fontInfo.ftFace.get(), codepoint) };
    if (!glyphIndex.value)
    {
        for (font_source const& fallbackFont: fontInfo.fallbacks)
        {
            optional<font_key> fallbackKeyOpt = _d->getOrCreateKeyForFont(fallbackFont, fontInfo.size);
            if (!fallbackKeyOpt.has_value())
                continue;
            Require(_d->fontKeyToHbFontInfoMapping.count(fallbackKeyOpt.value()) == 1);
            HbFontInfo const& fallbackFontInfo = _d->fontKeyToHbFontInfoMapping.at(fallbackKeyOpt.value());
            glyphIndex = glyph_index { FT_Get_Char_Index(fallbackFontInfo.ftFace.get(), codepoint) };
            if (glyphIndex.value)
                break;
        }
    }
    if (!glyphIndex.value)
        return nullopt;

    glyph_position gpos {};
    gpos.glyph = glyph_key { fontInfo.size, font, glyphIndex };
#if defined(GLYPH_KEY_DEBUG)
    gpos.glyph.text = std::u32string(1, codepoint);
#endif
    gpos.advance.x = this->metrics(font).advance;
    gpos.offset = crispy::point {}; // TODO (load from glyph metrics. Is this needed?)

    return gpos;
}

void open_shaper::shape(font_key font,
                        u32string_view codepoints,
                        gsl::span<unsigned> clusters,
                        unicode::Script script,
                        unicode::PresentationStyle presentation,
                        shape_result& result)
{
    assert(clusters.size() == codepoints.size());
    textShapingLog()("Shaping using font key: {}, text: \"{}\"", font, unicode::convert_to<char>(codepoints));
    if (!_d->fontKeyToHbFontInfoMapping.count(font))
        textShapingLog()("Font not found? {}", font);

    Require(_d->fontKeyToHbFontInfoMapping.count(font) == 1);
    HbFontInfo& fontInfo = _d->fontKeyToHbFontInfoMapping.at(font);
    hb_font_t* hbFont = fontInfo.hbFont.get();
    hb_buffer_t* hbBuf = _d->hb_buf.get();

    if (textShapingLog)
    {
        auto logMessage = textShapingLog();
        logMessage.append("Shaping codepoints (");
        // clang-format off
        logMessage.append([=]() { auto s = ostringstream(); s << presentation; return s.str(); }());
        // clang-format on
        logMessage.append("):");
        for (auto [i, codepoint]: crispy::indexed(codepoints))
            logMessage.append(" {}:U+{:x}", clusters[i], static_cast<unsigned>(codepoint));
        logMessage.append("\n");
        logMessage.append("Using font: key={}, path=\"{}\"\n", font, identifierOf(fontInfo.primary));
    }

    if (_d->tryShapeWithFallback(
            font, fontInfo, hbBuf, hbFont, script, presentation, codepoints, clusters, result))
        return;

    textShapingLog()("Shaping failed.");

    // Reshape each cluster individually.
    result.clear();
    auto cluster = clusters[0];
    size_t start = 0;
    for (size_t i = 1; i < clusters.size(); ++i)
    {
        if (cluster != clusters[i])
        {
            size_t const count = i - start;
            _d->tryShapeWithFallback(font,
                                     fontInfo,
                                     hbBuf,
                                     hbFont,
                                     script,
                                     presentation,
                                     codepoints.substr(start, count),
                                     clusters.subspan(start, count),
                                     result);
            start = i;
            cluster = clusters[i];
        }
    }

    // shape last cluster
    auto const end = clusters.size();
    _d->tryShapeWithFallback(font,
                             fontInfo,
                             hbBuf,
                             hbFont,
                             script,
                             presentation,
                             codepoints.substr(start, end - start),
                             clusters.subspan(start, end - start),
                             result);

    // last resort
    replaceMissingGlyphs(fontInfo.ftFace.get(), result);
}

optional<rasterized_glyph> open_shaper::rasterize(glyph_key glyph, render_mode mode)
{
    auto const font = glyph.font;
    auto* ftFace = _d->fontKeyToHbFontInfoMapping.at(font).ftFace.get();
    auto const glyphIndex = glyph.index;
    auto const flags = static_cast<FT_Int32>(ftRenderFlag(mode) | (FT_HAS_COLOR(ftFace) ? FT_LOAD_COLOR : 0));

    FT_Error ec = FT_Load_Glyph(ftFace, glyphIndex.value, flags);
    if (ec != FT_Err_Ok)
    {
        auto const missingGlyph = FT_Get_Char_Index(ftFace, MissingGlyphId);

        if (missingGlyph)
            ec = FT_Load_Glyph(ftFace, missingGlyph, flags);

        if (ec != FT_Err_Ok)
        {
            if (locatorLog)
                locatorLog()("Error loading glyph index {} for font {} {}. {}",
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
        if (FT_Render_Glyph(ftFace->glyph, ftRenderMode(mode)) != FT_Err_Ok)
        {
            rasterizerLog()("Failed to rasterize glyph {}.", glyph);
            return nullopt;
        }
    }

    auto output = rasterized_glyph {};
    output.bitmapSize.width = crispy::width::cast_from(ftFace->glyph->bitmap.width);
    output.bitmapSize.height = crispy::height::cast_from(ftFace->glyph->bitmap.rows);
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

            auto const ec = FT_Bitmap_Convert(_d->ft, &ftFace->glyph->bitmap, &ftBitmap, 1);
            if (ec != FT_Err_Ok)
                return nullopt;

            ftBitmap.num_grays = 256;

            output.format = bitmap_format::alpha_mask;
            output.bitmap.resize(height.as<size_t>()
                                 * width.as<size_t>()); // 8-bit channel (with values 0 or 255)

            auto const pitch = static_cast<size_t>(ftBitmap.pitch);
            for (auto const i: iota(size_t { 0 }, static_cast<size_t>(ftBitmap.rows)))
                for (auto const j: iota(size_t { 0 }, static_cast<size_t>(ftBitmap.width)))
                    output.bitmap[i * width.as<size_t>() + j] = min(
                        static_cast<uint8_t>(uint8_t(ftBitmap.buffer[i * pitch + j]) * 255), uint8_t { 255 });

            FT_Bitmap_Done(_d->ft, &ftBitmap);
            break;
        }
        case FT_PIXEL_MODE_GRAY: {
            output.format = bitmap_format::alpha_mask;
            output.bitmap.resize(unbox<size_t>(output.bitmapSize.height)
                                 * unbox<size_t>(output.bitmapSize.width));

            auto const pitch = static_cast<unsigned>(ftFace->glyph->bitmap.pitch);
            auto const* const s = ftFace->glyph->bitmap.buffer;
            for (auto const i: iota(0u, *output.bitmapSize.height))
                for (auto const j: iota(0u, *output.bitmapSize.width))
                    output.bitmap[i * *output.bitmapSize.width + j] = s[i * pitch + j];
            break;
        }
        case FT_PIXEL_MODE_LCD: {
            auto const& ftBitmap = ftFace->glyph->bitmap;
            // rasterizerLog()("Rasterizing using pixel mode: {}, rows={}, width={}, pitch={}, mode={}",
            //                 "lcd",
            //                 ftBitmap.rows,
            //                 ftBitmap.width / 3,
            //                 ftBitmap.pitch,
            //                 ftBitmap.pixel_mode);

            output.format = bitmap_format::rgb; // LCD
            output.bitmap.resize(static_cast<size_t>(ftBitmap.width) * static_cast<size_t>(ftBitmap.rows));
            output.bitmapSize.width /= crispy::width(3);

            auto const* s = ftBitmap.buffer;
            auto* t = output.bitmap.data();
            if (ftBitmap.width == static_cast<unsigned>(std::abs(ftBitmap.pitch)))
            {
                std::copy_n(s, ftBitmap.width * ftBitmap.rows, t);
            }
            else
            {
                for (auto const _: iota(0u, ftBitmap.rows))
                {
                    crispy::ignore_unused(_);
                    std::copy_n(s, ftBitmap.width, t);
                    s += ftBitmap.pitch;
                    t += ftBitmap.width;
                }
            }
            break;
        }
        case FT_PIXEL_MODE_BGRA: {
            auto const width = output.bitmapSize.width;
            auto const height = output.bitmapSize.height;
            // rasterizerLog()("rasterize.RGBA: {} + {}\n", output.bitmapSize, output.position);

            output.format = bitmap_format::rgba;
            output.bitmap.resize(output.bitmapSize.area() * 4);
            auto t = output.bitmap.begin();

            auto const pitch = static_cast<unsigned>(ftFace->glyph->bitmap.pitch);
            for (auto const i: iota(0u, height.as<size_t>()))
            {
                for (auto const j: iota(0u, width.as<size_t>()))
                {
                    auto const* s =
                        &ftFace->glyph->bitmap
                             .buffer[static_cast<size_t>(i) * pitch + static_cast<size_t>(j) * 4u];

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
            rasterizerLog()("Glyph requested that has an unsupported pixel_mode:{}",
                            ftFace->glyph->bitmap.pixel_mode);
            return nullopt;
    }

    Ensures(output.valid());

    if (rasterizerLog)
        rasterizerLog()("rasterize {} to {}", glyph, output);

    return output;
}

} // namespace text
