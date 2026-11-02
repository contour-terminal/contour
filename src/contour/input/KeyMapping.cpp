// SPDX-License-Identifier: Apache-2.0

#include <contour/input/KeyMapping.hpp>

namespace contour::input
{

vtbackend::Modifiers chordOf(Qt::KeyboardModifiers qtModifiers) noexcept
{
    using vtbackend::Modifier;

    auto chord = vtbackend::Modifiers {};
    if (qtModifiers & Qt::AltModifier)
        chord |= Modifier::Alt;
    if (qtModifiers & Qt::ShiftModifier)
        chord |= Modifier::Shift;
    if (qtModifiers & Qt::ControlModifier)
        chord |= Modifier::Control;
    if (qtModifiers & Qt::MetaModifier)
        chord |= Modifier::Super;
    return chord;
}

char32_t unshiftedCodepoint(char32_t ch) noexcept
{
    // The US-ASCII shifted -> base inverse of the shift level, kept consistent with the CharMappings
    // table in SessionInput.cpp (which reports the *shifted* symbol for a Shift+key chord). A row per
    // shifted symbol, so extending it is data, not logic.
    switch (ch)
    {
        case U')': return U'0';
        case U'!': return U'1';
        case U'@': return U'2';
        case U'#': return U'3';
        case U'$': return U'4';
        case U'%': return U'5';
        case U'^': return U'6';
        case U'&': return U'7';
        case U'*': return U'8';
        case U'(': return U'9';
        case U'<': return U',';
        case U'>': return U'.';
        case U'?': return U'/';
        case U':': return U';';
        case U'"': return U'\'';
        case U'{': return U'[';
        case U'}': return U']';
        case U'|': return U'\\';
        case U'~': return U'`';
        case U'_': return U'-';
        case U'+': return U'=';
        default: return ch;
    }
}

} // namespace contour::input
