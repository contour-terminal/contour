#include <text_shaper/directwrite_shaper.h>

#include <crispy/debuglog.h>

#include <algorithm>
#include <string>

// {{{ TODO: replace with libunicode
#include <codecvt>
#include <locale>
// }}}

#include <wrl/client.h>
#include <wrl/implements.h>
#include <dwrite.h>
#include <dwrite_3.h>

#include <iostream> // DEBUGGING ONLY

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::InhibitFtmBase;

using std::max;
using std::make_unique;
using std::move;
using std::nullopt;
using std::optional;
using std::pair;
using std::wstring;

namespace text {
namespace
{
    font_weight dwFontWeight(int _weight)
    {
        switch (_weight)
        {
            case DWRITE_FONT_WEIGHT_THIN: return font_weight::thin;
            case DWRITE_FONT_WEIGHT_EXTRA_LIGHT: return font_weight::extra_light;
            case DWRITE_FONT_WEIGHT_LIGHT: return font_weight::light;
            case DWRITE_FONT_WEIGHT_SEMI_LIGHT: return font_weight::demilight;
            case DWRITE_FONT_WEIGHT_REGULAR: return font_weight::normal;
            // XXX What about font_weight::book (which does exist via fontconfig)?
            case DWRITE_FONT_WEIGHT_MEDIUM: return font_weight::medium;
            case DWRITE_FONT_WEIGHT_DEMI_BOLD: return font_weight::demibold;
            case DWRITE_FONT_WEIGHT_BOLD: return font_weight::bold;
            case DWRITE_FONT_WEIGHT_EXTRA_BOLD: return font_weight::extra_bold;
            case DWRITE_FONT_WEIGHT_BLACK: return font_weight::black;
            case DWRITE_FONT_WEIGHT_EXTRA_BLACK: return font_weight::extra_black;
            default: // TODO: the others
                break;
        }
        return font_weight::normal; // TODO: rename normal to regular
    }

    font_slant dwFontSlant(int _style)
    {
        switch (_style)
        {
            case DWRITE_FONT_STYLE_NORMAL: return font_slant::normal;
            case DWRITE_FONT_STYLE_ITALIC: return font_slant::italic;
            case DWRITE_FONT_STYLE_OBLIQUE: return font_slant::oblique;
        }
        return font_slant::normal;
    }
}

struct FontInfo
{
    font_description description;
    font_size size;
    font_metrics metrics;
    int fontUnitsPerEm;

    ComPtr<IDWriteFont3> font;
    ComPtr<IDWriteFontFace5> fontFace;
};

struct directwrite_shaper::Private
{
    ComPtr<IDWriteFactory7> factory;
    ComPtr<IDWriteTextAnalyzer1> textAnalyzer;
    crispy::Point dpi_;
    std::wstring userLocale;
    std::unordered_map<font_key, FontInfo> fonts;

    font_key nextFontKey;

    Private(crispy::Point _dpi) :
        dpi_{ _dpi }
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

