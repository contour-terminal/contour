#include <text_shaper/DirectWriteShaper.hpp>

#include <text_shaper/DirectWriteAnalysisWrapper.hpp>
#include <text_shaper/FontLocator.hpp>

#include <core/WideString.hpp>

#include <libunicode/convert.h>

#include <algorithm>
#include <iostream> // DEBUGGING ONLY
#include <limits>
#include <ranges>
#include <string>
#include <string_view>

#include <dwrite.h>
#include <dwrite_3.h>

#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

using std::make_unique;
using std::max;
using std::move;
using std::nullopt;
using std::optional;
using std::pair;
using std::vector;
using std::wstring;

namespace text
{
namespace
{
    /// Rasterizes one glyph run into @p _it, returning false when DirectWrite could not produce the
    /// alpha texture.
    ///
    /// The HRESULT is load-bearing and used to be discarded. CreateAlphaTexture() leaves the buffer
    /// untouched on failure, and `tmp` is zero-initialized, so a swallowed failure did not skip the glyph
    /// -- it copied ZEROS out as if they were coverage. In an RGB subpixel bitmap a zero run is not a
    /// hole, it is *attenuated ink*: the glyph keeps its shape but loses weight over the affected span,
    /// which is exactly the "stem dims for two scanlines then recovers" artifact reported in #2040 and
    /// impossible to see on the FreeType path our CI actually builds.
    [[nodiscard]] bool renderGlyphRunToBitmap(IDWriteGlyphRunAnalysis* glyphAnalysis,
                                              const RECT& textureBounds,
                                              const DWRITE_COLOR_F& runColor,
                                              BitmapFormat targetFormat,
                                              std::vector<uint8_t>::iterator& _it)
    {
        auto const width = textureBounds.right - textureBounds.left;
        auto const height = textureBounds.bottom - textureBounds.top;

        // Spaces and other glyphs without ink have valid empty bounds and need no alpha texture.
        if (width == 0 || height == 0)
            return true;

        std::vector<uint8_t> tmp;
        tmp.resize(height * width * 3);

        auto const hr = glyphAnalysis->CreateAlphaTexture(
            DWRITE_TEXTURE_CLEARTYPE_3x1, &textureBounds, tmp.data(), tmp.size());
        if (FAILED(hr))
        {
            errorLog()("directwrite: CreateAlphaTexture failed for a {}x{} glyph run.", width, height);
            return false;
        }

        // TODO: #ifdef (__SSE2__) SIMD me :-)
        for (auto i = 0; i < height; i++)
            for (auto j = 0; j < width; j++)
            {
                auto const base = (i * width + j) * 3;
                auto const srcR = tmp[base];
                auto const srcG = tmp[base + 1];
                auto const srcB = tmp[base + 2];

                if (targetFormat == BitmapFormat::RGB)
                {
                    *_it++ = srcR;
                    *_it++ = srcG;
                    *_it++ = srcB;
                }

                if (targetFormat == BitmapFormat::RGBA)
                {
                    auto const r = runColor.r * 255;
                    auto const g = runColor.g * 255;
                    auto const b = runColor.b * 255;
                    auto const a = runColor.a;

                    float const redAlpha = a * srcR / 255.0;
                    float const greenAlpha = a * srcG / 255.0;
                    float const blueAlpha = a * srcB / 255.0;
                    float const averageAlpha = (redAlpha + greenAlpha + blueAlpha) / 3.0f;

                    auto const currentR = *_it;
                    auto const currentG = *(_it + 1);
                    auto const currentB = *(_it + 2);
                    auto const currentA = *(_it + 3);

                    // Bitmap composition
                    *_it++ = currentR * (1.0 - averageAlpha) + averageAlpha * r;
                    *_it++ = currentG * (1.0 - averageAlpha) + averageAlpha * g;
                    *_it++ = currentB * (1.0 - averageAlpha) + averageAlpha * b;
                    *_it++ = currentA * (1.0 - averageAlpha) + averageAlpha * 255;
                }
            }

        return true;
    }

