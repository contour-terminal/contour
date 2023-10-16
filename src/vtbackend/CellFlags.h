// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <fmt/format.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

namespace vtbackend
{

enum class CellFlags : uint32_t
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
    CharacterProtected = (1 << 16),   // Character is protected by selective erase operations.
    WideCharContinuation = (1 << 17), // Cell is a continuation of a wide char.
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

constexpr bool contains_all(CellFlags base, CellFlags test) noexcept
{
    return (static_cast<unsigned>(base) & static_cast<unsigned>(test)) == static_cast<unsigned>(test);
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

} // namespace vtbackend

// {{{
template <>
struct fmt::formatter<vtbackend::CellFlags>: fmt::formatter<std::string>
{
    auto format(const vtbackend::CellFlags flags, format_context& ctx) -> format_context::iterator
    {
        static const std::array<std::pair<vtbackend::CellFlags, std::string_view>, 18> nameMap = {
            std::pair { vtbackend::CellFlags::Bold, std::string_view("Bold") },
            std::pair { vtbackend::CellFlags::Faint, std::string_view("Faint") },
            std::pair { vtbackend::CellFlags::Italic, std::string_view("Italic") },
            std::pair { vtbackend::CellFlags::Underline, std::string_view("Underline") },
            std::pair { vtbackend::CellFlags::Blinking, std::string_view("Blinking") },
            std::pair { vtbackend::CellFlags::RapidBlinking, std::string_view("RapidBlinking") },
            std::pair { vtbackend::CellFlags::Inverse, std::string_view("Inverse") },
            std::pair { vtbackend::CellFlags::Hidden, std::string_view("Hidden") },
            std::pair { vtbackend::CellFlags::CrossedOut, std::string_view("CrossedOut") },
            std::pair { vtbackend::CellFlags::DoublyUnderlined, std::string_view("DoublyUnderlined") },
            std::pair { vtbackend::CellFlags::CurlyUnderlined, std::string_view("CurlyUnderlined") },
            std::pair { vtbackend::CellFlags::DottedUnderline, std::string_view("DottedUnderline") },
            std::pair { vtbackend::CellFlags::DashedUnderline, std::string_view("DashedUnderline") },
            std::pair { vtbackend::CellFlags::Framed, std::string_view("Framed") },
            std::pair { vtbackend::CellFlags::Encircled, std::string_view("Encircled") },
            std::pair { vtbackend::CellFlags::Overline, std::string_view("Overline") },
            std::pair { vtbackend::CellFlags::CharacterProtected, std::string_view("CharacterProtected") },
            std::pair { vtbackend::CellFlags::WideCharContinuation,
                        std::string_view("WideCharContinuation") },
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
