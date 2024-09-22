// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/flags.h>

#include <cstdint>
#include <format>

namespace vtbackend
{

enum class CellFlag : uint32_t
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

using CellFlags = crispy::flags<CellFlag>;

} // namespace vtbackend

// {{{
template <>
struct std::formatter<vtbackend::CellFlag>: std::formatter<std::string_view>
{
    auto format(const vtbackend::CellFlag value, auto& ctx) const
    {
        string_view s;

        // clang-format off
        switch (value)
        {
            case vtbackend::CellFlag::None: s = std::string_view("None"); break;
            case vtbackend::CellFlag::Bold: s = std::string_view("Bold"); break;
            case vtbackend::CellFlag::Faint: s = std::string_view("Faint"); break;
            case vtbackend::CellFlag::Italic: s = std::string_view("Italic"); break;
            case vtbackend::CellFlag::Underline: s = std::string_view("Underline"); break;
            case vtbackend::CellFlag::Blinking: s = std::string_view("Blinking"); break;
            case vtbackend::CellFlag::RapidBlinking: s = std::string_view("RapidBlinking"); break;
            case vtbackend::CellFlag::Inverse: s = std::string_view("Inverse"); break;
            case vtbackend::CellFlag::Hidden: s = std::string_view("Hidden"); break;
            case vtbackend::CellFlag::CrossedOut: s = std::string_view("CrossedOut"); break;
            case vtbackend::CellFlag::DoublyUnderlined: s = std::string_view("DoublyUnderlined"); break;
            case vtbackend::CellFlag::CurlyUnderlined: s = std::string_view("CurlyUnderlined"); break;
            case vtbackend::CellFlag::DottedUnderline: s = std::string_view("DottedUnderline"); break;
            case vtbackend::CellFlag::DashedUnderline: s = std::string_view("DashedUnderline"); break;
            case vtbackend::CellFlag::Framed: s = std::string_view("Framed"); break;
            case vtbackend::CellFlag::Encircled: s = std::string_view("Encircled"); break;
            case vtbackend::CellFlag::Overline: s = std::string_view("Overline"); break;
            case vtbackend::CellFlag::CharacterProtected: s = std::string_view("CharacterProtected"); break;
            case vtbackend::CellFlag::WideCharContinuation: s = std::string_view("WideCharContinuation"); break;
        }
        // clang-format on

        return formatter<string_view>::format(s, ctx);
    }
};
// }}}
