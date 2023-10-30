// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/ControlCode.h>
#include <vtbackend/InputGenerator.h>
#include <vtbackend/logging.h>

#include <crispy/assert.h>
#include <crispy/utils.h>

#include <libunicode/convert.h>

#include <fmt/format.h>

#include <array>
#include <iterator>
#include <string_view>
#include <unordered_map>

using namespace std;

#define ESC "\x1B"
#define CSI "\x1B["
#define SS3 "\x1BO"

namespace vtbackend
{

string to_string(Modifier modifier)
{
    return fmt::format("{}", modifier);
}

string to_string(Key key)
{
    return fmt::format("{}", key);
}

string to_string(MouseButton button)
{
    return fmt::format("{}", button);
}

// {{{ StandardKeyboardInputGenerator
bool StandardKeyboardInputGenerator::generateChar(char32_t characterEvent,
                                                  uint32_t physicalKey,
                                                  Modifier modifier,
                                                  KeyboardEventType eventType)
{
    crispy::ignore_unused(physicalKey);

    if (eventType == KeyboardEventType::Release)
        return false;

    char const chr = static_cast<char>(characterEvent);

    // See section "Alt and Meta Keys" in ctlseqs.txt from xterm.
    if (modifier == Modifier::Alt)
        // NB: There are other modes in xterm to send Alt+Key options or even send ESC on Meta key instead.
        append("\033");

    // Well accepted hack to distinguish between Backspace nad Ctrl+Backspace,
    // - Backspace is emitting 0x7f,
    // - Ctrl+Backspace is emitting 0x08
    if (characterEvent == 0x08)
    {
        if (!modifier.control())
            append("\x7f");
        else
            append("\x08");
        return true;
    }

    if (modifier == Modifier::Shift && characterEvent == 0x09)
    {
        append("\033[Z"); // introduced by linux_console in 1995, adopted by xterm in 2002
        return true;
    }

    // raw C0 code
    if (modifier == Modifier::Control && characterEvent < 32)
    {
        append(static_cast<uint8_t>(characterEvent));
        return true;
    }

    if (modifier == Modifier::Control && characterEvent == L' ')
    {
        append('\x00');
        return true;
    }

    if (modifier == Modifier::Control && crispy::ascending('A', chr, 'Z'))
    {
        append(static_cast<char>(chr - 'A' + 1));
        return true;
    }

    if (modifier == Modifier::Control && characterEvent >= '[' && characterEvent <= '_')
    {
        append(static_cast<char>(chr - 'A' + 1)); // remaining C0 characters 0x1B .. 0x1F
        return true;
    }

    if (modifier.without(Modifier::Alt).none() || modifier == Modifier::Shift)
    {
        append(unicode::convert_to<char>(characterEvent));
        return true;
    }

    if (characterEvent < 0x7F)
        append(static_cast<char>(characterEvent));
    else
        append(unicode::convert_to<char>(characterEvent));

    inputLog()("Sending {} \"{}\".", modifier, crispy::escape(unicode::convert_to<char>(characterEvent)));
    return true;
}

std::string StandardKeyboardInputGenerator::select(Modifier modifier, FunctionKeyMapping mapping) const
{
    auto const prefix = modifier.alt() ? "\033" : ""s;

    if (applicationCursorKeys() && !mapping.appCursor.empty())
        return prefix + std::string(mapping.appCursor);

    if (applicationKeypad() && !mapping.appKeypad.empty())
        return prefix + std::string(mapping.appKeypad);

    if (modifier && !mapping.mods.empty())
        return prefix + crispy::replace(mapping.mods, "{}"sv, makeVirtualTerminalParam(modifier));

    return prefix + std::string(mapping.std);
}

bool StandardKeyboardInputGenerator::generateKey(Key key, Modifier modifier, KeyboardEventType eventType)
{
    if (eventType == KeyboardEventType::Release)
        return false;

    // clang-format off
    switch (key)
    {
        case Key::F1: append(select(modifier, { .std = ESC "OP", .mods = ESC "O{}P" })); break;
        case Key::F2: append(select(modifier, { .std = ESC "OQ", .mods = ESC "O{}Q" })); break;
        case Key::F3: append(select(modifier, { .std = ESC "OR", .mods = ESC "O{}R" })); break;
        case Key::F4: append(select(modifier, { .std = ESC "OS", .mods = ESC "O{}S" })); break;
        case Key::F5: append(select(modifier, { .std = CSI "15~", .mods = CSI "15;{}~" })); break;
        case Key::F6: append(select(modifier, { .std = CSI "17~", .mods = CSI "17;{}~" })); break;
        case Key::F7: append(select(modifier, { .std = CSI "18~", .mods = CSI "18;{}~" })); break;
        case Key::F8: append(select(modifier, { .std = CSI "19~", .mods = CSI "19;{}~" })); break;
        case Key::F9: append(select(modifier, { .std = CSI "20~", .mods = CSI "20;{}~" })); break;
        case Key::F10: append(select(modifier, { .std = CSI "21~", .mods = CSI "21;{}~" })); break;
        case Key::F11: append(select(modifier, { .std = CSI "23~", .mods = CSI "23;{}~" })); break;
        case Key::F12: append(select(modifier, { .std = CSI "24~", .mods = CSI "24;{}~" })); break;
        case Key::F13: append(select(modifier, { .std = CSI "25~", .mods = CSI "25;{}~" })); break;
        case Key::F14: append(select(modifier, { .std = CSI "26~", .mods = CSI "26;{}~" })); break;
        case Key::F15: append(select(modifier, { .std = CSI "28~", .mods = CSI "28;{}~" })); break;
        case Key::F16: append(select(modifier, { .std = CSI "29~", .mods = CSI "29;{}~" })); break;
        case Key::F17: append(select(modifier, { .std = CSI "31~", .mods = CSI "31;{}~" })); break;
        case Key::F18: append(select(modifier, { .std = CSI "32~", .mods = CSI "32;{}~" })); break;
        case Key::F19: append(select(modifier, { .std = CSI "33~", .mods = CSI "33;{}~" })); break;
        case Key::F20: append(select(modifier, { .std = CSI "34~", .mods = CSI "34;{}~" })); break;
        case Key::F21: append(select(modifier, { .std = CSI "35~", .mods = CSI "35;{}~" })); break;
        case Key::F22: append(select(modifier, { .std = CSI "36~", .mods = CSI "36;{}~" })); break;
        case Key::F23: append(select(modifier, { .std = CSI "37~", .mods = CSI "37;{}~" })); break;
        case Key::F24: append(select(modifier, { .std = CSI "38~", .mods = CSI "38;{}~" })); break;
        case Key::F25: append(select(modifier, { .std = CSI "39~", .mods = CSI "39;{}~" })); break;
        case Key::F26: append(select(modifier, { .std = CSI "40~", .mods = CSI "40;{}~" })); break;
        case Key::F27: append(select(modifier, { .std = CSI "41~", .mods = CSI "41;{}~" })); break;
        case Key::F28: append(select(modifier, { .std = CSI "42~", .mods = CSI "42;{}~" })); break;
        case Key::F29: append(select(modifier, { .std = CSI "43~", .mods = CSI "43;{}~" })); break;
        case Key::F30: append(select(modifier, { .std = CSI "44~", .mods = CSI "44;{}~" })); break;
        case Key::F31: append(select(modifier, { .std = CSI "45~", .mods = CSI "45;{}~" })); break;
        case Key::F32: append(select(modifier, { .std = CSI "46~", .mods = CSI "46;{}~" })); break;
        case Key::F33: append(select(modifier, { .std = CSI "47~", .mods = CSI "47;{}~" })); break;
        case Key::F34: append(select(modifier, { .std = CSI "48~", .mods = CSI "48;{}~" })); break;
        case Key::F35: append(select(modifier, { .std = CSI "49~", .mods = CSI "49;{}~" })); break;
        case Key::Escape: append("\033"); break;
        case Key::Enter: append(select(modifier, { .std = "\r" })); break;
        case Key::Tab: append(select(modifier, { .std = "\t" })); break;
        case Key::Backspace:
            // Well accepted hack to distinguish between Backspace nad Ctrl+Backspace,
            // - Backspace is emitting 0x7f,
            // - Ctrl+Backspace is emitting 0x08
            append(select(modifier, { .std = !modifier.control() ? "\x7f" : "\x08" })); break;
        case Key::UpArrow: append(select(modifier, { .std = CSI "A", .mods = CSI "1;{}A", .appCursor = SS3 "A" })); break;
        case Key::DownArrow: append(select(modifier, { .std = CSI "B", .mods = CSI "1;{}B", .appCursor = SS3 "B" })); break;
        case Key::RightArrow: append(select(modifier, { .std = CSI "C", .mods = CSI "1;{}C", .appCursor = SS3 "C" })); break;
        case Key::LeftArrow: append(select(modifier, { .std = CSI "D", .mods = CSI "1;{}D", .appCursor = SS3 "D" })); break;
        case Key::Home: append(select(modifier, { .std = CSI "H", .mods = CSI "1;{}H", .appCursor = SS3 "H" })); break;
        case Key::End: append(select(modifier, { .std = CSI "F", .mods = CSI "1;{}F", .appCursor = SS3 "F" })); break;
        case Key::PageUp: append(select(modifier, { .std = CSI "5~", .mods = CSI "5;{}~", .appKeypad = CSI "5~" })); break;
        case Key::PageDown: append(select(modifier, { .std = CSI "6~", .mods = CSI "6;{}~", .appKeypad = CSI "6~" })); break;
        case Key::Insert: append(select(modifier, { .std = CSI "2~", .mods = CSI "2;{}~" })); break;
        case Key::Delete: append(select(modifier, { .std = CSI "3~", .mods = CSI "3;{}~" })); break;
        case Key::Numpad_Enter:    append(select(modifier, { .std = "\r", .appKeypad = SS3 "M" })); break;
        case Key::Numpad_Multiply: append(select(modifier, { .std = "*",  .appKeypad = SS3 "j" })); break;
        case Key::Numpad_Add:      append(select(modifier, { .std = "+",  .appKeypad = SS3 "k" })); break;
        case Key::Numpad_Subtract: append(select(modifier, { .std = "-",  .appKeypad = SS3 "m" })); break;
        case Key::Numpad_Decimal:  append(select(modifier, { .std = ".",  .appKeypad = CSI "3~" })); break;
        case Key::Numpad_Divide:   append(select(modifier, { .std = "/",  .appKeypad = SS3 "o" })); break;
        case Key::Numpad_0:        append(select(modifier, { .std = "0",  .appKeypad = CSI "2~" })); break;
        case Key::Numpad_1:        append(select(modifier, { .std = "1",  .appKeypad = SS3 "F" })); break;
        case Key::Numpad_2:        append(select(modifier, { .std = "2",  .appKeypad = CSI "B" })); break;
        case Key::Numpad_3:        append(select(modifier, { .std = "3",  .appKeypad = CSI "6~" })); break;
        case Key::Numpad_4:        append(select(modifier, { .std = "4",  .appKeypad = CSI "D" })); break;
        case Key::Numpad_5:        append(select(modifier, { .std = "5",  .appKeypad = CSI "E" })); break;
        case Key::Numpad_6:        append(select(modifier, { .std = "6",  .appKeypad = CSI "C" })); break;
        case Key::Numpad_7:        append(select(modifier, { .std = "7",  .appKeypad = SS3 "H" })); break;
        case Key::Numpad_8:        append(select(modifier, { .std = "8",  .appKeypad = CSI "A" })); break;
        case Key::Numpad_9:        append(select(modifier, { .std = "9",  .appKeypad = CSI "5~" })); break;
        case Key::Numpad_Equal:    append(select(modifier, { .std = "=",  .appKeypad = SS3 "X" })); break;
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
                                                  Modifier modifier,
                                                  KeyboardEventType eventType)
{
    if (enabled(eventType))
    {
        if (enabled(KeyboardEventFlag::DisambiguateEscapeCodes)
            && (modifier.any() || enabled(KeyboardEventFlag::ReportAllKeysAsEscapeCodes)))
        {
            append("\033[{};{}u",
                   encodeCharacter(characterEvent, physicalKey, modifier),
                   encodeModifiers(modifier, eventType));
            return true;
        }
    }

    return StandardKeyboardInputGenerator::generateChar(characterEvent, physicalKey, modifier, eventType);
}

constexpr unsigned encodeEventType(KeyboardEventType eventType) noexcept
{
    return static_cast<unsigned>(eventType);
}

std::string ExtendedKeyboardInputGenerator::encodeModifiers(Modifier modifier,
                                                            KeyboardEventType eventType) const
{
    if (enabled(KeyboardEventFlag::ReportEventTypes))
        return fmt::format("{}:{}", modifier.value(), encodeEventType(eventType));

    if (modifier.value() != 0)
        return std::to_string(1 + modifier.value());

    return "";
}

std::string ExtendedKeyboardInputGenerator::encodeCharacter(char32_t ch,
                                                            uint32_t physicalKey,
                                                            Modifier modifier) const
{
    // The codepoint is always the lower-case form
    // TODO: use libunicode for down-shifting
    auto unshiftedKey = ch < 0x80 ? fmt::format("{}", std::tolower(static_cast<char>(ch))) : ""s;

    auto result = std::move(unshiftedKey);

    if (enabled(KeyboardEventFlag::ReportAlternateKeys))
    {
        // The shifted key is simply the upper-case version of unicode-codepoint
        auto const shiftedKey = static_cast<uint32_t>(
            (modifier.shift() && (0x20 <= ch && ch < 0x80)) ? std::toupper(static_cast<char>(ch)) : 0);
        // TODO: use libunicode for up-shifting

        bool const showPhysicalKey = physicalKey && physicalKey != ch && physicalKey != shiftedKey;
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
        case Key::Home: return { 7, '~' }; // or 1 H
        case Key::End: return { 8, '~' };  // or 1 F
        case Key::CapsLock: return { 57358, 'u' };
        case Key::ScrollLock: return { 57359, 'u' };
        case Key::NumLock: return { 57360, 'u' };
        case Key::PrintScreen: return { 57361, 'u' };
        case Key::Pause: return { 57362, 'u' };
        case Key::Menu: return { 57363, 'u' };
        case Key::F1: return { 11, '~' }; // or 1 P
        case Key::F2: return { 12, '~' }; // or 1 Q
        case Key::F3: return { 13, '~' }; // or 1 R (not anymore)
        case Key::F4: return { 14, '~' }; // or 1 S
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

constexpr bool isModifierKey(Key key) noexcept
{
    // clang-format off
    switch (key)
    {
        case Key::LeftShift:
        case Key::LeftControl:
        case Key::LeftAlt:
        case Key::LeftSuper:
        case Key::LeftHyper:
        case Key::LeftMeta:
        case Key::RightShift:
        case Key::RightControl:
        case Key::RightAlt:
        case Key::RightSuper:
        case Key::RightHyper:
        case Key::RightMeta:
        case Key::IsoLevel3Shift:
        case Key::IsoLevel5Shift:
        case Key::CapsLock:
        case Key::NumLock:
            return true;
        default:
            return false;
    }
    // clang-format on
}

bool ExtendedKeyboardInputGenerator::generateKey(Key key, Modifier modifier, KeyboardEventType eventType)
{
    if (!enabled(eventType))
        return false;

    if (!enabled(KeyboardEventFlag::DisambiguateEscapeCodes))
        return StandardKeyboardInputGenerator::generateKey(key, modifier, eventType);

    if (modifier.none() && !enabled(KeyboardEventFlag::ReportAllKeysAsEscapeCodes))
    {
        // "The only exceptions are the Enter, Tab and Backspace keys which still generate the same bytes as
        // in legacy mode this is to allow the user to type and execute commands in the shell such as reset
        // after a program that sets this mode crashes without clearing it."
        switch (key)
        {
            case Key::Enter:
            case Key::Tab:
            case Key::Backspace: return StandardKeyboardInputGenerator::generateKey(key, modifier, eventType);
            default: break;
        }
    }

    if (isModifierKey(key) && !enabled(KeyboardEventFlag::ReportAllKeysAsEscapeCodes))
        return false;

    auto const [code, function] = mapKey(key);
    auto const encodedModifiers = encodeModifiers(modifier, eventType);
    auto controlSequence = fmt::format("\033[{}", code);
    if (!encodedModifiers.empty())
        controlSequence += fmt::format(";{}", encodedModifiers);
    controlSequence += function;
    append(controlSequence);

    return true;
}
// }}}

void InputGenerator::reset()
{
    _keyboardInputGenerator.reset();
    _bracketedPaste = false;
    _generateFocusEvents = false;
    _mouseProtocol = std::nullopt;
    _mouseTransport = MouseTransport::Default;
    _mouseWheelMode = MouseWheelMode::Default;

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

bool InputGenerator::generate(char32_t characterEvent,
                              uint32_t physicalKey,
                              Modifier modifier,
                              KeyboardEventType eventType)
{
    bool const success =
        _keyboardInputGenerator.generateChar(characterEvent, physicalKey, modifier, eventType);

    if (success)
    {
        _pendingSequence += _keyboardInputGenerator.take();
        inputLog()("Sending {} \"{}\" {}.",
                   modifier,
                   crispy::escape(unicode::convert_to<char>(characterEvent)),
                   eventType);
    }

    return success;
}

bool InputGenerator::generate(Key key, Modifier modifier, KeyboardEventType eventType)
{
    bool const success = _keyboardInputGenerator.generateKey(key, modifier, eventType);

    if (success)
    {
        _pendingSequence += _keyboardInputGenerator.take();
        inputLog()("Sending {} \"{}\" {}.", modifier, key, eventType);
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
    int n = snprintf(buf, sizeof(buf), "%u", asciiChar);
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
    constexpr uint8_t modifierBits(Modifier modifier) noexcept
    {
        uint8_t mods = 0;
        if (modifier.shift())
            mods |= 4;
        if (modifier.meta())
            mods |= 8;
        if (modifier.control())
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
                                   Modifier modifier,
                                   MouseButton button,
                                   CellLocation pos,
                                   PixelCoordinate pixelPosition,
                                   bool uiHandled)
{
    if (!_mouseProtocol.has_value())
        return false;

    // std::cout << fmt::format("generateMouse({}/{}): button:{}, modifier:{}, at:{}, type:{}\n",
    //                          _mouseTransport, *_mouseProtocol,
    //                          button, modifier, pos, eventType);

    switch (*_mouseProtocol)
    {
        case MouseProtocol::X10: // Old X10 mouse protocol
            if (eventType == MouseEventType::Press)
                mouseTransport(
                    eventType, buttonX10(button), modifierBits(modifier), pos, pixelPosition, uiHandled);
            return true;
        case MouseProtocol::NormalTracking: // Normal tracking mode, that's X10 with mouse release events and
                                            // modifiers
            if (eventType == MouseEventType::Press || eventType == MouseEventType::Release)
            {
                auto const buttonValue = _mouseTransport != MouseTransport::SGR
                                             ? buttonNormal(button, eventType)
                                             : buttonX10(button);
                mouseTransport(eventType, buttonValue, modifierBits(modifier), pos, pixelPosition, uiHandled);
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
                    eventType, draggableButton, modifierBits(modifier), pos, pixelPosition, uiHandled);
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
                    eventType, draggableButton, modifierBits(modifier), pos, pixelPosition, uiHandled);
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

    if (*pos.line < MaxCoordValue && *pos.column < MaxCoordValue)
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
    Modifier modifier, MouseButton button, CellLocation pos, PixelCoordinate pixelPosition, bool uiHandled)
{
    auto const logged = [=](bool success) -> bool {
        if (success)
            inputLog()("Sending mouse press {} {} at {}.", button, modifier, pos);
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
        MouseEventType::Press, modifier, button, _currentMousePosition, pixelPosition, uiHandled));
}

bool InputGenerator::generateMouseRelease(
    Modifier modifier, MouseButton button, CellLocation pos, PixelCoordinate pixelPosition, bool uiHandled)
{
    auto const logged = [=](bool success) -> bool {
        if (success)
            inputLog()("Sending mouse release {} {} at {}.", button, modifier, pos);
        return success;
    };

    _currentMousePosition = pos;

    if (auto i = _currentlyPressedMouseButtons.find(button); i != _currentlyPressedMouseButtons.end())
        _currentlyPressedMouseButtons.erase(i);

    return logged(generateMouse(
        MouseEventType::Release, modifier, button, _currentMousePosition, pixelPosition, uiHandled));
}

bool InputGenerator::generateMouseMove(Modifier modifier,
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
            modifier,
            buttonsPressed ? *_currentlyPressedMouseButtons.begin() // what if multiple are pressed?
                           : MouseButton::Release,
            pos,
            pixelPosition,
            uiHandled));

    return false;
}
// }}}

} // namespace vtbackend
