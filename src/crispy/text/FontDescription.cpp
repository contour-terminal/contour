#include <crispy/text/FontDescription.h>

#if defined(_WIN32)
#include <Windows.h>
#include <dwrite.h>
#include <dwrite_1.h>
#else
#include <fontconfig/fontconfig.h>
#endif

#include <cassert>
#include <memory>
#include <optional>
#include <unordered_set>
#include <iostream>

using std::string;
using std::make_unique;
using std::optional;
using std::nullopt;
using std::unique_ptr;
using std::unordered_set;
using std::vector;

namespace crispy::text
{

#if defined(_WIN32)

#define HR(hr) \
  if (FAILED(hr)) throw "Font loading error";

WCHAR *utf8ToUtf16(const char *input)
{
    unsigned int len = MultiByteToWideChar(CP_UTF8, 0, input, -1, NULL, 0);
    WCHAR *output = new WCHAR[len];
    MultiByteToWideChar(CP_UTF8, 0, input, -1, output, len);
    return output;
}

char *utf16ToUtf8(const WCHAR *input)
{
    unsigned int len = WideCharToMultiByte(CP_UTF8, 0, input, -1, NULL, 0, NULL, NULL);
    char *output = new char[len];
    WideCharToMultiByte(CP_UTF8, 0, input, -1, output, len, NULL, NULL);
    return output;
}

int getLocaleIndex(IDWriteLocalizedStrings* strings)
{
    wchar_t localeName[LOCALE_NAME_MAX_LENGTH];

    // Get the default locale for this user.
    int success = GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH);
    if (!success)
        return 0;

    // If the default locale is returned, find that locale name, otherwise use "en-us".
    BOOL exists = false;
    uint32_t index = 0;
    strings->FindLocaleName(localeName, &index, &exists);

    // if the above find did not find a match, retry with US English
    if (!exists)
        strings->FindLocaleName(L"en-us", &index, &exists);

    if (!exists)
        index = 0;

    // assert(exists || index == 0);

    return int(index);
}

// gets a localized string for a font
string getLocalizedString(IDWriteFont* font, DWRITE_INFORMATIONAL_STRING_ID string_id)
{
    IDWriteLocalizedStrings *strings = NULL;
    BOOL exists = false;
    font->GetInformationalStrings(string_id, &strings, &exists);
    if (!exists)
        return {};

    unsigned int index = getLocaleIndex(strings);
    unsigned int len = 0;
    WCHAR *str = NULL;

    HR(strings->GetStringLength(index, &len));
    str = new WCHAR[len + 1];

    HR(strings->GetString(index, str, len + 1));

    // convert to utf8
    string output = utf16ToUtf8(str);
    delete str;
    strings->Release();
    return output;
}

