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
    // A doubled brace is an escape for a literal brace: "{{" -> "{" and "}}" -> "}". This lets a
    // template (e.g. a user's tab rename) contain literal braces without them being parsed as a
    // placeholder. The escaped brace is emitted as a one-character view of one of the two input
    // braces, preserving the zero-copy contract (fragments borrow from @p text).

    auto fragments = interpolated_string {};

    // The start of the current run of literal text not yet emitted. We defer emitting literal text so
    // an escape ("{{"/"}}") can flush the run, emit the single literal brace, and continue the run
    // after it without producing empty or split fragments unnecessarily.
    size_t runStart = 0;
    auto flushLiteral = [&](size_t end) {
        if (auto const literal = text.substr(runStart, end - runStart); !literal.empty())
            fragments.emplace_back(literal);
    };

    size_t pos = 0;
    while (pos < text.size())
    {
        auto const ch = text[pos];

        // "}}" anywhere collapses to a single literal "}".
        if (ch == '}' && pos + 1 < text.size() && text[pos + 1] == '}')
        {
            flushLiteral(pos);
            fragments.emplace_back(text.substr(pos, 1)); // the literal '}'
            pos += 2;
            runStart = pos;
            continue;
        }

        if (ch != '{')
        {
            ++pos;
            continue;
        }

        // "{{" collapses to a single literal "{".
        if (pos + 1 < text.size() && text[pos + 1] == '{')
        {
            flushLiteral(pos);
            fragments.emplace_back(text.substr(pos, 1)); // the literal '{'
            pos += 2;
            runStart = pos;
            continue;
        }

        // A real placeholder: flush the literal run before it, then parse "{name...}".
        flushLiteral(pos);

        auto const closeBrace = text.find('}', pos);
        if (closeBrace == std::string_view::npos)
        {
            // no matching close brace found, so we're done (the unterminated remainder, leading '{'
            // included, is one placeholder — matching the prior behavior).
            fragments.emplace_back(parse_interpolation(text.substr(pos)));
            return fragments;
        }

        auto const fragment = text.substr(pos + 1, closeBrace - pos - 1);
        fragments.emplace_back(parse_interpolation(fragment));
        pos = closeBrace + 1;
        runStart = pos;
    }

    flushLiteral(text.size());
    return fragments;
}

} // namespace crispy
