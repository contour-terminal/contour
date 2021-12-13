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
#include <terminal/InputGenerator.h>
#include <terminal/ControlCode.h>
#include <terminal/logging.h>
#include <crispy/utils.h>

#include <unicode/convert.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>

using namespace std;

namespace terminal {

namespace mappings {
    struct KeyMapping {
        Key const key;
        std::string_view const mapping{};
    };

    // TODO: implement constexpr-binary-search by:
    // - adding operator<(KeyMapping a, KeyMapping b) { return a.key < b.key; }
    // - constexpr-evaluated sort()ed array returned in lambda-expr to be assigned to these globals here.
    // - make use of this property and let tryMap() do a std::binary_search()

    #define ESC "\x1B"
    #define CSI "\x1B["
    #define SS3 "\x1BO"

    // the modifier parameter is going to be replaced via fmt::format()
    array<KeyMapping, 30> functionKeysWithModifiers{
        // Note, that F1..F4 is using CSI too instead of ESC when used with modifier keys.
        // XXX: Maybe I am blind when reading ctlseqs.txt, but F1..F4 with "1;{}P".. seems not to
        // match what other terminal emulators send out with modifiers and I don't see how to match
        // xterm's behaviour along with getting for example vim working to bind to these.
        KeyMapping{Key::F1, ESC "O{}P"}, // "1;{}P"
        KeyMapping{Key::F2, ESC "O{}Q"}, // "1;{}Q"
        KeyMapping{Key::F3, ESC "O{}R"}, // "1;{}R"
        KeyMapping{Key::F4, ESC "O{}S"}, // "1;{}S"
        KeyMapping{Key::F5, CSI "15;{}~"},
        KeyMapping{Key::F6, CSI "17;{}~"},
        KeyMapping{Key::F7, CSI "18;{}~"},
        KeyMapping{Key::F8, CSI "19;{}~"},
        KeyMapping{Key::F9, CSI "20;{}~"},
        KeyMapping{Key::F10, CSI "21;{}~"},
        KeyMapping{Key::F11, CSI "23;{}~"},
        KeyMapping{Key::F12, CSI "24;{}~"},
        KeyMapping{Key::F13, CSI "25;{}~"},
        KeyMapping{Key::F14, CSI "26;{}~"},
        KeyMapping{Key::F15, CSI "28;{}~"},
        KeyMapping{Key::F16, CSI "29;{}~"},
        KeyMapping{Key::F17, CSI "31;{}~"},
        KeyMapping{Key::F18, CSI "32;{}~"},
        KeyMapping{Key::F19, CSI "33;{}~"},
        KeyMapping{Key::F20, CSI "34;{}~"},

        // cursor keys
        KeyMapping{Key::UpArrow, CSI "1;{}A"},
        KeyMapping{Key::DownArrow, CSI "1;{}B"},
        KeyMapping{Key::RightArrow, CSI "1;{}C"},
        KeyMapping{Key::LeftArrow, CSI "1;{}D"},

        // 6-key editing pad
        KeyMapping{Key::Insert, CSI "2;{}~"},
        KeyMapping{Key::Delete, CSI "3;{}~"},
        KeyMapping{Key::Home, CSI "1;{}H"},
        KeyMapping{Key::End, CSI "1;{}F"},
        KeyMapping{Key::PageUp, CSI "5;{}~"},
        KeyMapping{Key::PageDown, CSI "6;{}~"},
    };

    array<KeyMapping, 22> standard{
        // cursor keys
        KeyMapping{Key::UpArrow, CSI "A"},
        KeyMapping{Key::DownArrow, CSI "B"},
        KeyMapping{Key::RightArrow, CSI "C"},
        KeyMapping{Key::LeftArrow, CSI "D"},

        // 6-key editing pad
        KeyMapping{Key::Insert, CSI "2~"},
        KeyMapping{Key::Delete, CSI "3~"},
        KeyMapping{Key::Home, CSI "H"},
        KeyMapping{Key::End, CSI "F"},
        KeyMapping{Key::PageUp, CSI "5~"},
        KeyMapping{Key::PageDown, CSI "6~"},

        // function keys
        KeyMapping{Key::F1, ESC "OP"},
        KeyMapping{Key::F2, ESC "OQ"},
        KeyMapping{Key::F3, ESC "OR"},
        KeyMapping{Key::F4, ESC "OS"},
        KeyMapping{Key::F5, CSI "15~"},
        KeyMapping{Key::F6, CSI "17~"},
        KeyMapping{Key::F7, CSI "18~"},
        KeyMapping{Key::F8, CSI "19~"},
        KeyMapping{Key::F9, CSI "20~"},
        KeyMapping{Key::F10, CSI "21~"},
        KeyMapping{Key::F11, CSI "23~"},
        KeyMapping{Key::F12, CSI "24~"},
    };

