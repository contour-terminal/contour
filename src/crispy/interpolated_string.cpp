// SPDX-License-Identifier: Apache-2.0
#include <crispy/interpolated_string.h>

namespace crispy
{

namespace
{
    void parse_attribute(string_interpolation* interpolation, std::string_view attribute)
    {
        auto const equal = attribute.find('=');
        if (equal != std::string_view::npos)
        {
            auto const key = attribute.substr(0, equal);
            auto const value = attribute.substr(equal + 1);
            interpolation->attributes[key] = value;
        }
        else
        {
            interpolation->flags.insert(attribute);
        }
    }
} // anonymous namespace

string_interpolation parse_interpolation(std::string_view text)
{
    auto result = string_interpolation {};
    auto const colon = text.find(':');
    if (colon != std::string_view::npos)
    {
        result.name = text.substr(0, colon);
        auto const attributes = text.substr(colon + 1);
        size_t pos = 0;
        while (pos < attributes.size())
        {
            auto const comma = attributes.find(',', pos);
            if (comma == std::string_view::npos)
            {
                parse_attribute(&result, attributes.substr(pos));
                break;
            }
            else
            {
                parse_attribute(&result, attributes.substr(pos, comma - pos));
                pos = comma + 1;
            }
        }
    }
    else
    {
        result.name = text;
    }
    return result;
}

interpolated_string parse_interpolated_string(std::string_view text)
{
    // "< {Clock:Bold,Italic,Color=#FFFF00} | {VTType} | {InputMode} {Search:Bold,Color=Yellow} >"
    //
    // NB: There is no brace escaping. Every "{...}" run is parsed as a placeholder; braces cannot be
    // emitted literally. (An earlier attempt at "{{"/"}}" escaping was dropped to preserve backward
    // compatibility with existing templates that legitimately contained doubled braces.) Each parsed
    // interpolation carries its exact original "{...}" slice in `whole`, so a consumer that does not
    // recognize the name can emit it verbatim rather than dropping it (see expandTabLabel /
    // parseStatusLineSegment).

    auto fragments = interpolated_string {};

    // Builds an interpolation for the placeholder spanning [openBrace, closeBrace] (closeBrace == npos for
    // an unterminated trailing placeholder, which extends to the end of the input). Captures the whole
    // source slice — braces included — so unrecognized placeholders can be echoed verbatim downstream.
    auto makeInterpolation = [text](size_t openBrace, size_t closeBrace) {
        auto const terminated = closeBrace != std::string_view::npos;
        // The whole source slice, braces included; extends to end-of-input when unterminated.
        auto const whole =
            terminated ? text.substr(openBrace, closeBrace - openBrace + 1) : text.substr(openBrace);
        // A closed placeholder strips both braces before parsing; an unterminated one keeps the leading
        // '{' in the name (so "{WindowTitle" stays unrecognized and echoes verbatim, rather than behaving
        // like a valid "{WindowTitle}").
        auto interpolation = parse_interpolation(terminated ? whole.substr(1, whole.size() - 2) : whole);
        interpolation.whole = whole;
        return interpolation;
    };

    size_t pos = 0;
    while (pos < text.size())
    {
        auto const openBrace = text.find('{', pos);
        if (openBrace == std::string_view::npos)
        {
            // no more open braces found, so we're done.
            fragments.emplace_back(text.substr(pos));
            return fragments;
        }

        if (auto const textFragment = text.substr(pos, openBrace - pos); !textFragment.empty())
            // add text fragment before the open brace
            fragments.emplace_back(textFragment);

        auto const closeBrace = text.find('}', openBrace);
        if (closeBrace == std::string_view::npos)
        {
            // no matching close brace found, so we're done.
            fragments.emplace_back(makeInterpolation(openBrace, closeBrace));
            return fragments;
        }

        // add interpolation fragment
        fragments.emplace_back(makeInterpolation(openBrace, closeBrace));
        pos = closeBrace + 1;
    }

    return fragments;
}

} // namespace crispy
