// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/FontconfigLocator.hpp>

#include <text_shaper/Font.hpp>

#include <crispy/Assert.hpp>
#include <crispy/Utils.hpp>

#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <ranges>
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

    auto constexpr FontWeightMappings = std::array<std::pair<FontWeight, int>, 12> { {
        { FontWeight::Thin, FC_WEIGHT_THIN },
        { FontWeight::ExtraLight, FC_WEIGHT_EXTRALIGHT },
        { FontWeight::Light, FC_WEIGHT_LIGHT },
        { FontWeight::DemiLight, FC_WEIGHT_DEMILIGHT },
        { FontWeight::Book, FC_WEIGHT_BOOK },
        { FontWeight::Normal, FC_WEIGHT_NORMAL },
        { FontWeight::Medium, FC_WEIGHT_MEDIUM },
        { FontWeight::DemiBold, FC_WEIGHT_DEMIBOLD },
        { FontWeight::Bold, FC_WEIGHT_BOLD },
        { FontWeight::ExtraBold, FC_WEIGHT_EXTRABOLD },
        { FontWeight::Black, FC_WEIGHT_BLACK },
        { FontWeight::ExtraBlack, FC_WEIGHT_EXTRABLACK },
    } };

    // clang-format off
    auto constexpr FontSlantMappings = std::array<std::pair<FontSlant, int>, 3>{ {
        { FontSlant::Italic, FC_SLANT_ITALIC },
        { FontSlant::Oblique, FC_SLANT_OBLIQUE },
        { FontSlant::Normal, FC_SLANT_ROMAN }
    } };
    // clang-format on

    constexpr optional<FontWeight> fcToFontWeight(int value) noexcept
    {
        for (auto const& mapping: FontWeightMappings)
            if (mapping.second == value)
                return mapping.first;
        return nullopt;
    }

    constexpr optional<FontSlant> fcToFontSlant(int value) noexcept
    {
        for (auto const& mapping: FontSlantMappings)
            if (mapping.second == value)
                return mapping.first;
        return nullopt;
    }

    int fcWeight(FontWeight weight) noexcept
    {
        for (auto const& mapping: FontWeightMappings)
            if (mapping.first == weight)
                return mapping.second;
        crispy::fatal("Implementation error. font weight cannot be mapped.");
    }

    constexpr int fcSlant(FontSlant slant) noexcept
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
#ifdef FC_WEIGHT_DEMILIGHT
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

    /// Appends @p font to @p output as a FontPath, unless it names no file.
    ///
    /// Shared by both queries this locator answers -- by description and by coverage -- so that a font
    /// resolved one way carries exactly the same collection index, weight and slant as the other.
    void appendFontSource(FcPattern* font, FontSourceList& output)
    {
        FcChar8* file = nullptr;
        if (FcPatternGetString(font, FC_FILE, 0, &file) != FcResultMatch)
            return;

        auto integerValue = -1;
        auto weight = optional<FontWeight> { nullopt };
        auto slant = optional<FontSlant> { nullopt };
        auto ttcIndex = -1;

        if (FcPatternGetInteger(font, FC_INDEX, 0, &integerValue) == FcResultMatch && integerValue >= 0)
            ttcIndex = integerValue;
        if (FcPatternGetInteger(font, FC_WEIGHT, 0, &integerValue) == FcResultMatch)
            weight = fcToFontWeight(integerValue);
        if (FcPatternGetInteger(font, FC_SLANT, 0, &integerValue) == FcResultMatch)
            slant = fcToFontSlant(integerValue);

        output.emplace_back(FontPath { .value = string { (char const*) file },
                                       .collectionIndex = ttcIndex,
                                       .weight = weight,
                                       .slant = slant });
        locatorLog()("Font {} (ttc index {}, weight {}, slant {}) in chain: {}",
                     output.size(),
                     ttcIndex,
                     weight.has_value() ? std::format("{}", *weight) : "NONE",
                     slant.has_value() ? std::format("{}", *slant) : "NONE",
                     (char const*) file);
    }

} // namespace

struct FontconfigLocator::PrivateTag
{
    // currently empty, maybe later something (such as caching)?
    FcConfig* ftConfig = nullptr;

    PrivateTag(): ftConfig(FcInitLoadConfigAndFonts())
    {
        auto const start = std::chrono::steady_clock::now();
        FcInit();

        auto const elapsed = std::chrono::steady_clock::now() - start;
        auto const ms =
            static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count())
            / 1000.0;
        locatorLog()("FcInitLoadConfigAndFonts: {:.1f} ms", ms);
    }

    ~PrivateTag()
    {
        locatorLog()("~FontconfigLocator.dtor");
        FcConfigDestroy(ftConfig);
        FcFini();
    }
};

FontconfigLocator::FontconfigLocator(): _d { new PrivateTag(), [](PrivateTag* p) { delete p; } }
{
}