FontDescription resultFromFont(IDWriteFont *font)
{
    FontDescription out{};

    IDWriteFontFace *face = NULL;
    unsigned int numFiles = 0;

    HR(font->CreateFontFace(&face));

    // get the font files from this font face
    IDWriteFontFile *files = NULL;
    HR(face->GetFiles(&numFiles, NULL));
    HR(face->GetFiles(&numFiles, &files));

    // return the first one
    if (numFiles > 0)
    {
        IDWriteFontFileLoader *loader = NULL;
        IDWriteLocalFontFileLoader *fileLoader = NULL;
        unsigned int nameLength = 0;
        const void *referenceKey = NULL;
        unsigned int referenceKeySize = 0;
        WCHAR *name = NULL;

        HR(files[0].GetLoader(&loader));

        // check if this is a local font file
        HRESULT hr = loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader), (void **)&fileLoader);
        if (SUCCEEDED(hr))
        {
            // get the file path
            HR(files[0].GetReferenceKey(&referenceKey, &referenceKeySize));
            HR(fileLoader->GetFilePathLengthFromKey(referenceKey, referenceKeySize, &nameLength));

            name = new WCHAR[nameLength + 1];
            HR(fileLoader->GetFilePathFromKey(referenceKey, referenceKeySize, name, nameLength + 1));

            string psName = utf16ToUtf8(name);
            string postscriptName = getLocalizedString(font, DWRITE_INFORMATIONAL_STRING_POSTSCRIPT_NAME);
            string family = getLocalizedString(font, DWRITE_INFORMATIONAL_STRING_WIN32_FAMILY_NAMES);
            string style = getLocalizedString(font, DWRITE_INFORMATIONAL_STRING_WIN32_SUBFAMILY_NAMES);

            // this method requires windows 7, so we need to cast to an IDWriteFontFace1
            bool monospace = false;
            IDWriteFontFace1 *face1;
            HRESULT hr = face->QueryInterface(__uuidof(IDWriteFontFace1), (void **)&face1);
            if (SUCCEEDED(hr))
                monospace = face1->IsMonospacedFont() == TRUE;

            switch (font->GetWeight())
            {
                case DWRITE_FONT_WEIGHT_EXTRA_BLACK:
                case DWRITE_FONT_WEIGHT_EXTRA_BOLD:
                case DWRITE_FONT_WEIGHT_BOLD:
                case DWRITE_FONT_WEIGHT_SEMI_BOLD:
                case DWRITE_FONT_WEIGHT_HEAVY:
                case DWRITE_FONT_WEIGHT_MEDIUM:
                    out.weight = FontWeight::Bold;
                    break;
                case DWRITE_FONT_WEIGHT_EXTRA_LIGHT:
                case DWRITE_FONT_WEIGHT_SEMI_LIGHT:
                case DWRITE_FONT_WEIGHT_LIGHT:
                case DWRITE_FONT_WEIGHT_NORMAL:
                case DWRITE_FONT_WEIGHT_THIN:
                    out.weight = FontWeight::Normal;
                    break;
            }

            switch (font->GetStyle())
            {
                case DWRITE_FONT_STYLE_NORMAL:
                    out.slant = FontSlant::Normal;
                    break;
                case DWRITE_FONT_STYLE_ITALIC:
                case DWRITE_FONT_STYLE_OBLIQUE:
                    out.slant = FontSlant::Italic;
                    break;
            }

            out.path = psName;
            out.postscriptName = postscriptName;
            out.familyName = family;
            out.styleName = style;
            out.monospace = monospace;
            out.width = font->GetStretch();

            fileLoader->Release();
        }

        loader->Release();
    }

    face->Release();
    files->Release();

    return out;
}

using ResultSet = vector<FontDescription>;
ResultSet getAvailableFonts()
{
    ResultSet res{};
    int count = 0;

    IDWriteFactory* factory = nullptr;
    HR(DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown **>(&factory)));

    // Get the system font collection.
    IDWriteFontCollection* collection = nullptr;
    HR(factory->GetSystemFontCollection(&collection));

    // Get the number of font families in the collection.
    int const familyCount = collection->GetFontFamilyCount();

    // track postscript names we've already added
    // using a set so we don't get any duplicates.
    unordered_set<string> psNames;

    for (int i = 0; i < familyCount; i++)
    {
        IDWriteFontFamily* family = nullptr;

        // Get the font family.
        HR(collection->GetFontFamily(i, &family));
        int const fontCount = family->GetFontCount();

        for (int j = 0; j < fontCount; j++)
        {
            IDWriteFont* font = nullptr;
            HR(family->GetFont(j, &font));

            FontDescription result = resultFromFont(font);
            if (psNames.count(result.postscriptName) == 0)
            {
                res.emplace_back(resultFromFont(font));
                psNames.insert(result.postscriptName);
            }
        }

        family->Release();
    }

    collection->Release();
    factory->Release();

    return res;
}

static bool equalsIgnoreCase(string const& _left, string const& _right)
{
    if (_left.size() != _right.size())
        return false;

    for (size_t i = 0; i < _left.size(); ++i)
        if (tolower(_left[i]) != tolower(_right[i]))
            return false;

    return true;
}

