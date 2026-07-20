// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/open_shaper.h>

#include <text_shaper/cluster_spans.h>
#include <text_shaper/font.h>
#include <text_shaper/font_locator.h>

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
#include <span>
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

/// How many distinct sizes resize_font() may open a face at before it stops opening new ones.
///
/// `OSC 66` draws at `s * n/d` with `s` in 1..7 and `n < d <= 15`, so the protocol can name a few
/// hundred distinct factors per face -- far more than the 28 that whole-number scales alone would
/// suggest. The cap therefore sits well above what any document that is not deliberately cycling
/// through the fraction space will reach, so that ordinary content never degrades, while still
/// bounding what a hostile stream can pin: without it, faces accumulate for the life of the session
/// (only a font-description change clears them) and each one is an FT_Face plus an hb_font_t.
///
/// The trade this makes deliberately: past the cap, WHICH glyphs keep a re-hinted face depends on
/// the order they were asked for. That is the cost of having a hard bound at all -- eviction is not
/// available here, because font_keys are baked into texture-atlas keys -- and unbounded memory
/// growth driven by whatever is writing to the terminal is the worse of the two.
/// @see open_shaper::resize_font.
constexpr size_t MaxResizedFonts = 512;

struct HbFontInfo // NOLINT(readability-identifier-naming)
{
    font_source primary;
    font_source_list fallbacks;
    font_source_list allFallbacks; ///< Complete fallback list for on-demand extension.
    font_size size;
    ft_face_ptr ftFace;
    hb_font_ptr hbFont;
    std::optional<font_metrics> metrics {};

    /// The weight this face was actually loaded at, which together with @c size is its cache identity.
    ///
    /// Distinct from @c description.weight: a description is only attached to keys that came from
    /// load_font(), so every key minted for a fallback face or a resize carried a default-constructed
    /// description. Reading the weight from there filed those faces under a weight they were never
    /// loaded at.
    font_weight weight { font_weight::normal };

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

    /// Substitutes the replacement character for every glyph that no font in the fallback chain could
    /// render.
    ///
    /// @param ftFace The primary font's face, which supplies the replacement glyph.
    /// @param glyphs The glyphs of a single shaped run. Unresolved glyphs are expected to carry the
    ///               primary font's key, so that index and face agree.
    void replaceMissingGlyphs(FT_Face ftFace, std::span<glyph_position> glyphs)
    {
        auto const missingGlyph = FT_Get_Char_Index(ftFace, MissingGlyphId);

        if (!missingGlyph)
            return;

        for (auto& gpos: glyphs)
            if (glyphMissing(gpos))
                gpos.glyph.index = glyph_index { missingGlyph };
    }