FontSourceList FontconfigLocator::locate(FontDescription const& description)
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

    if (description.spacing != FontSpacing::Proportional)
    {
#ifdef _WIN32
        // On Windows FontConfig can't find "monospace". We need to use "Consolas" instead.
        if (description.familyName == "monospace")
            FcPatternAddString(pat.get(), FC_FAMILY, (FcChar8 const*) "Consolas");
#elifdef __APPLE__
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

    if (description.weight != FontWeight::Normal)
        FcPatternAddInteger(pat.get(), FC_WEIGHT, fcWeight(description.weight));
    if (description.slant != FontSlant::Normal)
        FcPatternAddInteger(pat.get(), FC_SLANT, fcSlant(description.slant));

    FcConfigSubstitute(_d->ftConfig, pat.get(), FcMatchPattern);
    FcDefaultSubstitute(pat.get());

    FcResult result = FcResultNoMatch;
    auto fs = unique_ptr<FcFontSet, void (*)(FcFontSet*)>(
        FcFontSort(_d->ftConfig, pat.get(), /*unicode-trim*/ FcTrue, /*FcCharSet***/ nullptr, &result),
        [](auto p) { FcFontSetDestroy(p); });

    if (!fs || result != FcResultMatch)
        return {};

    FontSourceList output;

#ifdef _WIN32
    auto const addFontFile = [&](std::string_view path) {
        output.emplace_back(FontPath { string { path } });
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
                && ((description.spacing == FontSpacing::Proportional && spacing < FC_PROPORTIONAL)
                    || (description.spacing == FontSpacing::Mono && spacing < FC_MONO)))
            {
                locatorLog()("Skipping font: {} ({} < {}).",
                             (char const*) file,
                             fcSpacingStr(spacing),
                             fcSpacingStr(FC_DUAL));
                return;
            }
        }

        appendFontSource(font, output);
    };

    // First font is the primary font that is best matching for description.family, we always
    // include that one.on.
    addFont(fs->fonts[0]);

    std::visit(crispy::Overloaded {
                   [](FontFallbackNone) {},
                   [&](FontFallbackList const& list) {
                       // find font in the fallback list and add it
                       for (auto&& fallbackFont: list.fallbackFonts)
                       {
                           for (auto i: std::views::iota(1, fs->nfont))
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
                       for (auto i: std::views::iota(1, fs->nfont))
                           addFont(fs->fonts[i]);
                   },
               },
               description.fontFallback);

#ifdef _WIN32
    #define FONTDIR "C:\\Windows\\Fonts\\"
    if (description.familyName == "emoji")
    {
        addFontFile(FONTDIR "seguiemj.ttf");
        addFontFile(FONTDIR "seguisym.ttf");
    }
    else if (description.weight != FontWeight::Normal && description.slant != FontSlant::Normal)
    {
        addFontFile(FONTDIR "consolaz.ttf");
        addFontFile(FONTDIR "seguisbi.ttf");
    }
    else if (description.weight != FontWeight::Normal)
    {
        addFontFile(FONTDIR "consolab.ttf");
        addFontFile(FONTDIR "seguisb.ttf");
    }
    else if (description.slant != FontSlant::Normal)
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

FontSourceList FontconfigLocator::all()
{
    FcPattern* pat = FcPatternCreate();
    FcObjectSet* os = FcObjectSetBuild(
#ifdef FC_COLOR
        FC_COLOR,
#endif
        FC_FAMILY,
        FC_FILE,
        FC_FULLNAME,
        FC_HINTING,
        FC_HINT_STYLE,
        FC_INDEX,
        FC_OUTLINE,
#ifdef FC_POSTSCRIPT_NAME
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

    FontSourceList output;

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
        output.emplace_back(FontPath { .value = (char const*) filename });
    }

    FcObjectSetDestroy(os);
    FcFontSetDestroy(fs);
    FcPatternDestroy(pat);

    return output;
}

FontSourceList FontconfigLocator::resolve(gsl::span<char32_t const> codepoints)
{
    // A coverage-driven lookup -- "which fonts contain THESE characters" -- as opposed to locate()'s
    // "which fonts are near this description".
    //
    // The two answer different questions, and the difference is not academic: fontconfig orders a
    // description's chain by how well each font matches the *description*, which routinely buries the
    // only face holding a script far down it. On a stock Fedora install the first CJK face sits at
    // position 83 of 201 for a monospace description, so no chain short enough to walk eagerly will
    // ever reach it. Asking about the codepoint finds it in one query.
    if (codepoints.empty())
        return {};

    auto charSet =
        unique_ptr<FcCharSet, void (*)(FcCharSet*)>(FcCharSetCreate(), [](auto p) { FcCharSetDestroy(p); });
    if (!charSet)
        return {};

    for (auto const codepoint: codepoints)
        FcCharSetAddChar(charSet.get(), static_cast<FcChar32>(codepoint));

    auto pat =
        unique_ptr<FcPattern, void (*)(FcPattern*)>(FcPatternCreate(), [](auto p) { FcPatternDestroy(p); });
    if (!pat)
        return {};

    FcPatternAddCharSet(pat.get(), FC_CHARSET, charSet.get());
    FcPatternAddBool(pat.get(), FC_OUTLINE, true);
    FcPatternAddBool(pat.get(), FC_SCALABLE, true);

    FcConfigSubstitute(_d->ftConfig, pat.get(), FcMatchPattern);
    FcDefaultSubstitute(pat.get());

    auto result = FcResultNoMatch;
    auto fs = unique_ptr<FcFontSet, void (*)(FcFontSet*)>(
        FcFontSort(_d->ftConfig, pat.get(), /*unicode-trim*/ FcTrue, /*FcCharSet***/ nullptr, &result),
        [](auto p) { FcFontSetDestroy(p); });

    if (!fs || result != FcResultMatch)
        return {};

    // Only the best few are of interest: this is consulted when every configured fallback has already
    // failed, and a font that fontconfig ranks below these for a charset query will not do better.
    constexpr auto MaxCandidates = 4;

    FontSourceList output;
    for (auto const i: std::views::iota(0, fs->nfont))
    {
        if (output.size() >= MaxCandidates)
            break;
        appendFontSource(fs->fonts[i], output);
    }

    locatorLog()("Resolved {} font(s) covering {} codepoint(s).", output.size(), codepoints.size());
    return output;
}

} // namespace text