    /// (DECCKM) Cursor key mode: mappings in when cursor key application mode is set.
    array<KeyMapping, 6> applicationCursorKeys{
        KeyMapping{Key::UpArrow, SS3 "A"},
        KeyMapping{Key::DownArrow, SS3 "B"},
        KeyMapping{Key::RightArrow, SS3 "C"},
        KeyMapping{Key::LeftArrow, SS3 "D"},
        KeyMapping{Key::Home, SS3 "H"},
        KeyMapping{Key::End, SS3 "F"},
    };

    array<KeyMapping, 21> applicationKeypad{
        KeyMapping{Key::Numpad_NumLock, SS3 "P"},
        KeyMapping{Key::Numpad_Divide, SS3 "Q"},
        KeyMapping{Key::Numpad_Multiply, SS3 "Q"},
        KeyMapping{Key::Numpad_Subtract, SS3 "Q"},
        KeyMapping{Key::Numpad_CapsLock, SS3 "m"},
        KeyMapping{Key::Numpad_Add, SS3 "l"},
        KeyMapping{Key::Numpad_Decimal, SS3 "n"},
        KeyMapping{Key::Numpad_Enter, SS3 "M"},
        KeyMapping{Key::Numpad_Equal, SS3 "X"},
        KeyMapping{Key::Numpad_0, SS3 "p"},
        KeyMapping{Key::Numpad_1, SS3 "q"},
        KeyMapping{Key::Numpad_2, SS3 "r"},
        KeyMapping{Key::Numpad_3, SS3 "s"},
        KeyMapping{Key::Numpad_4, SS3 "t"},
        KeyMapping{Key::Numpad_5, SS3 "u"},
        KeyMapping{Key::Numpad_6, SS3 "v"},
        KeyMapping{Key::Numpad_7, SS3 "w"},
        KeyMapping{Key::Numpad_8, SS3 "x"},
        KeyMapping{Key::Numpad_9, SS3 "y"},
        KeyMapping{Key::PageUp,   CSI "5~"},
        KeyMapping{Key::PageDown, CSI "6~"},
#if 0 // TODO
        KeyMapping{Key::Space,    SS3 " "}, // TODO
        KeyMapping{Key::Tab,      SS3 "I"},
        KeyMapping{Key::Enter,    SS3 "M"},
#endif
    };

    #undef ESC
    #undef CSI
    #undef SS3

    constexpr bool operator==(KeyMapping const& _km, Key _key) noexcept
    {
        return _km.key == _key;
    }

