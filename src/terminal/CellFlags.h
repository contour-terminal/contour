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

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

#include <fmt/format.h>

namespace terminal {

enum class CellFlags : uint16_t
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
};

constexpr CellFlags& operator|=(CellFlags& a, CellFlags b) noexcept
{
    a = static_cast<CellFlags>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
	return a;
}

constexpr CellFlags& operator&=(CellFlags& a, CellFlags b) noexcept
{
    a = static_cast<CellFlags>(static_cast<unsigned>(a) & static_cast<unsigned>(b));
	return a;
}

/// Tests if @p b is contained in @p a.
constexpr bool operator&(CellFlags a, CellFlags b) noexcept
{
    return (static_cast<unsigned>(a) & static_cast<unsigned>(b)) != 0;
}

constexpr bool contains_all(CellFlags _base, CellFlags _test) noexcept
{
    return (static_cast<unsigned>(_base) & static_cast<unsigned>(_test)) == static_cast<unsigned>(_test);
}

/// Merges two CellFlags sets.
constexpr CellFlags operator|(CellFlags a, CellFlags b) noexcept
{
    return static_cast<CellFlags>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}

/// Inverts the flags set.
constexpr CellFlags operator~(CellFlags a) noexcept
{
    return static_cast<CellFlags>(~static_cast<unsigned>(a));
}

/// Tests for all flags cleared state.
constexpr bool operator!(CellFlags a) noexcept
{
    return static_cast<unsigned>(a) == 0;
}

}

namespace fmt // {{{
{
    template <>
    struct formatter<terminal::CellFlags> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(const terminal::CellFlags _flags, FormatContext& ctx)
        {
            static const std::array<std::pair<terminal::CellFlags, std::string_view>, 15> nameMap = {
                std::pair{terminal::CellFlags::Bold, std::string_view("Bold")},
                std::pair{terminal::CellFlags::Faint, std::string_view("Faint")},
                std::pair{terminal::CellFlags::Italic, std::string_view("Italic")},
                std::pair{terminal::CellFlags::Underline, std::string_view("Underline")},
                std::pair{terminal::CellFlags::Blinking, std::string_view("Blinking")},
                std::pair{terminal::CellFlags::Inverse, std::string_view("Inverse")},
                std::pair{terminal::CellFlags::Hidden, std::string_view("Hidden")},
                std::pair{terminal::CellFlags::CrossedOut, std::string_view("CrossedOut")},
                std::pair{terminal::CellFlags::DoublyUnderlined, std::string_view("DoublyUnderlined")},
                std::pair{terminal::CellFlags::CurlyUnderlined, std::string_view("CurlyUnderlined")},
                std::pair{terminal::CellFlags::DottedUnderline, std::string_view("DottedUnderline")},
                std::pair{terminal::CellFlags::DashedUnderline, std::string_view("DashedUnderline")},
                std::pair{terminal::CellFlags::Framed, std::string_view("Framed")},
                std::pair{terminal::CellFlags::Encircled, std::string_view("Encircled")},
                std::pair{terminal::CellFlags::Overline, std::string_view("Overline")},
            };
            std::string s;
            for (auto const& mapping : nameMap)
            {
                if (mapping.first & _flags)
                {
                    if (!s.empty())
                        s += ",";
                    s += mapping.second;
                }
            }
            return format_to(ctx.out(), s);
        }
    };
} // }}}
