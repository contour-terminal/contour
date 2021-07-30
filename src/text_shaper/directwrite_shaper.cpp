#include <text_shaper/directwrite_shaper.h>

#include <crispy/debuglog.h>

#include <algorithm>
#include <string>

// {{{ TODO: replace with libunicode
#include <codecvt>
#include <locale>
// }}}

#include <wrl/client.h>
#include <dwrite.h>
#include <dwrite_3.h>

#include <iostream> // DEBUGGING ONLY

using Microsoft::WRL::ComPtr;

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
        // Fallback to locale where index = 0;
        return nullopt;
    }

    UINT32 length = 0;

    // TODO: very long font name?
    wchar_t name[64];
    familyNames->GetString(index, name, _countof(name));

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

        ComPtr<IDWriteFontFace> fontFace;
        font->CreateFontFace(fontFace.GetAddressOf());

        auto dwMetrics = DWRITE_FONT_METRICS{};
        font->GetMetrics(&dwMetrics);

        auto const dipScalar = _size.pt / dwMetrics.designUnitsPerEm * (d->dpi_.x / 96);
        auto const lineHeight = dwMetrics.ascent + dwMetrics.descent + dwMetrics.lineGap;

        auto fontInfo = FontInfo{};
        fontInfo.description = _description;
        fontInfo.size = _size;
        fontInfo.metrics.line_height = int(ceil(lineHeight * dipScalar));
        fontInfo.metrics.ascender = int(ceil(dwMetrics.ascent * dipScalar));
        fontInfo.metrics.descender = int(ceil(dwMetrics.descent * dipScalar));
        fontInfo.metrics.underline_position = int(ceil(dwMetrics.underlinePosition * dipScalar));
        fontInfo.metrics.underline_thickness = int(ceil(dwMetrics.underlineThickness * dipScalar));
        fontInfo.metrics.advance = int(ceil(d->computeAverageAdvance(fontFace.Get()) * dipScalar));

        font.As(&fontInfo.font);
        fontFace.As(&fontInfo.fontFace);

        auto key = d->create_font_key();
        d->fonts.emplace(pair{ key, move(fontInfo) });

        return key;
    }

    debuglog(FontFallbackTag).write("Font not found.");
    return nullopt;

#if 0
    IDWriteFontFallbackBuilder* ffb{};
    d->factory->CreateFontFallbackBuilder(&ffb);
    IDWriteFontFallback* ff;
    ffb->CreateFontFallback(&ff);

    IDWriteTextAnalyzer* textAnalyzer{};
    d->factory->CreateTextAnalyzer(&textAnalyzer);//?
    textAnalyzer->Release();

    IDWriteTextAnalysisSource *analysisSource;
    UINT32 textPosition;
    UINT32 textLength;
    IDWriteFontCollection *baseFontCollection;
    const wchar_t *baseFamilyName;
    DWRITE_FONT_WEIGHT baseWeight;
    DWRITE_FONT_STYLE baseStyle;
    DWRITE_FONT_STRETCH baseStretch;

    UINT32 mappedLength;
    IDWriteFont *mappedFont;
    FLOAT scale;
    ff->MapCharacters(analysisSource, textPosition, textLength, baseFontCollection,
                      baseFamilyName, baseWeight, baseStyle, baseStretch,
                      &mappedLength, &mappedFont, &scale);

    // DWRITE_FONT_FACE_TYPE fontFaceType = DWRITE_FONT_FACE_TYPE_UNKNOWN;
    // UINT32 numberOfFiles = 1;
    // IDWriteFontFile *const *fontFiles;
    // UINT32 faceIndex = 0;
    // DWRITE_FONT_SIMULATIONS fontFaceSimulationFlags;
    // IDWriteFontFace *fontFace{};
    // d->factory->CreateFontFace();

    // DWRITE_FONT_FACE_TYPE fontFaceType;
    // UINT32 numberOfFiles;
    // IDWriteFontFile *const *fontFiles;
    // UINT32 faceIndex;
    // DWRITE_FONT_SIMULATIONS fontFaceSimulationFlags;
    // IDWriteFontFace **fontFace = nullptr;
    // d->factory->CreateFontFace(fontFaceType, numberOfFiles, fontFiles, faceIndex, fontFaceSimulationFlags, fontFace);
    printf("done\n");
    return nullopt;
#endif
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
    FontInfo const& fontInfo = d->fonts.at(_font);
    IDWriteFontFace5* fontFace = fontInfo.fontFace.Get();

    std::vector<UINT16> _glyphIndices;
    std::vector<INT32> _glyphDesignUnitAdvances;

    BOOL isTextSimple = FALSE;
    UINT32 uiLengthRead = 0;
    const UINT32 glyphStart = 0;
    _glyphIndices.resize(textLength);

    d->textAnalyzer->GetTextComplexity(
        textString,
        textLength,
        fontFace,
        &isTextSimple,
        &uiLengthRead,
        &_glyphIndices.at(glyphStart));


    BOOL _isEntireTextSimple = isTextSimple && uiLengthRead == textLength;
    if (_isEntireTextSimple)
    {
        DWRITE_FONT_METRICS1 metrics;
        fontFace->GetMetrics(&metrics);

        _glyphDesignUnitAdvances.resize(textLength);
        USHORT designUnitsPerEm = metrics.designUnitsPerEm;
        fontFace->GetDesignGlyphAdvances(
            textLength,
            & _glyphIndices.at(glyphStart),
            & _glyphDesignUnitAdvances.at(glyphStart),
            false);

        for (size_t i = glyphStart; i < textLength; i++)
        {
            const auto cellWidth = static_cast<double>((float)_glyphDesignUnitAdvances.at(i)) / designUnitsPerEm * fontInfo.size.pt * (96.0 / 72.0)  * d->pixelPerDip();
            glyph_position gpos{};
            gpos.glyph = glyph_key{ _font, fontInfo.size, glyph_index{_glyphIndices.at(i)} };
            gpos.offset.x = static_cast<int>(cellWidth);
            //gpos.offset.y = static_cast<int>(static_cast<double>(pos[i].y_offset) / 64.0f);

            gpos.advance.x = static_cast<int>(cellWidth);
            //gpos.advance.y = static_cast<int>(static_cast<double>(pos[i].y_advance) / 64.0f);
            _result.emplace_back(gpos);
        }
    }
    else {
        // TODO
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