    template<size_t N>
    optional<string_view> tryMap(array<KeyMapping, N> const& _mappings, Key _key) noexcept
    {
        for (KeyMapping const& km : _mappings)
            if (km.key == _key)
                return { km.mapping };

        return nullopt;
    }
}

string to_string(Modifier _modifier)
{
    string out;
    auto const append = [&](const char* s) {
        if (!out.empty())
            out += ",";
        out += s;
    };

    if (_modifier.shift())
        append("Shift");
    if (_modifier.alt())
        append("Alt");
    if (_modifier.control())
        append("Control");
    if (_modifier.meta())
        append("Meta");

    return out;
}

string to_string(Key _key)
{
    switch (_key)
    {
        case Key::F1: return "F1";
        case Key::F2: return "F2";
        case Key::F3: return "F3";
        case Key::F4: return "F4";
        case Key::F5: return "F5";
        case Key::F6: return "F6";
        case Key::F7: return "F7";
        case Key::F8: return "F8";
        case Key::F9: return "F9";
        case Key::F10: return "F10";
        case Key::F11: return "F11";
        case Key::F12: return "F12";
        case Key::F13: return "F13";
        case Key::F14: return "F14";
        case Key::F15: return "F15";
        case Key::F16: return "F16";
        case Key::F17: return "F17";
        case Key::F18: return "F18";
        case Key::F19: return "F19";
        case Key::F20: return "F20";
        case Key::DownArrow: return "DownArrow";
        case Key::LeftArrow: return "LeftArrow";
        case Key::RightArrow: return "RightArrow";
        case Key::UpArrow: return "UpArrow";
        case Key::Insert: return "Insert";
        case Key::Delete: return "Delete";
        case Key::Home: return "Home";
        case Key::End: return "End";
        case Key::PageUp: return "PageUp";
        case Key::PageDown: return "PageDown";
        case Key::Numpad_NumLock: return "Numpad_NumLock";
        case Key::Numpad_Divide: return "Numpad_Divide";
        case Key::Numpad_Multiply: return "Numpad_Multiply";
        case Key::Numpad_Subtract: return "Numpad_Subtract";
        case Key::Numpad_CapsLock: return "Numpad_CapsLock";
        case Key::Numpad_Add: return "Numpad_Add";
        case Key::Numpad_Decimal: return "Numpad_Decimal";
        case Key::Numpad_Enter: return "Numpad_Enter";
        case Key::Numpad_Equal: return "Numpad_Equal";
        case Key::Numpad_0: return "Numpad_0";
        case Key::Numpad_1: return "Numpad_1";
        case Key::Numpad_2: return "Numpad_2";
        case Key::Numpad_3: return "Numpad_3";
        case Key::Numpad_4: return "Numpad_4";
        case Key::Numpad_5: return "Numpad_5";
        case Key::Numpad_6: return "Numpad_6";
        case Key::Numpad_7: return "Numpad_7";
        case Key::Numpad_8: return "Numpad_8";
        case Key::Numpad_9: return "Numpad_9";
    }
    return "(unknown)";
}

string to_string(MouseButton _button)
{
    switch (_button)
    {
        case MouseButton::Left: return "Left"s;
        case MouseButton::Right: return "Right"s;
        case MouseButton::Middle: return "Middle"s;
        case MouseButton::Release: return "Release"s;
        case MouseButton::WheelUp: return "WheelUp"s;
        case MouseButton::WheelDown: return "WheelDown"s;
    }
    return ""; // should never be reached
}

void InputGenerator::reset()
{
    cursorKeysMode_ = KeyMode::Normal;
    numpadKeysMode_ = KeyMode::Normal;
    bracketedPaste_ = false;
    generateFocusEvents_ = false;
    mouseProtocol_ = std::nullopt;
    mouseTransport_ = MouseTransport::Default;
    mouseWheelMode_ = MouseWheelMode::Default;

    // pendingSequence_ = {};
    // currentlyPressedMouseButtons_ = {};
    // currentMousePosition_ = {0, 0}; // current mouse position
}

void InputGenerator::setCursorKeysMode(KeyMode _mode)
{
    LOGSTORE(InputLog)("set cursor keys mode: {}", _mode);
    cursorKeysMode_ = _mode;
}

void InputGenerator::setNumpadKeysMode(KeyMode _mode)
{
    LOGSTORE(InputLog)("set numpad keys mode: {}", _mode);
    numpadKeysMode_ = _mode;
}

void InputGenerator::setApplicationKeypadMode(bool _enable)
{
    if (_enable)
        numpadKeysMode_ = KeyMode::Application;
    else
        numpadKeysMode_ = KeyMode::Normal; // aka. Numeric

    LOGSTORE(InputLog)("set application keypad mode: {} -> {}", _enable, numpadKeysMode_);
}

bool InputGenerator::generate(u32string const& _characterEvent, Modifier _modifier)
{
    for (char32_t const ch: _characterEvent)
        generate(ch, _modifier);
    return true;
}

bool InputGenerator::generate(char32_t _characterEvent, Modifier _modifier)
{
    char const chr = static_cast<char>(_characterEvent);

    // See section "Alt and Meta Keys" in ctlseqs.txt from xterm.
    if (_modifier.alt())
        // NB: There are other modes in xterm to send Alt+Key options or even send ESC on Meta key instead.
        append("\033");

    // Well accepted hack to distinguish between Backspace nad Ctrl+Backspace,
    // - Backspace is emitting 0x7f,
    // - Ctrl+Backspace is emitting 0x08
    if (_characterEvent == 0x08)
    {
        if (!_modifier.control())
            return append("\x7f");
        else
            return append("\x08");
    }

    if (_modifier == Modifier::Shift && _characterEvent == 0x09)
        return append("\033[Z"); // introduced by linux_console in 1995, adopted by xterm in 2002

    // raw C0 code
    if (_modifier == Modifier::Control && _characterEvent < 32)
        return append(static_cast<uint8_t>(_characterEvent));

    if (_modifier == Modifier::Control && _characterEvent == L' ')
        return append('\x00');

    if (_modifier == Modifier::Control && crispy::ascending('A', chr, 'Z'))
        return append(static_cast<char>(chr - 'A' + 1));

    if (_modifier == Modifier::Control && _characterEvent >= '[' && _characterEvent <= '_')
        return append(static_cast<char>(chr - 'A' + 1)); // remaining C0 characters 0x1B .. 0x1F

    if (_modifier.without(Modifier::Alt).none() || _modifier == Modifier::Shift)
        return append(unicode::convert_to<char>(_characterEvent));

    if (_characterEvent < 0x7F)
        append(static_cast<char>(_characterEvent));
    else
        append(unicode::convert_to<char>(_characterEvent));

    LOGSTORE(InputLog)("Sending \"{}\" {}.", crispy::escape(unicode::convert_to<char>(_characterEvent)), _modifier);
    return true;
}

bool InputGenerator::generate(Key _key, Modifier _modifier)
{
    auto const logged = [_key, _modifier](bool success) -> bool
    {
        if (success)
            LOGSTORE(InputLog)("Sending {} {}.", _key, _modifier);
        return success;
    };

    if (_modifier)
    {
        if (auto mapping = tryMap(mappings::functionKeysWithModifiers, _key); mapping)
            return logged(append(fmt::format(*mapping, makeVirtualTerminalParam(_modifier))));
    }

    if (applicationCursorKeys())
        if (auto mapping = tryMap(mappings::applicationCursorKeys, _key); mapping)
            return logged(append(*mapping));

    if (applicationKeypad())
        if (auto mapping = tryMap(mappings::applicationKeypad, _key); mapping)
            return logged(append(*mapping));

    if (auto mapping = tryMap(mappings::standard, _key); mapping)
        return logged(append(*mapping));

    return false;
}

void InputGenerator::generatePaste(std::string_view const& _text)
{
    LOGSTORE(InputLog)("Sending paste of {} bytes.", _text.size());

    if (bracketedPaste_)
        append("\033[200~"sv);

    append(_text);

    if (bracketedPaste_)
        append("\033[201~"sv);
}

void InputGenerator::swap(Sequence& _other)
{
    std::swap(pendingSequence_, _other);
}

inline bool InputGenerator::append(std::string_view _sequence)
{
    pendingSequence_.insert(end(pendingSequence_), begin(_sequence), end(_sequence));
    return true;
}

inline bool InputGenerator::append(char _asciiChar)
{
    pendingSequence_.push_back(_asciiChar);
    return true;
}

inline bool InputGenerator::append(uint8_t _byte)
{
    pendingSequence_.push_back(static_cast<char>(_byte));
    return true;
}

inline bool InputGenerator::append(unsigned int _number)
{
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%u", _number);
    return append(string_view(buf, n));
}

bool InputGenerator::generateFocusInEvent()
{
    if (generateFocusEvents())
    {
        append("\033[I");
        LOGSTORE(InputLog)("Sending focus-in event.");
        return true;
    }
    return false;
}

bool InputGenerator::generateFocusOutEvent()
{
    if (generateFocusEvents())
    {
        append("\033[O");
        LOGSTORE(InputLog)("Sending focus-out event.");
        return true;
    }
    return true;
}

bool InputGenerator::generateRaw(std::string_view const& _raw)
{
    append(_raw);
    return true;
}

// {{{ mouse handling
void InputGenerator::setMouseProtocol(MouseProtocol _mouseProtocol, bool _enabled)
{
    if (_enabled)
    {
        mouseWheelMode_ = MouseWheelMode::Default;
        mouseProtocol_ = _mouseProtocol;
    }
    else
        mouseProtocol_ = std::nullopt;
}

void InputGenerator::setMouseTransport(MouseTransport _mouseTransport)
{
    mouseTransport_ = _mouseTransport;
}

void InputGenerator::setMouseWheelMode(MouseWheelMode _mode) noexcept
{
    mouseWheelMode_ = _mode;
}

namespace
{
    constexpr uint8_t modifierBits(Modifier _modifier) noexcept
    {
        uint8_t mods = 0;
        if (_modifier.shift())
            mods |= 4;
        if (_modifier.meta())
            mods |= 8;
        if (_modifier.control())
            mods |= 16;
        return mods;
    }

