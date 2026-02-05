#include <text_shaper/directwrite_analysis_wrapper.h>
#include <text_shaper/directwrite_shaper.h>
#include <text_shaper/font_locator.h>

#include <algorithm>
#include <string>

// {{{ TODO: replace with libunicode
#include <codecvt>
#include <locale>
// }}}

#include <iostream> // DEBUGGING ONLY

#include <dwrite.h>
#include <dwrite_3.h>

#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

using std::codecvt_utf8;
using std::codecvt_utf8_utf16;
using std::make_unique;
using std::max;
using std::move;
using std::nullopt;
using std::optional;
using std::pair;
using std::string;
using std::u32string;
using std::vector;
using std::wstring;
using std::wstring_convert;

namespace text
{
namespace
{
    void renderGlyphRunToBitmap(IDWriteGlyphRunAnalysis* _glyphAnalysis,
                                const RECT& _textureBounds,
                                const DWRITE_COLOR_F& runColor,
                                bitmap_format _targetFormat,
                                std::vector<uint8_t>::iterator& _it)
    {
        const auto width = _textureBounds.right - _textureBounds.left;
        const auto height = _textureBounds.bottom - _textureBounds.top;

        std::vector<uint8_t> tmp;
        tmp.resize(height * width * 3);

        auto hr = _glyphAnalysis->CreateAlphaTexture(
            DWRITE_TEXTURE_CLEARTYPE_3x1, &_textureBounds, tmp.data(), tmp.size());

        // TODO: #ifdef (__SSE2__) SIMD me :-)
        for (auto i = 0; i < height; i++)
            for (auto j = 0; j < width; j++)
            {
                const auto base = (i * width + j) * 3;
                const auto srcR = tmp[base];
                const auto srcG = tmp[base + 1];
                const auto srcB = tmp[base + 2];

                if (_targetFormat == bitmap_format::rgb)
                {
                    *_it++ = srcR;
                    *_it++ = srcG;
                    *_it++ = srcB;
                }

                if (_targetFormat == bitmap_format::rgba)
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
    }

    constexpr double ptToEm(double pt)
    {
        return pt * (96.0 / 72.0);
    }
} // namespace

struct DxFontInfo
{
    font_description description;
    font_size size;
    font_metrics metrics;
    int fontUnitsPerEm;

    // Owning pointer
    IDWriteFontFace5* fontFace;
};

struct directwrite_shaper::Private
{
    ComPtr<IDWriteFactory7> factory;
    ComPtr<IDWriteTextAnalyzer1> textAnalyzer;
    font_locator* locator_;

    DPI dpi_;
    std::wstring userLocale;
    std::unordered_map<font_key, DxFontInfo> fonts;
    std::unordered_map<font_key, bool> fontsHasColor;

    font_key nextFontKey;

    Private(DPI dpi, font_locator& _locator): dpi_ { dpi }, locator_ { &_locator }
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

    font_key create_font_key()
    {
        auto result = nextFontKey;
        nextFontKey.value++;
        return result;
    }

