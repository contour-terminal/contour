// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/directwrite_analysis_wrapper.h>
#include <text_shaper/directwrite_locator.h>
#include <text_shaper/font.h>

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
    font_weight dwFontWeight(int _weight)
    {
        switch (_weight)
        {
            case DWRITE_FONT_WEIGHT_THIN: return font_weight::thin;
            case DWRITE_FONT_WEIGHT_EXTRA_LIGHT: return font_weight::extra_light;
            case DWRITE_FONT_WEIGHT_LIGHT: return font_weight::light;
            case DWRITE_FONT_WEIGHT_SEMI_LIGHT: return font_weight::demilight;
            case DWRITE_FONT_WEIGHT_REGULAR:
                return font_weight::normal;
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

    std::wstring get_font_path(IDWriteFontFace* fontFace)
    {
        ComPtr<IDWriteFontFile> fontFile;
        UINT32 numberOfFiles = 0;
        fontFace->GetFiles(&numberOfFiles, nullptr);
        fontFace->GetFiles(&numberOfFiles, &fontFile);

        ComPtr<IDWriteFontFileLoader> loader;
        fontFile->GetLoader(&loader);
        const void* key;
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

struct directwrite_locator::Private
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

directwrite_locator::directwrite_locator(): _d { new Private(), [](Private* p) { delete p; } }
{
}

font_source_list directwrite_locator::locate(font_description const& _fd)
{
    locatorLog()("Locating font chain for: {}", _fd);

    font_source_list output;

    // TODO: use libunicode for that (TODO: create wchar_t/char16_t converters in libunicode)
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> wStringConverter;
    std::wstring familyName = wStringConverter.from_bytes(_fd.familyName);

    UINT32 familyIndex;
    BOOL familyExists = FALSE;
    _d->systemFontCollection->FindFamilyName(familyName.data(), &familyIndex, &familyExists);

    if (!familyExists)
    {
        // Fallback to Consolas
        const wchar_t* consolas = L"Consolas";
        _d->systemFontCollection->FindFamilyName(consolas, &familyIndex, &familyExists);
    }

    ComPtr<IDWriteFontFamily> fontFamily;
    _d->systemFontCollection->GetFontFamily(familyIndex, &fontFamily);

    for (UINT32 k = 0, ke = fontFamily->GetFontCount(); k < ke; ++k)
    {
        ComPtr<IDWriteFont> font;
        fontFamily->GetFont(k, font.GetAddressOf());

        font_weight weight = dwFontWeight(font->GetWeight());
        if (weight != _fd.weight)
            continue;

        font_slant slant = dwFontSlant(font->GetStyle());
        if (slant != _fd.slant)
            continue;

        ComPtr<IDWriteFontFace> fontFace;
        font->CreateFontFace(&fontFace);
        output.emplace_back(font_path { wStringConverter.to_bytes(get_font_path(fontFace.Get())) });
        locatorLog()("Adding font file: {}", output.back());
    }

    return output;
}

font_source_list directwrite_locator::all()
{
    // TODO;
    return {};
}

font_source_list directwrite_locator::resolve(gsl::span<const char32_t> codepoints)
{
    font_source_list output;

    std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> conv1;
    std::string bytes = conv1.to_bytes(std::u32string { codepoints.data(), codepoints.size() });
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv2;
    std::wstring wText = conv2.from_bytes(bytes);

    const UINT32 textLength = wText.size();
    UINT32 mappedLength = 0;
    ComPtr<IDWriteFont> mappedFont;
    FLOAT scale = 0.0f;

    dwrite_analysis_wrapper analysisWrapper(wText, _d->userLocale);

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

        output.emplace_back(font_path { wStringConverter.to_bytes(get_font_path(fontFace.Get())) });
    }

    return output;
}
} // namespace text
