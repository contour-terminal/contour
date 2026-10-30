// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A global hotkey's chord as Qt spells it: what the Settings recorder captures, and what the menu bar's
/// key table is compared with. The key table itself is HotkeyKeys.hpp's; this is its Qt view.

#include <contour/config/HotkeyChord.hpp>
#include <contour/input/KeyMapping.hpp>
#include <contour/platform/HotkeyKeys.hpp>

#include <QtCore/QKeyCombination>
#include <QtCore/Qt>

#include <algorithm>
#include <array>
#include <optional>
#include <variant>

namespace contour::platform
{

/// One chord modifier and the Qt modifier it is. Super is the Meta modifier, as Qt spells Command (and the
/// Windows or Super key) under Qt::AA_MacDontSwapCtrlAndMeta.
struct HotkeyQtModifierRow
{
    vtbackend::Modifier modifier {};          ///< The chord modifier.
    Qt::KeyboardModifier qt = Qt::NoModifier; ///< Its Qt spelling.
};

/// The modifiers a hotkey may hold, in HotkeyModifierRows' order.
inline constexpr auto HotkeyQtModifierRows = std::array {
    HotkeyQtModifierRow { .modifier = vtbackend::Modifier::Control, .qt = Qt::ControlModifier },
    HotkeyQtModifierRow { .modifier = vtbackend::Modifier::Alt, .qt = Qt::AltModifier },
    HotkeyQtModifierRow { .modifier = vtbackend::Modifier::Shift, .qt = Qt::ShiftModifier },
    HotkeyQtModifierRow { .modifier = vtbackend::Modifier::Super, .qt = Qt::MetaModifier },
};

static_assert(HotkeyQtModifierRows.size() == HotkeyModifierRows.size(),
              "every hotkey modifier has a Qt spelling");

/// The Qt key of a printable character a hotkey may end on: letters are the upper-case codes Qt names
/// them by, every other character is its own code.
/// @param character The character, as HotkeyKeyRows lists it.
[[nodiscard]] constexpr Qt::Key qtKeyOfCharacter(char32_t character) noexcept
{
    if (character >= U'a' && character <= U'z')
        return static_cast<Qt::Key>(character - U'a' + U'A');
    return static_cast<Qt::Key>(character);
}

/// Spells @p chord as Qt does.
/// @param chord The chord.
/// @return The combination, or nothing when the chord's key has no row in HotkeyKeyRows.
[[nodiscard]] inline std::optional<QKeyCombination> qtKeyCombinationOf(config::HotkeyChord const& chord)
{
    if (hotkeyKeyRow(chord.key) == nullptr)
        return std::nullopt;
    auto modifiers = Qt::KeyboardModifiers {};
    for (auto const& row: HotkeyQtModifierRows)
        if (chord.modifiers.contains(row.modifier))
            modifiers |= row.qt;
    if (auto const* const character = std::get_if<char32_t>(&chord.key))
        return QKeyCombination(modifiers, qtKeyOfCharacter(*character));
    auto const key = std::get<vtbackend::Key>(chord.key);
    auto const row = std::ranges::find(input::NamedKeyRows, key, &input::NamedKeyRow::key);
    if (row == input::NamedKeyRows.end())
        return std::nullopt;
    return QKeyCombination(modifiers, row->qt);
}

/// Reads a chord a user typed.
/// @param qtKey    The Qt key code.
/// @param modifiers The Qt modifiers held with it.
/// @return The chord, or nothing when the key is not one a hotkey may end on (HotkeyKeyRows).
[[nodiscard]] inline std::optional<config::HotkeyChord> hotkeyChordOf(int qtKey,
                                                                      Qt::KeyboardModifiers modifiers)
{
    auto chord = config::HotkeyChord {};
    for (auto const& row: HotkeyQtModifierRows)
        if (modifiers.testFlag(row.qt))
            chord.modifiers.enable(row.modifier);
    if (auto const named = input::namedKeyOf(qtKey))
        chord.key = *named;
    else if (qtKey >= 0x20 && qtKey <= 0x7E)
        chord.key = static_cast<char32_t>(qtKey >= 'A' && qtKey <= 'Z' ? qtKey - 'A' + 'a' : qtKey);
    else
        return std::nullopt;
    if (hotkeyKeyRow(chord.key) == nullptr)
        return std::nullopt;
    return chord;
}

} // namespace contour::platform