    constexpr double ptToEm(double pt)
    {
        return pt * (96.0 / 72.0);
    }
} // namespace

struct DxFontInfo
{
    FontDescription description;
    FontSize size;
    FontMetrics metrics;
    int fontUnitsPerEm;

    ComPtr<IDWriteFontFace5> fontFace;
    FontPath source;
    FontSourceList fallbacks;
};

struct DirectWriteShaper::Private
{
    ComPtr<IDWriteFactory7> factory;
    ComPtr<IDWriteTextAnalyzer1> textAnalyzer;
    FontLocator* locator;

    DPI dpi;
    std::wstring userLocale;
    std::unordered_map<FontKey, DxFontInfo> fonts;
    std::unordered_map<FontKey, bool> fontsHasColor;

    FontKey nextFontKey;
    int fallbackLimit = 16;

    Private(DPI initialDPI, FontLocator& fontLocator): locator { &fontLocator }, dpi { initialDPI }
    {
        auto hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                      __uuidof(IDWriteFactory7),
                                      reinterpret_cast<IUnknown**>(factory.GetAddressOf()));
        ComPtr<IDWriteTextAnalyzer> analyzer;
        hr = factory->CreateTextAnalyzer(&analyzer);
        analyzer.As(&textAnalyzer);

        wchar_t locale[LOCALE_NAME_MAX_LENGTH];
        GetUserDefaultLocaleName(locale, sizeof(locale));
        userLocale = locale;
    }

    FontKey createFontKey()
    {
        auto result = nextFontKey;
        nextFontKey.value++;
        return result;
    }

    optional<text::FontKey> addFont(FontSource const& source,
                                    FontDescription const& description,
                                    FontSize size)
    {
        if (!std::holds_alternative<FontPath>(source))
        {
            return nullopt;
        }

        auto const& sourcePath = std::get<FontPath>(source);

        for (auto const& [fontKey, fontInfo]: fonts)
            if (description == fontInfo.description && size.pt == fontInfo.size.pt
                && sourcePath.value == fontInfo.source.value
                && sourcePath.collectionIndex == fontInfo.source.collectionIndex)
                return fontKey;

        auto const wSourcePath = core::toUtf16(sourcePath.value);
        if (!wSourcePath)
            return nullopt; // names no file

        ComPtr<IDWriteFontFile> fontFile;
        auto hr = factory->CreateFontFileReference(wSourcePath->c_str(), nullptr, &fontFile);
        if (FAILED(hr))
        {
            return nullopt;
        }

        BOOL isSupported {};
        DWRITE_FONT_FILE_TYPE fileType {};
        DWRITE_FONT_FACE_TYPE fontFaceType {};
        UINT32 numFaces {};
        ComPtr<IDWriteFontFace> fontFace;
        hr = fontFile->Analyze(&isSupported, &fileType, &fontFaceType, &numFaces);
        if (FAILED(hr) || !isSupported || sourcePath.collectionIndex < 0
            || static_cast<UINT32>(sourcePath.collectionIndex) >= numFaces)
            return nullopt;
        hr = factory->CreateFontFace(fontFaceType,
                                     1,
                                     fontFile.GetAddressOf(),
                                     static_cast<UINT32>(sourcePath.collectionIndex),
                                     DWRITE_FONT_SIMULATIONS_NONE,
                                     &fontFace);
        if (FAILED(hr))
        {
            return nullopt;
        }

        ComPtr<IDWriteFontFace3> fontFace3;
        if (FAILED(fontFace.As(&fontFace3)))
            return nullopt;

        auto dwMetrics = DWRITE_FONT_METRICS {};
        fontFace3->GetMetrics(&dwMetrics);

        auto const dipScalar = ptToEm(size.pt) / dwMetrics.designUnitsPerEm * pixelPerDip();
        auto const lineHeight = dwMetrics.ascent + dwMetrics.descent + dwMetrics.lineGap;

        DxFontInfo fontInfo {};
        fontInfo.description = description;
        fontInfo.source = sourcePath;
        fontInfo.size = size;
        fontInfo.metrics.lineHeight = int(ceil(lineHeight * dipScalar));
        fontInfo.metrics.ascender = int(ceil(dwMetrics.ascent * dipScalar));
        fontInfo.metrics.descender = int(ceil(dwMetrics.descent * dipScalar));
        fontInfo.metrics.underlinePosition = int(ceil(dwMetrics.underlinePosition * dipScalar));
        fontInfo.metrics.underlineThickness = int(ceil(dwMetrics.underlineThickness * dipScalar));
        fontInfo.metrics.advance = int(ceil(computeAverageAdvance(fontFace.Get()) * dipScalar));

        if (FAILED(fontFace.As(&fontInfo.fontFace)))
            return nullopt;

        auto key = createFontKey();
        fonts.emplace(pair { key, std::move(fontInfo) });
        fontsHasColor.emplace(pair { key, false });
        return key;
    }