    void prepareBuffer(hb_buffer_t* hbBuf,
                       u32string_view codepoints,
                       std::span<unsigned const> clusters,
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

    /// A run as shaped by exactly one font.
    struct shaped_run
    {
        shape_result glyphs;           ///< The shaped glyphs, in visual (left-to-right) order.
        vector<shaped_glyph_ref> refs; ///< Parallel to @c glyphs: cluster value and .notdef flag.
        bool anyMissing = false;       ///< Whether the font came up short on any glyph at all.
    };

    /// Which spacing class a fallback pass accepts.
    ///
    /// The fallback chain is walked twice, so that a font matching the primary's spacing is always tried
    /// before one that does not. A terminal wants a monospaced fallback where one exists, because a
    /// proportional face's advances do not line up with the cell grid -- but a proportional face still beats
    /// a replacement box, so it is accepted once nothing better has answered.
    enum class fallback_pass : uint8_t
    {
        Preferred, ///< Fonts whose spacing matches the primary font description's.
        Alternate  ///< The remainder. Skipped entirely under strict spacing.
    };

    /// A position in the two-pass walk over a font's fallback chain.
    struct fallback_cursor
    {
        fallback_pass pass = fallback_pass::Preferred;
        size_t index = 0;

        /// Whether the coverage-driven lookup has already been spent on this span.
        ///
        /// That lookup is the last resort, and it answers with a font chosen precisely because it
        /// covers the codepoints -- so if shaping against it STILL comes back .notdef, asking again
        /// would return the same font forever. One attempt per span, then the replacement glyph.
        bool coverageTried = false;
    };

    /// Shapes @p codepoints with @p hbFont alone, applying no fallback.
    ///
    /// Deliberately inspects nothing beyond its own output: glyphs an earlier call left unresolved must not
    /// make this one look like it failed.
    ///
    /// @param font        The key to stamp onto every glyph produced here.
    /// @param fontInfo    Font info of @p font, supplying size and font features.
    /// @param hbBuf       Scratch buffer. Its contents are copied out before returning, so the caller may
    ///                    reuse it immediately -- including for a nested fallback shaping.
    /// @param hbFont      The font to shape with.
    /// @param script      The run's script.
    /// @param presentation The run's presentation style.
    /// @param codepoints  The codepoints to shape.
    /// @param clusters    Per-codepoint cluster values.
    /// @return The shaped glyphs, each paired with its cluster and whether the font had a glyph for it.
    [[nodiscard]] shaped_run shapeWithFont(font_key font,
                                           HbFontInfo const& fontInfo,
                                           hb_buffer_t* hbBuf,
                                           hb_font_t* hbFont,
                                           unicode::Script script,
                                           unicode::PresentationStyle presentation,
                                           u32string_view codepoints,
                                           std::span<unsigned const> clusters)
    {
        assert(hbFont != nullptr);
        assert(hbBuf != nullptr);

        // hb_shape() returns on an empty buffer before it ever allocates positions, which leaves
        // hb_buffer_normalize_glyphs() below asserting on the buffer it is handed. An empty run shapes to
        // nothing anyway, so keep it away from HarfBuzz entirely.
        if (codepoints.empty())
            return {};

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

        auto output = shaped_run {};
        output.glyphs.reserve(glyphCount);
        output.refs.reserve(glyphCount);

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
            gpos.offset.x = static_cast<int>(static_cast<double>(pos[i].x_offset) / 64.0);
            gpos.offset.y = static_cast<int>(static_cast<double>(pos[i].y_offset) / 64.0);
            gpos.advance.x = static_cast<int>(static_cast<double>(pos[i].x_advance) / 64.0);
            gpos.advance.y = static_cast<int>(static_cast<double>(pos[i].y_advance) / 64.0);
            gpos.presentation = presentation;

            auto const missing = glyphMissing(gpos);
            output.anyMissing = output.anyMissing || missing;

            output.glyphs.emplace_back(gpos);
            output.refs.emplace_back(shaped_glyph_ref { .cluster = info[i].cluster, .missing = missing });
        }

        return output;
    }

    /// Appends the segment's glyphs unchanged.
    void appendGlyphs(shape_result& result, shaped_run const& shaped, cluster_group const& segment)
    {
        result.insert(result.end(),
                      shaped.glyphs.begin() + static_cast<ptrdiff_t>(segment.glyphBegin),
                      shaped.glyphs.begin() + static_cast<ptrdiff_t>(segment.glyphEnd));
    }

