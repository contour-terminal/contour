// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/font.h>
#include <text_shaper/fontconfig_locator.h>

#include <crispy/assert.h>
#include <crispy/utils.h>

#include <range/v3/view/drop.hpp>
#include <range/v3/view/iota.hpp>

#include <fontconfig/fontconfig.h>

#include <string_view>
#include <variant>

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

    string fcSpacingStr(int value)
    {
        switch (value)
        {
            case FC_PROPORTIONAL: return "proportional";
            case FC_DUAL: return "dual";
            case FC_MONO: return "mono";
            case FC_CHARCELL: return "charcell";
            default: return std::format("({})", value);
        }
    }

    auto constexpr FontWeightMappings = std::array<std::pair<font_weight, int>, 12> { {
        { font_weight::thin, FC_WEIGHT_THIN },
        { font_weight::extra_light, FC_WEIGHT_EXTRALIGHT },
        { font_weight::light, FC_WEIGHT_LIGHT },
        { font_weight::demilight, FC_WEIGHT_DEMILIGHT },
        { font_weight::book, FC_WEIGHT_BOOK },
        { font_weight::normal, FC_WEIGHT_NORMAL },
        { font_weight::medium, FC_WEIGHT_MEDIUM },
        { font_weight::demibold, FC_WEIGHT_DEMIBOLD },
        { font_weight::bold, FC_WEIGHT_BOLD },
        { font_weight::extra_bold, FC_WEIGHT_EXTRABOLD },
        { font_weight::black, FC_WEIGHT_BLACK },
        { font_weight::extra_black, FC_WEIGHT_EXTRABLACK },
    } };

    // clang-format off
    auto constexpr FontSlantMappings = std::array<std::pair<font_slant, int>, 3>{ {
        { font_slant::italic, FC_SLANT_ITALIC },
        { font_slant::oblique, FC_SLANT_OBLIQUE },
        { font_slant::normal, FC_SLANT_ROMAN }
    } };
    // clang-format on

    constexpr optional<font_weight> fcToFontWeight(int value) noexcept
    {
        for (auto const& mapping: FontWeightMappings)
            if (mapping.second == value)
                return mapping.first;
        return nullopt;
    }

    constexpr optional<font_slant> fcToFontSlant(int value) noexcept
    {
        for (auto const& mapping: FontSlantMappings)
            if (mapping.second == value)
                return mapping.first;
        return nullopt;
    }

    int fcWeight(font_weight weight) noexcept
    {
        for (auto const& mapping: FontWeightMappings)
            if (mapping.first == weight)
                return mapping.second;
        crispy::fatal("Implementation error. font weight cannot be mapped.");
    }

    constexpr int fcSlant(font_slant slant) noexcept
    {
        for (auto const& mapping: FontSlantMappings)
            if (mapping.first == slant)
                return mapping.second;
        return FC_SLANT_ROMAN;
    }

    char const* fcWeightStr(int value)
    {
        switch (value)
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

    char const* fcSlantStr(int value)
    {
        switch (value)
        {
            case FC_SLANT_ROMAN: return "Roman";
            case FC_SLANT_ITALIC: return "Italic";
            case FC_SLANT_OBLIQUE: return "Oblique";
            default: return "?";
        }
    }

} // namespace

struct fontconfig_locator::private_tag
{
    // currently empty, maybe later something (such as caching)?
    FcConfig* ftConfig = nullptr;

    private_tag()
    {
        FcInit();
        ftConfig = FcInitLoadConfigAndFonts(); // Most convenient of all the alternatives
    }

    ~private_tag()
    {
        locatorLog()("~fontconfig_locator.dtor");
        FcConfigDestroy(ftConfig);
        FcFini();
    }
};

fontconfig_locator::fontconfig_locator():
    _d { new private_tag(), [](private_tag* p) {
            delete p;
        } }
{
}

