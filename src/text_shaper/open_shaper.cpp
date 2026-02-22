// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/font.h>
#include <text_shaper/font_locator.h>
#include <text_shaper/open_shaper.h>

#include <crispy/algorithm.h>
#include <crispy/assert.h>
#include <crispy/times.h>

#include <libunicode/convert.h>
#include <libunicode/ucd_fmt.h>

#if __has_include(<cairo-ft.h>)
    #include <cairo-ft.h>
    #define CONTOUR_HAS_CAIRO 1
#endif

#include <ft2build.h>
#include FT_BITMAP_H
#include FT_ERRORS_H
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_LCD_FILTER_H
#include FT_STROKER_H

#if __has_include(<fontconfig/fontconfig.h>)
    #include <fontconfig/fontconfig.h>
#endif

#include <harfbuzz/hb-ft.h>
#include <harfbuzz/hb.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>

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
using std::views::iota;

using namespace std::string_literals;
using namespace std::string_view_literals;

namespace
{

struct FontInfo // NOLINT(readability-identifier-naming)
{
    string path;
    text::font_size size;
    text::font_weight weight;
};

[[maybe_unused]] bool operator==(FontInfo const& a, FontInfo const& b) noexcept
{
    return a.path == b.path && a.size.pt == b.size.pt && a.weight == b.weight;
}

} // namespace

#if defined(CONTOUR_HAS_CAIRO)
void cleanup_cairo_font_face(void*)
{
    // No-op destructor callback: the FT_Face lifetime is managed elsewhere.
}

std::optional<text::rasterized_glyph> rasterizeWithCairo(FT_Face ftFace,
                                                         text::glyph_key glyph,
                                                         text::render_mode /*mode*/)
{
    // 1. Setup Cairo surface
    auto width = static_cast<int>(ceil(static_cast<double>(ftFace->glyph->metrics.width) / 64.0));
    auto height = static_cast<int>(ceil(static_cast<double>(ftFace->glyph->metrics.height) / 64.0));

    // If FreeType doesn't report metrics (e.g. some COLRv1 fonts?), measure with Cairo.
    if (width <= 0 || height <= 0)
    {
        auto* dummySurface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        auto* cr = cairo_create(dummySurface);

        auto* fontFace = cairo_ft_font_face_create_for_ft_face(ftFace, 0);
        cairo_font_face_set_user_data(fontFace, nullptr, ftFace, cleanup_cairo_font_face);
        cairo_set_font_face(cr, fontFace);
        cairo_set_font_size(cr, static_cast<double>(ftFace->size->metrics.y_ppem));

        // Options
        auto* options = cairo_font_options_create();
        cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_BEST);
        cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_NONE);
        cairo_font_options_set_color_palette(options, 0);
        cairo_set_font_options(cr, options);
        cairo_font_options_destroy(options);

        auto cairoGlyph = cairo_glyph_t { .index = glyph.index.value, .x = 0, .y = 0 };
        auto extents = cairo_text_extents_t {};
        cairo_glyph_extents(cr, &cairoGlyph, 1, &extents);

        width = static_cast<int>(ceil(extents.width));
        height = static_cast<int>(ceil(extents.height));

        cairo_font_face_destroy(fontFace);
        cairo_destroy(cr);
        cairo_surface_destroy(dummySurface);

        if (width <= 0 || height <= 0)
            return std::nullopt;
    }

    auto const stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
    auto buffer = std::vector<uint8_t>(static_cast<size_t>(stride * height));

    auto surface =
        cairo_image_surface_create_for_data(buffer.data(), CAIRO_FORMAT_ARGB32, width, height, stride);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
    {
        cairo_surface_destroy(surface);
        return std::nullopt;
    }

    auto cr = cairo_create(surface);

    // 2. Create/Set Cairo Font Face
    auto* fontFace = cairo_ft_font_face_create_for_ft_face(ftFace, 0);
    cairo_font_face_set_user_data(fontFace, nullptr, ftFace, cleanup_cairo_font_face);
    cairo_set_font_face(cr, fontFace);

    // 3. Set Size (points)
    cairo_set_font_size(cr, static_cast<double>(ftFace->size->metrics.y_ppem)); // Set to pixel size

    // Check Status
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS || cairo_font_face_status(fontFace) != CAIRO_STATUS_SUCCESS)
    {
        cairo_font_face_destroy(fontFace);
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return std::nullopt;
    }

    // 4. Set Options
    auto* options = cairo_font_options_create();
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_BEST);
    cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_NONE);
    cairo_font_options_set_color_palette(options, 0);
    cairo_set_font_options(cr, options);
    cairo_font_options_destroy(options);

    // 5. Render Glyph
    auto cairoGlyph = cairo_glyph_t { .index = glyph.index.value, .x = 0, .y = 0 };

    auto extents = cairo_text_extents_t {};
    cairo_glyph_extents(cr, &cairoGlyph, 1, &extents);

    cairoGlyph.x = -extents.x_bearing;
    cairoGlyph.y = -extents.y_bearing;

    cairo_show_glyphs(cr, &cairoGlyph, 1);
    cairo_surface_flush(surface);

    // 6. Copy to output
    auto output = text::rasterized_glyph {};
    output.bitmapSize.width = vtbackend::Width::cast_from(width);
    output.bitmapSize.height = vtbackend::Height::cast_from(height);
    output.position.x = static_cast<int>(floor(extents.x_bearing));
    output.position.y = static_cast<int>(floor(-extents.y_bearing));
    output.format = text::bitmap_format::rgba;
    output.bitmap = std::move(buffer);

    size_t const pixelCount = static_cast<size_t>(width * height);
    auto* pixels = reinterpret_cast<uint32_t*>(output.bitmap.data());
    for (size_t i = 0; i < pixelCount; ++i)
    {
        uint32_t const p = pixels[i];
        uint8_t const a = (p >> 24) & 0xff;
        if (a > 0)
        {
            uint8_t r = (p >> 16) & 0xff;
            uint8_t g = (p >> 8) & 0xff;
            uint8_t b = (p >> 0) & 0xff;

            // Unpremultiply
            if (a < 255)
            {
                r = static_cast<uint8_t>(static_cast<int>(r) * 255 / static_cast<int>(a));
                g = static_cast<uint8_t>(static_cast<int>(g) * 255 / static_cast<int>(a));
                b = static_cast<uint8_t>(static_cast<int>(b) * 255 / static_cast<int>(a));
            }

            // Re-pack as RGBA (byte order: R G B A -> LE int: 0xAABBGGRR)
            pixels[i] = (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16)
                        | (static_cast<uint32_t>(g) << 8) | (static_cast<uint32_t>(r));
        }
    }

    cairo_font_face_destroy(fontFace);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    return output;
}
#endif // CONTOUR_HAS_CAIRO