    optional<text::font_key> add_font(font_source const& _source,
                                      font_description const& _description,
                                      font_size _size)
    {
        if (!std::holds_alternative<font_path>(_source))
        {
            return nullopt;
        }

        auto const& sourcePath = std::get<font_path>(_source);

        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> wStringConverter;
        std::wstring wSourcePath = wStringConverter.from_bytes(sourcePath.value);

        ComPtr<IDWriteFontFile> fontFile;
        auto hr = factory->CreateFontFileReference(wSourcePath.c_str(), NULL, &fontFile);
        if (FAILED(hr))
        {
            return nullopt;
        }

        BOOL isSupported {};
        DWRITE_FONT_FILE_TYPE fileType {};
        DWRITE_FONT_FACE_TYPE fontFaceType {};
        UINT32 numFaces {};
        IDWriteFontFace* fontFace {};
        fontFile->Analyze(&isSupported, &fileType, &fontFaceType, &numFaces);
        hr = factory->CreateFontFace(
            fontFaceType, 1, fontFile.GetAddressOf(), 0, DWRITE_FONT_SIMULATIONS_NONE, &fontFace);
        if (FAILED(hr))
        {
            return nullopt;
        }

        IDWriteFontFace3* fontFace3 {};
        fontFace->QueryInterface(&fontFace3);

        ComPtr<IDWriteLocalizedStrings> familyNames {};
        fontFace3->GetFamilyNames(&familyNames);

        BOOL localeExists = FALSE;
        unsigned index {};
        familyNames->FindLocaleName(userLocale.c_str(), &index, &localeExists);

        if (!localeExists)
        {
            // Fallback to en-US
            const wchar_t* localeName = L"en-US";
            familyNames->FindLocaleName(localeName, &index, &localeExists);
        }

        if (!localeExists)
        {
            // Fallback to locale where index = 0
            index = 0;

            std::wstring localeName {};
            UINT32 length = 0;
            familyNames->GetLocaleNameLength(index, &length);
            localeName.resize(length);
            familyNames->FindLocaleName(localeName.c_str(), &index, &localeExists);
        }

        UINT32 length = 0;
        familyNames->GetStringLength(index, &length);

        std::wstring resolvedFamilyName {};
        resolvedFamilyName.resize(length);

        familyNames->GetString(index, resolvedFamilyName.data(), length + 1);

        for (auto&& [fontKey, fontInfo]: fonts)
            if (_description == fontInfo.description && fontInfo.fontFace->Equals(fontFace))
                return fontKey;

        auto dwMetrics = DWRITE_FONT_METRICS {};
        fontFace3->GetMetrics(&dwMetrics);

        auto const dipScalar = ptToEm(_size.pt) / dwMetrics.designUnitsPerEm * pixelPerDip();
        auto const lineHeight = dwMetrics.ascent + dwMetrics.descent + dwMetrics.lineGap;

        DxFontInfo fontInfo {};
        fontInfo.description = _description;
        fontInfo.description.familyName = wStringConverter.to_bytes(resolvedFamilyName);
        fontInfo.description.wFamilyName = resolvedFamilyName;
        fontInfo.size = _size;
        fontInfo.metrics.lineHeight = int(ceil(lineHeight * dipScalar));
        fontInfo.metrics.ascender = int(ceil(dwMetrics.ascent * dipScalar));
        fontInfo.metrics.descender = int(ceil(dwMetrics.descent * dipScalar));
        fontInfo.metrics.underlinePosition = int(ceil(dwMetrics.underlinePosition * dipScalar));
        fontInfo.metrics.underlineThickness = int(ceil(dwMetrics.underlineThickness * dipScalar));
        fontInfo.metrics.advance = int(ceil(computeAverageAdvance(fontFace) * dipScalar));

        fontFace->QueryInterface(&fontInfo.fontFace);

        auto key = create_font_key();
        fonts.emplace(pair { key, std::move(fontInfo) });
        fontsHasColor.emplace(pair { key, false });
        return key;
    }

    int computeAverageAdvance(IDWriteFontFace* _fontFace)
    {
        auto constexpr firstCharIndex = UINT16 { 32 };
        auto constexpr lastCharIndex = UINT16 { 127 };
        auto constexpr charCount = lastCharIndex - firstCharIndex + 1;

        UINT32 codePoints[charCount] {};
        for (UINT16 i = 0; i < charCount; i++)
            codePoints[i] = firstCharIndex + i;
        UINT16 glyphIndices[charCount] {};
        _fontFace->GetGlyphIndicesA(codePoints, charCount, glyphIndices);

        DWRITE_GLYPH_METRICS dwGlyphMetrics[charCount] {};
        _fontFace->GetDesignGlyphMetrics(glyphIndices, charCount, dwGlyphMetrics);

        UINT32 maxAdvance = 0;
        for (int i = 0; i < charCount; i++)
            maxAdvance = max(maxAdvance, dwGlyphMetrics[i].advanceWidth);

        return int(maxAdvance);
    }