    text::font_key add_font(const std::wstring _familyName, font_description _description, font_size _size, ComPtr<IDWriteFont> _font)
    {
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> wStringConverter;
        std::string familyName = wStringConverter.to_bytes(_familyName);;
        _description.familyName = familyName;
        _description.wFamilyName = _familyName;

        ComPtr<IDWriteFontFace> fontFace;
        _font->CreateFontFace(fontFace.GetAddressOf());

        for (const std::pair<font_key, FontInfo>& kv : fonts)
        {
            const auto fontInfo = kv.second;
            if (_description == fontInfo.description && fontInfo.fontFace.Get()->Equals(fontFace.Get()))
            {
                return kv.first;
            }
        }

        auto dwMetrics = DWRITE_FONT_METRICS{};
        _font->GetMetrics(&dwMetrics);

        auto const dipScalar = _size.pt * (96.0 / 72.0) / dwMetrics.designUnitsPerEm * pixelPerDip();
        auto const lineHeight = dwMetrics.ascent + dwMetrics.descent + dwMetrics.lineGap;

        auto fontInfo = FontInfo{};
        fontInfo.description = _description;
        fontInfo.size = _size;
        fontInfo.metrics.line_height = int(ceil(lineHeight * dipScalar));
        fontInfo.metrics.ascender = int(ceil(dwMetrics.ascent * dipScalar));
        fontInfo.metrics.descender = int(ceil(dwMetrics.descent * dipScalar));
        fontInfo.metrics.underline_position = int(ceil(dwMetrics.underlinePosition * dipScalar));
        fontInfo.metrics.underline_thickness = int(ceil(dwMetrics.underlineThickness * dipScalar));
        fontInfo.metrics.advance = int(ceil(computeAverageAdvance(fontFace.Get()) * dipScalar));

        _font.As(&fontInfo.font);
        fontFace.As(&fontInfo.fontFace);

        auto key = create_font_key();
        fonts.emplace(pair{ key, move(fontInfo) });
        return key;
    }

    int computeAverageAdvance(IDWriteFontFace* _fontFace)
    {
        auto constexpr firstCharIndex = UINT16{32};
        auto constexpr lastCharIndex = UINT16{127};
        auto constexpr charCount = lastCharIndex - firstCharIndex + 1;

        UINT32 codePoints[charCount]{};
        for (UINT16 i = 0; i < charCount; i++)
            codePoints[i] = firstCharIndex + i;
        UINT16 glyphIndices[charCount]{};
        _fontFace->GetGlyphIndicesA(codePoints, charCount, glyphIndices);

        DWRITE_GLYPH_METRICS dwGlyphMetrics[charCount]{};
        _fontFace->GetDesignGlyphMetrics(glyphIndices, charCount, dwGlyphMetrics);

        UINT32 maxAdvance = 0;
        for (int i = 0; i < charCount; i++)
            maxAdvance = max(maxAdvance, dwGlyphMetrics[i].advanceWidth);

        return int(maxAdvance);
    }

    float pixelPerDip()
    {
        return dpi_.x / 96;
    }
};

class DWriteAnalysisWrapper : public RuntimeClass<RuntimeClassFlags<ClassicCom | InhibitFtmBase>, IDWriteTextAnalysisSource, IDWriteTextAnalysisSink>
{
public:
    DWriteAnalysisWrapper(const std::wstring& _text, const std::wstring& _userLocale) :
        text(_text),
        userLocale(_userLocale)
    {
    }

#pragma region IDWriteTextAnalysisSource
    HRESULT GetTextAtPosition(
        UINT32 textPosition,
        _Outptr_result_buffer_(*textLength) WCHAR const** textString,
        _Out_ UINT32* textLength)
    {
        *textString = nullptr;
        *textLength = 0;

        if (textPosition < text.size())
        {
            *textString = &text.at(textPosition);
            *textLength = text.size() - textPosition;
        }

        return S_OK;
    }

    HRESULT GetTextBeforePosition(
        UINT32 textPosition,
        _Outptr_result_buffer_(*textLength) WCHAR const** textString,
        _Out_ UINT32* textLength)
    {
        *textString = nullptr;
        *textLength = 0;

        if (textPosition > 0 && textPosition <= text.size())
        {
            *textString = text.data();
            *textLength = textPosition;
        }

        return S_OK;
    }

    DWRITE_READING_DIRECTION GetParagraphReadingDirection()
    {
        // TODO: is this always correct?
        return DWRITE_READING_DIRECTION::DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    }

    HRESULT GetLocaleName(
        UINT32 textPosition,
        _Out_ UINT32* textLength,
        _Outptr_result_z_ WCHAR const** localeName)
    {
        *localeName = userLocale.c_str();
        *textLength = text.size() - textPosition;

        return S_OK;
    }

