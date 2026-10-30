// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/input/InputGenerator.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <variant>

namespace contour::config
{

/// A key chord the operating system delivers while another application has the focus.
///
/// Spelled in contour.yml as an input mapping spells its chord: `{ mods: [Control], key: '`' }`.
struct HotkeyChord
{
    /// The modifiers held.
    vtbackend::Modifiers modifiers {};
    /// The key, or the character, the chord ends on.
    std::variant<vtbackend::Key, char32_t> key {};

    [[nodiscard]] bool operator==(HotkeyChord const&) const = default;
};

/// Why a chord cannot serve as a global hotkey, whatever the platform.
enum class HotkeyChordError : uint8_t
{
    NoModifier = 0, ///< It holds none of Control, Alt and Super; Shift alone would swallow typing.
};

/// Checks the platform-independent rule: a global hotkey needs at least one of Control, Alt and Super.
///
/// Whether the key has a platform row, and whether the chord belongs to the menu bar, are decided where
/// those tables are visible.
/// @param chord The chord to check.
/// @return The chord with a letter key in lower case, as the key table lists letters (an input mapping
///         spells them upper case), or why it is refused.
[[nodiscard]] constexpr std::expected<HotkeyChord, HotkeyChordError> validateHotkeyChord(
    HotkeyChord const& chord) noexcept
{
    auto const hasHeld = chord.modifiers.contains(vtbackend::Modifier::Control)
                         || chord.modifiers.contains(vtbackend::Modifier::Alt)
                         || chord.modifiers.contains(vtbackend::Modifier::Super);
    if (!hasHeld)
        return std::unexpected(HotkeyChordError::NoModifier);
    auto canonical = chord;
    if (auto* const character = std::get_if<char32_t>(&canonical.key);
        character != nullptr && *character >= U'A' && *character <= U'Z')
        *character = *character - U'A' + U'a';
    return canonical;
}

/// Spells @p chord as contour.yml spells a hotkey: `{ mods: [Control], key: '`' }`.
/// @param chord The chord.
/// @return The YAML flow mapping, which the loader reads back as the same chord.
[[nodiscard]] std::string hotkeyYaml(HotkeyChord const& chord);

/// Reads a hotkey as contour.yml spells it, the way the loader reads `quick_terminal.hotkey`.
/// @param text A YAML flow mapping, such as hotkeyYaml() writes or a hand-edited settings.yml holds.
/// @return The chord, or nothing where the loader would refuse it.
[[nodiscard]] std::optional<HotkeyChord> parseHotkeyYaml(std::string const& text);

} // namespace contour::config
