/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2021 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <text_shaper/font.h>
#include <text_shaper/fontconfig_locator.h>

#include <range/v3/view/iota.hpp>

#include <fontconfig/fontconfig.h>

#include <string_view>

using std::nullopt;
using std::optional;
using std::string;
using std::string_view;
using std::unique_ptr;
using std::vector;

using namespace std::string_view_literals;

namespace text
{

namespace
{

    constexpr string_view fcSpacingStr(int _value) noexcept
    {
        using namespace std::string_view_literals;
        switch (_value)
        {
        case FC_PROPORTIONAL: return "proportional"sv;
        case FC_DUAL: return "dual"sv;
        case FC_MONO: return "mono"sv;
        case FC_CHARCELL: return "charcell"sv;
        default: return "INVALID"sv;
        }
    }

    constexpr int fcWeight(font_weight _weight) noexcept
    {
        switch (_weight)
        {
        case font_weight::thin: return FC_WEIGHT_THIN;
        case font_weight::extra_light: return FC_WEIGHT_EXTRALIGHT;
        case font_weight::light: return FC_WEIGHT_LIGHT;
        case font_weight::demilight:
#if defined(FC_WEIGHT_DEMILIGHT)
            return FC_WEIGHT_DEMILIGHT;
#else
            return FC_WEIGHT_LIGHT; // Is this a good fallback? Maybe.
#endif
        case font_weight::book: return FC_WEIGHT_BOOK;
        case font_weight::normal: return FC_WEIGHT_NORMAL;
        case font_weight::medium: return FC_WEIGHT_MEDIUM;
        case font_weight::demibold: return FC_WEIGHT_DEMIBOLD;
        case font_weight::bold: return FC_WEIGHT_BOLD;
        case font_weight::extra_bold: return FC_WEIGHT_EXTRABOLD;
        case font_weight::black: return FC_WEIGHT_BLACK;
        case font_weight::extra_black: return FC_WEIGHT_EXTRABLACK;
        }
        return FC_WEIGHT_NORMAL;
    }

    constexpr int fcSlant(font_slant _slant) noexcept
    {
        switch (_slant)
        {
        case font_slant::italic: return FC_SLANT_ITALIC;
        case font_slant::oblique: return FC_SLANT_OBLIQUE;
        case font_slant::normal: return FC_SLANT_ROMAN;
        }
        return FC_SLANT_ROMAN;
    }

    char const* fcWeightStr(int _value)
    {
        switch (_value)
        {
        case FC_WEIGHT_THIN: return "Thin";
        case FC_WEIGHT_EXTRALIGHT: return "ExtraLight";
        case FC_WEIGHT_LIGHT: return "Light";
#if defined(FC_WEIGHT_DEMILIGHT)
        case FC_WEIGHT_DEMILIGHT: return "DemiLight";
#endif
        case FC_WEIGHT_BOOK: return "Book";
        case FC_WEIGHT_REGULAR: return "Regular";
        case FC_WEIGHT_MEDIUM: return "Medium";
        case FC_WEIGHT_DEMIBOLD: return "DemiBold";
        case FC_WEIGHT_BOLD: return "Bold";
        case FC_WEIGHT_EXTRABOLD: return "ExtraBold";
        case FC_WEIGHT_BLACK: return "Black";
        case FC_WEIGHT_EXTRABLACK: return "ExtraBlack";
        default: return "?";
        }
    }

    char const* fcSlantStr(int _value)
    {
        switch (_value)
        {
        case FC_SLANT_ROMAN: return "Roman";
        case FC_SLANT_ITALIC: return "Italic";
        case FC_SLANT_OBLIQUE: return "Oblique";
        default: return "?";
        }
    }

} // namespace

struct fontconfig_locator::Private
{
    // currently empty, maybe later something (such as caching)?
    FcConfig* ftConfig = nullptr;

    Private()
    {
        FcInit();
        ftConfig = FcInitLoadConfigAndFonts(); // Most convenient of all the alternatives
    }