    HRESULT GetNumberSubstitution(UINT32 textPosition,
        _Out_ UINT32* textLength,
        _COM_Outptr_ IDWriteNumberSubstitution** numberSubstitution)
    {

        *numberSubstitution = nullptr;
        *textLength = text.size() - textPosition;

        return S_OK;
    }
#pragma endregion

#pragma region IDWriteTextAnalysisSink
    HRESULT SetScriptAnalysis(UINT32 textPosition, UINT32 textLength, _In_ DWRITE_SCRIPT_ANALYSIS const* scriptAnalysis)
    {
        script = *scriptAnalysis;
        return S_OK;
    }

    HRESULT SetLineBreakpoints(UINT32 textPosition,
        UINT32 textLength,
        _In_reads_(textLength) DWRITE_LINE_BREAKPOINT const* lineBreakpoints)
    {
        return S_OK;
    }

    HRESULT SetBidiLevel(UINT32 textPosition,
        UINT32 textLength,
        UINT8 /*explicitLevel*/,
        UINT8 resolvedLevel)
    {
        return S_OK;
    }

    HRESULT SetNumberSubstitution(UINT32 textPosition,
        UINT32 textLength,
        _In_ IDWriteNumberSubstitution* numberSubstitution)
    {
        return S_OK;
    }
#pragma endregion

