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

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

namespace terminal
{

enum class cell_flags : uint32_t
{
    None = 0,

    Bold = (1 << 0),
    Faint = (1 << 1),
    Italic = (1 << 2),
    Underline = (1 << 3),
    Blinking = (1 << 4),
    Inverse = (1 << 5),
    Hidden = (1 << 6),
    CrossedOut = (1 << 7),
    DoublyUnderlined = (1 << 8),
    CurlyUnderlined = (1 << 9),
    DottedUnderline = (1 << 10),
    DashedUnderline = (1 << 11),
    Framed = (1 << 12),
    Encircled = (1 << 13),
    Overline = (1 << 14),
    RapidBlinking = (1 << 15),
    CharacterProtected = (1 << 16), // Character is protected by selective erase operations.
};

constexpr cell_flags& operator|=(cell_flags& a, cell_flags b) noexcept
{
    a = static_cast<cell_flags>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
    return a;
}

constexpr cell_flags& operator&=(cell_flags& a, cell_flags b) noexcept
{
    a = static_cast<cell_flags>(static_cast<unsigned>(a) & static_cast<unsigned>(b));
    return a;
}

/// Tests if @p b is contained in @p a.
constexpr bool operator&(cell_flags a, cell_flags b) noexcept
{
    return (static_cast<unsigned>(a) & static_cast<unsigned>(b)) != 0;
}

constexpr bool contains_all(cell_flags base, cell_flags test) noexcept
{
    return (static_cast<unsigned>(base) & static_cast<unsigned>(test)) == static_cast<unsigned>(test);
}

/// Merges two cell_flags sets.
constexpr cell_flags operator|(cell_flags a, cell_flags b) noexcept
{
    return static_cast<cell_flags>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}

/// Inverts the flags set.
constexpr cell_flags operator~(cell_flags a) noexcept
{
    return static_cast<cell_flags>(~static_cast<unsigned>(a));
}

/// Tests for all flags cleared state.
constexpr bool operator!(cell_flags a) noexcept
{
    return static_cast<unsigned>(a) == 0;
}

} // namespace terminal

// {{{
template <>
struct fmt::formatter<terminal::cell_flags>: fmt::formatter<std::string>
{
    auto format(const terminal::cell_flags flags, format_context& ctx) -> format_context::iterator
    {
        static const std::array<std::pair<terminal::cell_flags, std::string_view>, 17> nameMap = {
            std::pair { terminal::cell_flags::Bold, std::string_view("Bold") },
            std::pair { terminal::cell_flags::Faint, std::string_view("Faint") },
            std::pair { terminal::cell_flags::Italic, std::string_view("Italic") },
            std::pair { terminal::cell_flags::Underline, std::string_view("Underline") },
            std::pair { terminal::cell_flags::Blinking, std::string_view("Blinking") },
            std::pair { terminal::cell_flags::RapidBlinking, std::string_view("RapidBlinking") },
            std::pair { terminal::cell_flags::Inverse, std::string_view("Inverse") },
            std::pair { terminal::cell_flags::Hidden, std::string_view("Hidden") },
            std::pair { terminal::cell_flags::CrossedOut, std::string_view("CrossedOut") },
            std::pair { terminal::cell_flags::DoublyUnderlined, std::string_view("DoublyUnderlined") },
            std::pair { terminal::cell_flags::CurlyUnderlined, std::string_view("CurlyUnderlined") },
            std::pair { terminal::cell_flags::DottedUnderline, std::string_view("DottedUnderline") },
            std::pair { terminal::cell_flags::DashedUnderline, std::string_view("DashedUnderline") },
            std::pair { terminal::cell_flags::Framed, std::string_view("Framed") },
            std::pair { terminal::cell_flags::Encircled, std::string_view("Encircled") },
            std::pair { terminal::cell_flags::Overline, std::string_view("Overline") },
            std::pair { terminal::cell_flags::CharacterProtected, std::string_view("CharacterProtected") },
        };
        std::string s;
        for (auto const& mapping: nameMap)
        {
            if (mapping.first & flags)
            {
                if (!s.empty())
                    s += ",";
                s += mapping.second;
            }
        }
        return formatter<std::string>::format(s, ctx);
    }
};
// }}}