    int computeAverageAdvance(IDWriteFontFace* fontFace)
    {
        auto constexpr firstCharIndex = UINT16 { 32 };
        auto constexpr lastCharIndex = UINT16 { 127 };
        auto constexpr charCount = lastCharIndex - firstCharIndex + 1;

        UINT32 codePoints[charCount] {};
        for (UINT16 i = 0; i < charCount; i++)
            codePoints[i] = firstCharIndex + i;
        UINT16 glyphIndices[charCount] {};
        fontFace->GetGlyphIndicesA(codePoints, charCount, glyphIndices);

        DWRITE_GLYPH_METRICS dwGlyphMetrics[charCount] {};
        fontFace->GetDesignGlyphMetrics(glyphIndices, charCount, dwGlyphMetrics);

        UINT32 maxAdvance = 0;
        for (int i = 0; i < charCount; i++)
            maxAdvance = max(maxAdvance, dwGlyphMetrics[i].advanceWidth);

        return int(maxAdvance);
    }

    float pixelPerDip() { return static_cast<float>(dpi.x) / 96.0f; }
};

DirectWriteShaper::DirectWriteShaper(DPI dpi, FontLocator& locator):
    _d(new Private(dpi, locator), [](Private* p) { delete p; })
{
}

optional<FontKey> DirectWriteShaper::loadFont(FontDescription const& description, FontSize size)
{
    locatorLog().operator()("Loading font chain for: {}", description);
    FontSourceList sources = _d->locator->locate(description);
    if (sources.empty())
        return nullopt;

    optional<FontKey> fontKeyOpt = _d->addFont(sources[0], description, size);
    if (!fontKeyOpt.has_value())
        return nullopt;

    _d->fonts.at(*fontKeyOpt).fallbacks = FontSourceList(sources.begin() + 1, sources.end());
    return fontKeyOpt;
}

FontMetrics DirectWriteShaper::metrics(FontKey key) const
{
    DxFontInfo const& fontInfo = _d->fonts.at(key);
    return fontInfo.metrics;
}

