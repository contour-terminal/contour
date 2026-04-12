// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/ControlCode.h>
#include <vtbackend/InputGenerator.h>
#include <vtbackend/logging.h>

#include <crispy/assert.h>
#include <crispy/utils.h>

#include <libunicode/case_mapping.h>
#include <libunicode/convert.h>

#include <array>
#include <format>
#include <iterator>
#include <string_view>
#include <unordered_map>

using namespace std;

#define ESC "\x1B"
#define CSI "\x1B["
#define SS3 "\x1BO"

namespace vtbackend
{

string to_string(Modifiers modifiers)
{
    return std::format("{}", modifiers);
}

string to_string(Key key)
{
    return std::format("{}", key);
}

string to_string(MouseButton button)
{
    return std::format("{}", button);
}

/// Returns true if the given modifiers consist only of lock modifiers (CapsLock/NumLock) or no modifiers.
constexpr bool hasOnlyLockModifiers(Modifiers modifiers) noexcept
{
    return modifiers.without(LockModifiers).none();
}

/// Maps a character to its Ctrl-modified equivalent, following Kitty's ctrled_key algorithm.
/// @param ch The character to map (e.g., 'a' for Ctrl+A).
/// @returns The Ctrl-mapped byte, or std::nullopt if no legacy Ctrl mapping exists.
constexpr std::optional<char> ctrlMappedKey(char32_t ch) noexcept
{
    // clang-format off
    if (ch >= 'a' && ch <= 'z') return static_cast<char>(ch - 'a' + 1);
    if (ch >= 'A' && ch <= 'Z') return static_cast<char>(ch - 'A' + 1);
    switch (ch)
    {
        case '@': case ' ': case '2':                   return '\x00';
        case '3': case '[':                             return '\x1B';
        case '4': case '\\':                            return '\x1C';
        case '5': case ']':                             return '\x1D';
        case '6': case '~': case '^':                   return '\x1E';
        case '7': case '/': case '?': case '_':         return '\x1F';
        case '8':                                       return '\x7F';
        case '0':                                       return '0';
        case '1':                                       return '1';
        case '9':                                       return '9';
        case '\x09':                                    return '\x09'; // Tab
        default:                                        return std::nullopt;
    }
    // clang-format on
}

// {{{ StandardKeyboardInputGenerator
bool StandardKeyboardInputGenerator::generateChar(char32_t characterEvent,
                                                  uint32_t physicalKey,
                                                  Modifiers modifiers,
                                                  KeyboardEventType eventType)
{
    crispy::ignore_unused(physicalKey);

    if (eventType == KeyboardEventType::Release)
        return false;

    // Strip lock modifiers (NumLock, CapsLock) as they are irrelevant for legacy character generation.
    modifiers = modifiers.without(LockModifiers);

    // Well accepted hack to distinguish between Backspace and Ctrl+Backspace.
    // DECBKM (Backarrow Key Mode) swaps the default byte:
    //   DECBKM reset (default): Backspace → DEL (0x7F), Ctrl+Backspace → BS (0x08)
    //   DECBKM set:             Backspace → BS  (0x08), Ctrl+Backspace → DEL (0x7F)
    if (characterEvent == 0x08)
    {
        auto const hasCtrl = modifiers.contains(Modifier::Control);
        auto const sendBS = _backarrowKey ? !hasCtrl : hasCtrl;
        append(sendBS ? "\x08" : "\x7f");
        return true;
    }

    // Backtab handling: any modifier combination that includes Shift + Tab.
    // Kitty checks (mods & SHIFT) rather than exact equality, allowing Alt+Shift+Tab etc.
    if (modifiers.contains(Modifier::Shift) && characterEvent == 0x09)
    {
        auto const hasAlt = modifiers.contains(Modifier::Alt);
        append(hasAlt ? "\x1b\x1b[Z" : "\x1b[Z");
        return true;
    }

    // Raw C0 code (Ctrl only, character already a control code).
    if (modifiers == Modifier::Control && characterEvent < 32)
    {
        append(static_cast<char>(characterEvent));
        return true;
    }

    // Strip Shift from effective modifiers for legacy encoding dispatch.
    // The platform already provides the shifted character (e.g., 'A' for Shift+'a').
    // Exception: Ctrl+Shift+letter is unrepresentable in legacy encoding,
    // so we keep Shift to prevent the Ctrl mapping from matching (Issue 4).
    auto effectiveMods = modifiers;
    auto const isLetter =
        (characterEvent >= 'A' && characterEvent <= 'Z') || (characterEvent >= 'a' && characterEvent <= 'z');
    if (!(effectiveMods.contains(Modifier::Control) && isLetter))
        effectiveMods = effectiveMods.without(Modifier::Shift);

    // Ctrl mapping via ctrlMappedKey (with optional Alt prefix).
    // Matches Kitty's encode_printable_ascii_key_legacy dispatch on remaining mods after Shift strip.
    if (effectiveMods == Modifier::Control
        || effectiveMods == (Modifiers { Modifier::Control } | Modifier::Alt))
    {
        auto const mapped = ctrlMappedKey(characterEvent);
        if (mapped.has_value())
        {
            if (effectiveMods.contains(Modifier::Alt))
                append('\x1b');
            append(*mapped);
            return true;
        }
    }

    // No effective modifiers or Alt-only: send the character (with optional ESC prefix for Alt).
    if (effectiveMods.none() || effectiveMods == Modifier::Alt)
    {
        if (effectiveMods.contains(Modifier::Alt))
            append('\x1b');
        if (characterEvent < 0x7F)
            append(static_cast<char>(characterEvent));
        else
            append(unicode::convert_to<char>(characterEvent));
        return true;
    }

    // Fallback: send raw character for unhandled modifier combinations.
    if (characterEvent < 0x7F)
        append(static_cast<char>(characterEvent));
    else
        append(unicode::convert_to<char>(characterEvent));

    inputLog()("Sending {} \"{}\".", modifiers, crispy::escape(unicode::convert_to<char>(characterEvent)));
    return true;
}

std::string StandardKeyboardInputGenerator::selectNumpad(Modifiers modifiers,
                                                         FunctionKeyMapping mapping) const
{
    if (modifiers.contains(Modifier::NumLock))
        return select(modifiers, { .std = mapping.std, .mods = mapping.std, .appKeypad = mapping.std });

    return select(modifiers.without(Modifier::NumLock), mapping);
}

std::string StandardKeyboardInputGenerator::select(Modifiers modifiers, FunctionKeyMapping mapping) const
{
    if (modifiers.without(Modifier::NumLock) && !mapping.mods.empty())
        return crispy::replace(
            mapping.mods, "{}"sv, makeVirtualTerminalParam(modifiers.without(Modifier::NumLock)));

    auto const prefix = modifiers.contains(Modifier::Alt) ? "\033" : ""s;

    if (applicationCursorKeys() && !mapping.appCursor.empty())
        return prefix + std::string(mapping.appCursor);

    if (applicationKeypad() && !mapping.appKeypad.empty())
        return prefix + std::string(mapping.appKeypad);

    return prefix + std::string(mapping.std);
}

bool StandardKeyboardInputGenerator::generateKey(Key key, Modifiers modifiers, KeyboardEventType eventType)
{
    if (eventType == KeyboardEventType::Release)
        return false;

    // clang-format off
    switch (key)
    {
        case Key::F1: append(select(modifiers, { .std = ESC "OP", .mods = CSI "1;{}P" })); break;
        case Key::F2: append(select(modifiers, { .std = ESC "OQ", .mods = CSI "1;{}Q" })); break;
        case Key::F3: append(select(modifiers, { .std = ESC "OR", .mods = CSI "13;{}~" })); break;
        case Key::F4: append(select(modifiers, { .std = ESC "OS", .mods = CSI "1;{}S" })); break;
        case Key::F5: append(select(modifiers, { .std = CSI "15~", .mods = CSI "15;{}~" })); break;
        case Key::F6: append(select(modifiers, { .std = CSI "17~", .mods = CSI "17;{}~" })); break;
        case Key::F7: append(select(modifiers, { .std = CSI "18~", .mods = CSI "18;{}~" })); break;
        case Key::F8: append(select(modifiers, { .std = CSI "19~", .mods = CSI "19;{}~" })); break;
        case Key::F9: append(select(modifiers, { .std = CSI "20~", .mods = CSI "20;{}~" })); break;
        case Key::F10: append(select(modifiers, { .std = CSI "21~", .mods = CSI "21;{}~" })); break;
        case Key::F11: append(select(modifiers, { .std = CSI "23~", .mods = CSI "23;{}~" })); break;
        case Key::F12: append(select(modifiers, { .std = CSI "24~", .mods = CSI "24;{}~" })); break;
        case Key::F13: append(select(modifiers, { .std = CSI "25~", .mods = CSI "25;{}~" })); break;
        case Key::F14: append(select(modifiers, { .std = CSI "26~", .mods = CSI "26;{}~" })); break;
        case Key::F15: append(select(modifiers, { .std = CSI "28~", .mods = CSI "28;{}~" })); break;
        case Key::F16: append(select(modifiers, { .std = CSI "29~", .mods = CSI "29;{}~" })); break;
        case Key::F17: append(select(modifiers, { .std = CSI "31~", .mods = CSI "31;{}~" })); break;
        case Key::F18: append(select(modifiers, { .std = CSI "32~", .mods = CSI "32;{}~" })); break;
        case Key::F19: append(select(modifiers, { .std = CSI "33~", .mods = CSI "33;{}~" })); break;
        case Key::F20: append(select(modifiers, { .std = CSI "34~", .mods = CSI "34;{}~" })); break;
        case Key::F21: append(select(modifiers, { .std = CSI "35~", .mods = CSI "35;{}~" })); break;
        case Key::F22: append(select(modifiers, { .std = CSI "36~", .mods = CSI "36;{}~" })); break;
        case Key::F23: append(select(modifiers, { .std = CSI "37~", .mods = CSI "37;{}~" })); break;
        case Key::F24: append(select(modifiers, { .std = CSI "38~", .mods = CSI "38;{}~" })); break;
        case Key::F25: append(select(modifiers, { .std = CSI "39~", .mods = CSI "39;{}~" })); break;
        case Key::F26: append(select(modifiers, { .std = CSI "40~", .mods = CSI "40;{}~" })); break;
        case Key::F27: append(select(modifiers, { .std = CSI "41~", .mods = CSI "41;{}~" })); break;
        case Key::F28: append(select(modifiers, { .std = CSI "42~", .mods = CSI "42;{}~" })); break;
        case Key::F29: append(select(modifiers, { .std = CSI "43~", .mods = CSI "43;{}~" })); break;
        case Key::F30: append(select(modifiers, { .std = CSI "44~", .mods = CSI "44;{}~" })); break;
        case Key::F31: append(select(modifiers, { .std = CSI "45~", .mods = CSI "45;{}~" })); break;
        case Key::F32: append(select(modifiers, { .std = CSI "46~", .mods = CSI "46;{}~" })); break;
        case Key::F33: append(select(modifiers, { .std = CSI "47~", .mods = CSI "47;{}~" })); break;
        case Key::F34: append(select(modifiers, { .std = CSI "48~", .mods = CSI "48;{}~" })); break;
        case Key::F35: append(select(modifiers, { .std = CSI "49~", .mods = CSI "49;{}~" })); break;
        case Key::Escape:
        {
            // Alt+Escape sends ESC ESC (Kitty: prefix = mods & ALT ? "\x1b" : "").
            auto const hasAlt = modifiers.contains(Modifier::Alt);
            append(hasAlt ? "\033\033" : "\033");
            break;
        }
        case Key::Enter: append(select(modifiers, { .std = "\r" })); break;
        case Key::Tab:
        {
            // Explicit Tab/Backtab handling with Alt prefix support.
            if (modifiers.contains(Modifier::Shift))
            {
                auto const hasAlt = modifiers.contains(Modifier::Alt);
                append(hasAlt ? "\x1b\x1b[Z" : "\x1b[Z");
            }
            else
            {
                auto const hasAlt = modifiers.contains(Modifier::Alt);
                append(hasAlt ? "\x1b\t" : "\t");
            }
            break;
        }
        case Key::Backspace:
        {
            // DECBKM swaps the default byte sent by Backspace (see generateChar for details).
            auto const hasCtrl = static_cast<bool>(modifiers & Modifier::Control);
            auto const sendBS = _backarrowKey ? !hasCtrl : hasCtrl;
            append(select(modifiers, { .std = sendBS ? "\x08" : "\x7F" }));
            break;
        }
        case Key::UpArrow: append(select(modifiers, { .std = CSI "A", .mods = CSI "1;{}A", .appCursor = SS3 "A" })); break;
        case Key::DownArrow: append(select(modifiers, { .std = CSI "B", .mods = CSI "1;{}B", .appCursor = SS3 "B" })); break;
        case Key::RightArrow: append(select(modifiers, { .std = CSI "C", .mods = CSI "1;{}C", .appCursor = SS3 "C" })); break;
        case Key::LeftArrow: append(select(modifiers, { .std = CSI "D", .mods = CSI "1;{}D", .appCursor = SS3 "D" })); break;
        case Key::Home: append(select(modifiers, { .std = CSI "H", .mods = CSI "1;{}H", .appCursor = SS3 "H" })); break;
        case Key::End: append(select(modifiers, { .std = CSI "F", .mods = CSI "1;{}F", .appCursor = SS3 "F" })); break;
        case Key::PageUp: append(select(modifiers, { .std = CSI "5~", .mods = CSI "5;{}~", .appKeypad = CSI "5~" })); break;
        case Key::PageDown: append(select(modifiers, { .std = CSI "6~", .mods = CSI "6;{}~", .appKeypad = CSI "6~" })); break;
        case Key::Insert: append(select(modifiers, { .std = CSI "2~", .mods = CSI "2;{}~" })); break;
        case Key::Delete: append(select(modifiers, { .std = CSI "3~", .mods = CSI "3;{}~" })); break;
        case Key::Numpad_Enter:    append(selectNumpad(modifiers, { .std = "\r", .appKeypad = SS3 "M" })); break;
        case Key::Numpad_Multiply: append(selectNumpad(modifiers, { .std = "*",  .appKeypad = SS3 "j" })); break;
        case Key::Numpad_Add:      append(selectNumpad(modifiers, { .std = "+",  .appKeypad = SS3 "k" })); break;
        case Key::Numpad_Subtract: append(selectNumpad(modifiers, { .std = "-",  .appKeypad = SS3 "m" })); break;
        case Key::Numpad_Decimal:  append(selectNumpad(modifiers, { .std = ".",  .appKeypad = CSI "3~" })); break;
        case Key::Numpad_Divide:   append(selectNumpad(modifiers, { .std = "/",  .appKeypad = SS3 "o" })); break;
        case Key::Numpad_0:        append(selectNumpad(modifiers, { .std = "0",  .appKeypad = CSI "2~" })); break;
        case Key::Numpad_1:        append(selectNumpad(modifiers, { .std = "1",  .appKeypad = SS3 "F" })); break;
        case Key::Numpad_2:        append(selectNumpad(modifiers, { .std = "2",  .appKeypad = CSI "B" })); break;
        case Key::Numpad_3:        append(selectNumpad(modifiers, { .std = "3",  .appKeypad = CSI "6~" })); break;
        case Key::Numpad_4:        append(selectNumpad(modifiers, { .std = "4",  .appKeypad = CSI "D" })); break;
        case Key::Numpad_5:        append(selectNumpad(modifiers, { .std = "5",  .appKeypad = CSI "E" })); break;
        case Key::Numpad_6:        append(selectNumpad(modifiers, { .std = "6",  .appKeypad = CSI "C" })); break;
        case Key::Numpad_7:        append(selectNumpad(modifiers, { .std = "7",  .appKeypad = SS3 "H" })); break;
        case Key::Numpad_8:        append(selectNumpad(modifiers, { .std = "8",  .appKeypad = CSI "A" })); break;
        case Key::Numpad_9:        append(selectNumpad(modifiers, { .std = "9",  .appKeypad = CSI "5~" })); break;
        case Key::Numpad_Equal:    append(selectNumpad(modifiers, { .std = "=",  .appKeypad = SS3 "X" })); break;
        // {{{ unsupported keys in legacy input protocol
        case Key::MediaPlay:
        case Key::MediaStop:
        case Key::MediaPrevious:
        case Key::MediaNext:
        case Key::MediaPause:
        case Key::MediaTogglePlayPause:
        case Key::VolumeUp:
        case Key::VolumeDown:
        case Key::VolumeMute:
        case Key::LeftShift:
        case Key::RightShift:
        case Key::LeftControl:
        case Key::RightControl:
        case Key::LeftAlt:
        case Key::RightAlt:
        case Key::LeftSuper:
        case Key::RightSuper:
        case Key::LeftHyper:
        case Key::RightHyper:
        case Key::LeftMeta:
        case Key::RightMeta:
        case Key::IsoLevel3Shift:
        case Key::IsoLevel5Shift:
        case Key::CapsLock:
        case Key::ScrollLock:
        case Key::NumLock:
        case Key::PrintScreen:
        case Key::Pause:
        case Key::Menu:
            return false;
        // }}}
    }
    // clang-format on

    return true;
}
// }}}

// {{{ ExtendedKeyboardInputGenerator

bool ExtendedKeyboardInputGenerator::generateChar(char32_t characterEvent,
                                                  uint32_t physicalKey,
                                                  Modifiers modifiers,
                                                  KeyboardEventType eventType)
{
    if (!enabled(eventType))
        return false;

    if (!isNonLegacyMode())
        return StandardKeyboardInputGenerator::generateChar(
            characterEvent, physicalKey, modifiers, eventType);

    auto const hasRealMods = !hasOnlyLockModifiers(modifiers);
    auto const needsAction =
        enabled(KeyboardEventFlag::ReportEventTypes) && eventType != KeyboardEventType::Press;
    auto const reportAllKeys = enabled(KeyboardEventFlag::ReportAllKeysAsEscapeCodes);
    auto const disambiguate = enabled(KeyboardEventFlag::DisambiguateEscapeCodes);

    // No real mods, no action encoding needed, not report-all-keys: legacy UTF-8
    if (!hasRealMods && !needsAction && !reportAllKeys)
        return StandardKeyboardInputGenerator::generateChar(
            characterEvent, physicalKey, modifiers, eventType);

    // Printable chars with only Shift modifier are unambiguous: stay in legacy mode.
    // CSI u is only needed when mods other than Shift are present (or reportAllKeys is set).
    auto const hasOnlyShiftOrLocks = modifiers.without(LockModifiers).without(Modifier::Shift).none();
    auto const canUseLegacyEncoding = hasOnlyShiftOrLocks || !disambiguate;
    if (!needsAction && !reportAllKeys && canUseLegacyEncoding)
        return StandardKeyboardInputGenerator::generateChar(
            characterEvent, physicalKey, modifiers, eventType);

    // CSI u encoding (conditionally include semicolon only when modifiers non-empty)
    auto const encodedMods = encodeModifiers(modifiers, eventType);
    auto const modsPart = encodedMods.empty() ? std::string {} : std::format(";{}", encodedMods);
    append("\033[{}{}u", encodeCharacter(characterEvent, physicalKey, modifiers), modsPart);
    return true;
}

constexpr unsigned encodeEventType(KeyboardEventType eventType) noexcept
{
    return static_cast<unsigned>(eventType);
}

std::string ExtendedKeyboardInputGenerator::encodeModifiers(Modifiers modifiers,
                                                            KeyboardEventType eventType) const
{
    // Per Kitty spec: action is omitted for Press (the default), only encoded for Repeat/Release.
    if (enabled(KeyboardEventFlag::ReportEventTypes) && eventType != KeyboardEventType::Press)
        return std::format("{}:{}", 1 + modifiers.value(), encodeEventType(eventType));

    if (modifiers.value() != 0)
        return std::to_string(1 + modifiers.value());

    return "";
}

std::string ExtendedKeyboardInputGenerator::encodeCharacter(char32_t ch,
                                                            uint32_t physicalKey,
                                                            Modifiers modifiers) const
{
    // Per Kitty spec: unicode-key-code is always the un-shifted base key.
    // For letters, simple_lowercase normalizes both ch and physicalKey (A→a).
    // For non-letter shifted symbols (Shift+3→#), simple_lowercase('#')='#'
    // which is WRONG — the base key '3' (from physicalKey) must be used instead.
    // We apply simple_lowercase to physicalKey to normalize letter keys that may
    // arrive as uppercase from the platform (e.g., physicalKey='L' for the L key).
    auto const baseKey =
        (physicalKey >= 32 && physicalKey < 0x110000)
            ? static_cast<uint32_t>(unicode::simple_lowercase(static_cast<char32_t>(physicalKey)))
            : static_cast<uint32_t>(unicode::simple_lowercase(ch));
    auto result = std::to_string(baseKey);

    if (enabled(KeyboardEventFlag::ReportAlternateKeys))
    {
        // Shifted key = the character actually produced when Shift is active.
        // For Shift+3→'#': shiftedKey=35. For Shift+A→'A': shiftedKey=65.
        uint32_t shiftedKey = 0;
        if (modifiers.contains(Modifier::Shift))
        {
            auto const producedKey = static_cast<uint32_t>(ch);
            if (producedKey != baseKey)
                shiftedKey = producedKey;
        }

        bool const showPhysicalKey = physicalKey && physicalKey != baseKey && physicalKey != shiftedKey;

        if (shiftedKey || showPhysicalKey)
            result += ':';
        if (shiftedKey)
            result += std::to_string(shiftedKey);

        // The base layout key is the key corresponding to the physical key in the standard PC-101 key layout
        if (showPhysicalKey)
        {
            result += ':';
            result += std::to_string(physicalKey);
        }
    }

    return result;
}

constexpr pair<unsigned, char> mapKey(Key key) noexcept
{
    switch (key)
    {
        case Key::Escape: return { 27, 'u' };
        case Key::Enter: return { 13, 'u' };
        case Key::Tab: return { 9, 'u' };
        case Key::Backspace: return { 127, 'u' };
        case Key::Insert: return { 2, '~' };
        case Key::Delete: return { 3, '~' };
        case Key::LeftArrow: return { 1, 'D' };
        case Key::RightArrow: return { 1, 'C' };
        case Key::UpArrow: return { 1, 'A' };
        case Key::DownArrow: return { 1, 'B' };
        case Key::PageUp: return { 5, '~' };
        case Key::PageDown: return { 6, '~' };
        case Key::Home: return { 1, 'H' };
        case Key::End: return { 1, 'F' };
        case Key::CapsLock: return { 57358, 'u' };
        case Key::ScrollLock: return { 57359, 'u' };
        case Key::NumLock: return { 57360, 'u' };
        case Key::PrintScreen: return { 57361, 'u' };
        case Key::Pause: return { 57362, 'u' };
        case Key::Menu: return { 57363, 'u' };
        case Key::F1: return { 1, 'P' };
        case Key::F2: return { 1, 'Q' };
        case Key::F3: return { 13, '~' }; // Tilde-form to avoid conflict with CSI R (Cursor Position Report)
        case Key::F4: return { 1, 'S' };
        case Key::F5: return { 15, '~' };
        case Key::F6: return { 17, '~' };
        case Key::F7: return { 18, '~' };
        case Key::F8: return { 19, '~' };
        case Key::F9: return { 20, '~' };
        case Key::F10: return { 21, '~' };
        case Key::F11: return { 23, '~' };
        case Key::F12: return { 24, '~' };
        case Key::F13: return { 57376, 'u' };
        case Key::F14: return { 57377, 'u' };
        case Key::F15: return { 57378, 'u' };
        case Key::F16: return { 57379, 'u' };
        case Key::F17: return { 57380, 'u' };
        case Key::F18: return { 57381, 'u' };
        case Key::F19: return { 57382, 'u' };
        case Key::F20: return { 57383, 'u' };
        case Key::F21: return { 57384, 'u' };
        case Key::F22: return { 57385, 'u' };
        case Key::F23: return { 57386, 'u' };
        case Key::F24: return { 57387, 'u' };
        case Key::F25: return { 57388, 'u' };
        case Key::F26: return { 57389, 'u' };
        case Key::F27: return { 57390, 'u' };
        case Key::F28: return { 57391, 'u' };
        case Key::F29: return { 57392, 'u' };
        case Key::F30: return { 57393, 'u' };
        case Key::F31: return { 57394, 'u' };
        case Key::F32: return { 57395, 'u' };
        case Key::F33: return { 57396, 'u' };
        case Key::F34: return { 57397, 'u' };
        case Key::F35: return { 57398, 'u' };
        case Key::MediaPlay: return { 57428, 'u' };
        case Key::MediaPause: return { 57429, 'u' };
        case Key::MediaTogglePlayPause: return { 57430, 'u' };
        case Key::MediaStop: return { 57432, 'u' };
        case Key::MediaNext: return { 57435, 'u' };
        case Key::MediaPrevious: return { 57436, 'u' };
        case Key::VolumeDown: return { 57438, 'u' };
        case Key::VolumeUp: return { 57439, 'u' };
        case Key::VolumeMute: return { 57440, 'u' };
        case Key::LeftShift: return { 57441, 'u' };
        case Key::LeftControl: return { 57442, 'u' };
        case Key::LeftAlt: return { 57443, 'u' };
        case Key::LeftSuper: return { 57444, 'u' };
        case Key::LeftHyper: return { 57445, 'u' };
        case Key::LeftMeta: return { 57446, 'u' };
        case Key::RightShift: return { 57447, 'u' };
        case Key::RightControl: return { 57448, 'u' };
        case Key::RightAlt: return { 57449, 'u' };
        case Key::RightSuper: return { 57450, 'u' };
        case Key::RightHyper: return { 57451, 'u' };
        case Key::RightMeta: return { 57452, 'u' };
        case Key::IsoLevel3Shift: return { 57453, 'u' };
        case Key::IsoLevel5Shift: return { 57454, 'u' };
        case Key::Numpad_0: return { 57399, 'u' };
        case Key::Numpad_1: return { 57400, 'u' };
        case Key::Numpad_2: return { 57401, 'u' };
        case Key::Numpad_3: return { 57402, 'u' };
        case Key::Numpad_4: return { 57403, 'u' };
        case Key::Numpad_5: return { 57404, 'u' };
        case Key::Numpad_6: return { 57405, 'u' };
        case Key::Numpad_7: return { 57406, 'u' };
        case Key::Numpad_8: return { 57407, 'u' };
        case Key::Numpad_9: return { 57408, 'u' };
        case Key::Numpad_Decimal: return { 57409, 'u' };
        case Key::Numpad_Divide: return { 57410, 'u' };
        case Key::Numpad_Multiply: return { 57411, 'u' };
        case Key::Numpad_Subtract: return { 57412, 'u' };
        case Key::Numpad_Add: return { 57413, 'u' };
        case Key::Numpad_Enter: return { 57414, 'u' };
        case Key::Numpad_Equal: return { 57415, 'u' };
    }

    // TODO: implement me
    // case Key::Numpad_Separator: return { 57416, 'u' };
    // case Key::Numpad_Left: return { 57417, 'u' };
    // case Key::Numpad_Right: return { 57418, 'u' };
    // case Key::Numpad_Up: return { 57419, 'u' };
    // case Key::Numpad_Down: return { 57420, 'u' };
    // case Key::Numpad_PageUp: return { 57421, 'u' };
    // case Key::Numpad_PageDown: return { 57422, 'u' };
    // case Key::Numpad_Home: return { 57423, 'u' };
    // case Key::Numpad_End: return { 57424, 'u' };
    // case Key::Numpad_Insert: return { 57425, 'u' };
    // case Key::Numpad_Delete: return { 57426, 'u' };
    // case Key::Numpad_Begin: return { 57427, 'u' };

    crispy::unreachable();
}

/// Returns the associated text codepoint for a numpad key, or 0 if none.
/// Per the Kitty keyboard protocol, associated text is the character the key would produce.
constexpr char32_t numpadAssociatedText(Key key) noexcept
{
    switch (key)
    {
        case Key::Numpad_0: return U'0';
        case Key::Numpad_1: return U'1';
        case Key::Numpad_2: return U'2';
        case Key::Numpad_3: return U'3';
        case Key::Numpad_4: return U'4';
        case Key::Numpad_5: return U'5';
        case Key::Numpad_6: return U'6';
        case Key::Numpad_7: return U'7';
        case Key::Numpad_8: return U'8';
        case Key::Numpad_9: return U'9';
        case Key::Numpad_Decimal: return U'.';
        case Key::Numpad_Divide: return U'/';
        case Key::Numpad_Multiply: return U'*';
        case Key::Numpad_Subtract: return U'-';
        case Key::Numpad_Add: return U'+';
        case Key::Numpad_Enter: return U'\r';
        case Key::Numpad_Equal: return U'=';
        default: return 0;
    }
}

bool ExtendedKeyboardInputGenerator::generateKey(Key key, Modifiers modifiers, KeyboardEventType eventType)
{
    if (!enabled(eventType))
        return false;

    if (!isNonLegacyMode())
        return StandardKeyboardInputGenerator::generateKey(key, modifiers, eventType);

    // Enter/Tab/Backspace: legacy when only lock mods and not report-all-keys
    if (hasOnlyLockModifiers(modifiers) && !enabled(KeyboardEventFlag::ReportAllKeysAsEscapeCodes))
    {
        switch (key)
        {
            case Key::Enter:
            case Key::Tab:
            case Key::Backspace:
                return StandardKeyboardInputGenerator::generateKey(key, modifiers, eventType);
            default: break;
        }
    }

    if (isModifierKey(key) && !enabled(KeyboardEventFlag::ReportAllKeysAsEscapeCodes))
        return false;

    auto const [code, function] = mapKey(key);
    auto const encodedModifiers = encodeModifiers(modifiers, eventType);

    // Check if we should append associated text (third CSI u parameter)
    auto const associatedText =
        enabled(KeyboardEventFlag::ReportAssociatedText) ? numpadAssociatedText(key) : char32_t { 0 };

    // Per Kitty spec: omit key number when code==1 and no modifiers/alternates/text.
    auto controlSequence = std::string("\033[");
    if (code != 1 || !encodedModifiers.empty() || associatedText)
        controlSequence += std::to_string(code);
    if (!encodedModifiers.empty() || associatedText)
    {
        // When associated text is present, the modifier field must be emitted
        // (use "1" as default when no modifiers, per Kitty spec encoding: 1 + 0 = 1)
        controlSequence += std::format(";{}", encodedModifiers.empty() ? "1" : encodedModifiers);
    }
    if (associatedText)
        controlSequence += std::format(";{}", static_cast<unsigned>(associatedText));
    controlSequence += function;
    append(controlSequence);

    return true;
}
// }}}

// {{{ Win32 Input Mode (DEC private mode 9001)

// clang-format off

/// Maps Windows Virtual Key codes used in Win32 Input Mode.
namespace VK
{
    constexpr uint32_t Back      = 0x08;
    constexpr uint32_t Tab       = 0x09;
    constexpr uint32_t Return    = 0x0D;
    constexpr uint32_t Pause     = 0x13;
    constexpr uint32_t Capital   = 0x14;
    constexpr uint32_t Escape    = 0x1B;
    constexpr uint32_t Prior     = 0x21;
    constexpr uint32_t Next      = 0x22;
    constexpr uint32_t End       = 0x23;
    constexpr uint32_t Home      = 0x24;
    constexpr uint32_t Left      = 0x25;
    constexpr uint32_t Up        = 0x26;
    constexpr uint32_t Right     = 0x27;
    constexpr uint32_t Down      = 0x28;
    constexpr uint32_t Snapshot  = 0x2C;
    constexpr uint32_t Insert    = 0x2D;
    constexpr uint32_t Delete    = 0x2E;
    constexpr uint32_t LWin      = 0x5B;
    constexpr uint32_t RWin      = 0x5C;
    constexpr uint32_t Apps      = 0x5D;
    constexpr uint32_t Numpad0   = 0x60;
    constexpr uint32_t Numpad1   = 0x61;
    constexpr uint32_t Numpad2   = 0x62;
    constexpr uint32_t Numpad3   = 0x63;
    constexpr uint32_t Numpad4   = 0x64;
    constexpr uint32_t Numpad5   = 0x65;
    constexpr uint32_t Numpad6   = 0x66;
    constexpr uint32_t Numpad7   = 0x67;
    constexpr uint32_t Numpad8   = 0x68;
    constexpr uint32_t Numpad9   = 0x69;
    constexpr uint32_t Multiply  = 0x6A;
    constexpr uint32_t Add       = 0x6B;
    constexpr uint32_t Subtract  = 0x6D;
    constexpr uint32_t Decimal   = 0x6E;
    constexpr uint32_t Divide    = 0x6F;
    constexpr uint32_t F1        = 0x70;
    constexpr uint32_t F2        = 0x71;
    constexpr uint32_t F3        = 0x72;
    constexpr uint32_t F4        = 0x73;
    constexpr uint32_t F5        = 0x74;
    constexpr uint32_t F6        = 0x75;
    constexpr uint32_t F7        = 0x76;
    constexpr uint32_t F8        = 0x77;
    constexpr uint32_t F9        = 0x78;
    constexpr uint32_t F10       = 0x79;
    constexpr uint32_t F11       = 0x7A;
    constexpr uint32_t F12       = 0x7B;
    constexpr uint32_t F13       = 0x7C;
    constexpr uint32_t F14       = 0x7D;
    constexpr uint32_t F15       = 0x7E;
    constexpr uint32_t F16       = 0x7F;
    constexpr uint32_t F17       = 0x80;
    constexpr uint32_t F18       = 0x81;
    constexpr uint32_t F19       = 0x82;
    constexpr uint32_t F20       = 0x83;
    constexpr uint32_t F21       = 0x84;
    constexpr uint32_t F22       = 0x85;
    constexpr uint32_t F23       = 0x86;
    constexpr uint32_t F24       = 0x87;
    constexpr uint32_t NumLock   = 0x90;
    constexpr uint32_t Scroll    = 0x91;
    constexpr uint32_t Shift     = 0x10; // VK_SHIFT (generic)
    constexpr uint32_t Control   = 0x11; // VK_CONTROL (generic)
    constexpr uint32_t Menu      = 0x12; // VK_MENU / Alt (generic)
    constexpr uint32_t RMenu     = 0xA5;
    constexpr uint32_t MediaNextTrack     = 0xB0;
    constexpr uint32_t MediaPrevTrack     = 0xB1;
    constexpr uint32_t MediaStop          = 0xB2;
    constexpr uint32_t MediaPlayPause     = 0xB3;
    constexpr uint32_t VolumeMute         = 0xAD;
    constexpr uint32_t VolumeDown         = 0xAE;
    constexpr uint32_t VolumeUp           = 0xAF;
} // namespace VK

// clang-format on

/// Returns true if the given Key represents an enhanced key in Windows terminology.
/// Enhanced keys have E0 scan code prefixes on a standard 101/102-key keyboard layout.
/// This sets the ENHANCED_KEY (0x0100) flag in dwControlKeyState.
constexpr bool isEnhancedKey(Key key) noexcept
{
    // clang-format off
    switch (key)
    {
        // Dedicated navigation cluster (not numpad equivalents)
        case Key::UpArrow:
        case Key::DownArrow:
        case Key::LeftArrow:
        case Key::RightArrow:
        case Key::Home:
        case Key::End:
        case Key::Insert:
        case Key::Delete:
        case Key::PageUp:
        case Key::PageDown:
        // Numpad keys with E0 scan codes
        case Key::Numpad_Enter:
        case Key::Numpad_Divide:
        // Right-side modifiers (E0 prefix distinguishes from left-side)
        case Key::RightControl:
        case Key::RightAlt:
        // Other enhanced keys
        case Key::PrintScreen:
            return true;
        default:
            return false;
    }
    // clang-format on
}

constexpr uint32_t InputGenerator::keyToVirtualKeyCode(Key key)
{
    // clang-format off
    switch (key)
    {
        case Key::F1:  return VK::F1;
        case Key::F2:  return VK::F2;
        case Key::F3:  return VK::F3;
        case Key::F4:  return VK::F4;
        case Key::F5:  return VK::F5;
        case Key::F6:  return VK::F6;
        case Key::F7:  return VK::F7;
        case Key::F8:  return VK::F8;
        case Key::F9:  return VK::F9;
        case Key::F10: return VK::F10;
        case Key::F11: return VK::F11;
        case Key::F12: return VK::F12;
        case Key::F13: return VK::F13;
        case Key::F14: return VK::F14;
        case Key::F15: return VK::F15;
        case Key::F16: return VK::F16;
        case Key::F17: return VK::F17;
        case Key::F18: return VK::F18;
        case Key::F19: return VK::F19;
        case Key::F20: return VK::F20;
        case Key::F21: return VK::F21;
        case Key::F22: return VK::F22;
        case Key::F23: return VK::F23;
        case Key::F24: return VK::F24;
        case Key::F25: return VK::F24; // No VK for F25+, map to F24
        case Key::F26: return VK::F24;
        case Key::F27: return VK::F24;
        case Key::F28: return VK::F24;
        case Key::F29: return VK::F24;
        case Key::F30: return VK::F24;
        case Key::F31: return VK::F24;
        case Key::F32: return VK::F24;
        case Key::F33: return VK::F24;
        case Key::F34: return VK::F24;
        case Key::F35: return VK::F24;

        case Key::Escape:    return VK::Escape;
        case Key::Enter:     return VK::Return;
        case Key::Tab:       return VK::Tab;
        case Key::Backspace: return VK::Back;

        case Key::DownArrow:  return VK::Down;
        case Key::LeftArrow:  return VK::Left;
        case Key::RightArrow: return VK::Right;
        case Key::UpArrow:    return VK::Up;

        case Key::Insert:   return VK::Insert;
        case Key::Delete:   return VK::Delete;
        case Key::Home:     return VK::Home;
        case Key::End:      return VK::End;
        case Key::PageUp:   return VK::Prior;
        case Key::PageDown: return VK::Next;

        case Key::MediaPlay:            return VK::MediaPlayPause;
        case Key::MediaStop:            return VK::MediaStop;
        case Key::MediaPrevious:        return VK::MediaPrevTrack;
        case Key::MediaNext:            return VK::MediaNextTrack;
        case Key::MediaPause:           return VK::MediaPlayPause;
        case Key::MediaTogglePlayPause: return VK::MediaPlayPause;

        case Key::VolumeUp:   return VK::VolumeUp;
        case Key::VolumeDown: return VK::VolumeDown;
        case Key::VolumeMute: return VK::VolumeMute;

        case Key::LeftShift:    return VK::Shift;   // Generic VK_SHIFT, matching Windows KEY_EVENT_RECORD
        case Key::RightShift:   return VK::Shift;   // Qt cannot distinguish left/right
        case Key::LeftControl:  return VK::Control;  // Generic VK_CONTROL
        case Key::RightControl: return VK::Control;  // Qt cannot distinguish left/right
        case Key::LeftAlt:      return VK::Menu;     // Generic VK_MENU
        case Key::RightAlt:     return VK::Menu;     // Qt cannot distinguish left/right
        case Key::LeftSuper:    return VK::LWin;
        case Key::RightSuper:   return VK::RWin;
        case Key::LeftHyper:    return VK::LWin;
        case Key::RightHyper:   return VK::RWin;
        case Key::LeftMeta:     return VK::LWin;
        case Key::RightMeta:    return VK::RWin;
        case Key::IsoLevel3Shift: return VK::RMenu;
        case Key::IsoLevel5Shift: return VK::RMenu;

        case Key::CapsLock:    return VK::Capital;
        case Key::ScrollLock:  return VK::Scroll;
        case Key::NumLock:     return VK::NumLock;
        case Key::PrintScreen: return VK::Snapshot;
        case Key::Pause:       return VK::Pause;
        case Key::Menu:        return VK::Apps;

        case Key::Numpad_Divide:   return VK::Divide;
        case Key::Numpad_Multiply: return VK::Multiply;
        case Key::Numpad_Subtract: return VK::Subtract;
        case Key::Numpad_Add:      return VK::Add;
        case Key::Numpad_Decimal:  return VK::Decimal;
        case Key::Numpad_Enter:    return VK::Return;
        case Key::Numpad_Equal:    return VK::Return; // No VK_NUMPAD_EQUAL, use Return
        case Key::Numpad_0: return VK::Numpad0;
        case Key::Numpad_1: return VK::Numpad1;
        case Key::Numpad_2: return VK::Numpad2;
        case Key::Numpad_3: return VK::Numpad3;
        case Key::Numpad_4: return VK::Numpad4;
        case Key::Numpad_5: return VK::Numpad5;
        case Key::Numpad_6: return VK::Numpad6;
        case Key::Numpad_7: return VK::Numpad7;
        case Key::Numpad_8: return VK::Numpad8;
        case Key::Numpad_9: return VK::Numpad9;
    }
    // clang-format on
    return 0;
}

constexpr Win32ControlKeyState InputGenerator::buildWin32ControlKeyState(Modifiers modifiers)
{
    auto state = Win32ControlKeyState {};
    if (modifiers.test(Modifier::Shift))
        state.enable(Win32ControlKeyFlag::ShiftPressed);
    if (modifiers.test(Modifier::Alt))
        state.enable(Win32ControlKeyFlag::LeftAltPressed);
    if (modifiers.test(Modifier::Control))
        state.enable(Win32ControlKeyFlag::LeftCtrlPressed);
    if (modifiers.test(Modifier::CapsLock))
        state.enable(Win32ControlKeyFlag::CapsLockOn);
    if (modifiers.test(Modifier::NumLock))
        state.enable(Win32ControlKeyFlag::NumLockOn);
    return state;
}

bool InputGenerator::generateWin32KeyInput(uint32_t virtualKeyCode,
                                           char32_t unicodeChar,
                                           Modifiers modifiers,
                                           KeyboardEventType eventType,
                                           Win32ControlKeyState extraControlKeyState)
{
    // Format: CSI Vk ; Sc ; Uc ; Kd ; Cs ; Rc _
    auto const kd = (eventType == KeyboardEventType::Release) ? 0u : 1u;
    auto const cs = buildWin32ControlKeyState(modifiers).with(extraControlKeyState);

    // Apply Ctrl mapping to the unicode character, matching Windows KEY_EVENT_RECORD behavior.
    // When Ctrl is held, Windows reports the control character (e.g., Ctrl+R → 0x12),
    // not the raw letter. Without this, ConPTY/PSReadLine may not recognize Ctrl+key combos.
    auto uc = static_cast<uint32_t>(unicodeChar);
    if (modifiers.test(Modifier::Control))
    {
        if (auto const mapped = ctrlMappedKey(unicodeChar); mapped.has_value())
            uc = static_cast<uint32_t>(static_cast<unsigned char>(*mapped));
    }

    append(std::format("\033[{};{};{};{};{};{}_",
                       virtualKeyCode,
                       0u, // scan code (not available from Qt)
                       uc,
                       kd,
                       cs.value(),
                       1u)); // repeat count
    inputLog()("Sending Win32 input: VK={:#x} UC={:#x} KD={} CS={:#x} {}.",
               virtualKeyCode,
               uc,
               kd,
               cs.value(),
               eventType);
    return true;
}

// }}}

void InputGenerator::reset()
{
    _keyboardInputGenerator.reset();
    _bracketedPaste = false;
    _generateFocusEvents = false;
    _win32InputMode = false;
    _mouseProtocol = std::nullopt;
    _mouseTransport = MouseTransport::Default;
    _mouseWheelMode = MouseWheelMode::Default;
    _modifyOtherKeys = 0;

    // _pendingSequence = {};
    // _currentMousePosition = {0, 0}; // current mouse position
    // _currentlyPressedMouseButtons = {};
}

void InputGenerator::setCursorKeysMode(KeyMode mode)
{
    inputLog()("set cursor keys mode: {}", mode);
    _keyboardInputGenerator.setCursorKeysMode(mode);
}

void InputGenerator::setNumpadKeysMode(KeyMode mode)
{
    inputLog()("set numpad keys mode: {}", mode);
    _keyboardInputGenerator.setNumpadKeysMode(mode);
}

void InputGenerator::setApplicationKeypadMode(bool enable)
{
    _keyboardInputGenerator.setApplicationKeypadMode(enable);
    inputLog()("set application keypad mode: {}", enable);
}

void InputGenerator::setBackarrowKeyMode(bool enable)
{
    _keyboardInputGenerator.setBackarrowKeyMode(enable);
    inputLog()("set backarrow key mode: {}", enable);
}

bool InputGenerator::generate(char32_t characterEvent,
                              uint32_t physicalKey,
                              Modifiers modifiers,
                              KeyboardEventType eventType)
{
    // Win32 Input Mode supersedes all other keyboard protocols when active.
    // It is the native ConPTY input format and carries richer key information
    // (VK codes, scan codes) than CSI u in the Windows environment.
    if (_win32InputMode)
        return generateWin32KeyInput(physicalKey, characterEvent, modifiers, eventType);

    // modifyOtherKeys mode 2: emit CSI 27 ; modifier ; codepoint ~ for modified keys.
    // This takes precedence over the CSI u keyboard protocol but only when no CSI u flags are active.
    if (_modifyOtherKeys == 2 && !_keyboardInputGenerator.flags().any()
        && eventType != KeyboardEventType::Release && modifiers.without(Modifier::Shift).any()
        && characterEvent < 0x110000)
    {
        auto const mod = makeVirtualTerminalParam(modifiers);
        append(std::format("\033[27;{};{}~", mod, static_cast<uint32_t>(characterEvent)));
        inputLog()("Sending modifyOtherKeys mode 2 {} \"{}\" {}.",
                   modifiers,
                   crispy::escape(unicode::convert_to<char>(characterEvent)),
                   eventType);
        return true;
    }

    bool const success =
        _keyboardInputGenerator.generateChar(characterEvent, physicalKey, modifiers, eventType);

    if (success)
    {
        _pendingSequence += _keyboardInputGenerator.take();
        inputLog()("Sending {} \"{}\" {}.",
                   modifiers,
                   crispy::escape(unicode::convert_to<char>(characterEvent)),
                   eventType);
    }

    return success;
}

/// Strips the modifier corresponding to a modifier-only key from the modifier set.
/// On key release, Windows KEY_EVENT_RECORDs reflect the post-release state
/// (e.g., releasing Alt clears LEFT_ALT_PRESSED from dwControlKeyState).
/// Qt may still report the pre-release modifier, so we strip it explicitly.
constexpr Modifiers stripSelfModifier(Key key, Modifiers modifiers) noexcept
{
    // clang-format off
    switch (key)
    {
        case Key::LeftShift:    case Key::RightShift:   return modifiers.without(Modifier::Shift);
        case Key::LeftControl:  case Key::RightControl: return modifiers.without(Modifier::Control);
        case Key::LeftAlt:      case Key::RightAlt:     return modifiers.without(Modifier::Alt);
        case Key::LeftSuper:    case Key::RightSuper:
        case Key::LeftMeta:     case Key::RightMeta:    return modifiers.without(Modifier::Super);
        default:                                        return modifiers;
    }
    // clang-format on
}

bool InputGenerator::generate(Key key, Modifiers modifiers, KeyboardEventType eventType)
{
    if (_win32InputMode)
    {
        auto effectiveModifiers = modifiers;
        if (eventType == KeyboardEventType::Release)
            effectiveModifiers = stripSelfModifier(key, effectiveModifiers);
        auto const extra = isEnhancedKey(key) ? Win32ControlKeyState { Win32ControlKeyFlag::EnhancedKey }
                                              : Win32ControlKeyState {};
        return generateWin32KeyInput(keyToVirtualKeyCode(key), 0, effectiveModifiers, eventType, extra);
    }

    bool const success = _keyboardInputGenerator.generateKey(key, modifiers, eventType);

    if (success)
    {
        _pendingSequence += _keyboardInputGenerator.take();
        inputLog()("Sending {} \"{}\" {}.", modifiers, key, eventType);
    }

    return success;
}

void InputGenerator::generatePaste(std::string_view const& text)
{
    inputLog()("Sending paste of {} bytes.", text.size());

    if (text.empty())
        return;

    if (_bracketedPaste)
        append("\033[200~"sv);

    append(text);

    if (_bracketedPaste)
        append("\033[201~"sv);
}

inline bool InputGenerator::append(std::string_view sequence)
{
    _pendingSequence.insert(end(_pendingSequence), begin(sequence), end(sequence));
    return true;
}

inline bool InputGenerator::append(char asciiChar)
{
    _pendingSequence.push_back(asciiChar);
    return true;
}

inline bool InputGenerator::append(uint8_t byte)
{
    _pendingSequence.push_back(static_cast<char>(byte));
    return true;
}

inline bool InputGenerator::append(unsigned int asciiChar)
{
    char buf[16];
    int const n = snprintf(buf, sizeof(buf), "%u", asciiChar);
    return append(string_view(buf, static_cast<size_t>(n)));
}

bool InputGenerator::generateFocusInEvent()
{
    if (generateFocusEvents())
    {
        append("\033[I");
        inputLog()("Sending focus-in event.");
        return true;
    }
    return false;
}

bool InputGenerator::generateFocusOutEvent()
{
    if (generateFocusEvents())
    {
        append("\033[O");
        inputLog()("Sending focus-out event.");
        return true;
    }
    return true;
}

bool InputGenerator::generateRaw(std::string_view const& raw)
{
    append(raw);
    return true;
}

// {{{ mouse handling
void InputGenerator::setMouseProtocol(MouseProtocol mouseProtocol, bool enabled)
{
    if (enabled)
    {
        _mouseWheelMode = MouseWheelMode::Default;
        _mouseProtocol = mouseProtocol;
    }
    else
        _mouseProtocol = std::nullopt;
}

void InputGenerator::setMouseTransport(MouseTransport mouseTransport)
{
    _mouseTransport = mouseTransport;
}

void InputGenerator::setMouseWheelMode(MouseWheelMode mode) noexcept
{
    _mouseWheelMode = mode;
}

namespace
{
    constexpr uint8_t modifierBits(Modifiers modifiers) noexcept
    {
        uint8_t mods = 0;
        if (modifiers.contains(Modifier::Shift))
            mods |= 4;
        if (modifiers.contains(Modifier::Super))
            mods |= 8;
        if (modifiers.contains(Modifier::Control))
            mods |= 16;
        return mods;
    }

