// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/DirectWriteLocator.hpp>

#include <text_shaper/DirectWriteAnalysisWrapper.hpp>
#include <text_shaper/Font.hpp>

#include <string_view>

// {{{ TODO: replace with libunicode
#include <codecvt>
#include <locale>
// }}}

#include <dwrite.h>
#include <dwrite_3.h>

#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

using std::nullopt;
using std::optional;
using std::string;
using std::string_view;
using std::unique_ptr;
using std::vector;

using namespace std::string_view_literals;

namespace text
{
namespace // {{{ support
{
    FontWeight dwFontWeight(int weight)
    {
        switch (weight)
        {
            case DWRITE_FONT_WEIGHT_THIN: return FontWeight::Thin;
            case DWRITE_FONT_WEIGHT_EXTRA_LIGHT: return FontWeight::ExtraLight;
            case DWRITE_FONT_WEIGHT_LIGHT: return FontWeight::Light;
            case DWRITE_FONT_WEIGHT_SEMI_LIGHT: return FontWeight::DemiLight;
            case DWRITE_FONT_WEIGHT_REGULAR:
                return FontWeight::Normal;
                // XXX What about FontWeight::Book (which does exist via fontconfig)?
            case DWRITE_FONT_WEIGHT_MEDIUM: return FontWeight::Medium;
            case DWRITE_FONT_WEIGHT_DEMI_BOLD: return FontWeight::DemiBold;
            case DWRITE_FONT_WEIGHT_BOLD: return FontWeight::Bold;
            case DWRITE_FONT_WEIGHT_EXTRA_BOLD: return FontWeight::ExtraBold;
            case DWRITE_FONT_WEIGHT_BLACK: return FontWeight::Black;
            case DWRITE_FONT_WEIGHT_EXTRA_BLACK: return FontWeight::ExtraBlack;
            default: // TODO: the others
                break;
        }
        return FontWeight::Normal; // TODO: rename normal to regular
    }

    FontSlant dwFontSlant(int style)
    {
        switch (style)
        {
            case DWRITE_FONT_STYLE_NORMAL: return FontSlant::Normal;
            case DWRITE_FONT_STYLE_ITALIC: return FontSlant::Italic;
            case DWRITE_FONT_STYLE_OBLIQUE: return FontSlant::Oblique;
        }
        return FontSlant::Normal;
    }

    std::wstring getFontPath(IDWriteFontFace* fontFace)
    {
        ComPtr<IDWriteFontFile> fontFile;
        UINT32 numberOfFiles = 0;
        fontFace->GetFiles(&numberOfFiles, nullptr);
        fontFace->GetFiles(&numberOfFiles, &fontFile);

        ComPtr<IDWriteFontFileLoader> loader;
        fontFile->GetLoader(&loader);
        void const* key;
        UINT32 keySize;
        fontFile->GetReferenceKey(&key, &keySize);
        ComPtr<IDWriteLocalFontFileLoader> localLoader;
        loader.As(&localLoader);

        UINT32 pathLen;
        localLoader->GetFilePathLengthFromKey(key, keySize, &pathLen);
        std::wstring path;
        path.resize(pathLen);
        localLoader->GetFilePathFromKey(key, keySize, path.data(), pathLen + 1);

        return path;
    }
} // namespace

struct DirectWriteLocator::Private
{
    ComPtr<IDWriteFactory7> factory;
    ComPtr<IDWriteFontCollection> systemFontCollection;
    ComPtr<IDWriteFontFallback> systemFontFallback;

    std::wstring userLocale;

    Private()
    {
        auto hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                      __uuidof(IDWriteFactory7),
                                      reinterpret_cast<IUnknown**>(factory.GetAddressOf()));

        wchar_t locale[LOCALE_NAME_MAX_LENGTH];
        GetUserDefaultLocaleName(locale, sizeof(locale));
        userLocale = locale;

        factory->GetSystemFontCollection(&systemFontCollection);
        factory->GetSystemFontFallback(&systemFontFallback);
    }
};

DirectWriteLocator::DirectWriteLocator(): _d { new Private(), [](Private* p) { delete p; } }
{
}

FontSourceList DirectWriteLocator::locate(FontDescription const& fd)
{
    locatorLog()("Locating font chain for: {}", fd);

    FontSourceList output;

    // TODO: use libunicode for that (TODO: create wchar_t/char16_t converters in libunicode)
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> wStringConverter;
    std::wstring familyName = wStringConverter.from_bytes(fd.familyName);

    UINT32 familyIndex;
    BOOL familyExists = FALSE;
    _d->systemFontCollection->FindFamilyName(familyName.data(), &familyIndex, &familyExists);

    if (!familyExists)
    {
        // Fallback to Consolas
        wchar_t const* consolas = L"Consolas";
        _d->systemFontCollection->FindFamilyName(consolas, &familyIndex, &familyExists);
    }

    ComPtr<IDWriteFontFamily> fontFamily;
    _d->systemFontCollection->GetFontFamily(familyIndex, &fontFamily);

    for (UINT32 k = 0, ke = fontFamily->GetFontCount(); k < ke; ++k)
    {
        ComPtr<IDWriteFont> font;
        fontFamily->GetFont(k, font.GetAddressOf());

        FontWeight weight = dwFontWeight(font->GetWeight());
        if (weight != fd.weight)
            continue;

        FontSlant slant = dwFontSlant(font->GetStyle());
        if (slant != fd.slant)
            continue;

        ComPtr<IDWriteFontFace> fontFace;
        font->CreateFontFace(&fontFace);
        output.emplace_back(FontPath { wStringConverter.to_bytes(getFontPath(fontFace.Get())) });
        locatorLog()("Adding font file: {}", output.back());
    }

    return output;
}

FontSourceList DirectWriteLocator::all()
{
    // TODO;
    return {};
}

FontSourceList DirectWriteLocator::resolve(gsl::span<char32_t const> codepoints)
{
    FontSourceList output;

    std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> conv1;
    std::string bytes = conv1.to_bytes(std::u32string { codepoints.data(), codepoints.size() });
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv2;
    std::wstring wText = conv2.from_bytes(bytes);

    const UINT32 textLength = wText.size();
    UINT32 mappedLength = 0;
    ComPtr<IDWriteFont> mappedFont;
    FLOAT scale = 0.0f;

    DWriteAnalysisWrapper analysisWrapper(wText, _d->userLocale);

    _d->systemFontFallback->MapCharacters(&analysisWrapper,
                                          0,
                                          textLength,
                                          _d->systemFontCollection.Get(),
                                          nullptr,
                                          DWRITE_FONT_WEIGHT_NORMAL,
                                          DWRITE_FONT_STYLE_NORMAL,
                                          DWRITE_FONT_STRETCH_NORMAL,
                                          &mappedLength,
                                          &mappedFont,
                                          &scale);

    if (mappedFont)
    {
        ComPtr<IDWriteFontFace> fontFace;
        mappedFont->CreateFontFace(&fontFace);

        // TODO: use libunicode for that (TODO: create wchar_t/char16_t converters in libunicode)
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> wStringConverter;

        output.emplace_back(FontPath { wStringConverter.to_bytes(getFontPath(fontFace.Get())) });
    }

    return output;
}
} // namespace text