void DirectWriteShaper::shape(FontKey font,
                              std::u32string_view text,
                              gsl::span<unsigned> clusters,
                              unicode::Script /*script*/,
                              unicode::PresentationStyle presentation,
                              ShapeResult& result)
{
    if (text.empty() || text.size() != clusters.size())
        return;
    auto const& primary = _d->fonts.at(font);
    auto const limit = _d->fallbackLimit < 0
                           ? primary.fallbacks.size()
                           : std::min(primary.fallbacks.size(), static_cast<size_t>(_d->fallbackLimit));
    auto const canFallback =
        _d->fallbackLimit != 0 && !std::holds_alternative<FontFallbackNone>(primary.description.fontFallback);
    enum class Pass : uint8_t
    {
        Preferred,
        Alternate
    };
    struct Cursor
    {
        size_t index = 0;
        Pass pass = Pass::Preferred;
        bool hasTriedCoverage = false;
    };
    auto const nextFont = [&](Cursor& cursor) -> std::optional<FontKey> {
        if (!canFallback)
            return std::nullopt;
        while (true)
        {
            if (cursor.index >= limit)
            {
                if (cursor.pass == Pass::Alternate || primary.description.strictSpacing)
                    return std::nullopt;
                cursor = Cursor { .pass = Pass::Alternate };
                continue;
            }
            auto const candidate =
                _d->addFont(primary.fallbacks[cursor.index++], primary.description, primary.size);
            if (!candidate || *candidate == font)
                continue;
            auto const spacingMatches = (_d->fonts.at(*candidate).fontFace->IsMonospacedFont() != FALSE)
                                        == (primary.description.spacing == FontSpacing::Mono);
            if (spacingMatches == (cursor.pass == Pass::Preferred))
                return candidate;
        }
    };
    auto const shapeSpan = [&](auto const& self,
                               FontKey current,
                               std::u32string_view input,
                               std::span<unsigned const> cells,
                               Cursor cursor) -> void {
        auto glyphs = ShapeResult {};
        auto refs = std::vector<ShapedGlyphRef> {};
        shapeWithFont(current, input, cells, presentation, glyphs, refs);
        if (std::ranges::none_of(refs, [](auto const& ref) { return ref.missing; }))
        {
            result.insert(result.end(), glyphs.begin(), glyphs.end());
            return;
        }
        for (auto const& segment: fallbackSegments(refs, cells))
        {
            auto position = cursor;
            auto fallback = std::optional<FontKey> {};
            auto const count = segment.codepointEnd - segment.codepointBegin;
            auto const missing = input.substr(segment.codepointBegin, count);
            if (segment.missing && !segment.empty())
            {
                fallback = nextFont(position);
                if (!fallback && canFallback && !position.hasTriedCoverage)
                {
                    position.hasTriedCoverage = true;
                    for (auto const& source: _d->locator->resolve(gsl::span(missing.data(), missing.size())))
                    {
                        auto const candidate = _d->addFont(source, primary.description, primary.size);
                        if (candidate && *candidate != current && *candidate != font)
                        {
                            fallback = candidate;
                            break;
                        }
                    }
                }
            }
            if (fallback)
                self(self, *fallback, missing, cells.subspan(segment.codepointBegin, count), position);
            else
                for (auto const i: std::views::iota(segment.glyphBegin, segment.glyphEnd))
                {
                    auto glyph = glyphs[i];
                    if (glyph.glyph.index.value == 0)
                        glyph.glyph.font = font;
                    result.emplace_back(std::move(glyph));
                }
        }
    };
    shapeSpan(shapeSpan, font, text, std::span<unsigned const>(clusters.data(), clusters.size()), Cursor {});
}
void DirectWriteShaper::shapeWithFont(FontKey font,
                                      std::u32string_view text,
                                      std::span<unsigned const> clusters,
                                      unicode::PresentationStyle presentation,
                                      ShapeResult& result,
                                      std::vector<ShapedGlyphRef>& refs)
{
    if (text.empty() || text.size() != clusters.size())
        return;
    auto const wText = unicode::convert_to<wchar_t>(text);
    auto utf16Clusters = std::vector<unsigned> {};
    utf16Clusters.reserve(wText.size());
    for (auto const i: std::views::iota(size_t { 0 }, text.size()))
        utf16Clusters.insert(utf16Clusters.end(), text[i] > 0xFFFF ? 2 : 1, clusters[i]);
    if (wText.empty() || wText.size() != utf16Clusters.size()
        || wText.size() > std::numeric_limits<UINT16>::max())
        return;
    WCHAR const* textString = wText.c_str();
    UINT32 textLength = wText.size();
    auto const& fontInfo = _d->fonts.at(font);
    IDWriteFontFace5* fontFace = fontInfo.fontFace.Get();

    vector<UINT16> glyphIndices;
    vector<INT32> glyphDesignUnitAdvances;
    vector<UINT16> glyphClusters;

    BOOL isTextSimple = FALSE;
    UINT32 uiLengthRead = 0;
    const UINT32 glyphStart = 0;

    glyphIndices.resize(textLength);
    glyphClusters.resize(textLength);

    _d->textAnalyzer->GetTextComplexity(
        textString, textLength, fontFace, &isTextSimple, &uiLengthRead, &glyphIndices.at(glyphStart));

    BOOL isEntireTextSimple = isTextSimple && uiLengthRead == textLength
                              && std::ranges::none_of(glyphIndices, [](auto glyph) { return glyph == 0; });
    // Note that some fonts won't report isTextSimple even for ASCII only strings, due to the existence of
    // "locl" table.
    if (isEntireTextSimple)
    {
        // "Simple" shaping assumes every character has the exact same width which can be calculate from the
        // metrics.
        //  This saves us from the need of expensive shaping operation.
        DWRITE_FONT_METRICS1 metrics {};
        fontFace->GetMetrics(&metrics);

        glyphDesignUnitAdvances.resize(textLength);
        USHORT designUnitsPerEm = metrics.designUnitsPerEm;
        fontFace->GetDesignGlyphAdvances(
            textLength, &glyphIndices.at(glyphStart), &glyphDesignUnitAdvances.at(glyphStart), false);

        for (auto const i: std::views::iota(size_t { 0 }, size_t { textLength }))
        {
            glyphClusters[i] = static_cast<UINT16>(i);
            auto const cellWidth = static_cast<double>((float) glyphDesignUnitAdvances.at(i))
                                   / designUnitsPerEm * ptToEm(fontInfo.size.pt) * _d->pixelPerDip();
            GlyphPosition gpos {};
            gpos.presentation = presentation;
            gpos.glyph = GlyphKey { fontInfo.size, font, GlyphIndex { glyphIndices.at(i) } };
            gpos.advance.x = static_cast<int>(cellWidth);
            result.emplace_back(gpos);
        }
    }
    else
    {
        // Complex shaping
        UINT32 const textStart = 0;
        DWriteAnalysisWrapper analysisWrapper(wText, _d->userLocale);

        // Script analysis
        _d->textAnalyzer->AnalyzeScript(&analysisWrapper, 0, wText.size(), &analysisWrapper);

        UINT32 actualGlyphCount = 0;
        UINT32 maxGlyphCount = std::min<UINT32>(textLength * 3 / 2 + 16, std::numeric_limits<UINT16>::max());
        vector<DWRITE_SHAPING_TEXT_PROPERTIES> textProps(textLength);
        vector<DWRITE_SHAPING_GLYPH_PROPERTIES> glyphProps(maxGlyphCount);

        glyphIndices.resize(maxGlyphCount);
        while (true)
        {
            auto hr = _d->textAnalyzer->GetGlyphs(&wText.at(textStart),
                                                  textLength,
                                                  fontFace,
                                                  0, // isSideways,
                                                  0, // isRightToLeft
                                                  &analysisWrapper.script,
                                                  _d->userLocale.data(),
                                                  nullptr,       // _numberSubstitution
                                                  nullptr,       // features
                                                  nullptr,       // featureLengths
                                                  0,             // featureCount
                                                  maxGlyphCount, // maxGlyphCount
                                                  &glyphClusters.at(textStart),
                                                  &textProps.at(0),
                                                  &glyphIndices.at(glyphStart),
                                                  &glyphProps.at(0),
                                                  &actualGlyphCount);
            if (hr != HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) && hr != E_NOT_SUFFICIENT_BUFFER)
            {
                if (FAILED(hr))
                {
                    errorLog()("directwrite: GetGlyphs failed.");
                    return;
                }
                break;
            }
            if (maxGlyphCount == std::numeric_limits<UINT16>::max())
                return;
            maxGlyphCount = std::min<UINT32>(maxGlyphCount * 2, std::numeric_limits<UINT16>::max());
            glyphProps.resize(maxGlyphCount);
            glyphIndices.resize(maxGlyphCount);
        }
        glyphIndices.resize(actualGlyphCount);
        if (actualGlyphCount == 0)
            return;
        vector<float> glyphAdvances(actualGlyphCount);
        vector<DWRITE_GLYPH_OFFSET> glyphOffsets(actualGlyphCount);

        auto hr = _d->textAnalyzer->GetGlyphPlacements(&wText.at(textStart),
                                                       &glyphClusters.at(textStart),
                                                       &textProps.at(0),
                                                       textLength,
                                                       &glyphIndices.at(glyphStart),
                                                       &glyphProps.at(0),
                                                       actualGlyphCount,
                                                       fontFace,
                                                       fontInfo.size.pt,
                                                       0, // isSideways,
                                                       0, // isRightToLeft
                                                       &analysisWrapper.script,
                                                       _d->userLocale.data(),
                                                       nullptr, // features
                                                       nullptr, // featureLengths
                                                       0,       // featureCount
                                                       &glyphAdvances.at(glyphStart),
                                                       &glyphOffsets.at(glyphStart));

        // Checked for the same reason CreateAlphaTexture() is: the out-parameters are zero-initialized
        // vectors DirectWrite leaves untouched on failure, so a discarded HRESULT here does not lose the
        // placements loudly -- it gives every glyph in the run an advance of 0, stacking the whole run on
        // one pen position. Emitting no positions lets the caller fall back instead.
        if (FAILED(hr))
        {
            errorLog()("directwrite: GetGlyphPlacements failed for a {}-glyph run.", actualGlyphCount);
            return;
        }

        for (auto const i: std::views::iota(size_t { 0 }, size_t { actualGlyphCount }))
        {
            GlyphPosition gpos {};
            gpos.glyph = GlyphKey { fontInfo.size, font, GlyphIndex { glyphIndices.at(i) } };
            gpos.presentation = presentation;
            gpos.offset.x = static_cast<int>(glyphOffsets.at(i).advanceOffset);
            // gpos.offset.y = static_cast<int>(static_cast<double>(pos[i].y_offset) / 64.0f);

            gpos.advance.x = static_cast<int>(glyphAdvances.at(i));
            // gpos.advance.y = static_cast<int>(static_cast<double>(pos[i].y_advance) / 64.0f);
            result.emplace_back(gpos);
        }
    }
    // DirectWrite maps UTF-16 units to the first glyph of a shaping cluster. Translate that map
    // into the cell clusters consumed by the shared fallbackSegments() helper. Surrogate halves
    // share one cell; expanded glyphs inherit their cluster's first glyph.
    refs.resize(result.size(), { std::numeric_limits<unsigned>::max(), false });
    for (auto const i: std::views::iota(size_t { 0 }, glyphClusters.size()))
    {
        if (glyphClusters[i] >= refs.size())
        {
            result.clear();
            refs.clear();
            return;
        }
        auto& cluster = refs[glyphClusters[i]].cluster;
        cluster = std::min(cluster, utf16Clusters[i]);
    }
    auto cluster = clusters.front();
    for (auto const i: std::views::iota(size_t { 0 }, refs.size()))
    {
        if (refs[i].cluster != std::numeric_limits<unsigned>::max())
            cluster = refs[i].cluster;
        refs[i] = { .cluster = cluster, .missing = result[i].glyph.index.value == 0 };
    }
}

