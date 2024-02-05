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

    auto fragments = interpolated_string {};

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
            fragments.emplace_back(parse_interpolation(text.substr(openBrace)));
            return fragments;
        }
        else
        {
            // add interpolation fragment
            auto const fragment = text.substr(openBrace + 1, closeBrace - openBrace - 1);
            fragments.emplace_back(parse_interpolation(fragment));
            pos = closeBrace + 1;
        }
    }

    return fragments;
}

} // namespace crispy