    DWRITE_SCRIPT_ANALYSIS script;

private:
    const std::wstring& text;
    const std::wstring& userLocale;
};

directwrite_shaper::directwrite_shaper(crispy::Point _dpi) :
    d(new Private(_dpi), [](Private* p) { delete p; })
{
}

optional<font_key> directwrite_shaper::load_font(font_description const& _description, font_size _size)
{
    debuglog(FontFallbackTag).write("Loading font chain for: {}", _description);

    IDWriteFontCollection* fontCollection{};
    d->factory->GetSystemFontCollection(&fontCollection);

    // TODO: use libunicode for that (TODO: create wchar_t/char16_t converters in libunicode)
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> wStringConverter;
    std::wstring familyName = wStringConverter.from_bytes(_description.familyName);

    UINT32 familyIndex;
    BOOL familyExists = FALSE;
    fontCollection->FindFamilyName(familyName.data(), &familyIndex, &familyExists);

    if (!familyExists) {
        // Fallback to Consolas
        const wchar_t* consolas = L"Consolas";
        fontCollection->FindFamilyName(consolas, &familyIndex, &familyExists);
    }

    ComPtr<IDWriteFontFamily> fontFamily;
    fontCollection->GetFontFamily(familyIndex, &fontFamily);

    ComPtr<IDWriteLocalizedStrings> familyNames;
    fontFamily->GetFamilyNames(&familyNames);

    BOOL localeExists = FALSE;
    unsigned index{};
    familyNames->FindLocaleName(d->userLocale.c_str(), &index, &localeExists);

    if (!localeExists) {
        // Fallback to en-US
        const wchar_t* localeName = L"en-US";
        familyNames->FindLocaleName(localeName, &index, &localeExists);
    }

    if (!localeExists) {
        // Fallback to locale where index = 0
        index = 0;

        std::wstring localeName{};
        UINT32 length = 0;
        familyNames->GetLocaleNameLength(index, &length);
        localeName.resize(length);
        familyNames->FindLocaleName(localeName.c_str(), &index, &localeExists);
    }

    UINT32 length = 0;
    familyNames->GetStringLength(index, &length);

    std::wstring resolvedFamilyName;
    resolvedFamilyName.resize(length);

    familyNames->GetString(index, resolvedFamilyName.data(), length + 1);

    for (UINT32 k = 0, ke = fontFamily->GetFontCount(); k < ke; ++k)
    {
        ComPtr<IDWriteFont> font;
        fontFamily->GetFont(k, font.GetAddressOf());

        font_weight weight = dwFontWeight(font->GetWeight());
        if (weight != _description.weight)
            continue;

        font_slant slant = dwFontSlant(font->GetStyle());
        if (weight != _description.weight)
            continue;

        return d->add_font(resolvedFamilyName, _description, _size, font);
    }

    debuglog(FontFallbackTag).write("Font not found.");
    return nullopt;
}

font_metrics directwrite_shaper::metrics(font_key _key) const
{
    FontInfo const& fontInfo = d->fonts.at(_key);
    return fontInfo.metrics;
}

void directwrite_shaper::shape(font_key _font,
                               std::u32string_view _text,
                               crispy::span<unsigned> _clusters,
                               unicode::Script _script,
                               shape_result& _result)
{
    std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> conv1;
    std::string bytes = conv1.to_bytes(std::u32string{ _text });
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv2;
    std::wstring wText = conv2.from_bytes(bytes);

    WCHAR const* textString = wText.c_str();
    UINT32 textLength = wText.size();
    FontInfo fontInfo = d->fonts.at(_font);
    IDWriteFontFace5* fontFace = fontInfo.fontFace.Get();

    std::vector<UINT16> glyphIndices;
    std::vector<INT32> glyphDesignUnitAdvances;
    std::vector<UINT16> glyphClusters;

    BOOL isTextSimple = FALSE;
    UINT32 uiLengthRead = 0;
    const UINT32 glyphStart = 0;

    glyphIndices.resize(textLength);
    glyphClusters.resize(textLength);

    d->textAnalyzer->GetTextComplexity(
        textString,
        textLength,
        fontFace,
        &isTextSimple,
        &uiLengthRead,
        &glyphIndices.at(glyphStart));


    BOOL isEntireTextSimple = isTextSimple && uiLengthRead == textLength;
    // Note that some fonts won't report isTextSimple even for ASCII only strings, due to the existence of "locl" table.
    if (isEntireTextSimple)
    {
        // "Simple" shaping assumes every character has the exact same width which can be calculate from the metrics.
        //  This saves us from the need of expensive shaping operation.
        DWRITE_FONT_METRICS1 metrics;
        fontFace->GetMetrics(&metrics);

        glyphDesignUnitAdvances.resize(textLength);
        USHORT designUnitsPerEm = metrics.designUnitsPerEm;
        fontFace->GetDesignGlyphAdvances(
            textLength,
            & glyphIndices.at(glyphStart),
            & glyphDesignUnitAdvances.at(glyphStart),
            false);

        for (size_t i = glyphStart; i < textLength; i++)
        {
            const auto cellWidth = static_cast<double>((float)glyphDesignUnitAdvances.at(i)) / designUnitsPerEm * fontInfo.size.pt * (96.0 / 72.0)  * d->pixelPerDip();
            glyph_position gpos{};
            gpos.glyph = glyph_key{ _font, fontInfo.size, glyph_index{glyphIndices.at(i)} };
            // gpos.offset.x = static_cast<int>(cellWidth);
            //gpos.offset.y = static_cast<int>(static_cast<double>(pos[i].y_offset) / 64.0f);

            gpos.advance.x = static_cast<int>(cellWidth);
            //gpos.advance.y = static_cast<int>(static_cast<double>(pos[i].y_advance) / 64.0f);
            _result.emplace_back(gpos);
        }
    }
    else {
        // Complex shaping
        const UINT32 textStart = 0;
        DWriteAnalysisWrapper analysisWrapper(wText, d->userLocale);

        // Fallback analysis
        ComPtr<IDWriteTextFormat> format;
        auto hr = d->factory->CreateTextFormat(
            fontInfo.description.wFamilyName.c_str(),
            nullptr,
            fontInfo.font->GetWeight(),
            fontInfo.font->GetStyle(),
            fontInfo.font->GetStretch(),
            fontInfo.size.pt,
            d->userLocale.c_str(),
            &format);

        ComPtr<IDWriteTextFormat1> format1;

        if (SUCCEEDED(hr) && SUCCEEDED(format->QueryInterface(IID_PPV_ARGS(&format1))))
        {
            ComPtr<IDWriteFontFallback> fallback;
            format1->GetFontFallback(&fallback);

            ComPtr<IDWriteFontCollection> collection;
            format1->GetFontCollection(&collection);

            std::wstring familyName;
            familyName.resize(format1->GetFontFamilyNameLength() + 1);
            format1->GetFontFamilyName(familyName.data(), familyName.size());

            const auto weight = format1->GetFontWeight();
            const auto style = format1->GetFontStyle();
            const auto stretch = format1->GetFontStretch();

            if (!fallback)
            {
                d->factory->GetSystemFontFallback(&fallback);
            }

            UINT32 mappedLength = 0;
            ComPtr<IDWriteFont> mappedFont;
            FLOAT scale = 0.0f;

            fallback->MapCharacters(&analysisWrapper,
                0,
                textLength,
                collection.Get(),
                familyName.data(),
                weight,
                style,
                stretch,
                &mappedLength,
                &mappedFont,
                &scale);

            if (mappedFont)
            {
                ComPtr<IDWriteFontFamily> fontFamily;
                mappedFont->GetFontFamily(&fontFamily);

                ComPtr<IDWriteLocalizedStrings> familyNames;
                fontFamily->GetFamilyNames(&familyNames);

                BOOL localeExists = FALSE;
                unsigned index{};
                familyNames->FindLocaleName(d->userLocale.c_str(), &index, &localeExists);

                if (!localeExists) {
                    // Fallback to en-US
                    const wchar_t* localeName = L"en-US";
                    familyNames->FindLocaleName(localeName, &index, &localeExists);
                }

                UINT32 length = 0;
                familyNames->GetStringLength(index, &length);

                std::wstring resolvedFamilyName;
                resolvedFamilyName.resize(length);

                familyNames->GetString(index, resolvedFamilyName.data(), length + 1);

                _font = d->add_font(resolvedFamilyName, fontInfo.description, fontInfo.size, mappedFont);
                fontInfo = d->fonts.at(_font);
                fontFace = fontInfo.fontFace.Get();
            }
        }

        // Script analysis
        d->textAnalyzer->AnalyzeScript(&analysisWrapper, 0, wText.size(), &analysisWrapper);

        UINT32 actualGlyphCount = 0;
        UINT32 maxGlyphCount = textLength;
        std::vector<DWRITE_SHAPING_TEXT_PROPERTIES> textProps(textLength);
        std::vector<DWRITE_SHAPING_GLYPH_PROPERTIES> glyphProps(textLength);

        uint8_t attempt = 0;

        do {
            auto hr = d->textAnalyzer->GetGlyphs(
                &wText.at(textStart),
                textLength,
                fontFace,
                0, // isSideways,
                0, // isRightToLeft
                &analysisWrapper.script,
                d->userLocale.data(),
                nullptr, // _numberSubstitution
                nullptr, // features
                nullptr, // featureLengths
                0, // featureCount
                maxGlyphCount, // maxGlyphCount
                &glyphClusters.at(textStart),
                &textProps.at(0),
                &glyphIndices.at(glyphStart),
                &glyphProps.at(0),
                &actualGlyphCount);
            if (hr == E_NOT_SUFFICIENT_BUFFER)
            {
                // Using a larger buffer.
                maxGlyphCount *= 2;
                const UINT32 totalGlyphsArrayCount = glyphStart + maxGlyphCount;

                glyphProps.resize(maxGlyphCount);
                glyphIndices.resize(totalGlyphsArrayCount);
            }
            else
            {
                break;
            }
        } while (attempt < 2);

        std::vector<float> glyphAdvances(actualGlyphCount);
        std::vector<DWRITE_GLYPH_OFFSET> glyphOffsets(actualGlyphCount);

        hr = d->textAnalyzer->GetGlyphPlacements(
            &wText.at(textStart),
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
            0, // featureCount
            &glyphAdvances.at(glyphStart),
            &glyphOffsets.at(glyphStart));

        for (size_t i = glyphStart; i < textLength; i++)
        {
            glyph_position gpos{};
            gpos.glyph = glyph_key{ _font, fontInfo.size, glyph_index{glyphIndices.at(i)} };
            gpos.offset.x = static_cast<int>(glyphOffsets.at(i).advanceOffset);
            //gpos.offset.y = static_cast<int>(static_cast<double>(pos[i].y_offset) / 64.0f);

            gpos.advance.x = static_cast<int>(glyphAdvances.at(i));
            //gpos.advance.y = static_cast<int>(static_cast<double>(pos[i].y_advance) / 64.0f);
            _result.emplace_back(gpos);
        }
    }
}

std::optional<rasterized_glyph> directwrite_shaper::rasterize(glyph_key _glyph, render_mode _mode)
{
    FontInfo const& fontInfo = d->fonts.at(_glyph.font);
    IDWriteFontFace5* fontFace = fontInfo.fontFace.Get();
    const float fontEmSize = _glyph.size.pt * (96.0 / 72.0);

    const UINT16 glyphIndex = static_cast<UINT16>(_glyph.index.value);
    const DWRITE_GLYPH_OFFSET glyphOffset{};
    const float glyphAdvances = 0;

    DWRITE_GLYPH_RUN glyphRun;
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
    auto hr = fontFace->GetRecommendedRenderingMode(
        fontEmSize,
        d->pixelPerDip(),
        DWRITE_MEASURING_MODE::DWRITE_MEASURING_MODE_NATURAL,
        renderingParams.Get(),
        &renderingMode);
    if (FAILED(hr)) {
        renderingMode = DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC;
    }

    ComPtr<IDWriteGlyphRunAnalysis> glyphAnalysis;
    rasterized_glyph output{};

    d->factory->CreateGlyphRunAnalysis(
        &glyphRun,
        d->pixelPerDip(),
        nullptr,
        renderingMode,
        DWRITE_MEASURING_MODE::DWRITE_MEASURING_MODE_NATURAL,
        0.0f,
        0.0f,
        &glyphAnalysis);

    RECT textureBounds{};
    glyphAnalysis->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &textureBounds);