    constexpr uint8_t buttonNumber(MouseButton _button) noexcept
    {
        switch (_button)
        {
            case MouseButton::Left:
                return 0;
            case MouseButton::Middle:
                return 1;
            case MouseButton::Right:
                return 2;
            case MouseButton::Release:
                return 3;
            case MouseButton::WheelUp:
                return 4;
            case MouseButton::WheelDown:
                return 5;
        }
        return 0; // should never happen
    }

    constexpr bool isMouseWheel(MouseButton _button) noexcept
    {
        return _button == MouseButton::WheelUp
            || _button == MouseButton::WheelDown;
    }

    constexpr uint8_t buttonX10(MouseButton _button) noexcept
    {
        return isMouseWheel(_button) ? buttonNumber(_button) + 0x3c
                                     : buttonNumber(_button);
    }

    constexpr uint8_t buttonNormal(MouseButton _button, InputGenerator::MouseEventType _eventType) noexcept
    {
        return _eventType == InputGenerator::MouseEventType::Release ? 3 : buttonX10(_button);
    }

    constexpr uint8_t buttonButton(MouseButton _button, InputGenerator::MouseEventType _eventType) noexcept
    {
        return buttonNormal(_button, _eventType)
             + (_eventType == InputGenerator::MouseEventType::Drag ? 0x20 : 0);
    }
}

bool InputGenerator::generateMouse(MouseButton _button,
                                   Modifier _modifier,
                                   Coordinate _pos,
                                   MouseEventType _eventType)
{
    if (!mouseProtocol_.has_value())
        return false;

    // std::cout << fmt::format("generateMouse({}/{}): button:{}, modifier:{}, at:{}, type:{}\n",
    //                          mouseTransport_, *mouseProtocol_,
    //                          _button, _modifier, _pos, _eventType);

    switch (*mouseProtocol_)
    {
        case MouseProtocol::X10: // Old X10 mouse protocol
            if (_eventType == MouseEventType::Press)
                mouseTransport(buttonX10(_button), modifierBits(_modifier), _pos, _eventType);
            return true;
        case MouseProtocol::NormalTracking: // Normal tracking mode, that's X10 with mouse release events and modifiers
            if (_eventType == MouseEventType::Press ||
                _eventType == MouseEventType::Release)
            {
                auto const button = mouseTransport_ != MouseTransport::SGR
                                  ? buttonNormal(_button, _eventType)
                                  : buttonX10(_button);
                mouseTransport(button, modifierBits(_modifier), _pos, _eventType);
            }
            return true;
        case MouseProtocol::ButtonTracking: // Button-event tracking protocol.
            // like normal event tracking, but with drag events
            if (_eventType == MouseEventType::Press ||
                _eventType == MouseEventType::Drag ||
                _eventType == MouseEventType::Release)
            {
                auto const button = mouseTransport_ != MouseTransport::SGR
                                  ? buttonNormal(_button, _eventType)
                                  : buttonX10(_button);

                uint8_t const draggableButton = _eventType == MouseEventType::Drag
                                              ? button + 0x20
                                              : button;

                mouseTransport(draggableButton, modifierBits(_modifier), _pos, _eventType);
                return true;
            }
            return false;
        case MouseProtocol::AnyEventTracking: // Like ButtonTracking but any motion events (not just dragging)
            // TODO: make sure we can receive mouse-move events even without mouse pressed.
            {
                auto const button = mouseTransport_ != MouseTransport::SGR
                                  ? buttonNormal(_button, _eventType)
                                  : buttonX10(_button);

                uint8_t const draggableButton = _eventType == MouseEventType::Drag
                                              ? button + 0x20
                                              : button;

                mouseTransport(draggableButton, modifierBits(_modifier), _pos, _eventType);
            }
            return true;
        case MouseProtocol::HighlightTracking: // Highlight mouse tracking
            return false; // TODO: do we want to implement this?
    }

    return false;
}

bool InputGenerator::mouseTransport(uint8_t _button, uint8_t _modifier, Coordinate _pos, MouseEventType _eventType)
{
    switch (mouseTransport_)
    {
        case MouseTransport::Default: // mode: 9
            mouseTransportX10(_button, _modifier, _pos);
            return true;
        case MouseTransport::Extended: // mode: 1005
            // TODO (like Default but with UTF-8 encoded coords)
            return false;
        case MouseTransport::SGR:      // mode: 1006
            return mouseTransportSGR(_button, _modifier, _pos, _eventType);
        case MouseTransport::URXVT:    // mode: 1015
            return mouseTransportURXVT(_button, _modifier, _pos, _eventType);
    }

    return false;
}

bool InputGenerator::mouseTransportX10(uint8_t _button, uint8_t _modifier, Coordinate _pos)
{
    constexpr int SkipCount = 0x20; // TODO std::numeric_limits<ControlCode>::max();
    constexpr int MaxCoordValue = std::numeric_limits<uint8_t>::max() - SkipCount;

    if (*_pos.line < MaxCoordValue && *_pos.column < MaxCoordValue)
    {
        uint8_t const button = SkipCount + static_cast<uint8_t>(_button | _modifier);
        uint8_t const line = static_cast<uint8_t>(SkipCount + *_pos.line + 1);
        uint8_t const column = static_cast<uint8_t>(SkipCount + *_pos.column + 1);
        append("\033[M");
        append(button);
        append(column);
        append(line);
        return true;
    }
    else
        return false;
}

bool InputGenerator::mouseTransportSGR(uint8_t _button, uint8_t _modifier, Coordinate _pos, MouseEventType _eventType)
{
    append("\033[<");
    append(static_cast<unsigned>(_button | _modifier));
    append(';');
    append(static_cast<unsigned>(*_pos.column + 1));
    append(';');
    append(static_cast<unsigned>(*_pos.line + 1));
    append(_eventType != MouseEventType::Release ? 'M' : 'm');

    return true;
}

bool InputGenerator::mouseTransportURXVT(uint8_t _button, uint8_t _modifier, Coordinate _pos, MouseEventType _eventType)
{
    if (_eventType == MouseEventType::Press)
    {
        append("\033[");
        append(static_cast<unsigned>(_button | _modifier));
        append(';');
        append(static_cast<unsigned>(*_pos.column + 1));
        append(';');
        append(static_cast<unsigned>(*_pos.line + 1));
        append('M');
    }
    return true;
}

bool InputGenerator::generateMousePress(MouseButton _button, Modifier _modifier, Coordinate _pos)
{
    auto const logged = [=](bool success) -> bool
    {
        if (success)
            LOGSTORE(InputLog)("Sending mouse press {} {} at {}.", _button, _modifier, _pos);
        return success;
    };

    currentMousePosition_ = _pos;

    if (!mouseProtocol_.has_value())
        return false;

    switch (mouseWheelMode())
    {
        case MouseWheelMode::NormalCursorKeys:
            switch (_button)
            {
                case MouseButton::WheelUp:
                    return logged(append("\033[A"));
                case MouseButton::WheelDown:
                    return logged(append("\033[B"));
                default:
                    break;
            }
            break;
        case MouseWheelMode::ApplicationCursorKeys:
            switch (_button)
            {
                case MouseButton::WheelUp:
                    return logged(append("\033OA"));
                case MouseButton::WheelDown:
                    return logged(append("\033OB"));
                default:
                    break;
            }
            break;
        case MouseWheelMode::Default:
            break;
    }

    if (!isMouseWheel(_button))
        if (!currentlyPressedMouseButtons_.count(_button))
            currentlyPressedMouseButtons_.insert(_button);

    return logged(generateMouse(_button, _modifier, currentMousePosition_, MouseEventType::Press));
}

bool InputGenerator::generateMouseRelease(MouseButton _button, Modifier _modifier, Coordinate _pos)
{
    auto const logged = [=](bool success) -> bool
    {
        if (success)
            LOGSTORE(InputLog)("Sending mouse release {} {} at {}.", _button, _modifier, _pos);
        return success;
    };

    currentMousePosition_ = _pos;

    if (auto i = currentlyPressedMouseButtons_.find(_button); i != currentlyPressedMouseButtons_.end())
        currentlyPressedMouseButtons_.erase(i);

    return logged(generateMouse(_button, _modifier, currentMousePosition_, MouseEventType::Release));
}

bool InputGenerator::generateMouseMove(Coordinate _pos, Modifier _modifier)
{
    auto const logged = [=](bool success) -> bool
    {
        if (success)
            LOGSTORE(InputLog)("Sending mouse move at {} {}.", _pos, _modifier);
        return success;
    };

    if (_pos == currentMousePosition_)
        return false;

    currentMousePosition_ = _pos;

    if (!mouseProtocol_.has_value())
        return false;

    bool const buttonsPressed = !currentlyPressedMouseButtons_.empty();

    bool const report = (mouseProtocol_.value() == MouseProtocol::ButtonTracking && buttonsPressed)
                      || mouseProtocol_.value() == MouseProtocol::AnyEventTracking;

    if (report)
        return logged(generateMouse(buttonsPressed ? *currentlyPressedMouseButtons_.begin() // what if multiple are pressed?
                                                   : MouseButton::Release,
                                    _modifier,
                                    _pos,
                                    MouseEventType::Drag));

    return false;
}
// }}}

} // namespace terminal