    /// Appends the segment's glyphs, re-homing the unresolved ones onto @p primaryFont.
    ///
    /// Without this, replaceMissingGlyphs() would stamp the primary face's replacement glyph index onto a key
    /// still naming whichever fallback font was tried last -- an index that means nothing in that font.
    void appendUnresolvedGlyphs(shape_result& result,
                                shaped_run const& shaped,
                                cluster_group const& segment,
                                font_key primaryFont)
    {
        for (auto const i: iota(segment.glyphBegin, segment.glyphEnd))
        {
            auto gpos = shaped.glyphs[i];
            if (glyphMissing(gpos))
                gpos.glyph.font = primaryFont;
            result.emplace_back(gpos);
        }
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

    /// One remembered answer from resolveByCoverage(), with the face parameters it was found for.
    ///
    /// The answer is a font_key, and a font_key encodes a point size and a weight as well as a file,
    /// so an answer found at one size is wrong at another. A font-size-only change reloads the font
    /// keys WITHOUT clearing this cache (@see Renderer::applyPendingReconfig), which is how a
    /// coverage-resolved CJK glyph kept rasterizing at the pre-zoom size while the text around it
    /// scaled, and how a bold run reused the regular-weight fallback face.
    ///
    /// Recorded beside the answer rather than folded into the map's KEY, because this cache is also
    /// the guard that stops a span being resolved over and over: there must be an entry for a
    /// codepoint once it has been tried, whatever the outcome. A key that varied with the face
    /// parameters could miss forever, and shapeRunWithFallback() does not terminate if it does.
    struct coverage_cache_entry
    {
        font_size size;
        font_weight weight;
        optional<font_key> resolved; ///< nullopt when no installed font covers the codepoint.
    };

    /// Cache for resolveByCoverage(), keyed by the first codepoint of an unresolvable span. Holds
    /// negative answers too, so a codepoint no installed font covers is queried only once.
    ///
    /// Unlike @c locateCache this does NOT survive clear_cache(): its values are font_keys owned by
    /// @c fontKeyToHbFontInfoMapping, not font files, and outliving that map would make them dangle.
    unordered_map<char32_t, coverage_cache_entry> coverageCache;

    /// How many faces resize_font() has minted, against @c resizedFontLimit.
    size_t resizedFontCount = 0;

    /// The ceiling on @c resizedFontCount. @see MaxResizedFonts, open_shaper::set_resized_font_limit.
    size_t resizedFontLimit = MaxResizedFonts;

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

    /// @return the key already held for this face, or nullopt when opening it would load a new one.
    ///
    /// Lets a caller find out whether a request is a cache HIT without paying for the miss, which is
    /// what resize_font() needs to keep a budget on faces it would otherwise mint without limit.
    [[nodiscard]] optional<font_key> findKeyForFont(font_source const& source,
                                                    font_size fontSize,
                                                    font_weight fontWeight) const
    {
        auto const i = fontPathAndSizeToKeyMapping.find(
            FontInfo { .path = identifierOf(source), .size = fontSize, .weight = fontWeight });
        return i != fontPathAndSizeToKeyMapping.end() ? optional { i->second } : nullopt;
    }

    optional<font_key> getOrCreateKeyForFont(font_source const& source,
                                             font_size fontSize,
                                             font_weight fontWeight)
    {
        if (auto const existing = findKeyForFont(source, fontSize, fontWeight))
            return existing;

        auto const sourceId = identifierOf(source);
        if (std::any_of(blacklistedSources.begin(), blacklistedSources.end(), [&](auto const& a) {
                return a == sourceId;
            }))
            return nullopt;

        auto ftFacePtrOpt = loadFace(source, fontSize, dpi, ft);
        if (!ftFacePtrOpt.has_value())
        {
            // Blacklisting says "this file is not a font I can use", so it must not be concluded from
            // a failure to open a file that has already opened at another size. A bitmap font has
            // only the strikes it ships, and `OSC 66` asks for arbitrary sizes -- so one scaled write
            // could otherwise retire the user's font for the rest of the session, including at the
            // size it was happily rendering at, since clear_cache() does not clear this list.
            auto const loadedAtAnotherSize = std::ranges::any_of(
                fontPathAndSizeToKeyMapping, [&](auto const& entry) { return entry.first.path == sourceId; });
            if (!loadedAtAnotherSize)
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
                                     .hbFont = std::move(hbFontPtr),
                                     .weight = fontWeight };

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
        {
            // FreeType built without FT_CONFIG_OPTION_SUBPIXEL_RENDERING (e.g. Homebrew's on macOS)
            // makes FT_Library_SetLcdFilter a no-op that returns FT_Err_Unimplemented_Feature. That is
            // expected — LCD/subpixel filtering is simply unavailable — so keep it at render-log
            // verbosity (off by default) instead of alarming the user on every startup. Any other,
            // genuinely unexpected error code still surfaces via errorLog().
            if (ec == FT_Err_Unimplemented_Feature)
                rasterizerLog()("freetype: LCD filter unavailable (subpixel rendering not built in).");
            else
                errorLog()("freetype: Failed to set LCD filter. {}", ftErrorStr(ec));
        }
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

    /// @return Whether @p fontInfo's face is fixed-width (monospaced).
    [[nodiscard]] static bool isMonospace(HbFontInfo const& fontInfo) noexcept
    {
        return (fontInfo.ftFace->face_flags & FT_FACE_FLAG_FIXED_WIDTH) != 0;
    }

    /// @return Whether @p fallbackFontInfo is a font that @p pass accepts.
    [[nodiscard]] static bool admissibleInPass(HbFontInfo const& primaryFontInfo,
                                               fallback_pass pass,
                                               HbFontInfo const& fallbackFontInfo) noexcept
    {
        auto const spacingMatchesPrimary =
            isMonospace(fallbackFontInfo) == (primaryFontInfo.description.spacing == font_spacing::mono);

        return spacingMatchesPrimary == (pass == fallback_pass::Preferred);
    }

    /// Resolves the next usable fallback font, advancing @p cursor past it.
    ///
    /// The chain is walked once per spacing class: fonts matching the primary's spacing first, then --
    /// unless the font description demands strict spacing -- the rest. Between them the two passes visit
    /// every font exactly once, so no font is ever shaped against twice. Fonts that fail to load are
    /// skipped, and the chain is extended on demand as the walk runs off its end.
    ///
    /// @param fontInfo The primary font, which owns the fallback chain.
    /// @param cursor   Where to resume the walk. Advanced past the returned font.
    /// @return The next font to try, or nullopt once both passes are exhausted.
    [[nodiscard]] optional<font_key> nextFallbackFont(HbFontInfo& fontInfo, fallback_cursor& cursor)
    {
        for (;;)
        {
            if (cursor.index >= fontInfo.fallbacks.size() && !extendFallbacks(fontInfo))
            {
                // This pass has run out of fonts. Strict spacing forbids the alternate pass outright.
                if (cursor.pass == fallback_pass::Alternate || fontInfo.description.strictSpacing)
                    return nullopt;

                cursor = fallback_cursor { .pass = fallback_pass::Alternate, .index = 0 };
                continue;
            }

            auto const pass = cursor.pass;
            auto const& fallbackSource = fontInfo.fallbacks[cursor.index];
            ++cursor.index;

            auto const fallbackKeyOpt = getOrCreateKeyForFont(fallbackSource, fontInfo.size, fontInfo.weight);
            if (!fallbackKeyOpt.has_value())
                continue;

            auto const fallbackIterator = fontKeyToHbFontInfoMapping.find(*fallbackKeyOpt);
            Require(fallbackIterator != fontKeyToHbFontInfoMapping.end());
            auto const& fallbackFontInfo = fallbackIterator->second;

            if (!admissibleInPass(fontInfo, pass, fallbackFontInfo))
                continue;

            textShapingLog()(
                "Trying fallback font key:{}, source: {}", *fallbackKeyOpt, fallbackFontInfo.primary);
            return fallbackKeyOpt;
        }
    }

    /// Asks the locator which font covers @p codepoints, once the fallback chain has been walked out.
    ///
    /// The chain is ordered by how well each font matches the font DESCRIPTION, which routinely buries
    /// the only face holding a script far past any length worth walking eagerly -- on a stock Fedora
    /// install the first CJK face sorts 83rd of 201 for a monospace description, while the chain limit
    /// defaults to 16. Asking about the codepoint instead finds it in one query, which keeps the limit
    /// a bound on how many fonts are tried BLINDLY rather than a bound on which scripts can render.
    ///
    /// @param fontInfo   The primary font, supplying the size and weight to load a candidate at.
    /// @param codepoints The span that no font in the chain could render.
    /// @return A font covering @p codepoints, or nullopt when the locator names none that loads.
    [[nodiscard]] optional<font_key> resolveByCoverage(HbFontInfo const& fontInfo, u32string_view codepoints)
    {
        if (codepoints.empty() || !locator)
            return nullopt;

        // Keyed on the first codepoint: a run that reaches this path is overwhelmingly one script, and
        // a fontconfig charset query is far too expensive to repeat per run. Negative answers are cached
        // too -- a codepoint no font on the system covers must not re-query on every frame.
        //
        // A remembered answer is only USED when it was found for the face parameters in hand: the key
        // it holds encodes a size and a weight, so one found at another size would rasterize the glyph
        // at that size while the text around it scaled. A stale entry is re-resolved and overwritten
        // rather than sitting alongside a second one, which is what keeps this a guard as well as a
        // cache -- @see coverage_cache_entry.
        auto const cacheKey = codepoints.front();
        if (auto const i = coverageCache.find(cacheKey); i != coverageCache.end()
                                                         && i->second.size.pt == fontInfo.size.pt
                                                         && i->second.weight == fontInfo.weight)
            return i->second.resolved;

        auto resolved = optional<font_key> { nullopt };
        for (auto const& source: locator->resolve(gsl::span(codepoints.data(), codepoints.size())))
        {
            resolved = getOrCreateKeyForFont(source, fontInfo.size, fontInfo.weight);
            if (resolved.has_value())
            {
                textShapingLog()("Resolved U+{:04X} by coverage to font key:{}.",
                                 static_cast<uint32_t>(cacheKey),
                                 *resolved);
                break;
            }
        }

        coverageCache[cacheKey] =
            coverage_cache_entry { .size = fontInfo.size, .weight = fontInfo.weight, .resolved = resolved };
        return resolved;
    }

    /// Shapes @p codepoints with @p shapingFont, then resolves each span that font cannot render against
    /// the rest of @p primaryFontInfo's fallback chain, appending the spliced glyphs to @p result.
    ///
    /// Glyphs the shaping font covers keep its key; only the maximal cluster-aligned spans that came out
    /// .notdef are re-shaped, and their glyphs are spliced back where they belong, so left-to-right order
    /// survives. The run may therefore end up spanning several fonts, which is fine: every glyph_key
    /// carries its own font key and the rasterizer honours it.
    ///
    /// @param primaryFont     The font the run was requested with. Owns the fallback chain, and takes
    ///                        ownership of any glyph no font in that chain could render.
    /// @param primaryFontInfo Font info of @p primaryFont.
    /// @param shapingFont     The font to shape with here; equal to @p primaryFont at the top level.
    /// @param cursor          Where to resume the fallback walk for spans this font cannot render.
    /// @param script          The run's script.
    /// @param presentation    The run's presentation style.
    /// @param codepoints      The codepoints to shape.
    /// @param clusters        Per-codepoint cluster values.
    /// @param result          Receives the shaped glyphs, appended.
    /// @return Whether every glyph appended was resolved by some font.
    bool shapeRunWithFallback(font_key primaryFont,
                              HbFontInfo& primaryFontInfo,
                              font_key shapingFont,
                              fallback_cursor cursor,
                              unicode::Script script,
                              unicode::PresentationStyle presentation,
                              u32string_view codepoints,
                              std::span<unsigned const> clusters,
                              shape_result& result)
    {
        auto const fontInfoIterator = fontKeyToHbFontInfoMapping.find(shapingFont);
        Require(fontInfoIterator != fontKeyToHbFontInfoMapping.end());
        auto const& shapingFontInfo = fontInfoIterator->second;

        auto const shaped = shapeWithFont(shapingFont,
                                          shapingFontInfo,
                                          hbBuf.get(),
                                          shapingFontInfo.hbFont.get(),
                                          script,
                                          presentation,
                                          codepoints,
                                          clusters);
        if (shaped.glyphs.empty())
            return true;

        // The overwhelmingly common case: this font renders the whole run. Take the glyphs as they are and
        // do not pay for the segmentation at all.
        if (!shaped.anyMissing)
        {
            result.insert(result.end(), shaped.glyphs.begin(), shaped.glyphs.end());
            return true;
        }

        auto const segments = fallbackSegments(shaped.refs, clusters);
        auto allResolved = true;

        for (cluster_group const& segment: segments)
        {
            if (!segment.missing)
            {
                appendGlyphs(result, shaped, segment);
                continue;
            }

            // A span mapping to no codepoints has nothing to re-shape; recursing on it would not
            // terminate.
            auto spanCursor = cursor;
            auto fallbackOpt = segment.empty() ? nullopt : nextFallbackFont(primaryFontInfo, spanCursor);

            // The chain is out of fonts, but "no font in the first N of a description-ordered list"
            // is not the same as "no font on this system". Ask which one actually covers these
            // codepoints before settling for the replacement glyph.
            if (!fallbackOpt.has_value() && !segment.empty() && !spanCursor.coverageTried)
            {
                auto const missing = segment.codepointEnd - segment.codepointBegin;
                fallbackOpt =
                    resolveByCoverage(primaryFontInfo, codepoints.substr(segment.codepointBegin, missing));
                spanCursor.coverageTried = true;
            }

            if (!fallbackOpt.has_value())
            {
                appendUnresolvedGlyphs(result, shaped, segment, primaryFont);
                allResolved = false;
                continue;
            }

            auto const count = segment.codepointEnd - segment.codepointBegin;
            if (!shapeRunWithFallback(primaryFont,
                                      primaryFontInfo,
                                      *fallbackOpt,
                                      spanCursor,
                                      script,
                                      presentation,
                                      codepoints.substr(segment.codepointBegin, count),
                                      clusters.subspan(segment.codepointBegin, count),
                                      result))
                allResolved = false;
        }

        return allResolved;
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

void open_shaper::set_resized_font_limit(size_t limit)
{
    _d->resizedFontLimit = limit;
}

void open_shaper::clear_cache()
{
    locatorLog()("Clearing cache ({} keys, {} font infos).",
                 _d->fontPathAndSizeToKeyMapping.size(),
                 _d->fontKeyToHbFontInfoMapping.size());
    _d->fontPathAndSizeToKeyMapping.clear();
    _d->fontKeyToHbFontInfoMapping.clear();
    // coverageCache stores font_keys owned by the two maps just cleared. Keeping it would hand back
    // keys that no longer resolve, and shapeRunWithFallback requires the lookup to succeed -- so the
    // next frame drawing a coverage-resolved codepoint would abort the render thread. Font keys are
    // never reissued (nextFontKey only ever counts up), so a stale key cannot even be mistaken for a
    // live one. Unlike locateCache, this cache is NOT description-independent: it answers with a key,
    // not a font file.
    _d->coverageCache.clear();

    // The faces the budget was counting are gone with the maps, so the allowance starts over.
    _d->resizedFontCount = 0;
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

font_key open_shaper::resize_font(font_key key, font_size size)
{
    auto const i = _d->fontKeyToHbFontInfoMapping.find(key);
    if (i == _d->fontKeyToHbFontInfoMapping.end())
        return key;

    auto const& fontInfo = i->second;
    if (fontInfo.size.pt == size.pt)
        return key;

    // The SAME source, re-opened at the new size. Going through getOrCreateKeyForFont keeps the
    // one-key-per-(source, size, weight) invariant, so asking twice costs one FreeType face, and the
    // fallback chain the primary owns is left alone -- only the face this glyph came from is resized.
    //
    // The size asked for here is WIRE-DRIVEN: it is the cell size times an `OSC 66` draw factor, and
    // that factor is `s * n/d` with all three chosen by whatever is writing to the terminal. Every
    // distinct value mints an FT_Face and a hb_font_t that nothing ever releases -- only a font
    // description change clears these maps -- so a remote host cycling through the factors the
    // protocol allows can pin hundreds of megabytes of font faces. Already-loaded sizes stay free;
    // once the budget for NEW ones is spent, this reports "not resized" by handing back the key it
    // was given, and the caller falls back to magnifying the raster it already has.
    auto const resized = _d->findKeyForFont(fontInfo.primary, size, fontInfo.weight);
    if (resized.has_value())
        return *resized;

    if (_d->resizedFontCount >= _d->resizedFontLimit)
    {
        locatorLog()("Resized-font budget of {} exhausted; drawing at {} by magnification instead.",
                     _d->resizedFontLimit,
                     size);
        return key;
    }

    auto const minted = _d->getOrCreateKeyForFont(fontInfo.primary, size, fontInfo.weight);
    if (!minted.has_value())
        return key;

    ++_d->resizedFontCount;
    return *minted;
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
    auto const fontInfoIterator = _d->fontKeyToHbFontInfoMapping.find(font);
    Require(fontInfoIterator != _d->fontKeyToHbFontInfoMapping.end());
    auto& fontInfo = fontInfoIterator->second;

    // The font the glyph is finally found in, which need not be the one that was asked for.
    auto resolvedFont = font;
    auto glyphIndex = glyph_index { FT_Get_Char_Index(fontInfo.ftFace.get(), codepoint) };

    auto cursor = fallback_cursor {};
    while (!glyphIndex.value)
    {
        auto const fallbackKeyOpt = _d->nextFallbackFont(fontInfo, cursor);
        if (!fallbackKeyOpt.has_value())
            return nullopt;

        auto const& fallbackFontInfo = _d->fontKeyToHbFontInfoMapping.at(*fallbackKeyOpt);
        glyphIndex = glyph_index { FT_Get_Char_Index(fallbackFontInfo.ftFace.get(), codepoint) };
        resolvedFont = *fallbackKeyOpt;
    }

    glyph_position gpos {};

    // A glyph index means nothing outside the face it came from, so the key has to name that face rather
    // than the font that was asked for.
    gpos.glyph = glyph_key { .size = fontInfo.size, .font = resolvedFont, .index = glyphIndex };
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

    auto const resultOffset = result.size();
    auto const inputClusters = std::span<unsigned const> { clusters.data(), clusters.size() };

    if (_d->shapeRunWithFallback(font,
                                 fontInfo,
                                 font,
                                 fallback_cursor {},
                                 script,
                                 presentation,
                                 codepoints,
                                 inputClusters,
                                 result))
        return;

    textShapingLog()("Shaping incomplete; substituting the replacement character.");

    replaceMissingGlyphs(fontInfo.ftFace.get(), std::span<glyph_position> { result }.subspan(resultOffset));
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