bool resultMatches(FontDescription const& result, FontDescription const& desc)
{
    return equalsIgnoreCase(desc.familyName, result.familyName)
        // && desc.styleName == result.styleName
        // && equalsIgnoreCase(desc.postscriptName, result.postscriptName)
        && desc.weight == result.weight
        && desc.slant == result.slant
        && desc.monospace == result.monospace;
}

std::ostream& operator<<(std::ostream& os, FontDescription const& f)
{
    os << f.familyName
        << " slant=" << int(f.slant)
        << " weight=" << int(f.weight)
        << " space=" << (f.monospace ? "mono" : "prop");
    return os;
}

ResultSet findFonts(FontDescription const& desc)
{
    using std::cout;
    cout << "Find fonts for: " << desc << '\n';
    ResultSet fonts = getAvailableFonts();

    for (auto it = fonts.begin(); it != fonts.end();)
    {
        auto const matches = resultMatches(*it, desc);
        if (matches)
        {
            cout << "match: " << *it << '\n';
            it++;
        }
        else
            it = fonts.erase(it);
    }

    return fonts;
}

optional<FontDescription> findFont(FontDescription const& desc)
{
    ResultSet fonts = findFonts(desc);

    // if we didn't find anything, try again with only the font traits, no string names
    if (fonts.size() == 0)
    {
        FontDescription fallback = FontDescription{
            "", "", "", "",
            desc.weight, desc.slant, false};

        fonts = findFonts(fallback);
    }

    // ok, nothing. shouldn't happen often.
    // just return the first available font
    if (fonts.size() == 0)
        fonts = getAvailableFonts();

    // hopefully we found something now.
    // copy and return the first result
    if (fonts.size() > 0)
        return {fonts.front()};

    // whoa, weird. no fonts installed or something went wrong.
    return nullopt;
}

// custom text renderer used to determine the fallback font for a given char
class FontFallbackRenderer : public IDWriteTextRenderer
{
public:
    IDWriteFontCollection* systemFonts;
    IDWriteFont* font;
    unsigned long refCount;

    explicit FontFallbackRenderer(IDWriteFontCollection *collection) :
        systemFonts{ collection },
        font{ nullptr },
        refCount{ 0 }
    {
        collection->AddRef();
    }

    ~FontFallbackRenderer()
    {
        if (systemFonts)
            systemFonts->Release();

        if (font)
            font->Release();
    }

    // IDWriteTextRenderer methods
    IFACEMETHOD(DrawGlyphRun)
    (
        void *clientDrawingContext,
        FLOAT baselineOriginX,
        FLOAT baselineOriginY,
        DWRITE_MEASURING_MODE measuringMode,
        DWRITE_GLYPH_RUN const *glyphRun,
        DWRITE_GLYPH_RUN_DESCRIPTION const *glyphRunDescription,
        IUnknown *clientDrawingEffect)
    {

        // save the font that was actually rendered
        return systemFonts->GetFontFromFontFace(glyphRun->fontFace, &font);
    }

    IFACEMETHOD(DrawUnderline)
    (
        void *clientDrawingContext,
        FLOAT baselineOriginX,
        FLOAT baselineOriginY,
        DWRITE_UNDERLINE const *underline,
        IUnknown *clientDrawingEffect)
    {
        return E_NOTIMPL;
    }

    IFACEMETHOD(DrawStrikethrough)
    (
        void *clientDrawingContext,
        FLOAT baselineOriginX,
        FLOAT baselineOriginY,
        DWRITE_STRIKETHROUGH const *strikethrough,
        IUnknown *clientDrawingEffect)
    {
        return E_NOTIMPL;
    }

    IFACEMETHOD(DrawInlineObject)
    (
        void *clientDrawingContext,
        FLOAT originX,
        FLOAT originY,
        IDWriteInlineObject *inlineObject,
        BOOL isSideways,
        BOOL isRightToLeft,
        IUnknown *clientDrawingEffect)
    {
        return E_NOTIMPL;
    }

    // IDWritePixelSnapping methods
    IFACEMETHOD(IsPixelSnappingDisabled)
    (void *clientDrawingContext, BOOL *isDisabled)
    {
        *isDisabled = FALSE;
        return S_OK;
    }