namespace std
{
template <>
struct hash<FontInfo>
{
    size_t operator()(FontInfo const& fd) const noexcept
    {
        auto fnv = crispy::fnv<char>();
        return size_t(
            fnv(fnv(fd.path), to_string(fd.size.pt), std::format("{}", fd.weight))); // SSO should kick in.
    }
};
} // namespace std

namespace text
{

using hb_buffer_ptr = unique_ptr<hb_buffer_t, void (*)(hb_buffer_t*)>;
using hb_font_ptr = unique_ptr<hb_font_t, void (*)(hb_font_t*)>;
using ft_face_ptr = unique_ptr<FT_FaceRec_, void (*)(FT_FaceRec_*)>;

auto constexpr MissingGlyphId = 0xFFFDu;

/// Maximum number of fallback fonts loaded initially per font key.
/// Additional fallbacks are loaded on demand when a glyph isn't found
/// in the initial set.
constexpr size_t InitialFallbackCount = 8;

struct HbFontInfo // NOLINT(readability-identifier-naming)
{
    font_source primary;
    font_source_list fallbacks;
    font_source_list allFallbacks; ///< Complete fallback list for on-demand extension.
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
            FT_Error const ec =
                FT_New_Face(ft, sourcePath.value.c_str(), sourcePath.collectionIndex, &ftFace);
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
            int const faceIndex = 0;
            auto const& memory = get<font_memory_ref>(source);
            FT_Error const ec = FT_New_Memory_Face(
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

        bool sizeSet = false;
        if (FT_HAS_COLOR(ftFace))
        {
            if (FT_Palette_Select(ftFace, 0, nullptr) != FT_Err_Ok)
                rasterizerLog()("Failed to select default palette for font {}.", ftFace->family_name);

            auto const strikeIndexOpt = ftBestStrikeIndex(ftFace, fontSize.pt, dpi);
            if (strikeIndexOpt.has_value())
            {
                auto const strikeIndex = strikeIndexOpt.value();
                FT_Error const ec = FT_Select_Size(ftFace, strikeIndex);
                if (ec != FT_Err_Ok)
                    errorLog()("Failed to FT_Select_Size(index={}, source {}): {}",
                               strikeIndex,
                               source,
                               ftErrorStr(ec));
                else
                    rasterizerLog()("Picked color font's strike index {} ({}x{}) from {}\n",
                                    strikeIndex,
                                    ftFace->available_sizes[strikeIndex].width,
                                    ftFace->available_sizes[strikeIndex].height,
                                    source);
                sizeSet = true;
            }
        }

        if (!sizeSet)
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

        for (size_t i = 0; i < result.size(); ++i)
        {
            auto& gpos = result[i];
            if (glyphMissing(gpos))
                gpos.glyph.index = glyph_index { missingGlyph };
        }
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
            gpos.glyph =
                glyph_key { .size = fontInfo.size, .font = font, .index = glyph_index { info[i].codepoint } };
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

struct open_shaper::private_open_shaper // {{{
{
    crispy::finally ftCleanup;
    FT_Library ft {};
    font_locator* locator = nullptr;
    DPI dpi;
    // Default must match vtrasterizer::DefaultMaxFallbackCount.
    // Cannot include the header directly due to dependency direction (vtrasterizer depends on text_shaper).
    // The actual value is passed at runtime via set_font_fallback_limit().
    int fontFallbackLimit = 16; ///< Maximum total fallback fonts per key. -1 = unlimited, 0 = disabled.
    unordered_map<FontInfo, font_key> fontPathAndSizeToKeyMapping;
    unordered_map<font_key, HbFontInfo> fontKeyToHbFontInfoMapping; // from font_key to FontInfo struct

    /// Persistent cache for locate() results.
    /// Survives clear_cache() since font descriptions map to the same font files
    /// regardless of DPI or font size changes.
    unordered_map<font_description, font_source_list> locateCache;

    // Blacklisted font files as we tried them already and failed.
    std::vector<std::string> blacklistedSources;

    // The key (for caching) should be composed out of:
    // (file_path, file_mtime, font_weight, font_slant, pixel_size)

    unordered_map<glyph_key, rasterized_glyph> glyphs;
    hb_buffer_ptr hbBuf;
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

    optional<font_key> getOrCreateKeyForFont(font_source const& source,
                                             font_size fontSize,
                                             font_weight fontWeight)
    {
        auto const sourceId = identifierOf(source);
        if (auto i = fontPathAndSizeToKeyMapping.find(
                FontInfo { .path = sourceId, .size = fontSize, .weight = fontWeight });
            i != fontPathAndSizeToKeyMapping.end())
            return i->second;

        if (std::any_of(blacklistedSources.begin(), blacklistedSources.end(), [&](auto const& a) {
                return a == sourceId;
            }))
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

        auto fontInfo = HbFontInfo { .primary = source,
                                     .fallbacks = {},
                                     .allFallbacks = {},
                                     .size = fontSize,
                                     .ftFace = std::move(ftFacePtr),
                                     .hbFont = std::move(hbFontPtr) };

        auto key = create_font_key();
        fontPathAndSizeToKeyMapping.emplace(
            pair { FontInfo { .path = sourceId, .size = fontSize, .weight = fontWeight }, key });
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

        output.lineHeight = scaleVertical(ftFace, ftFace->height);
        output.advance = computeAverageAdvance(ftFace);
        if (!output.advance)
            output.advance = int(double(output.lineHeight) * 2.0 / 3.0);
        output.ascender = scaleVertical(ftFace, ftFace->ascender);
        output.descender = scaleVertical(ftFace, ftFace->descender);
        output.underlinePosition = scaleVertical(ftFace, ftFace->underline_position);
        output.underlineThickness = scaleVertical(ftFace, ftFace->underline_thickness);

        return output;
    }

    private_open_shaper(DPI dpi, font_locator& locator):
        ftCleanup { [this]() { FT_Done_FreeType(ft); } },
        locator { &locator },
        dpi { dpi },
        hbBuf(hb_buffer_create(), [](auto p) { hb_buffer_destroy(p); }),
        nextFontKey {}
    {
        if (auto const ec = FT_Init_FreeType(&ft); ec != FT_Err_Ok)
            throw runtime_error { "freetype: Failed to initialize. "s + ftErrorStr(ec) };

        if (auto const ec = FT_Library_SetLcdFilter(ft, FT_LCD_FILTER_DEFAULT); ec != FT_Err_Ok)
            errorLog()("freetype: Failed to set LCD filter. {}", ftErrorStr(ec));
    }

    /// Extends fontInfo.fallbacks by appending the next batch from allFallbacks.
    /// Returns true if new fallbacks were added.
    static bool extendFallbacks(HbFontInfo& fontInfo)
    {
        auto const currentCount = fontInfo.fallbacks.size();
        auto const totalCount = fontInfo.allFallbacks.size();
        if (currentCount >= totalCount)
            return false;

        auto const nextBatchEnd = std::min(currentCount + InitialFallbackCount, totalCount);
        fontInfo.fallbacks.insert(fontInfo.fallbacks.end(),
                                  fontInfo.allFallbacks.begin() + static_cast<ptrdiff_t>(currentCount),
                                  fontInfo.allFallbacks.begin() + static_cast<ptrdiff_t>(nextBatchEnd));
        return true;
    }

    /// Updates an existing FT_Face's char size to the new DPI in-place,
    /// avoiding the cost of reloading the font file from disk.
    void updateFaceDpi(HbFontInfo& fontInfo, DPI newDpi)
    {
        auto* ftFace = fontInfo.ftFace.get();

        if (FT_HAS_COLOR(ftFace))
        {
            auto const strikeIndexOpt = ftBestStrikeIndex(ftFace, fontInfo.size.pt, newDpi);
            if (strikeIndexOpt.has_value())
                if (auto const ec = FT_Select_Size(ftFace, *strikeIndexOpt); ec != FT_Err_Ok)
                    errorLog()("Failed to FT_Select_Size(index={}) during DPI update: {}",
                               *strikeIndexOpt,
                               ftErrorStr(ec));
        }
        else
        {
            auto const size = static_cast<FT_F26Dot6>(ceil(fontInfo.size.pt * 64.0));
            if (auto const ec = FT_Set_Char_Size(
                    ftFace, size, 0, static_cast<FT_UInt>(newDpi.x), static_cast<FT_UInt>(newDpi.y));
                ec != FT_Err_Ok)
                errorLog()("Failed to FT_Set_Char_Size during DPI update: {}", ftErrorStr(ec));
        }

        // Notify HarfBuzz that the underlying FT_Face metrics changed.
        hb_ft_font_changed(fontInfo.hbFont.get());

        // Invalidate cached metrics so they are recomputed on next access.
        fontInfo.metrics.reset();
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

        // Try fallbacks, extending the list on demand from allFallbacks when exhausted.
        // NB: We use an index-based while loop because extendFallbacks() may grow
        // fontInfo.fallbacks during iteration; range-based loops would miss new entries.
        auto fallbackIndex = size_t { 0 };
        while (fallbackIndex < fontInfo.fallbacks.size())
        {
            result.resize(initialResultOffset); // rollback to initial size

            auto const& fallbackFont = fontInfo.fallbacks[fallbackIndex];
            auto fallbackKeyOpt =
                getOrCreateKeyForFont(fallbackFont, fontInfo.size, fontInfo.description.weight);
            if (!fallbackKeyOpt.has_value())
            {
                ++fallbackIndex;
                continue;
            }

            // Skip if main font is monospace but fallbacks font is not.
            if (fontInfo.description.strictSpacing
                && fontInfo.description.spacing != font_spacing::proportional)
            {
                Require(fontKeyToHbFontInfoMapping.count(fallbackKeyOpt.value()) == 1);
                auto const& fallbackFontInfo = fontKeyToHbFontInfoMapping.at(fallbackKeyOpt.value());
                auto const fontIsMonospace = fallbackFontInfo.ftFace->face_flags & FT_FACE_FLAG_FIXED_WIDTH;
                if (!fontIsMonospace)
                {
                    ++fallbackIndex;
                    continue;
                }
            }

            Require(fontKeyToHbFontInfoMapping.count(fallbackKeyOpt.value()) == 1);
            auto& fallbackFontInfo = fontKeyToHbFontInfoMapping.at(fallbackKeyOpt.value());
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

            ++fallbackIndex;

            // If we've exhausted the current fallback list, try extending it.
            if (fallbackIndex == fontInfo.fallbacks.size())
                extendFallbacks(fontInfo);
        }

        return false;
    }
}; // }}}

open_shaper::open_shaper(DPI dpi, font_locator& locator):
    _d(new private_open_shaper(dpi, locator), [](private_open_shaper* p) { delete p; })
{
}

void open_shaper::set_dpi(DPI dpi)
{
    if (!dpi)
        return;

    auto const oldDpi = _d->dpi;
    _d->dpi = dpi;

    if (oldDpi == dpi)
        return;

    // Update all existing FT_Face objects in-place with the new DPI,
    // avoiding the cost of destroying and reloading fonts from disk.
    for (auto& [key, fontInfo]: _d->fontKeyToHbFontInfoMapping)
        _d->updateFaceDpi(fontInfo, dpi);
}

void open_shaper::set_locator(font_locator& locator)
{
    _d->locator = &locator;
}

void open_shaper::set_font_fallback_limit(int limit)
{
    _d->fontFallbackLimit = limit;
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
    // Check the persistent locate cache before calling into fontconfig.
    auto cacheIt = _d->locateCache.find(description);
    if (cacheIt == _d->locateCache.end())
    {
        auto sources = _d->locator->locate(description);
        cacheIt = _d->locateCache.emplace(description, std::move(sources)).first;
    }

    auto const& cachedSources = cacheIt->second;
    if (cachedSources.empty())
        return nullopt;

    auto fontKeyOpt = _d->getOrCreateKeyForFont(cachedSources[0], size, description.weight);
    if (!fontKeyOpt.has_value())
        return nullopt;

    // Build the full fallback list (excluding the primary font).
    auto allFallbacks = font_source_list(cachedSources.begin() + 1, cachedSources.end());

    // Apply the global fallback limit.
    if (_d->fontFallbackLimit == 0)
    {
        allFallbacks.clear();
    }
    else if (_d->fontFallbackLimit > 0 && allFallbacks.size() > static_cast<size_t>(_d->fontFallbackLimit))
    {
        allFallbacks.resize(static_cast<size_t>(_d->fontFallbackLimit));
    }
    // fontFallbackLimit == -1 means unlimited — keep all.

    // Initially load only a limited number of fallbacks; the rest are extended on demand.
    auto const initialCount = std::min(allFallbacks.size(), InitialFallbackCount);
    auto initialFallbacks =
        font_source_list(allFallbacks.begin(), allFallbacks.begin() + static_cast<ptrdiff_t>(initialCount));

    auto& fontInfo = _d->fontKeyToHbFontInfoMapping.at(*fontKeyOpt);
    fontInfo.fallbacks = std::move(initialFallbacks);
    fontInfo.allFallbacks = std::move(allFallbacks);
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
    auto& fontInfo = _d->fontKeyToHbFontInfoMapping.at(font);

    glyph_index glyphIndex { FT_Get_Char_Index(fontInfo.ftFace.get(), codepoint) };
    if (!glyphIndex.value)
    {
        // Try fallbacks with on-demand extension from allFallbacks.
        // NB: while loop because extendFallbacks() may grow the list during iteration.
        auto fallbackIndex = size_t { 0 };
        while (fallbackIndex < fontInfo.fallbacks.size())
        {
            auto const& fallbackFont = fontInfo.fallbacks[fallbackIndex];
            auto fallbackKeyOpt =
                _d->getOrCreateKeyForFont(fallbackFont, fontInfo.size, fontInfo.description.weight);
            if (!fallbackKeyOpt.has_value())
            {
                ++fallbackIndex;
                continue;
            }
            Require(_d->fontKeyToHbFontInfoMapping.count(fallbackKeyOpt.value()) == 1);
            auto const& fallbackFontInfo = _d->fontKeyToHbFontInfoMapping.at(fallbackKeyOpt.value());
            glyphIndex = glyph_index { FT_Get_Char_Index(fallbackFontInfo.ftFace.get(), codepoint) };
            if (glyphIndex.value)
                break;

            ++fallbackIndex;

            // Extend fallbacks on demand when we've exhausted the current list.
            if (fallbackIndex == fontInfo.fallbacks.size())
                private_open_shaper::extendFallbacks(fontInfo);
        }
    }
    if (!glyphIndex.value)
        return nullopt;

    glyph_position gpos {};
    gpos.glyph = glyph_key { .size = fontInfo.size, .font = font, .index = glyphIndex };
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
    hb_buffer_t* hbBuf = _d->hbBuf.get();

    if (textShapingLog)
    {
        auto logMessage = textShapingLog();
        logMessage.append("Shaping codepoints (");
        // clang-format off
        logMessage.append([=]() { auto s = ostringstream(); s << presentation; return s.str(); }());
        // clang-format on
        logMessage.append("):");
        size_t i = 0;
        for (auto const codepoint: codepoints)
        {
            logMessage.append(" {}:U+{:x}", clusters[i], static_cast<unsigned>(codepoint));
            ++i;
        }
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

/// Rasterizes a glyph with a pre-computed FT_Stroker outline into a two-channel RGBA bitmap.
///
/// The fill glyph is stored in the R channel, the outline in the G channel, B=0, A=max(R,G).
/// This allows the fragment shader to composite fill color over outline color at render time
/// without re-rasterization when colors change.
///
/// @param ftLib           FreeType library handle.
/// @param ftFace          FreeType font face (already sized).
/// @param glyph           glyph key for logging.
/// @param glyphIndex      FreeType glyph index.
/// @param outlineThickness outline radius in pixel units.
///
/// @return rasterized_glyph with bitmap_format::outlined, or nullopt on failure.
optional<rasterized_glyph> rasterizeOutlined(
    FT_Library ftLib, FT_Face ftFace, glyph_key const& glyph, glyph_index glyphIndex, float outlineThickness)
{
    // Load the glyph outline (vector, not bitmap).
    auto ec = FT_Load_Glyph(ftFace, glyphIndex.value, FT_LOAD_NO_BITMAP);
    if (ec != FT_Err_Ok)
    {
        rasterizerLog()("rasterizeOutlined: FT_Load_Glyph failed for {}.", glyph);
        return nullopt;
    }

    // FT_Stroker requires vector outlines. Bail if the glyph is already a bitmap.
    if (ftFace->glyph->format != FT_GLYPH_FORMAT_OUTLINE)
    {
        rasterizerLog()("rasterizeOutlined: glyph {} is not an outline glyph, skipping stroke.", glyph);
        return nullopt;
    }

    // Extract two copies of the glyph: one for fill, one for outline.
    FT_Glyph fillGlyph = nullptr;
    FT_Glyph outlineGlyph = nullptr;
    ec = FT_Get_Glyph(ftFace->glyph, &fillGlyph);
    if (ec != FT_Err_Ok)
        return nullopt;

    ec = FT_Get_Glyph(ftFace->glyph, &outlineGlyph);
    if (ec != FT_Err_Ok)
    {
        FT_Done_Glyph(fillGlyph);
        return nullopt;
    }

    // Rasterize the fill glyph (grayscale).
    // Outlined glyphs always use FT_RENDER_MODE_NORMAL regardless of the configured
    // render_mode because the fill and outline are composited as separate alpha channels
    // in the shader. Sub-pixel (LCD) rendering is not applicable to this two-channel format.
    ec = FT_Glyph_To_Bitmap(&fillGlyph, FT_RENDER_MODE_NORMAL, nullptr, true);
    if (ec != FT_Err_Ok)
    {
        FT_Done_Glyph(fillGlyph);
        FT_Done_Glyph(outlineGlyph);
        return nullopt;
    }

    // Create the stroker and apply it to the outline glyph.
    FT_Stroker stroker = nullptr;
    ec = FT_Stroker_New(ftLib, &stroker);
    if (ec != FT_Err_Ok)
    {
        rasterizerLog()("rasterizeOutlined: FT_Stroker_New failed for {} (ec={}).", glyph, ec);
        FT_Done_Glyph(fillGlyph);
        FT_Done_Glyph(outlineGlyph);
        return nullopt;
    }
    FT_Stroker_Set(stroker,
                   static_cast<FT_Fixed>(outlineThickness * 64.0f), // 26.6 fixed-point
                   FT_STROKER_LINECAP_ROUND,
                   FT_STROKER_LINEJOIN_ROUND,
                   0);

    // Apply the full stroke (both inside and outside borders).
    // Using FT_Glyph_Stroke rather than FT_Glyph_StrokeBorder for robustness:
    // the full stroke always produces a well-formed closed outline regardless of
    // the glyph's winding direction. The overlap with the fill area is harmless
    // because the shader composites fill OVER outline.
    ec = FT_Glyph_Stroke(&outlineGlyph, stroker, true /*destroy*/);
    FT_Stroker_Done(stroker);
    if (ec != FT_Err_Ok)
    {
        rasterizerLog()("rasterizeOutlined: FT_Glyph_Stroke failed for {} (ec={}).", glyph, ec);
        FT_Done_Glyph(fillGlyph);
        FT_Done_Glyph(outlineGlyph);
        return nullopt;
    }

    // Rasterize the stroked outline glyph (grayscale).
    ec = FT_Glyph_To_Bitmap(&outlineGlyph, FT_RENDER_MODE_NORMAL, nullptr, true);
    if (ec != FT_Err_Ok)
    {
        rasterizerLog()("rasterizeOutlined: FT_Glyph_To_Bitmap failed for outline of {} (ec={}).", glyph, ec);
        FT_Done_Glyph(fillGlyph);
        FT_Done_Glyph(outlineGlyph);
        return nullopt;
    }

    auto* fillBitmapGlyph = reinterpret_cast<FT_BitmapGlyph>(fillGlyph);
    auto* outlineBitmapGlyph = reinterpret_cast<FT_BitmapGlyph>(outlineGlyph);

    auto const& fillBmp = fillBitmapGlyph->bitmap;
    auto const& outlineBmp = outlineBitmapGlyph->bitmap;

    // Guard against empty outline bitmap (degenerate glyph or stroker failure).
    if (outlineBmp.width == 0 || outlineBmp.rows == 0 || outlineBmp.buffer == nullptr)
    {
        rasterizerLog()("rasterizeOutlined: outline bitmap is empty for {}.", glyph);
        FT_Done_Glyph(fillGlyph);
        FT_Done_Glyph(outlineGlyph);
        return nullopt;
    }

    // The outline bitmap is larger than the fill (extends outward).
    // Compute the offset of the fill within the outline bitmap using their bearings.
    auto const fillOffsetX = fillBitmapGlyph->left - outlineBitmapGlyph->left;
    auto const fillOffsetY = outlineBitmapGlyph->top - fillBitmapGlyph->top;

    auto const outWidth = static_cast<int>(outlineBmp.width);
    auto const outHeight = static_cast<int>(outlineBmp.rows);

    // Composite into RGBA: R=fill, G=outline, B=0, A=max(fill,outline)
    auto output = rasterized_glyph {};
    output.index = glyphIndex;
    output.bitmapSize.width = vtbackend::Width::cast_from(outWidth);
    output.bitmapSize.height = vtbackend::Height::cast_from(outHeight);
    output.position.x = outlineBitmapGlyph->left;
    output.position.y = outlineBitmapGlyph->top;
    output.format = bitmap_format::outlined;
    output.bitmap.resize(static_cast<size_t>(outWidth) * static_cast<size_t>(outHeight) * 4);

    // FT_Bitmap::pitch is signed: positive means rows are top-down, negative means bottom-up.
    // In both cases, buffer points to the first scanline (top row) and row * pitch + col
    // correctly addresses each pixel regardless of pitch sign.
    auto const outPitch = static_cast<int>(outlineBmp.pitch);
    auto const fillPitch = static_cast<int>(fillBmp.pitch);
    auto const fillW = static_cast<int>(fillBmp.width);
    auto const fillH = static_cast<int>(fillBmp.rows);

    for (auto const row: iota(0, outHeight))
    {
        for (auto const col: iota(0, outWidth))
        {
            auto const pixelIdx = static_cast<size_t>((row * outWidth) + col) * 4;

            // Outline alpha from G channel
            auto const outlineAlpha = outlineBmp.buffer[row * outPitch + col];

            // Fill alpha from R channel (offset into the outline bitmap)
            auto const fillRow = row - fillOffsetY;
            auto const fillCol = col - fillOffsetX;
            uint8_t fillAlpha = 0;
            if (fillRow >= 0 && fillRow < fillH && fillCol >= 0 && fillCol < fillW)
                fillAlpha = fillBmp.buffer[fillRow * fillPitch + fillCol];

            output.bitmap[pixelIdx + 0] = fillAlpha;                    // R = fill
            output.bitmap[pixelIdx + 1] = outlineAlpha;                 // G = outline
            output.bitmap[pixelIdx + 2] = 0;                            // B = unused
            output.bitmap[pixelIdx + 3] = max(fillAlpha, outlineAlpha); // A = max
        }
    }

    FT_Done_Glyph(fillGlyph);
    FT_Done_Glyph(outlineGlyph);

    Ensures(output.valid());

    if (rasterizerLog)
        rasterizerLog()("rasterizeOutlined {} to {}", glyph, output);

    return output;
}

optional<rasterized_glyph> open_shaper::rasterize(glyph_key glyph, render_mode mode, float outlineThickness)
{
    auto const font = glyph.font;
    auto* ftFace = _d->fontKeyToHbFontInfoMapping.at(font).ftFace.get();
    auto const glyphIndex = glyph.index;

    // When outline is requested, try the FT_Stroker path first.
    // This requires vector outlines; bitmap/emoji fonts fall through to normal rendering.
    if (outlineThickness > 0.0f && !FT_HAS_COLOR(ftFace))
    {
        if (auto result = rasterizeOutlined(_d->ft, ftFace, glyph, glyphIndex, outlineThickness))
            return result;
        rasterizerLog()("WARNING: rasterizeOutlined failed for glyph {}, falling back to normal rendering.",
                        glyph);
        // Fall through to normal rendering if stroking fails (e.g., bitmap-only font).
    }

    auto const flags = static_cast<FT_Int32>(FT_HAS_COLOR(ftFace) ? FT_LOAD_COLOR : ftRenderFlag(mode));

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

    // NB: color bitmap fonts (like Noto Color Emoji) are bitmap fonts, they do not need rendering
    // But vector color fonts (like Noto COLRv1) do.
    if (FT_HAS_COLOR(ftFace))
    {
        if (ftFace->glyph->format != FT_GLYPH_FORMAT_BITMAP)
        {
#if defined(CONTOUR_HAS_CAIRO)
            if (auto result = rasterizeWithCairo(ftFace, glyph, mode))
                return result;
            // If Cairo fails, fall through to FreeType rendering (which might produce outlines or empty
            // bitmaps)
#endif
        }
    }

    if (ftFace->glyph->format != FT_GLYPH_FORMAT_BITMAP)
    {
        auto const renderMode = FT_HAS_COLOR(ftFace) ? FT_RENDER_MODE_NORMAL : ftRenderMode(mode);
        if (FT_Render_Glyph(ftFace->glyph, renderMode) != FT_Err_Ok)
        {
            rasterizerLog()("Failed to rasterize glyph {}.", glyph);
            return nullopt;
        }
    }

    auto output = rasterized_glyph {};
    output.bitmapSize.width = vtbackend::Width::cast_from(ftFace->glyph->bitmap.width);
    output.bitmapSize.height = vtbackend::Height::cast_from(ftFace->glyph->bitmap.rows);
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
                    output.bitmap[(i * width.as<size_t>()) + j] =
                        min(static_cast<uint8_t>(uint8_t(ftBitmap.buffer[(i * pitch) + j]) * 255),
                            uint8_t { 255 });

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
                    output.bitmap[(i * unbox(output.bitmapSize.width)) + j] = s[(i * pitch) + j];
            break;
        }
        case FT_PIXEL_MODE_LCD: {
            auto const& ftBitmap = ftFace->glyph->bitmap;

            output.format = bitmap_format::rgb; // LCD
            output.bitmap.resize(static_cast<size_t>(ftBitmap.width) * static_cast<size_t>(ftBitmap.rows));
            output.bitmapSize.width /= vtbackend::Width(3);

            auto const* s = ftBitmap.buffer;
            auto* t = output.bitmap.data();
            if (std::cmp_equal(ftBitmap.width, std::abs(ftBitmap.pitch)))
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
                             .buffer[(static_cast<size_t>(i) * pitch) + (static_cast<size_t>(j) * 4u)];

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
