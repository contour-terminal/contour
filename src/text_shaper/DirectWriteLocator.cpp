// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/DirectWriteLocator.hpp>

#include <text_shaper/DirectWriteAnalysisWrapper.hpp>
#include <text_shaper/Font.hpp>

#include <core/WideString.hpp>

#include <libunicode/convert.h>

#include <array>
#include <string>
#include <string_view>

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
    [[nodiscard]] DWRITE_FONT_WEIGHT dwFontWeight(FontWeight weight)
    {
        static constexpr auto weights = std::array {
            DWRITE_FONT_WEIGHT_THIN,       DWRITE_FONT_WEIGHT_EXTRA_LIGHT, DWRITE_FONT_WEIGHT_LIGHT,
            DWRITE_FONT_WEIGHT_SEMI_LIGHT, DWRITE_FONT_WEIGHT_NORMAL,      DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_WEIGHT_MEDIUM,     DWRITE_FONT_WEIGHT_SEMI_BOLD,   DWRITE_FONT_WEIGHT_BOLD,
            DWRITE_FONT_WEIGHT_EXTRA_BOLD, DWRITE_FONT_WEIGHT_BLACK,       DWRITE_FONT_WEIGHT_EXTRA_BLACK
        };
        return weights[static_cast<size_t>(weight)];
    }

    [[nodiscard]] DWRITE_FONT_STYLE dwFontSlant(FontSlant slant)
    {
        switch (slant)
        {
            case FontSlant::Normal: return DWRITE_FONT_STYLE_NORMAL;
            case FontSlant::Italic: return DWRITE_FONT_STYLE_ITALIC;
            case FontSlant::Oblique: return DWRITE_FONT_STYLE_OBLIQUE;
        }
        return DWRITE_FONT_STYLE_NORMAL;
    }
    [[nodiscard]] std::wstring getFontPath(IDWriteFontFace* fontFace)
    {
        ComPtr<IDWriteFontFile> fontFile;
        UINT32 numberOfFiles = 0;
        if (FAILED(fontFace->GetFiles(&numberOfFiles, nullptr)) || numberOfFiles != 1
            || FAILED(fontFace->GetFiles(&numberOfFiles, &fontFile)))
            return {};

        ComPtr<IDWriteFontFileLoader> loader;
        if (FAILED(fontFile->GetLoader(&loader)))
            return {};
        void const* key {};
        UINT32 keySize {};
        if (FAILED(fontFile->GetReferenceKey(&key, &keySize)))
            return {};
        ComPtr<IDWriteLocalFontFileLoader> localLoader;
        if (FAILED(loader.As(&localLoader)))
            return {};

        UINT32 pathLen {};
        if (FAILED(localLoader->GetFilePathLengthFromKey(key, keySize, &pathLen)))
            return {};
        std::wstring path;
        path.resize(pathLen + 1);
        if (FAILED(localLoader->GetFilePathFromKey(key, keySize, path.data(), pathLen + 1)))
            return {};
        path.resize(pathLen);

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

    // A name that is not UTF-8 names no family, and the fallback below is used.
    auto const familyName = core::toUtf16(fd.familyName).value_or(std::wstring {});

    // Resolve each requested family independently: a missing fallback must not silently turn into
    // Consolas, and a family without an exact weight/slant still has a nearest matching face.
    auto const addFamily = [&](std::wstring const& name) {
        UINT32 familyIndex {};
        BOOL familyExists = FALSE;
        if (FAILED(_d->systemFontCollection->FindFamilyName(name.c_str(), &familyIndex, &familyExists))
            || !familyExists)
            return;
        ComPtr<IDWriteFontFamily> family;
        if (FAILED(_d->systemFontCollection->GetFontFamily(familyIndex, &family)))
            return;
        ComPtr<IDWriteFont> font;
        if (FAILED(family->GetFirstMatchingFont(
                dwFontWeight(fd.weight), DWRITE_FONT_STRETCH_NORMAL, dwFontSlant(fd.slant), &font)))
            return;
        ComPtr<IDWriteFont1> font1;
        if (fd.strictSpacing && SUCCEEDED(font.As(&font1)) && fd.spacing == FontSpacing::Mono
            && !font1->IsMonospacedFont())
            return;
        ComPtr<IDWriteFontFace> face;
        if (FAILED(font->CreateFontFace(&face)))
            return;
        auto path = core::toUtf8(getFontPath(face.Get()));
        if (path.empty())
            return;
        auto const faceIndex = static_cast<int>(face->GetIndex());
        if (std::ranges::any_of(output, [&](FontSource const& source) {
                auto const& existing = std::get<FontPath>(source);
                return existing.value == path && existing.collectionIndex == faceIndex;
            }))
            return;
        output.emplace_back(FontPath { .value = std::move(path), .collectionIndex = faceIndex });
        locatorLog()("Adding font file: {}", output.back());
    };

    addFamily(familyName);
    if (output.empty())
        addFamily(L"Consolas");
    if (auto const* list = std::get_if<FontFallbackList>(&fd.fontFallback))
        for (auto const& fallback: list->fallbackFonts)
            if (auto const name = core::toUtf16(fallback)) // A name that is not UTF-8 names no family.
                addFamily(*name);

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

    auto const wText =
        unicode::convert_to<wchar_t>(std::u32string_view { codepoints.data(), codepoints.size() });

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
        if (FAILED(mappedFont->CreateFontFace(&fontFace)))
            return output;
        auto path = core::toUtf8(getFontPath(fontFace.Get()));
        if (!path.empty())
            output.emplace_back(FontPath { .value = std::move(path),
                                           .collectionIndex = static_cast<int>(fontFace->GetIndex()) });
    }

    return output;
}
} // namespace text