    float pixelPerDip() { return static_cast<float>(dpi_.x) / 96.0f; }
};

directwrite_shaper::directwrite_shaper(DPI _dpi, font_locator& _locator):
    d(new Private(_dpi, _locator), [](Private* p) { delete p; })
{
}

optional<font_key> directwrite_shaper::load_font(font_description const& _description, font_size _size)
{
    locatorLog().operator()("Loading font chain for: {}", _description);
    font_source_list sources = d->locator_->locate(_description);
    if (sources.empty())
        return nullopt;

    optional<font_key> fontKeyOpt = d->add_font(sources[0], _description, _size);
    if (!fontKeyOpt.has_value())
        return nullopt;

    return fontKeyOpt;
}

font_metrics directwrite_shaper::metrics(font_key _key) const
{
    DxFontInfo const& fontInfo = d->fonts.at(_key);
    return fontInfo.metrics;
}

void directwrite_shaper::shape(font_key _font,
                               std::u32string_view _text,
                               gsl::span<unsigned> _clusters,
                               unicode::Script _script,
                               unicode::PresentationStyle _presentation,
                               shape_result& _result)
{
    wstring_convert<codecvt_utf8<char32_t>, char32_t> conv1;
    string bytes = conv1.to_bytes(u32string { _text });
    wstring_convert<codecvt_utf8_utf16<wchar_t>> conv2;
    wstring wText = conv2.from_bytes(bytes);

    WCHAR const* textString = wText.c_str();
    UINT32 textLength = wText.size();
    DxFontInfo fontInfo = d->fonts.at(_font);
    IDWriteFontFace5* fontFace = fontInfo.fontFace;

    vector<UINT16> glyphIndices;
    vector<INT32> glyphDesignUnitAdvances;
    vector<UINT16> glyphClusters;

    BOOL isTextSimple = FALSE;
    UINT32 uiLengthRead = 0;
    const UINT32 glyphStart = 0;

    glyphIndices.resize(textLength);
    glyphClusters.resize(textLength);

    d->textAnalyzer->GetTextComplexity(
        textString, textLength, fontFace, &isTextSimple, &uiLengthRead, &glyphIndices.at(glyphStart));

    BOOL isEntireTextSimple = isTextSimple && uiLengthRead == textLength;
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

        for (size_t i = glyphStart; i < textLength; i++)
        {
            const auto cellWidth = static_cast<double>((float) glyphDesignUnitAdvances.at(i))
                                   / designUnitsPerEm * ptToEm(fontInfo.size.pt) * d->pixelPerDip();
            glyph_position gpos {};
            gpos.presentation = _presentation;
            gpos.glyph = glyph_key { fontInfo.size, _font, glyph_index { glyphIndices.at(i) } };
            gpos.advance.x = static_cast<int>(cellWidth);
            _result.emplace_back(gpos);
        }
    }
    else
    {
        // Complex shaping
        UINT32 const textStart = 0;
        dwrite_analysis_wrapper analysisWrapper(wText, d->userLocale);

        // Script analysis
        d->textAnalyzer->AnalyzeScript(&analysisWrapper, 0, wText.size(), &analysisWrapper);

        UINT32 actualGlyphCount = 0;
        UINT32 maxGlyphCount = textLength;
        vector<DWRITE_SHAPING_TEXT_PROPERTIES> textProps(textLength);
        vector<DWRITE_SHAPING_GLYPH_PROPERTIES> glyphProps(textLength);

        uint8_t attempt = 0;

        do
        {
            auto hr = d->textAnalyzer->GetGlyphs(&wText.at(textStart),
                                                 textLength,
                                                 fontFace,
                                                 0, // isSideways,
                                                 0, // isRightToLeft
                                                 &analysisWrapper.script,
                                                 d->userLocale.data(),
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
            attempt++;

            if (SUCCEEDED(hr) && std::find(glyphIndices.begin(), glyphIndices.end(), 0) != glyphIndices.end())
            {
                // glyphIndices contains 0 means some glyphs are missing from the current font.
                // Need to perform fallback analysis.
                font_source_list sources = d->locator_->resolve(gsl::span(_text.data(), _text.size()));
                if (sources.size() > 0)
                {
                    optional<font_key> fontKeyOpt =
                        d->add_font(sources[0], fontInfo.description, fontInfo.size);
                    if (fontKeyOpt.has_value())
                    {
                        _font = fontKeyOpt.value();
                        fontInfo = d->fonts.at(_font);
                        fontFace = fontInfo.fontFace;
                    }
                }
                continue;
            }
            else if (hr == E_NOT_SUFFICIENT_BUFFER)
            {
                // Using a larger buffer.
                maxGlyphCount *= 2;
                UINT32 const totalGlyphsArrayCount = glyphStart + maxGlyphCount;

                glyphProps.resize(maxGlyphCount);
                glyphIndices.resize(totalGlyphsArrayCount);
            }
            else
            {
                break;
            }
        } while (attempt < 3);

        vector<float> glyphAdvances(actualGlyphCount);
        vector<DWRITE_GLYPH_OFFSET> glyphOffsets(actualGlyphCount);

        auto hr = d->textAnalyzer->GetGlyphPlacements(&wText.at(textStart),
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
                                                      d->userLocale.data(),
                                                      nullptr, // features
                                                      nullptr, // featureLengths
                                                      0,       // featureCount
                                                      &glyphAdvances.at(glyphStart),
                                                      &glyphOffsets.at(glyphStart));

        for (size_t i = glyphStart; i < actualGlyphCount; i++)
        {
            glyph_position gpos {};
            gpos.glyph = glyph_key { fontInfo.size, _font, glyph_index { glyphIndices.at(i) } };
            gpos.offset.x = static_cast<int>(glyphOffsets.at(i).advanceOffset);
            // gpos.offset.y = static_cast<int>(static_cast<double>(pos[i].y_offset) / 64.0f);

            gpos.advance.x = static_cast<int>(glyphAdvances.at(i));
            // gpos.advance.y = static_cast<int>(static_cast<double>(pos[i].y_advance) / 64.0f);
            _result.emplace_back(gpos);
        }
    }
}

std::optional<rasterized_glyph> directwrite_shaper::rasterize(glyph_key _glyph, render_mode _mode)
{
    DxFontInfo const& fontInfo = d->fonts.at(_glyph.font);
    IDWriteFontFace5* fontFace = fontInfo.fontFace;
    float const fontEmSize = static_cast<float>(ptToEm(_glyph.size.pt));

    UINT16 const glyphIndex = static_cast<UINT16>(_glyph.index.value);
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
    d->factory->CreateRenderingParams(&renderingParams);

    DWRITE_RENDERING_MODE renderingMode;
    auto hr = fontFace->GetRecommendedRenderingMode(fontEmSize,
                                                    d->pixelPerDip(),
                                                    DWRITE_MEASURING_MODE::DWRITE_MEASURING_MODE_NATURAL,
                                                    renderingParams.Get(),
                                                    &renderingMode);
    if (FAILED(hr))
    {
        renderingMode = DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC;
    }

    ComPtr<IDWriteGlyphRunAnalysis> glyphAnalysis;
    rasterized_glyph output {};

    d->factory->CreateGlyphRunAnalysis(&glyphRun,
                                       d->pixelPerDip(),
                                       nullptr,
                                       renderingMode,
                                       DWRITE_MEASURING_MODE::DWRITE_MEASURING_MODE_NATURAL,
                                       0.0f,
                                       0.0f,
                                       &glyphAnalysis);

    RECT textureBounds {};
    glyphAnalysis->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &textureBounds);