font_source_list fontconfig_locator::locate(font_description const& description)
{
    locatorLog()("Locating font chain for: {}", description);
    auto pat =
        unique_ptr<FcPattern, void (*)(FcPattern*)>(FcPatternCreate(), [](auto p) { FcPatternDestroy(p); });

    FcPatternAddBool(pat.get(), FC_OUTLINE, true);
    FcPatternAddBool(pat.get(), FC_SCALABLE, true);
    // FcPatternAddBool(pat.get(), FC_EMBEDDED_BITMAP, false);

    // XXX It should be recommended to turn that on if you are looking for colored fonts,
    //     such as for emoji, but it seems like fontconfig doesn't care, it works either way.
    //
    // bool const color = true;
    // FcPatternAddBool(pat.get(), FC_COLOR, color);

    if (!description.familyName.empty())
        FcPatternAddString(pat.get(), FC_FAMILY, (FcChar8 const*) description.familyName.c_str());

    if (description.spacing != font_spacing::proportional)
    {
#if defined(_WIN32)
        // On Windows FontConfig can't find "monospace". We need to use "Consolas" instead.
        if (description.familyName == "monospace")
            FcPatternAddString(pat.get(), FC_FAMILY, (FcChar8 const*) "Consolas");
#elif defined(__APPLE__)
        // Same for macOS, we use "Menlo" for "monospace".
        if (description.familyName == "monospace")
            FcPatternAddString(pat.get(), FC_FAMILY, (FcChar8 const*) "Menlo");
#else
        if (description.familyName != "monospace")
            FcPatternAddString(pat.get(), FC_FAMILY, (FcChar8 const*) "monospace");
#endif
        FcPatternAddInteger(pat.get(), FC_SPACING, FC_MONO);
        FcPatternAddInteger(pat.get(), FC_SPACING, FC_DUAL);
    }

    if (description.weight != font_weight::normal)
        FcPatternAddInteger(pat.get(), FC_WEIGHT, fcWeight(description.weight));
    if (description.slant != font_slant::normal)
        FcPatternAddInteger(pat.get(), FC_SLANT, fcSlant(description.slant));

    FcConfigSubstitute(_d->ftConfig, pat.get(), FcMatchPattern);
    FcDefaultSubstitute(pat.get());

    FcResult result = FcResultNoMatch;
    auto fs = unique_ptr<FcFontSet, void (*)(FcFontSet*)>(
        FcFontSort(_d->ftConfig, pat.get(), /*unicode-trim*/ FcTrue, /*FcCharSet***/ nullptr, &result),
        [](auto p) { FcFontSetDestroy(p); });

    if (!fs || result != FcResultMatch)
        return {};

    font_source_list output;

#if defined(_WIN32)
    auto const addFontFile = [&](std::string_view path) {
        output.emplace_back(font_path { string { path } });
    };
#endif

    auto addFont = [&](auto const& font) {
        FcChar8* file = nullptr;
        if (FcPatternGetString(font, FC_FILE, 0, &file) != FcResultMatch)
            return;

        int spacing = -1;
        FcPatternGetInteger(font, FC_SPACING, 0, &spacing);
        if (description.strictSpacing)
        {
            // Some fonts don't seem to tell us their spacing attribute. ;-(
            // But instead of ignoring them all together, try to be more friendly.
            if (spacing != -1
                && ((description.spacing == font_spacing::proportional && spacing < FC_PROPORTIONAL)
                    || (description.spacing == font_spacing::mono && spacing < FC_MONO)))
            {
                locatorLog()("Skipping font: {} ({} < {}).",
                             (char const*) (file),
                             fcSpacingStr(spacing),
                             fcSpacingStr(FC_DUAL));
                return;
            }
        }

        int integerValue = -1;
        optional<font_weight> weight = nullopt;
        optional<font_slant> slant = nullopt;
        int ttcIndex = -1;

        if (FcPatternGetInteger(font, FC_INDEX, 0, &integerValue) == FcResultMatch && integerValue >= 0)
            ttcIndex = integerValue;
        if (FcPatternGetInteger(font, FC_WEIGHT, 0, &integerValue) == FcResultMatch)
            weight = fcToFontWeight(integerValue);
        if (FcPatternGetInteger(font, FC_SLANT, 0, &integerValue) == FcResultMatch)
            slant = fcToFontSlant(integerValue);

        output.emplace_back(font_path { .value = string { (char const*) (file) },
                                        .collectionIndex = ttcIndex,
                                        .weight = weight,
                                        .slant = slant });
        locatorLog()("Font {} (ttc index {}, weight {}, slant {}, spacing {}) in chain: {}",
                     output.size(),
                     ttcIndex,
                     weight.has_value() ? std::format("{}", *weight) : "NONE",
                     slant.has_value() ? std::format("{}", *slant) : "NONE",
                     spacing,
                     (char const*) file);
    };

    // First font is the primary font that is best matching for description.family, we always
    // include that one.on.
    addFont(fs->fonts[0]);

    std::visit(crispy::overloaded {
                   [](font_fallback_none) {},
                   [&](font_fallback_list const& list) {
                       // find font in the fallback list and add it
                       for (auto&& fallbackFont: list.fallbackFonts)
                       {
                           for (auto i: ranges::views::ints(1, fs->nfont))
                           {
                               FcPattern* font = fs->fonts[i];

                               FcChar8* family = nullptr;
                               FcPatternGetString(font, FC_FAMILY, 0, &family);

                               // remove spaces from the fonts names
                               auto fallbackFontNoSpaces = fallbackFont;
                               // NOLINTBEGIN
                               fallbackFontNoSpaces.erase(
                                   std::remove(fallbackFontNoSpaces.begin(), fallbackFontNoSpaces.end(), ' '),
                                   fallbackFontNoSpaces.end());
                               std::string familyNoSpaces = (char const*) family;
                               familyNoSpaces.erase(
                                   std::remove(familyNoSpaces.begin(), familyNoSpaces.end(), ' '),
                                   familyNoSpaces.end());
                               // NOLINTEND
                               if (fallbackFontNoSpaces == familyNoSpaces)
                               {
                                   addFont(font);
                                   break;
                               }
                           }
                       }
                   },
                   [&](std::monostate) {
                       for (auto i: ranges::views::ints(1, fs->nfont))
                           addFont(fs->fonts[i]);
                   },
               },
               description.fontFallback);

#if defined(_WIN32)
    #define FONTDIR "C:\\Windows\\Fonts\\"
    if (description.familyName == "emoji")
    {
        addFontFile(FONTDIR "seguiemj.ttf");
        addFontFile(FONTDIR "seguisym.ttf");
    }
    else if (description.weight != font_weight::normal && description.slant != font_slant::normal)
    {
        addFontFile(FONTDIR "consolaz.ttf");
        addFontFile(FONTDIR "seguisbi.ttf");
    }
    else if (description.weight != font_weight::normal)
    {
        addFontFile(FONTDIR "consolab.ttf");
        addFontFile(FONTDIR "seguisb.ttf");
    }
    else if (description.slant != font_slant::normal)
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
    FcFontSet* fs = FcFontList(_d->ftConfig, pat, os);

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

        locatorLog()("font({}, {}, {})", fcWeightStr(weight), fcSlantStr(slant), (char*) family);
        output.emplace_back(font_path { .value = (char const*) filename });
    }

    FcObjectSetDestroy(os);
    FcFontSetDestroy(fs);
    FcPatternDestroy(pat);

    return output;
}

font_source_list fontconfig_locator::resolve(gsl::span<const char32_t> /*codepoints*/)
{
    // that's also possible via FC, not sure yet we will/want-to need that.
    return {}; // TODO
}

} // namespace text
