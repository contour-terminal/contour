// This file is part of the "libterminal" project, http://github.com/christianparpart/libterminal>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#pragma once

#include <fmt/format.h>
#include <cctype>
#include <numeric>
#include <string>

namespace terminal {

template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts> overloaded(Ts...)->overloaded<Ts...>;

inline std::string escape(uint8_t ch)
{
    switch (ch)
    {
        case '\\':
            return "\\\\";
        case 0x1B:
            return "\\033";
        case '\t':
            return "\\t";
        case '\r':
            return "\\r";
        case '\n':
            return "\\n";
        default:
            if (std::isprint(ch))
                return fmt::format("{}", static_cast<char>(ch));
            else
                return fmt::format("\\x{:02X}", ch);
    }
}

template <typename T>
inline std::string escape(T begin, T end)
{
    return std::accumulate(begin, end, std::string{}, [](auto const& a, auto ch) { return a + escape(ch); });
    // auto result = std::string{};
    // for (T cur = begin; cur != end; ++cur)
    //     result += *cur;
    // return result;
}

inline std::string escape(std::string const& s)
{
    return escape(begin(s), end(s));
}

}  // namespace terminal
