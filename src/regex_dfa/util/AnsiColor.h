// This file is part of the "x0" project, http://github.com/christianparpart/x0>
//   (c) 2009-2019 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT
#pragma once

#include <array>

namespace AnsiColor {

enum Code : unsigned {
    Clear = 0,
    Reset = Clear,
    Bold = 0x0001,  // 1
    Dark = 0x0002,  // 2
    Undef1 = 0x0004,
    Underline = 0x0008,  // 4
    Blink = 0x0010,      // 5
    Undef2 = 0x0020,
    Reverse = 0x0040,    // 7
    Concealed = 0x0080,  // 8
    AllFlags = 0x00FF,
    Black = 0x0100,
    Red = 0x0200,
    Green = 0x0300,
    Yellow = 0x0400,
    Blue = 0x0500,
    Magenta = 0x0600,
    Cyan = 0x0700,
    White = 0x0800,
    AnyFg = 0x0F00,
    OnBlack = 0x1000,
    OnRed = 0x2000,
    OnGreen = 0x3000,
    OnYellow = 0x4000,
    OnBlue = 0x5000,
    OnMagenta = 0x6000,
    OnCyan = 0x7000,
    OnWhite = 0x8000,
    AnyBg = 0xF000
};

/// Combines two ANSI escape sequences into one Code.
constexpr inline Code operator|(Code a, Code b)
{
    return Code{unsigned(a) | unsigned(b)};
}

/**
 * Counts the number of ANSI escape sequences in @p codes.
 */
constexpr unsigned count(Code codes)
{
    if (codes == Clear)
        return 1;

    unsigned i = 0;

    if (codes & AllFlags)
        for (int k = 0; k < 8; ++k)
            if (codes & (1 << k))
                ++i;

    if (codes & AnyFg)
        ++i;

    if (codes & AnyBg)
        ++i;

    return i;
}

/**
 * Retrieves the number of bytes required to store the ANSI escape sequences of @p codes
 * without prefix/suffix notation.
 */
constexpr unsigned capacity(Code codes)
{
    if (codes == Clear)
        return 1;

    unsigned i = 0;

    if (codes & AllFlags)
        for (int k = 0; k < 8; ++k)
            if (codes & (1 << k))
                ++i;

    if (codes & AnyFg)
        i += 2;

    if (codes & AnyBg)
        i += 2;

    return i + (count(codes) - 1);
}

/// Constructs a sequence of ANSI codes for the colors in this @p codes.
template <const Code value, const bool EOS = true>
constexpr auto codes()
{
    std::array<char, capacity(value) + 3 + (EOS ? 1 : 0)> result{};

    size_t n = 0;  // n'th escape sequence being iterate through
    size_t i = 0;  // i'th byte in output array

    result[i++] = '\x1B';
    result[i++] = '[';

    if constexpr (value != 0)
    {
        if (value & AllFlags)
        {
            for (int k = 0; k < 8; ++k)
            {
                if (value & (1 << k))
                {
                    if (n++)
                        result[i++] = ';';
                    result[i++] = k + '1';
                }
            }
        }

        if (value & AnyFg)
        {
            if (n++)
                result[i++] = ';';
            unsigned const val = ((value >> 8) & 0x0F) + 29;  // 36 -> {'3', '6'}
            result[i++] = (val / 10) + '0';
            result[i++] = (val % 10) + '0';
        }

        if (value & AnyBg)
        {
            if (n++)
                result[i++] = ';';
            unsigned const val = ((value >> 12) & 0x0F) + 39;
            result[i++] = (val / 10) + '0';
            result[i++] = (val % 10) + '0';
        }
    }
    else
        result[i++] = '0';  // reset/clear

    result[i++] = 'm';

    return result;
}

}  // namespace AnsiColor
