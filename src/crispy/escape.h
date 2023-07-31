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
#pragma once

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <iterator>
#include <numeric>
#include <string>
#include <string_view>

namespace crispy
{

enum class numeric_escape
{
    Octal,
    Hex
};

inline std::string escape(uint8_t ch, numeric_escape numericEscape = numeric_escape::Hex)
{
    switch (ch)
    {
        case '\\': return "\\\\";
        case 0x1B: return "\\e";
        case '\t': return "\\t";
        case '\r': return "\\r";
        case '\n': return "\\n";
        case '"': return "\\\"";
        default:
            if (0x20 <= ch && ch < 0x7E)
                return fmt::format("{}", static_cast<char>(ch));
            else if (numericEscape == numeric_escape::Hex)
                return fmt::format("\\x{:02x}", static_cast<uint8_t>(ch) & 0xFF);
            else
                return fmt::format("\\{:03o}", static_cast<uint8_t>(ch) & 0xFF);
    }
}

template <typename T>
inline std::string escape(T begin, T end, numeric_escape numericEscape = numeric_escape::Hex)
{
    static_assert(sizeof(*std::declval<T>()) == 1,
                  "should be only 1 byte, such as: char, char8_t, uint8_t, byte, ...");
    // return std::accumulate(begin, end, std::string {}, [](auto const& a, auto ch) {
    //     return a + escape(static_cast<uint8_t>(ch));
    // });
    auto result = std::string {};
    for (T cur = begin; cur != end; ++cur)
        result += escape(static_cast<uint8_t>(*cur), numericEscape);
    return result;
}

inline std::string escape(std::string_view s, numeric_escape numericEscape = numeric_escape::Hex)
{
    return escape(begin(s), end(s), numericEscape);
}

inline std::string unescape(std::string_view escapedText)
{
    std::string out;
    out.reserve(escapedText.size());

    enum class state
    {
        Text,
        Escape,
        Octal1,
        Octal2,
        Hex1,
        Hex2
    };
    state stateText = state::Text;
    char buf[3] = {};

    for (char const ch: escapedText)
    {
        switch (stateText)
        {
            case state::Text:
                if (ch == '\\')
                    stateText = state::Escape;
                else
                    out.push_back(ch);
                break;
            case state::Escape:
                switch (ch)
                {
                    case '0':
                        //.
                        stateText = state::Octal1;
                        break;
                    case 'x':
                        //.
                        stateText = state::Hex1;
                        break;
                    case 'e':
                        stateText = state::Text;
                        out.push_back('\033');
                        break;
                    case 'a':
                        out.push_back(0x07);
                        stateText = state::Text;
                        break;
                    case 'b':
                        out.push_back(0x08);
                        stateText = state::Text;
                        break;
                    case 't':
                        out.push_back(0x09);
                        stateText = state::Text;
                        break;
                    case 'n':
                        out.push_back(0x0A);
                        stateText = state::Text;
                        break;
                    case 'v':
                        out.push_back(0x0B);
                        stateText = state::Text;
                        break;
                    case 'f':
                        out.push_back(0x0C);
                        stateText = state::Text;
                        break;
                    case 'r':
                        out.push_back(0x0D);
                        stateText = state::Text;
                        break;
                    case '\\':
                        out.push_back('\\');
                        stateText = state::Text;
                        break;
                    default:
                        // Unknown escape sequence, so just continue as text.
                        out.push_back('\\');
                        out.push_back(ch);
                        stateText = state::Text;
                        break;
                }
                break;
            case state::Octal1:
                buf[0] = ch;
                stateText = state::Octal2;
                break;
            case state::Octal2:
                buf[1] = ch;
                out.push_back(static_cast<char>(strtoul(buf, nullptr, 8)));
                stateText = state::Text;
                break;
            case state::Hex1:
                buf[0] = ch;
                stateText = state::Hex2;
                break;
            case state::Hex2:
                buf[1] = ch;
                out.push_back(static_cast<char>(strtoul(buf, nullptr, 16)));
                stateText = state::Text;
                break;
        }
    }

    return out;
}

} // namespace crispy
