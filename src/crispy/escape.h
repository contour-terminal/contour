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
#include <unicode/utf8.h>

#include <algorithm>
#include <cctype>
#include <iterator>
#include <numeric>
#include <string>
#include <string_view>

namespace crispy {

inline std::string escape(uint8_t ch)
{
    switch (ch)
    {
        case '\\':
            return "\\\\";
        case 0x1B:
            return "\\e";
        case '\t':
            return "\\t";
        case '\r':
            return "\\r";
        case '\n':
            return "\\n";
        case '"':
            return "\\\"";
        default:
            if (std::isprint(static_cast<char>(ch)))
                return fmt::format("{}", static_cast<char>(ch));
            else
                return fmt::format("\\{:03o}", static_cast<uint8_t>(ch) & 0xFF);
    }
}

template <typename T>
inline std::string escape(T begin, T end)
{
    static_assert(sizeof(*std::declval<T>()) == 1, "should be only 1 byte, such as: char, char8_t, uint8_t, byte, ...");
    return std::accumulate(begin, end, std::string{}, [](auto const& a, auto ch) { return a + escape(ch); });
    // auto result = std::string{};
    // for (T cur = begin; cur != end; ++cur)
    //     result += *cur;
    // return result;
}

inline std::string escape(std::string_view s)
{
    return escape(begin(s), end(s));
}

inline std::string unescape(std::string_view _value) {
    std::string out;
    out.reserve(_value.size());

    enum class State { Text, Escape, Octal1, Octal2, Hex1, Hex2 };
    State state = State::Text;
    char buf[3] = {};

    for (size_t i = 0; i < _value.size(); ++i)
    {
        switch (state)
        {
            case State::Text:
                if (_value[i] == '\\')
                    state = State::Escape;
                else
                    out.push_back(_value[i]);
                break;
            case State::Escape:
                if (_value[i] == '0')
                    state = State::Octal1;
                else if (_value[i] == 'x')
                    state = State::Hex1;
                else if (_value[i] == 'e')
                {
                    state = State::Text;
                    out.push_back('\033');
                }
                else if (_value[i] == '\\')
                {
                    out.push_back('\\');
                    state = State::Text;
                }
                else
                {
                    // Unknown escape sequence, so just continue as text.
                    out.push_back('\\');
                    out.push_back(_value[i]);
                    state = State::Text;
                }
                break;
            case State::Octal1:
                buf[0] = _value[i];
                state = State::Octal2;
                break;
            case State::Octal2:
                buf[1] = _value[i];
                out.push_back(static_cast<char>(strtoul(buf, nullptr, 8)));
                state = State::Text;
                break;
            case State::Hex1:
                buf[0] = _value[i];
                state = State::Hex2;
                break;
            case State::Hex2:
                buf[1] = _value[i];
                out.push_back(static_cast<char>(strtoul(buf, nullptr, 16)));
                state = State::Text;
                break;
        }
    }

    return out;
}

}  // namespace terminal