    IFACEMETHOD(GetCurrentTransform)
    (void *clientDrawingContext, DWRITE_MATRIX *transform)
    {
        const DWRITE_MATRIX ident = {1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
        *transform = ident;
        return S_OK;
    }

    IFACEMETHOD(GetPixelsPerDip)
    (void *clientDrawingContext, FLOAT *pixelsPerDip)
    {
        *pixelsPerDip = 1.0f;
        return S_OK;
    }

    // IUnknown methods
    IFACEMETHOD_(unsigned long, AddRef)
    ()
    {
        return InterlockedIncrement(&refCount);
    }

    IFACEMETHOD_(unsigned long, Release)
    ()
    {
        unsigned long newCount = InterlockedDecrement(&refCount);
        if (newCount == 0)
        {
            delete this;
            return 0;
        }

        return newCount;
    }

    IFACEMETHOD(QueryInterface)
    (IID const &riid, void **ppvObject)
    {
        if (__uuidof(IDWriteTextRenderer) == riid)
        {
            *ppvObject = this;
        }
        else if (__uuidof(IDWritePixelSnapping) == riid)
        {
            *ppvObject = this;
        }
        else if (__uuidof(IUnknown) == riid)
        {
            *ppvObject = this;
        }
        else
        {
            *ppvObject = nullptr;
            return E_FAIL;
        }

        this->AddRef();
        return S_OK;
    }
};

// @param string  some sample string used for the fallback renderer
FontDescription substituteFont(std::string const& postscriptName, std::string const& _string)
{
    FontDescription res;

    IDWriteFactory *factory = NULL;
    HR(DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown **>(&factory)));

    // Get the system font collection.
    IDWriteFontCollection *collection = NULL;
    HR(factory->GetSystemFontCollection(&collection));

    // find the font for the given postscript name
    FontDescription desc{};
    desc.postscriptName = postscriptName;
    optional<FontDescription> font = findFont(desc);

    // create a text format object for this font
    IDWriteTextFormat *format = nullptr;
    if (font.has_value())
    {
        WCHAR* familyName = utf8ToUtf16(font->familyName.c_str());

        // create a text format
        HR(factory->CreateTextFormat(
            familyName,
            collection,
            font.value().weight == FontWeight::Normal ? DWRITE_FONT_WEIGHT_NORMAL : DWRITE_FONT_WEIGHT_BOLD,
            font.value().slant == FontSlant::Normal ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
            (DWRITE_FONT_STRETCH) font.value().width,
            12.0,
            L"en-us",
            &format));

        delete familyName;
    }
    else
    {
        // this should never happen, but just in case, let the system
        // decide the default font in case findFont returned nothing.
        HR(factory->CreateTextFormat(
            L"",
            collection,
            DWRITE_FONT_WEIGHT_REGULAR,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            12.0,
            L"en-us",
            &format));
    }

    // convert utf8 string for substitution to utf16
    WCHAR *str = utf8ToUtf16(_string.c_str());

    // create a text layout for the substitution string
    IDWriteTextLayout *layout = NULL;
    HR(factory->CreateTextLayout(
        str,
        static_cast<UINT32>(wcslen(str)),
        format,
        100.0,
        100.0,
        &layout));

    // render it using a custom renderer that saves the physical font being used
    auto renderer = make_unique<FontFallbackRenderer>(collection);
    HR(layout->Draw(nullptr, renderer.get(), 100.0, 100.0));

    // if we found something, create a result object
    if (renderer->font)
        res = resultFromFont(renderer->font);

    // free all the things
    layout->Release();
    format->Release();

    delete str;
    collection->Release();
    factory->Release();

    return res;
}

vector<FontDescription> findFonts(FontPattern const& _pattern)
{
    FontDescription desc{};
    desc.familyName = _pattern.family;
    desc.slant = _pattern.slant;
    desc.weight = _pattern.weight;
    desc.monospace = _pattern.monospace;
    return findFonts(desc);
}
#endif

#if defined(__linux__) || defined(__APPLE__)
    // use Fontconfig
#endif

}