    output.bitmapSize.width = vtbackend::Width(textureBounds.right - textureBounds.left);
    output.bitmapSize.height = vtbackend::Height(textureBounds.bottom - textureBounds.top);
    output.position.x = textureBounds.left;
    output.position.y = -textureBounds.top;

    auto const [width, height] = output.bitmapSize;

    IDWriteFactory2* factory2;
    IDWriteColorGlyphRunEnumerator* glyphRunEnumerator;
    if (SUCCEEDED(d->factory->QueryInterface(IID_PPV_ARGS(&factory2))))
    {
        hr = factory2->TranslateColorGlyphRun(0.0f,
                                              0.0f,
                                              &glyphRun,
                                              nullptr,
                                              DWRITE_MEASURING_MODE::DWRITE_MEASURING_MODE_NATURAL,
                                              nullptr,
                                              0,
                                              &glyphRunEnumerator);

        if (hr == DWRITE_E_NOCOLOR)
        {
            output.bitmap.resize(*height * *width * 3);
            output.format = bitmap_format::rgb;

            auto t = output.bitmap.begin();

            renderGlyphRunToBitmap(glyphAnalysis.Get(), textureBounds, DWRITE_COLOR_F {}, output.format, t);

            return output;
        }
        else
        {
            output.bitmap.resize(*height * *width * 4);
            output.format = bitmap_format::rgba;

            d->fontsHasColor.at(_glyph.font) = true;
            while (true)
            {
                BOOL haveRun;
                glyphRunEnumerator->MoveNext(&haveRun);
                if (!haveRun)
                    break;

                DWRITE_COLOR_GLYPH_RUN const* colorRun;
                hr = glyphRunEnumerator->GetCurrentRun(&colorRun);
                if (FAILED(hr))
                {
                    break;
                }

                ComPtr<IDWriteGlyphRunAnalysis> colorGlyphsAnalysis;

                d->factory->CreateGlyphRunAnalysis(&colorRun->glyphRun,
                                                   d->pixelPerDip(),
                                                   nullptr,
                                                   renderingMode,
                                                   DWRITE_MEASURING_MODE::DWRITE_MEASURING_MODE_NATURAL,
                                                   0.0f,
                                                   0.0f,
                                                   &colorGlyphsAnalysis);

                auto t = output.bitmap.begin();
                auto const color = colorRun->paletteIndex == 0xFFFF ? DWRITE_COLOR_F {} : colorRun->runColor;
                renderGlyphRunToBitmap(colorGlyphsAnalysis.Get(), textureBounds, color, output.format, t);
            }

            return output;
        }
    }

    return nullopt;
}

void directwrite_shaper::set_dpi(DPI dpi)
{
    d->dpi_ = dpi;
    clear_cache();
}

void directwrite_shaper::set_locator(font_locator& _locator)
{
    d->locator_ = &_locator;
}

void directwrite_shaper::clear_cache()
{
    // TODO: clear the cache
}

void directwrite_shaper::set_font_fallback_limit([[maybe_unused]] int limit)
{
    // DirectWrite manages font fallback internally.
}

optional<glyph_position> directwrite_shaper::shape(font_key _font, char32_t _codepoint)
{
    return nullopt; // TODO
}

} // namespace text
