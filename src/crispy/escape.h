// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <iterator>
#include <numeric>
#include <string>
#include <string_view>

namespace crispy
{

enum class numeric_escape : uint8_t
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
                return std::format("{}", static_cast<char>(ch));
            else if (numericEscape == numeric_escape::Hex)
                return std::format("\\x{:02x}", static_cast<uint8_t>(ch) & 0xFF);
            else
                return std::format("\\{:03o}", static_cast<uint8_t>(ch) & 0xFF);
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

template <typename T>
inline std::string escapeMarkdown(T begin, T end)
{
    static_assert(sizeof(*std::declval<T>()) == 1,
                  "should be only 1 byte, such as: char, char8_t, uint8_t, byte, ...");

    auto escapeMarkdown = [](uint8_t ch) -> std::string {
        if (ch == '`')
            return "``` ";
        return std::format("{}", static_cast<char>(ch));
    };
    auto result = std::string {};
    for (T cur = begin; cur != end; ++cur)
        result += escapeMarkdown(static_cast<uint8_t>(*cur));
    return result;
}

inline std::string escape(std::string_view s, numeric_escape numericEscape = numeric_escape::Hex)
{
    return escape(begin(s), end(s), numericEscape);
}

inline std::string escapeMarkdown(std::string_view s)
{
    return escapeMarkdown(begin(s), end(s));
}

inline std::string unescape(std::string_view escapedText)
{
    std::string out;
    out.reserve(escapedText.size());

    enum class state_type : uint8_t
    {
        Text,
        Escape,
        Octal1,
        Octal2,
        Hex1,
        Hex2
    };
    state_type state = state_type::Text;
    char buf[3] = {};

    for (char const ch: escapedText)
    {
        switch (state)
        {
            case state_type::Text:
                if (ch == '\\')
                    state = state_type::Escape;
                else
                    out.push_back(ch);
                break;
            case state_type::Escape:
                switch (ch)
                {
                    case '0':
                        //.
                        state = state_type::Octal1;
                        break;
                    case 'x':
                        //.
                        state = state_type::Hex1;
                        break;
                    case 'e':
                        state = state_type::Text;
                        out.push_back('\033');
                        break;
                    case 'a':
                        out.push_back(0x07);
                        state = state_type::Text;
                        break;
                    case 'b':
                        out.push_back(0x08);
                        state = state_type::Text;
                        break;
                    case 't':
                        out.push_back(0x09);
                        state = state_type::Text;
                        break;
                    case 'n':
                        out.push_back(0x0A);
                        state = state_type::Text;
                        break;
                    case 'v':
                        out.push_back(0x0B);
                        state = state_type::Text;
                        break;
                    case 'f':
                        out.push_back(0x0C);
                        state = state_type::Text;
                        break;
                    case 'r':
                        out.push_back(0x0D);
                        state = state_type::Text;
                        break;
                    case '\\':
                        out.push_back('\\');
                        state = state_type::Text;
                        break;
                    default:
                        // Unknown escape sequence, so just continue as text.
                        out.push_back('\\');
                        out.push_back(ch);
                        state = state_type::Text;
                        break;
                }
                break;
            case state_type::Octal1:
                buf[0] = ch;
                state = state_type::Octal2;
                break;
            case state_type::Octal2:
                buf[1] = ch;
                out.push_back(static_cast<char>(strtoul(buf, nullptr, 8)));
                state = state_type::Text;
                break;
            case state_type::Hex1:
                buf[0] = ch;
                state = state_type::Hex2;
                break;
            case state_type::Hex2:
                buf[1] = ch;
                out.push_back(static_cast<char>(strtoul(buf, nullptr, 16)));
                state = state_type::Text;
                break;
        }
    }

    return out;
}

} // namespace crispy