    output.size.width = crispy::Width(textureBounds.right - textureBounds.left);
    output.size.height = crispy::Height(textureBounds.bottom - textureBounds.top);
    output.position.x = textureBounds.left;
    output.position.y = -textureBounds.top;

    auto const width = output.size.width;
    auto const height = output.size.height;
    output.bitmap.resize(*height * *width * 3);
    output.format = bitmap_format::rgb;

    std::vector<uint8_t> tmp;
    tmp.resize(*height * *width * 3);

    hr = glyphAnalysis->CreateAlphaTexture(
        DWRITE_TEXTURE_CLEARTYPE_3x1,
        &textureBounds,
        tmp.data(),
        tmp.size()
    );

    auto t = output.bitmap.begin();

    for (auto i = 0; i < *height; i++)
        for (auto j = 0; j < *width; j++)
        {
            const auto base = ((*height - 1 - i) * *width + j) * 3;

            *t++ = tmp[base];
            *t++ = tmp[base + 1];
            *t++ = tmp[base + 2];
        }

    if (FAILED(hr)) {
        return nullopt;
    }

    return output;
}

bool directwrite_shaper::has_color(font_key _font) const
{
    // TODO: use internal hash map to font info
    return false;
}

void directwrite_shaper::set_dpi(crispy::Point _dpi)
{
    d->dpi_ = _dpi;
    clear_cache();
}

void directwrite_shaper::set_locator(std::unique_ptr<font_locator> _locator)
{
    // TODO: use the given font locator for subsequent font location requests.
}

void directwrite_shaper::clear_cache()
{
    // TODO: clear the cache
}

optional<glyph_position> directwrite_shaper::shape(font_key _font,
                                                   char32_t _codepoint)
{
    return nullopt; // TODO
}

} // end namespace