    constexpr uint8_t buttonNumber(MouseButton button) noexcept
    {
        switch (button)
        {
            case MouseButton::Left: return 0;
            case MouseButton::Middle: return 1;
            case MouseButton::Right: return 2;
            case MouseButton::Release: return 3;
            case MouseButton::WheelUp: return 4;
            case MouseButton::WheelDown: return 5;
            case MouseButton::WheelRight: return 6;
            case MouseButton::WheelLeft: return 7;
        }
        return 0; // should never happen
    }

    constexpr bool isMouseWheel(MouseButton button) noexcept
    {
        return button == MouseButton::WheelUp || button == MouseButton::WheelDown
               || button == MouseButton::WheelLeft || button == MouseButton::WheelRight;
    }

    constexpr uint8_t buttonX10(MouseButton button) noexcept
    {
        return isMouseWheel(button) ? uint8_t(buttonNumber(button) + 0x3c) : buttonNumber(button);
    }

    constexpr uint8_t buttonNormal(MouseButton button, InputGenerator::MouseEventType eventType) noexcept
    {
        return eventType == InputGenerator::MouseEventType::Release ? 3 : buttonX10(button);
    }
} // namespace

bool InputGenerator::generateMouse(MouseEventType eventType,
                                   Modifiers modifiers,
                                   MouseButton button,
                                   CellLocation pos,
                                   PixelCoordinate pixelPosition,
                                   bool uiHandled)
{
    if (!_mouseProtocol.has_value())
        return false;

    // std::cout << std::format("generateMouse({}/{}): button:{}, modifier:{}, at:{}, type:{}\n",
    //                          _mouseTransport, *_mouseProtocol,
    //                          button, modifier, pos, eventType);

    switch (*_mouseProtocol)
    {
        case MouseProtocol::X10: // Old X10 mouse protocol
            if (eventType == MouseEventType::Press)
                mouseTransport(
                    eventType, buttonX10(button), modifierBits(modifiers), pos, pixelPosition, uiHandled);
            return true;
        case MouseProtocol::NormalTracking: // Normal tracking mode, that's X10 with mouse release events and
                                            // modifiers
            if (eventType == MouseEventType::Press || eventType == MouseEventType::Release)
            {
                auto const buttonValue = _mouseTransport != MouseTransport::SGR
                                             ? buttonNormal(button, eventType)
                                             : buttonX10(button);
                mouseTransport(
                    eventType, buttonValue, modifierBits(modifiers), pos, pixelPosition, uiHandled);
            }
            return true;
        case MouseProtocol::ButtonTracking: // Button-event tracking protocol.
            // like normal event tracking, but with drag events
            if (eventType == MouseEventType::Press || eventType == MouseEventType::Drag
                || eventType == MouseEventType::Release)
            {
                auto const buttonValue = _mouseTransport != MouseTransport::SGR
                                             ? buttonNormal(button, eventType)
                                             : buttonX10(button);

                uint8_t const draggableButton =
                    eventType == MouseEventType::Drag ? uint8_t(buttonValue + 0x20) : buttonValue;

                mouseTransport(
                    eventType, draggableButton, modifierBits(modifiers), pos, pixelPosition, uiHandled);
                return true;
            }
            return false;
        case MouseProtocol::AnyEventTracking: // Like ButtonTracking but any motion events (not just dragging)
            // TODO: make sure we can receive mouse-move events even without mouse pressed.
            {
                auto const buttonValue = _mouseTransport != MouseTransport::SGR
                                             ? buttonNormal(button, eventType)
                                             : buttonX10(button);

                uint8_t const draggableButton =
                    eventType == MouseEventType::Drag ? uint8_t(buttonValue + 0x20) : buttonValue;

                mouseTransport(
                    eventType, draggableButton, modifierBits(modifiers), pos, pixelPosition, uiHandled);
            }
            return true;
        case MouseProtocol::HighlightTracking: // Highlight mouse tracking
            return false;                      // TODO: do we want to implement this?
    }

    return false;
}

bool InputGenerator::mouseTransport(MouseEventType eventType,
                                    uint8_t button,
                                    uint8_t modifier,
                                    CellLocation pos,
                                    PixelCoordinate pixelPosition,
                                    bool uiHandled)
{
    if (pos.line.value < 0 || pos.column.value < 0)
        // Negative coordinates are not supported. Avoid sending bad values.
        return true;

    switch (_mouseTransport)
    {
        case MouseTransport::Default: // mode: 9
            mouseTransportX10(button, modifier, pos);
            return true;
        case MouseTransport::Extended: // mode: 1005
            // TODO (like Default but with UTF-8 encoded coords)
            mouseTransportExtended(button, modifier, pos);
            return false;
        case MouseTransport::SGR: // mode: 1006
            return mouseTransportSGR(eventType, button, modifier, *pos.column + 1, *pos.line + 1, uiHandled);
        case MouseTransport::URXVT: // mode: 1015
            return mouseTransportURXVT(eventType, button, modifier, pos);
        case MouseTransport::SGRPixels: // mode: 1016
            return mouseTransportSGR(
                eventType, button, modifier, pixelPosition.x.value, pixelPosition.y.value, uiHandled);
    }

    return false;
}

bool InputGenerator::mouseTransportExtended(uint8_t button, uint8_t modifier, CellLocation pos)
{
    constexpr auto SkipCount = uint8_t { 0x20 }; // TODO std::numeric_limits<ControlCode>::max();
    constexpr auto MaxCoordValue = 2015;

    if (*pos.line < MaxCoordValue && *pos.column < MaxCoordValue)
    {
        auto const buttonValue = static_cast<uint8_t>(SkipCount + static_cast<uint8_t>(button | modifier));
        auto const line = static_cast<char32_t>(SkipCount + *pos.line + 1);
        auto const column = static_cast<char32_t>(SkipCount + *pos.column + 1);
        append("\033[M");
        append(buttonValue);
        append(unicode::convert_to<char>(column));
        append(unicode::convert_to<char>(line));
        return true;
    }
    else
        return false;
}

bool InputGenerator::mouseTransportX10(uint8_t button, uint8_t modifier, CellLocation pos)
{
    constexpr uint8_t SkipCount = 0x20; // TODO std::numeric_limits<ControlCode>::max();
    constexpr uint8_t MaxCoordValue = std::numeric_limits<uint8_t>::max() - SkipCount;

    if (std::cmp_less(unbox(pos.line), MaxCoordValue) && std::cmp_less(unbox(pos.column), MaxCoordValue))
    {
        auto const buttonValue = static_cast<uint8_t>(SkipCount + static_cast<uint8_t>(button | modifier));
        auto const line = static_cast<uint8_t>(SkipCount + *pos.line + 1);
        auto const column = static_cast<uint8_t>(SkipCount + *pos.column + 1);
        append("\033[M");
        append(buttonValue);
        append(column);
        append(line);
        return true;
    }
    else
        return false;
}

bool InputGenerator::mouseTransportSGR(
    MouseEventType eventType, uint8_t button, uint8_t modifier, int x, int y, bool uiHandled)
{
    append("\033[<");
    append(static_cast<unsigned>(button | modifier));
    append(';');
    append(static_cast<unsigned>(x));
    append(';');
    append(static_cast<unsigned>(y));

    if (_passiveMouseTracking)
    {
        append(';');
        append(uiHandled ? '1' : '0');
    }

    append(eventType != MouseEventType::Release ? 'M' : 'm');

    return true;
}

bool InputGenerator::mouseTransportURXVT(MouseEventType eventType,
                                         uint8_t button,
                                         uint8_t modifier,
                                         CellLocation pos)
{
    if (eventType == MouseEventType::Press)
    {
        append("\033[");
        append(static_cast<unsigned>(button | modifier));
        append(';');
        append(static_cast<unsigned>(*pos.column + 1));
        append(';');
        append(static_cast<unsigned>(*pos.line + 1));
        append('M');
    }
    return true;
}

bool InputGenerator::generateMousePress(
    Modifiers modifiers, MouseButton button, CellLocation pos, PixelCoordinate pixelPosition, bool uiHandled)
{
    auto const logged = [=](bool success) -> bool {
        if (success)
            inputLog()("Sending mouse press {} {} at {}.", button, modifiers, pos);
        return success;
    };

    _currentMousePosition = pos;

    if (!_mouseProtocol.has_value())
        return false;

    switch (mouseWheelMode())
    {
        case MouseWheelMode::NormalCursorKeys:
            if (_passiveMouseTracking)
                break;
            switch (button)
            {
                case MouseButton::WheelUp: return logged(append("\033[A"));
                case MouseButton::WheelDown: return logged(append("\033[B"));
                default: break;
            }
            break;
        case MouseWheelMode::ApplicationCursorKeys:
            if (_passiveMouseTracking)
                break;
            switch (button)
            {
                case MouseButton::WheelUp: return logged(append("\033OA"));
                case MouseButton::WheelDown: return logged(append("\033OB"));
                default: break;
            }
            break;
        case MouseWheelMode::Default: break;
    }

    if (!isMouseWheel(button))
        if (!_currentlyPressedMouseButtons.count(button))
            _currentlyPressedMouseButtons.insert(button);

    return logged(generateMouse(
        MouseEventType::Press, modifiers, button, _currentMousePosition, pixelPosition, uiHandled));
}

bool InputGenerator::generateMouseRelease(
    Modifiers modifiers, MouseButton button, CellLocation pos, PixelCoordinate pixelPosition, bool uiHandled)
{
    auto const logged = [=](bool success) -> bool {
        if (success)
            inputLog()("Sending mouse release {} {} at {}.", button, modifiers, pos);
        return success;
    };

    _currentMousePosition = pos;

    if (auto i = _currentlyPressedMouseButtons.find(button); i != _currentlyPressedMouseButtons.end())
        _currentlyPressedMouseButtons.erase(i);

    return logged(generateMouse(
        MouseEventType::Release, modifiers, button, _currentMousePosition, pixelPosition, uiHandled));
}

bool InputGenerator::generateMouseMove(Modifiers modifiers,
                                       CellLocation pos,
                                       PixelCoordinate pixelPosition,
                                       bool uiHandled)
{
    if (pos == _currentMousePosition && _mouseTransport != MouseTransport::SGRPixels)
        // Only generate a mouse move event if the coordinate of interest(!) has actually changed.
        return false;

    auto const logged = [&](bool success) -> bool {
        if (success)
        {
            inputLog()("[{}:{}] Sending mouse move at {} ({}:{}).",
                       _mouseProtocol.value(),
                       _mouseTransport,
                       pos,
                       pixelPosition.x.value,
                       pixelPosition.y.value);
        }
        return success;
    };

    _currentMousePosition = pos;

    if (!_mouseProtocol.has_value())
        return false;

    bool const buttonsPressed = !_currentlyPressedMouseButtons.empty();

    bool const report = (_mouseProtocol.value() == MouseProtocol::ButtonTracking && buttonsPressed)
                        || _mouseProtocol.value() == MouseProtocol::AnyEventTracking;

    if (report)
        return logged(generateMouse(
            MouseEventType::Drag,
            modifiers,
            buttonsPressed ? *_currentlyPressedMouseButtons.begin() // what if multiple are pressed?
                           : MouseButton::Release,
            pos,
            pixelPosition,
            uiHandled));

    return false;
}
// }}}

} // namespace vtbackend
