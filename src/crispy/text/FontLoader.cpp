/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
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
#include <crispy/text/FontLoader.h>
#include <crispy/text/Font.h>
#include <crispy/logger.h>

#include <fmt/format.h>

#include <stdexcept>
#include <vector>
#include <iostream>

#if defined(__linux__) || defined(__APPLE__)
#define HAVE_FONTCONFIG // TODO: use cmake, dude!
#endif

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_LCD_FILTER_H
#include FT_ERRORS_H

#if defined(HAVE_FONTCONFIG)
#include <fontconfig/fontconfig.h>
#endif

namespace crispy::text {

using namespace std;

namespace {
    inline string ftErrorStr([[maybe_unused]] FT_Error ec)
    {
#if defined(FT_CONFIG_OPTION_ERROR_STRINGS)
        return FT_Error_String(ec);
#else
        return ""s;
#endif
    }

    static bool endsWithIgnoreCase(string_view const& _text, string const& _suffix)
    {
        if (_text.size() < _suffix.size())
            return false;

        auto const* text = &_text[_text.size() - _suffix.size()];
        for (size_t i = 0; i < _suffix.size(); ++i)
            if (tolower(text[i]) != tolower(_suffix[i]))
                return false;

        return true;
    }

    static vector<string> getFontFilePaths(string_view const& _family, FontStyle _style, bool _monospace)
    {
        std::cerr << fmt::format("getFontFilePaths: family=({}), style={}, {}\n", _family, _style, _monospace ? "monospace" : "anyspace");
        if (endsWithIgnoreCase(_family, ".ttf") || endsWithIgnoreCase(_family, ".otf")) // TODO: and regular file exists
            return {string(_family)};

        #if defined(HAVE_FONTCONFIG) // {{{
        auto const family = string(_family);
        auto pat = unique_ptr<FcPattern, void(*)(FcPattern*)>(
            FcPatternCreate(),
            [](auto p) { FcPatternDestroy(p); });

        FcPatternAddBool(pat.get(), FC_OUTLINE, true);
        FcPatternAddBool(pat.get(), FC_SCALABLE, true);

        // XXX It should be recommended to turn that on if you are looking for colored fonts,
        //     such as for emoji, but it seems like fontconfig doesn't care, it works either way.
        //
        // if (_color)
        //     FcPatternAddBool(pat.get(), FC_COLOR, true);

        if (!_family.empty())
            FcPatternAddString(pat.get(), FC_FAMILY, (FcChar8 const*) family.c_str());

        if (_monospace)
        {
            if (_family != "monospace")
                FcPatternAddString(pat.get(), FC_FAMILY, (FcChar8 const*) "monospace");
            FcPatternAddInteger(pat.get(), FC_SPACING, FC_MONO);
            FcPatternAddInteger(pat.get(), FC_SPACING, FC_DUAL);
        }

        if (int(_style) & int(FontStyle::Bold))
            FcPatternAddInteger(pat.get(), FC_WEIGHT, FC_WEIGHT_BOLD);

        if (int(_style) & int(FontStyle::Italic))
            FcPatternAddInteger(pat.get(), FC_SLANT, FC_SLANT_ITALIC);

        FcConfigSubstitute(nullptr, pat.get(), FcMatchPattern);
        FcDefaultSubstitute(pat.get());

        FcResult result = FcResultNoMatch;
        auto fs = unique_ptr<FcFontSet, void(*)(FcFontSet*)>(
            FcFontSort(nullptr, pat.get(), /*unicode-trim*/FcTrue, /*FcCharSet***/nullptr, &result),
            [](auto p) { FcFontSetDestroy(p); });

        if (!fs || result != FcResultMatch)
            return {};

        vector<string> output;
        for (int i = 0; i < fs->nfont; ++i)
        {
            FcPattern* font = fs->fonts[i];

            FcChar8* file;
            if (FcPatternGetString(font, FC_FILE, 0, &file) != FcResultMatch)
                continue;

            int spacing = -1; // ignore font if we cannot retrieve spacing information
            FcPatternGetInteger(font, FC_SPACING, 0, &spacing);

            if (spacing >= FC_DUAL || !_monospace)
                output.emplace_back((char const*)(file));
        }
        return output;
        #endif // }}}

        #if defined(_WIN32)
        // TODO: Read https://docs.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-enumfontfamiliesexa
        // This is pretty damn hard coded, and to be properly implemented once the other font related code's done,
        // *OR* being completely deleted when FontConfig's windows build fix released and available via vcpkg.
        switch (_style)
        {
            case FontStyle::Bold:
                return {"C:\\Windows\\Fonts\\consolab.ttf"};
            case FontStyle::Italic:
                return {"C:\\Windows\\Fonts\\consolai.ttf"};
            case FontStyle::BoldItalic:
                return {"C:\\Windows\\Fonts\\consolaz.ttf"};
            case FontStyle::Regular:
                return {"C:\\Windows\\Fonts\\consola.ttf"};
        }
        #endif
    }
}

FontLoader::FontLoader(int _dpiX, int _dpiY) :
    ft_{},
    dpi_{ _dpiX, _dpiY }
{
#if defined(HAVE_FONTCONFIG)
    FcInit();
#endif

    if (auto const ec = FT_Init_FreeType(&ft_); ec != FT_Err_Ok)
        throw runtime_error{ "freetype: Failed to initialize. "s + ftErrorStr(ec)};

    if (auto const ec = FT_Library_SetLcdFilter(ft_, FT_LCD_FILTER_DEFAULT); ec != FT_Err_Ok)
        debuglog().write("freetype: Failed to set LCD filter. {}", ftErrorStr(ec));
}

FontLoader::~FontLoader()
{
    FT_Done_FreeType(ft_);

#if defined(HAVE_FONTCONFIG)
    FcFini();
#endif
}

void FontLoader::setDpi(Vec2 _dpi)
{
    dpi_ = _dpi;
}

FontList FontLoader::load(std::string_view const& _family, FontStyle _style, double _fontSize, bool _monospace)
{
    FontList out;

    for (auto const& filename : getFontFilePaths(_family, _style, _monospace))
        out.emplace_back(ft_, dpi_, filename);

    if (!out.empty())
    {
        if (out.front().load())
            out.front().setFontSize(_fontSize);
    }
    else
        debuglog().write("FontLoader: loading font \"{}\" \"{}\" failed. No font candiates found.");

    return out;
}

} // end namespace