std::optional<RasterizedGlyph> DirectWriteShaper::rasterize(GlyphKey glyph,
                                                            RenderMode mode,
                                                            float /*outlineThickness*/)
{
    DxFontInfo const& fontInfo = _d->fonts.at(glyph.font);
    IDWriteFontFace5* fontFace = fontInfo.fontFace.Get();
    float const fontEmSize = static_cast<float>(ptToEm(glyph.size.pt));

    UINT16 const glyphIndex = static_cast<UINT16>(glyph.index.value);
    DWRITE_GLYPH_OFFSET const glyphOffset {};
    float const glyphAdvances = 0;

    DWRITE_GLYPH_RUN glyphRun {};
    glyphRun.fontEmSize = fontEmSize;
    glyphRun.fontFace = fontFace;
    glyphRun.glyphAdvances = &(glyphAdvances);
    glyphRun.glyphCount = 1;
    glyphRun.glyphIndices = &(glyphIndex);
    glyphRun.glyphOffsets = &(glyphOffset);
    glyphRun.isSideways = false;
    glyphRun.bidiLevel = 0;

    ComPtr<IDWriteRenderingParams> renderingParams;
    _d->factory->CreateRenderingParams(&renderingParams);

    DWRITE_RENDERING_MODE renderingMode;
    auto hr = fontFace->GetRecommendedRenderingMode(fontEmSize,
                                                    _d->pixelPerDip(),
                                                    DWRITE_MEASURING_MODE::DWRITE_MEASURING_MODE_NATURAL,
                                                    renderingParams.Get(),
                                                    &renderingMode);
    if (FAILED(hr))
    {
        renderingMode = DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC;
    }

    ComPtr<IDWriteGlyphRunAnalysis> glyphAnalysis;
    RasterizedGlyph output {};

    // Checked for the same reason the CreateAlphaTexture()/GetGlyphPlacements() results are, but this
    // one is worse than a bad bitmap: on failure glyphAnalysis stays NULL and the
    // GetAlphaTextureBounds() call immediately below dereferences it.
    hr = _d->factory->CreateGlyphRunAnalysis(&glyphRun,
                                             _d->pixelPerDip(),
                                             nullptr,
                                             renderingMode,
                                             DWRITE_MEASURING_MODE::DWRITE_MEASURING_MODE_NATURAL,
                                             0.0f,
                                             0.0f,
                                             &glyphAnalysis);
    if (FAILED(hr) || !glyphAnalysis)
    {
        errorLog()("directwrite: CreateGlyphRunAnalysis failed for glyph {}.", glyph.index.value);
        return nullopt;
    }

    // A zero-initialized RECT that DirectWrite leaves untouched on failure is not distinguishable
    // from a legitimately empty glyph, and every size below is derived from it -- a failure here
    // yields a 0x0 bitmap whose emptiness looks deliberate.
    RECT textureBounds {};
    hr = glyphAnalysis->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &textureBounds);
    if (FAILED(hr))
    {
        errorLog()("directwrite: GetAlphaTextureBounds failed for glyph {}.", glyph.index.value);
        return nullopt;
    }

    output.bitmapSize.width = vtbackend::Width(textureBounds.right - textureBounds.left);
    output.bitmapSize.height = vtbackend::Height(textureBounds.bottom - textureBounds.top);
    output.position.x = textureBounds.left;
    output.position.y = -textureBounds.top;

    auto const [width, height] = output.bitmapSize;

    // ComPtr, not raw: both of these used to be leaked outright -- QueryInterface() and
    // TranslateColorGlyphRun() each return a reference this function never released, once per glyph
    // rasterization. Every other COM pointer here is already a ComPtr.
    ComPtr<IDWriteFactory2> factory2;
    ComPtr<IDWriteColorGlyphRunEnumerator> glyphRunEnumerator;
    if (SUCCEEDED(_d->factory->QueryInterface(IID_PPV_ARGS(&factory2))))
    {
        hr = factory2->TranslateColorGlyphRun(0.0f,
                                              0.0f,
                                              &glyphRun,
                                              nullptr,
                                              DWRITE_MEASURING_MODE::DWRITE_MEASURING_MODE_NATURAL,
                                              nullptr,
                                              0,
                                              glyphRunEnumerator.GetAddressOf());

        // DWRITE_E_NOCOLOR is the "this glyph has no color layers" answer, not the only failure:
        // E_INVALIDARG (a font whose COLR/CPAL tables DirectWrite rejects) and E_OUTOFMEMORY both
        // land here too, and both leave glyphRunEnumerator NULL. Testing only for NOCOLOR sent every
        // other failure into the color branch, whose first act is glyphRunEnumerator->MoveNext() --
        // a call through a null pointer, and a deterministic one now that the enumerator is a ComPtr
        // rather than an uninitialized raw pointer. Treat any failure as "render it monochrome",
        // which is the same graceful degradation NOCOLOR already gets.
        if (FAILED(hr))
        {
            if (hr != DWRITE_E_NOCOLOR)
                errorLog()("directwrite: TranslateColorGlyphRun failed for glyph {}; rendering it "
                           "without color.",
                           glyph.index.value);

            output.bitmap.resize(*height * *width * 3);
            output.format = BitmapFormat::RGB;

            auto t = output.bitmap.begin();

            // Report the failure rather than returning a bitmap the texture step never filled: the
            // caller's std::nullopt path skips the glyph, where a zero-filled buffer would have been
            // drawn as a faded one.
            if (!renderGlyphRunToBitmap(
                    glyphAnalysis.Get(), textureBounds, DWRITE_COLOR_F {}, output.format, t))
                return std::nullopt;

            return output;
        }
        else
        {
            output.bitmap.resize(*height * *width * 4);
            output.format = BitmapFormat::RGBA;

            _d->fontsHasColor.at(glyph.font) = true;
            while (true)
            {
                // haveRun is an out-parameter DirectWrite leaves UNTOUCHED on failure, so a
                // discarded HRESULT here branches the loop on an indeterminate value -- reading
                // uninitialized memory to decide whether another layer exists.
                BOOL haveRun = FALSE;
                hr = glyphRunEnumerator->MoveNext(&haveRun);
                if (FAILED(hr))
                {
                    errorLog()("directwrite: MoveNext failed while enumerating color runs for glyph {}.",
                               glyph.index.value);
                    break;
                }
                if (!haveRun)
                    break;

                DWRITE_COLOR_GLYPH_RUN const* colorRun;
                hr = glyphRunEnumerator->GetCurrentRun(&colorRun);
                if (FAILED(hr))
                {
                    break;
                }

                ComPtr<IDWriteGlyphRunAnalysis> colorGlyphsAnalysis;

                // Same null-dereference shape as the monochrome path above: on failure
                // colorGlyphsAnalysis stays NULL and renderGlyphRunToBitmap() would call through it.
                hr = _d->factory->CreateGlyphRunAnalysis(&colorRun->glyphRun,
                                                         _d->pixelPerDip(),
                                                         nullptr,
                                                         renderingMode,
                                                         DWRITE_MEASURING_MODE::DWRITE_MEASURING_MODE_NATURAL,
                                                         0.0f,
                                                         0.0f,
                                                         &colorGlyphsAnalysis);
                if (FAILED(hr) || !colorGlyphsAnalysis)
                {
                    errorLog()("directwrite: CreateGlyphRunAnalysis failed for a color run of glyph {}.",
                               glyph.index.value);
                    return nullopt;
                }

                auto t = output.bitmap.begin();
                auto const color = colorRun->paletteIndex == 0xFFFF ? DWRITE_COLOR_F {} : colorRun->runColor;
                // A color glyph composites one layer per run, so a failed layer leaves the ones already
                // blended in place; abandoning the glyph is still better than compositing zeros over them.
                if (!renderGlyphRunToBitmap(
                        colorGlyphsAnalysis.Get(), textureBounds, color, output.format, t))
                    return std::nullopt;
            }

            return output;
        }
    }

    return nullopt;
}

void DirectWriteShaper::setDPI(DPI dpi)
{
    _d->dpi = dpi;
    clearCache();
}

void DirectWriteShaper::setLocator(FontLocator& locator)
{
    _d->locator = &locator;
}

void DirectWriteShaper::clearCache()
{
    // TODO: clear the cache
}

void DirectWriteShaper::setFontFallbackLimit(int limit)
{
    _d->fallbackLimit = limit;
}

optional<GlyphPosition> DirectWriteShaper::shape(FontKey font, char32_t codepoint)
{
    return nullopt; // TODO
}

} // namespace text