    ~Private()
    {
        LOGSTORE(LocatorLog)("~fontconfig_locator.dtor");
        FcConfigDestroy(ftConfig);
        FcFini();
    }
};

fontconfig_locator::fontconfig_locator():
    d { new Private(), [](Private* p) {
           delete p;
       } }
{
}

fontconfig_locator::~fontconfig_locator()
{
}

font_source_list fontconfig_locator::locate(font_description const& _fd)
{
    LOGSTORE(LocatorLog)("Locating font chain for: {}", _fd);
    auto pat =
        unique_ptr<FcPattern, void (*)(FcPattern*)>(FcPatternCreate(), [](auto p) { FcPatternDestroy(p); });

    FcPatternAddBool(pat.get(), FC_OUTLINE, true);
    FcPatternAddBool(pat.get(), FC_SCALABLE, true);
    // FcPatternAddBool(pat.get(), FC_EMBEDDED_BITMAP, false);

    // XXX It should be recommended to turn that on if you are looking for colored fonts,
    //     such as for emoji, but it seems like fontconfig doesn't care, it works either way.
    //
    // bool const _color = true;
    // FcPatternAddBool(pat.get(), FC_COLOR, _color);

    if (!_fd.familyName.empty())
        FcPatternAddString(pat.get(), FC_FAMILY, (FcChar8 const*) _fd.familyName.c_str());

    if (_fd.spacing != font_spacing::proportional)
    {
#if defined(_WIN32)
        // On Windows FontConfig can't find "monospace". We need to use "Consolas" instead.
        if (_fd.familyName == "monospace")
            FcPatternAddString(pat.get(), FC_FAMILY, (FcChar8 const*) "Consolas");
#elif defined(__APPLE__)
        // Same for macOS, we use "Menlo" for "monospace".
        if (_fd.familyName == "monospace")
            FcPatternAddString(pat.get(), FC_FAMILY, (FcChar8 const*) "Menlo");
#else
        if (_fd.familyName != "monospace")
            FcPatternAddString(pat.get(), FC_FAMILY, (FcChar8 const*) "monospace");
#endif
        FcPatternAddInteger(pat.get(), FC_SPACING, FC_MONO);
        FcPatternAddInteger(pat.get(), FC_SPACING, FC_DUAL);
    }

    if (_fd.weight != font_weight::normal)
        FcPatternAddInteger(pat.get(), FC_WEIGHT, fcWeight(_fd.weight));
    if (_fd.slant != font_slant::normal)
        FcPatternAddInteger(pat.get(), FC_SLANT, fcSlant(_fd.slant));

    FcConfigSubstitute(nullptr, pat.get(), FcMatchPattern);
    FcDefaultSubstitute(pat.get());

    FcResult result = FcResultNoMatch;
    auto fs = unique_ptr<FcFontSet, void (*)(FcFontSet*)>(
        FcFontSort(nullptr, pat.get(), /*unicode-trim*/ FcTrue, /*FcCharSet***/ nullptr, &result),
        [](auto p) { FcFontSetDestroy(p); });

    if (!fs || result != FcResultMatch)
        return {};

    font_source_list output;

    auto const addFontFile = [&](std::string_view path) {
        output.emplace_back(font_path { string { path } });
    };

    for (int i = 0; i < fs->nfont; ++i)
    {
        FcPattern* font = fs->fonts[i];

        FcChar8* file;
        if (FcPatternGetString(font, FC_FILE, 0, &file) != FcResultMatch)
            continue;

#if defined(FC_COLOR) // Not available on OS/X?
// FcBool color = FcFalse;
// FcPatternGetInteger(font, FC_COLOR, 0, &color);
// if (color && !_color)
// {
//     LOGSTORE(LocatorLog)("Skipping font (contains color). {}", (char const*) file);
//     continue;
// }
#endif

        int spacing = -1; // ignore font if we cannot retrieve spacing information
        FcPatternGetInteger(font, FC_SPACING, 0, &spacing);
        if (_fd.strict_spacing)
        {
            if ((_fd.spacing == font_spacing::proportional && spacing < FC_PROPORTIONAL)
                || (_fd.spacing == font_spacing::mono && spacing < FC_MONO))
            {
                LOGSTORE(LocatorLog)
                ("Skipping font: {} ({} < {}).",
                 (char const*) (file),
                 fcSpacingStr(spacing),
                 fcSpacingStr(FC_DUAL));
                continue;
            }
        }

        output.emplace_back(font_path { string { (char const*) (file) } });
        // LOGSTORE(LocatorLog)("Found font: {}", (char const*) file);
    }

#if defined(_WIN32)
    #define FONTDIR "C:\\Windows\\Fonts\\"
    if (_fd.familyName == "emoji")
    {
        addFontFile(FONTDIR "seguiemj.ttf");
        addFontFile(FONTDIR "seguisym.ttf");
    }
    else if (_fd.weight != font_weight::normal && _fd.slant != font_slant::normal)
    {
        addFontFile(FONTDIR "consolaz.ttf");
        addFontFile(FONTDIR "seguisbi.ttf");
    }
    else if (_fd.weight != font_weight::normal)
    {
        addFontFile(FONTDIR "consolab.ttf");
        addFontFile(FONTDIR "seguisb.ttf");
    }
    else if (_fd.slant != font_slant::normal)
    {
        addFontFile(FONTDIR "consolai.ttf");
        addFontFile(FONTDIR "seguisli.ttf");
    }
    else
    {
        addFontFile(FONTDIR "consola.ttf");
        addFontFile(FONTDIR "seguisym.ttf");
    }

    #undef FONTDIR
#endif

    return output;
}

font_source_list fontconfig_locator::all()
{
    FcPattern* pat = FcPatternCreate();
    FcObjectSet* os = FcObjectSetBuild(
#if defined(FC_COLOR)
        FC_COLOR,
#endif
        FC_FAMILY,
        FC_FILE,
        FC_FULLNAME,
        FC_HINTING,
        FC_HINT_STYLE,
        FC_INDEX,
        FC_OUTLINE,
#if defined(FC_POSTSCRIPT_NAME)
        FC_POSTSCRIPT_NAME,
#endif
        FC_SCALABLE,
        FC_SLANT,
        FC_SPACING,
        FC_STYLE,
        FC_WEIGHT,
        FC_WIDTH,
        NULL);
    FcFontSet* fs = FcFontList(nullptr, pat, os);

    font_source_list output;

    for (auto i = 0; i < fs->nfont; ++i)
    {
        FcPattern* font = fs->fonts[i];

        FcChar8* filename = nullptr;
        FcPatternGetString(font, FC_FILE, 0, &filename);

        FcChar8* family = nullptr;
        FcPatternGetString(font, FC_FAMILY, 0, &family);

        int weight = -1;
        FcPatternGetInteger(font, FC_WEIGHT, 0, &weight);

        int slant = -1;
        FcPatternGetInteger(font, FC_SLANT, 0, &slant);

        int spacing = -1; // ignore font if we cannot retrieve spacing information
        FcPatternGetInteger(font, FC_SPACING, 0, &spacing);

        if (spacing < FC_DUAL)
            continue;

        LOGSTORE(LocatorLog)("font({}, {}, {})", fcWeightStr(weight), fcSlantStr(slant), (char*) family);
        output.emplace_back(font_path { (char const*) filename });
    }

    FcObjectSetDestroy(os);
    FcFontSetDestroy(fs);
    FcPatternDestroy(pat);

    return output;
}

font_source_list fontconfig_locator::resolve(gsl::span<const char32_t> codepoints)
{
    // that's also possible via FC, not sure yet we will/want-to need that.
    return {}; // TODO
}

} // namespace text
