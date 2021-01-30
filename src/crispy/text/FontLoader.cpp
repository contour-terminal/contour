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

    static vector<string> getFontFilePaths(string_view const& _family, FontStyle _style)
    {
        auto const pattern = _style == FontStyle::Regular ? string(_family) : fmt::format("{}:style={}", _family, _style);
        if (endsWithIgnoreCase(pattern, ".ttf") || endsWithIgnoreCase(pattern, ".otf")) // TODO: and regular file exists
            return {string(_family)};

        #if defined(HAVE_FONTCONFIG)
        #if 0
        FcPattern* fcPattern = FcNameParse((FcChar8 const*) pattern.c_str());
        #else
        auto fcPattern = FcPatternCreate();
        auto family = string(_family);
        FcPatternAddString(fcPattern, FC_FAMILY, (FcChar8 const*) family.c_str());
        FcPatternAddInteger(fcPattern, FC_SPACING, FC_MONO);
        //FcPatternAddDouble(fcPattern, FC_SIZE, sizeInPt); // do we want to provide these hints?
        //FcPatternAddDouble(fcPattern, FC_DPI, _dpi);
        if (int(_style) & int(FontStyle::Bold))
            FcPatternAddInteger(fcPattern, FC_WEIGHT, FC_WEIGHT_BOLD);
        if (int(_style) & int(FontStyle::Italic))
            FcPatternAddInteger(fcPattern, FC_SLANT, FC_SLANT_ITALIC);
        #endif

        // FcConfig* fcConfig = nullptr;
        FcConfig* fcConfig = FcInitLoadConfigAndFonts();
        FcDefaultSubstitute(fcPattern);
        FcConfigSubstitute(fcConfig, fcPattern, FcMatchPattern);

        vector<string> paths;

        // find font along with all its fallback fonts
        FcCharSet* fcCharSet = nullptr;
        FcResult fcResult = FcResultNoMatch;
#if 1
        FcFontSet* fcFontSet = FcFontSort(fcConfig, fcPattern, /*trim*/FcTrue, &fcCharSet, &fcResult);
#else
        FcFontSet* fcFontSet = nullptr;
        FcObjectSet* fcObjectSet = FcObjectSetBuild(FC_FILE, FC_POSTSCRIPT_NAME, FC_FAMILY, FC_STYLE, FC_FULLNAME, FC_WEIGHT, FC_WIDTH, FC_SLANT, FC_HINT_STYLE, FC_INDEX, FC_HINTING, FC_SCALABLE, FC_OUTLINE, FC_COLOR, FC_SPACING, NULL);
        fcFontSet = FcFontList(nullptr, fcPattern, fcObjectSet);
#endif
        for (int i = 0; i < fcFontSet->nfont; ++i)
        {
            FcChar8* fcFile = nullptr;
            if (FcPatternGetString(fcFontSet->fonts[i], FC_FILE, 0, &fcFile) == FcResultMatch)
            {
                // FcBool fcColor = false;
                // FcPatternGetBool(fcFontSet->fonts[i], FC_COLOR, 0, &fcColor);
                if (fcFile)
                    paths.emplace_back((char const*) fcFile);
            }
        }
        FcFontSetDestroy(fcFontSet);
        FcCharSetDestroy(fcCharSet);

        FcPatternDestroy(fcPattern);
        FcConfigDestroy(fcConfig);
        return paths;
        #endif

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
    if (auto const ec = FT_Init_FreeType(&ft_); ec != FT_Err_Ok)
        throw runtime_error{ "freetype: Failed to initialize. "s + ftErrorStr(ec)};

    if (auto const ec = FT_Library_SetLcdFilter(ft_, FT_LCD_FILTER_DEFAULT); ec != FT_Err_Ok)
        debuglog().write("freetype: Failed to set LCD filter. {}", ftErrorStr(ec));
}

FontLoader::~FontLoader()
{
    FT_Done_FreeType(ft_);
}

void FontLoader::setDpi(Vec2 _dpi)
{
    dpi_ = _dpi;
}

FontList FontLoader::load(std::string_view const& _family, FontStyle _style, double _fontSize)
{
    FontList out;

    for (auto const& filename : getFontFilePaths(_family, _style))
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
