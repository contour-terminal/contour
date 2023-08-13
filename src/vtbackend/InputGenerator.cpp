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
#include <vtbackend/ControlCode.h>
#include <vtbackend/InputGenerator.h>
#include <vtbackend/logging.h>

#include <crispy/utils.h>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <libunicode/convert.h>

using namespace std;

namespace terminal
{

namespace mappings
{
    struct key_mapping
    {
        key key;
        std::string_view mapping {};
    };

    // TODO: implement constexpr-binary-search by:
    // - adding operator<(KeyMapping a, KeyMapping b) { return a.key < b.key; }
    // - constexpr-evaluated sort()ed array returned in lambda-expr to be assigned to these globals here.
    // - make use of this property and let tryMap() do a std::binary_search()

#define ESC "\x1B"
#define CSI "\x1B["
#define SS3 "\x1BO"

    // the modifier parameter is going to be replaced via fmt::format()
    array<key_mapping, 30> const functionKeysWithModifiers {
        // clang-format off
        // Note, that F1..F4 is using CSI too instead of ESC when used with modifier keys.
        // XXX: Maybe I am blind when reading ctlseqs.txt, but F1..F4 with "1;{}P".. seems not to
        // match what other terminal emulators send out with modifiers and I don't see how to match
        // xterm's behaviour along with getting for example vim working to bind to these.
        key_mapping { key::F1, ESC "O{}P" }, // "1;{}P"
        key_mapping { key::F2, ESC "O{}Q" }, // "1;{}Q"
        key_mapping { key::F3, ESC "O{}R" }, // "1;{}R"
        key_mapping { key::F4, ESC "O{}S" }, // "1;{}S"
        key_mapping { key::F5, CSI "15;{}~" },
        key_mapping { key::F6, CSI "17;{}~" },
        key_mapping { key::F7, CSI "18;{}~" },
        key_mapping { key::F8, CSI "19;{}~" },
        key_mapping { key::F9, CSI "20;{}~" },
        key_mapping { key::F10, CSI "21;{}~" },
        key_mapping { key::F11, CSI "23;{}~" },
        key_mapping { key::F12, CSI "24;{}~" },
        key_mapping { key::F13, CSI "25;{}~" },
        key_mapping { key::F14, CSI "26;{}~" },
        key_mapping { key::F15, CSI "28;{}~" },
        key_mapping { key::F16, CSI "29;{}~" },
        key_mapping { key::F17, CSI "31;{}~" },
        key_mapping { key::F18, CSI "32;{}~" },
        key_mapping { key::F19, CSI "33;{}~" },
        key_mapping { key::F20, CSI "34;{}~" },

        // cursor keys
        key_mapping { key::UpArrow, CSI "1;{}A" },
        key_mapping { key::DownArrow, CSI "1;{}B" },
        key_mapping { key::RightArrow, CSI "1;{}C" },
        key_mapping { key::LeftArrow, CSI "1;{}D" },

        // 6-key editing pad
        key_mapping { key::Insert, CSI "2;{}~" },
        key_mapping { key::Delete, CSI "3;{}~" },
        key_mapping { key::Home, CSI "1;{}H" },
        key_mapping { key::End, CSI "1;{}F" },
        key_mapping { key::PageUp, CSI "5;{}~" },
        key_mapping { key::PageDown, CSI "6;{}~" },
        // clang-format on
    };

    array<key_mapping, 22> const standard {
        // clang-format off
        // cursor keys
        key_mapping { key::UpArrow, CSI "A" },
        key_mapping { key::DownArrow, CSI "B" },
        key_mapping { key::RightArrow, CSI "C" },
        key_mapping { key::LeftArrow, CSI "D" },

        // 6-key editing pad
        key_mapping { key::Insert, CSI "2~" },
        key_mapping { key::Delete, CSI "3~" },
        key_mapping { key::Home, CSI "H" },
        key_mapping { key::End, CSI "F" },
        key_mapping { key::PageUp, CSI "5~" },
        key_mapping { key::PageDown, CSI "6~" },

        // function keys
        key_mapping { key::F1, ESC "OP" },
        key_mapping { key::F2, ESC "OQ" },
        key_mapping { key::F3, ESC "OR" },
        key_mapping { key::F4, ESC "OS" },
        key_mapping { key::F5, CSI "15~" },
        key_mapping { key::F6, CSI "17~" },
        key_mapping { key::F7, CSI "18~" },
        key_mapping { key::F8, CSI "19~" },
        key_mapping { key::F9, CSI "20~" },
        key_mapping { key::F10, CSI "21~" },
        key_mapping { key::F11, CSI "23~" },
        key_mapping { key::F12, CSI "24~" },
        // clang-format on
    };

    /// (DECCKM) Cursor key mode: mappings in when cursor key application mode is set.
    array<key_mapping, 6> const applicationCursorKeys {
        // clang-format off
        key_mapping { key::UpArrow, SS3 "A" },
        key_mapping { key::DownArrow, SS3 "B" },
        key_mapping { key::RightArrow, SS3 "C" },
        key_mapping { key::LeftArrow, SS3 "D" },
        key_mapping { key::Home, SS3 "H" },
        key_mapping { key::End, SS3 "F" },
        // clang-format on
    };

    array<key_mapping, 21> const applicationKeypad
    {
        // clang-format off
        key_mapping { key::Numpad_NumLock, SS3 "P" },
        key_mapping { key::Numpad_Divide, SS3 "Q" },
        key_mapping { key::Numpad_Multiply, SS3 "Q" },
        key_mapping { key::Numpad_Subtract, SS3 "Q" },
        key_mapping { key::Numpad_CapsLock, SS3 "m" },
        key_mapping { key::Numpad_Add, SS3 "l" },
        key_mapping { key::Numpad_Decimal, SS3 "n" },
        key_mapping { key::Numpad_Enter, SS3 "M" },
        key_mapping { key::Numpad_Equal, SS3 "X" },
        key_mapping { key::Numpad_0, SS3 "p" },
        key_mapping { key::Numpad_1, SS3 "q" },
        key_mapping { key::Numpad_2, SS3 "r" },
        key_mapping { key::Numpad_3, SS3 "s" },
        key_mapping { key::Numpad_4, SS3 "t" },
        key_mapping { key::Numpad_5, SS3 "u" },
        key_mapping { key::Numpad_6, SS3 "v" },
        key_mapping { key::Numpad_7, SS3 "w" },
        key_mapping { key::Numpad_8, SS3 "x" },
        key_mapping { key::Numpad_9, SS3 "y" },
        key_mapping { key::PageUp, CSI "5~" },
        key_mapping { key::PageDown, CSI "6~" },
#if 0 // TODO
        KeyMapping{Key::Space,    SS3 " "}, // TODO
        KeyMapping{Key::Tab,      SS3 "I"},
        KeyMapping{Key::Enter,    SS3 "M"},
#endif
        // clang-format on
    };

#undef ESC
#undef CSI
#undef SS3

    constexpr bool operator==(key_mapping const& km, key key) noexcept
    {
        return km.key == key;
    }

    template <size_t N>
    optional<string_view> tryMap(array<key_mapping, N> const& mappings, key key) noexcept
    {
        for (key_mapping const& km: mappings)
            if (km.key == key)
                return { km.mapping };

        return nullopt;
    }
} // namespace mappings

string to_string(modifier modifier)
{
    string out;
    auto const append = [&](const char* s) {
        if (!out.empty())
            out += ",";
        out += s;
    };

    if (modifier.shift())
        append("Shift");
    if (modifier.alt())
        append("Alt");
    if (modifier.control())
        append("Control");
    if (modifier.meta())
        append("Meta");

    return out;
}

string to_string(key key)
{
    switch (key)
    {
        case key::F1: return "F1";
        case key::F2: return "F2";
        case key::F3: return "F3";
        case key::F4: return "F4";
        case key::F5: return "F5";
        case key::F6: return "F6";
        case key::F7: return "F7";
        case key::F8: return "F8";
        case key::F9: return "F9";
        case key::F10: return "F10";
        case key::F11: return "F11";
        case key::F12: return "F12";
        case key::F13: return "F13";
        case key::F14: return "F14";
        case key::F15: return "F15";
        case key::F16: return "F16";
        case key::F17: return "F17";
        case key::F18: return "F18";
        case key::F19: return "F19";
        case key::F20: return "F20";
        case key::DownArrow: return "DownArrow";
        case key::LeftArrow: return "LeftArrow";
        case key::RightArrow: return "RightArrow";
        case key::UpArrow: return "UpArrow";
        case key::Insert: return "Insert";
        case key::Delete: return "Delete";
        case key::Home: return "Home";
        case key::End: return "End";
        case key::PageUp: return "PageUp";
        case key::PageDown: return "PageDown";
        case key::Numpad_NumLock: return "Numpad_NumLock";
        case key::Numpad_Divide: return "Numpad_Divide";
        case key::Numpad_Multiply: return "Numpad_Multiply";
        case key::Numpad_Subtract: return "Numpad_Subtract";
        case key::Numpad_CapsLock: return "Numpad_CapsLock";
        case key::Numpad_Add: return "Numpad_Add";
        case key::Numpad_Decimal: return "Numpad_Decimal";
        case key::Numpad_Enter: return "Numpad_Enter";
        case key::Numpad_Equal: return "Numpad_Equal";
        case key::Numpad_0: return "Numpad_0";
        case key::Numpad_1: return "Numpad_1";
        case key::Numpad_2: return "Numpad_2";
        case key::Numpad_3: return "Numpad_3";
        case key::Numpad_4: return "Numpad_4";
        case key::Numpad_5: return "Numpad_5";
        case key::Numpad_6: return "Numpad_6";
        case key::Numpad_7: return "Numpad_7";
        case key::Numpad_8: return "Numpad_8";
        case key::Numpad_9: return "Numpad_9";
    }
    return "(unknown)";
}

string to_string(mouse_button button)
{
    switch (button)
    {
        case mouse_button::Left: return "Left"s;
        case mouse_button::Right: return "Right"s;
        case mouse_button::Middle: return "Middle"s;
        case mouse_button::Release: return "Release"s;
        case mouse_button::WheelUp: return "WheelUp"s;
        case mouse_button::WheelDown: return "WheelDown"s;
    }
    return ""; // should never be reached
}

void input_generator::reset()
{
    _cursorKeysMode = key_mode::Normal;
    _numpadKeysMode = key_mode::Normal;
    _bracketedPaste = false;
    _generateFocusEvents = false;
    _mouseProtocol = std::nullopt;
    _mouseTransport = mouse_transport::Default;
    _mouseWheelMode = mouse_wheel_mode::Default;

    // _pendingSequence = {};
    // _currentMousePosition = {0, 0}; // current mouse position
    // _currentlyPressedMouseButtons = {};
}

void input_generator::setCursorKeysMode(key_mode mode)
{
    InputLog()("set cursor keys mode: {}", mode);
    _cursorKeysMode = mode;
}

void input_generator::setNumpadKeysMode(key_mode mode)
{
    InputLog()("set numpad keys mode: {}", mode);
    _numpadKeysMode = mode;
}

void input_generator::setApplicationKeypadMode(bool enable)
{
    if (enable)
        _numpadKeysMode = key_mode::Application;
    else
        _numpadKeysMode = key_mode::Normal; // aka. Numeric

    InputLog()("set application keypad mode: {} -> {}", enable, _numpadKeysMode);
}

bool input_generator::generate(u32string const& characterEvent, modifier modifier)
{
    for (char32_t const ch: characterEvent)
        generate(ch, modifier);
    return true;
}

bool input_generator::generate(char32_t characterEvent, modifier modifier)
{
    char const chr = static_cast<char>(characterEvent);

    // See section "Alt and Meta Keys" in ctlseqs.txt from xterm.
    if (modifier.alt())
        // NB: There are other modes in xterm to send Alt+Key options or even send ESC on Meta key instead.
        append("\033");

    // Well accepted hack to distinguish between Backspace nad Ctrl+Backspace,
    // - Backspace is emitting 0x7f,
    // - Ctrl+Backspace is emitting 0x08
    if (characterEvent == 0x08)
    {
        if (!modifier.control())
            return append("\x7f");
        else
            return append("\x08");
    }

    if (modifier == modifier::Shift && characterEvent == 0x09)
        return append("\033[Z"); // introduced by linux_console in 1995, adopted by xterm in 2002

    // raw C0 code
    if (modifier == modifier::Control && characterEvent < 32)
        return append(static_cast<uint8_t>(characterEvent));

    if (modifier == modifier::Control && characterEvent == L' ')
        return append('\x00');

    if (modifier == modifier::Control && crispy::ascending('A', chr, 'Z'))
        return append(static_cast<char>(chr - 'A' + 1));

    if (modifier == modifier::Control && characterEvent >= '[' && characterEvent <= '_')
        return append(static_cast<char>(chr - 'A' + 1)); // remaining C0 characters 0x1B .. 0x1F

    if (modifier.without(modifier::Alt).none() || modifier == modifier::Shift)
        return append(unicode::convert_to<char>(characterEvent));

    if (characterEvent < 0x7F)
        append(static_cast<char>(characterEvent));
    else
        append(unicode::convert_to<char>(characterEvent));

    InputLog()("Sending {} \"{}\".", modifier, crispy::escape(unicode::convert_to<char>(characterEvent)));
    return true;
}

bool input_generator::generate(key key, modifier modifier)
{
    auto const logged = [key, modifier](bool success) -> bool {
        if (success)
            InputLog()("Sending {} {}.", modifier, key);
        return success;
    };

    if (modifier)
    {
        if (auto mapping = tryMap(mappings::functionKeysWithModifiers, key); mapping)
            return logged(append(crispy::replace(*mapping, "{}"sv, makeVirtualTerminalParam(modifier))));
    }

    if (applicationCursorKeys())
        if (auto mapping = tryMap(mappings::applicationCursorKeys, key); mapping)
            return logged(append(*mapping));

    if (applicationKeypad())
        if (auto mapping = tryMap(mappings::applicationKeypad, key); mapping)
            return logged(append(*mapping));

    if (auto mapping = tryMap(mappings::standard, key); mapping)
        return logged(append(*mapping));

    return false;
}

void input_generator::generatePaste(std::string_view const& text)
{
    InputLog()("Sending paste of {} bytes.", text.size());

    if (text.empty())
        return;

    if (_bracketedPaste)
        append("\033[200~"sv);

    append(text);

    if (_bracketedPaste)
        append("\033[201~"sv);
}

inline bool input_generator::append(std::string_view sequence)
{
    _pendingSequence.insert(end(_pendingSequence), begin(sequence), end(sequence));
    return true;
}

inline bool input_generator::append(char asciiChar)
{
    _pendingSequence.push_back(asciiChar);
    return true;
}

inline bool input_generator::append(uint8_t byte)
{
    _pendingSequence.push_back(static_cast<char>(byte));
    return true;
}

inline bool input_generator::append(unsigned int asciiChar)
{
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%u", asciiChar);
    return append(string_view(buf, static_cast<size_t>(n)));
}

bool input_generator::generateFocusInEvent()
{
    if (generateFocusEvents())
    {
        append("\033[I");
        InputLog()("Sending focus-in event.");
        return true;
    }
    return false;
}

bool input_generator::generateFocusOutEvent()
{
    if (generateFocusEvents())
    {
        append("\033[O");
        InputLog()("Sending focus-out event.");
        return true;
    }
    return true;
}

bool input_generator::generateRaw(std::string_view const& raw)
{
    append(raw);
    return true;
}

// {{{ mouse handling
void input_generator::setMouseProtocol(mouse_protocol mouseProtocol, bool enabled)
{
    if (enabled)
    {
        _mouseWheelMode = mouse_wheel_mode::Default;
        _mouseProtocol = mouseProtocol;
    }
    else
        _mouseProtocol = std::nullopt;
}

void input_generator::setMouseTransport(mouse_transport mouseTransport)
{
    _mouseTransport = mouseTransport;
}

void input_generator::setMouseWheelMode(mouse_wheel_mode mode) noexcept
{
    _mouseWheelMode = mode;
}

namespace
{
    constexpr uint8_t modifierBits(modifier modifier) noexcept
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

    constexpr uint8_t buttonNumber(mouse_button button) noexcept
    {
        switch (button)
        {
            case mouse_button::Left: return 0;
            case mouse_button::Middle: return 1;
            case mouse_button::Right: return 2;
            case mouse_button::Release: return 3;
            case mouse_button::WheelUp: return 4;
            case mouse_button::WheelDown: return 5;
        }
        return 0; // should never happen
    }

    constexpr bool isMouseWheel(mouse_button button) noexcept
    {
        return button == mouse_button::WheelUp || button == mouse_button::WheelDown;
    }

    constexpr uint8_t buttonX10(mouse_button button) noexcept
    {
        return isMouseWheel(button) ? uint8_t(buttonNumber(button) + 0x3c) : buttonNumber(button);
    }

    constexpr uint8_t buttonNormal(mouse_button button, input_generator::mouse_event_type eventType) noexcept
    {
        return eventType == input_generator::mouse_event_type::Release ? 3 : buttonX10(button);
    }
} // namespace

bool input_generator::generateMouse(mouse_event_type eventType,
                                    modifier modifier,
                                    mouse_button button,
                                    cell_location pos,
                                    pixel_coordinate pixelPosition,
                                    bool uiHandled)
{
    if (!_mouseProtocol.has_value())
        return false;

    // std::cout << fmt::format("generateMouse({}/{}): button:{}, modifier:{}, at:{}, type:{}\n",
    //                          _mouseTransport, *_mouseProtocol,
    //                          button, modifier, pos, eventType);

    switch (*_mouseProtocol)
    {
        case mouse_protocol::X10: // Old X10 mouse protocol
            if (eventType == mouse_event_type::Press)
                mouseTransport(
                    eventType, buttonX10(button), modifierBits(modifier), pos, pixelPosition, uiHandled);
            return true;
        case mouse_protocol::NormalTracking: // Normal tracking mode, that's X10 with mouse release events and
                                             // modifiers
            if (eventType == mouse_event_type::Press || eventType == mouse_event_type::Release)
            {
                auto const buttonValue = _mouseTransport != mouse_transport::SGR
                                             ? buttonNormal(button, eventType)
                                             : buttonX10(button);
                mouseTransport(eventType, buttonValue, modifierBits(modifier), pos, pixelPosition, uiHandled);
            }
            return true;
        case mouse_protocol::ButtonTracking: // Button-event tracking protocol.
            // like normal event tracking, but with drag events
            if (eventType == mouse_event_type::Press || eventType == mouse_event_type::Drag
                || eventType == mouse_event_type::Release)
            {
                auto const buttonValue = _mouseTransport != mouse_transport::SGR
                                             ? buttonNormal(button, eventType)
                                             : buttonX10(button);

                uint8_t const draggableButton =
                    eventType == mouse_event_type::Drag ? uint8_t(buttonValue + 0x20) : buttonValue;

                mouseTransport(
                    eventType, draggableButton, modifierBits(modifier), pos, pixelPosition, uiHandled);
                return true;
            }
            return false;
        case mouse_protocol::AnyEventTracking: // Like ButtonTracking but any motion events (not just
                                               // dragging)
            // TODO: make sure we can receive mouse-move events even without mouse pressed.
            {
                auto const buttonValue = _mouseTransport != mouse_transport::SGR
                                             ? buttonNormal(button, eventType)
                                             : buttonX10(button);

                uint8_t const draggableButton =
                    eventType == mouse_event_type::Drag ? uint8_t(buttonValue + 0x20) : buttonValue;

                mouseTransport(
                    eventType, draggableButton, modifierBits(modifier), pos, pixelPosition, uiHandled);
            }
            return true;
        case mouse_protocol::HighlightTracking: // Highlight mouse tracking
            return false;                       // TODO: do we want to implement this?
    }

    return false;
}

bool input_generator::mouseTransport(mouse_event_type eventType,
                                     uint8_t button,
                                     uint8_t modifier,
                                     cell_location pos,
                                     pixel_coordinate pixelPosition,
                                     bool uiHandled)
{
    if (pos.line.value < 0 || pos.column.value < 0)
        // Negative coordinates are not supported. Avoid sending bad values.
        return true;

    switch (_mouseTransport)
    {
        case mouse_transport::Default: // mode: 9
            mouseTransportX10(button, modifier, pos);
            return true;
        case mouse_transport::Extended: // mode: 1005
            // TODO (like Default but with UTF-8 encoded coords)
            mouseTransportExtended(button, modifier, pos);
            return false;
        case mouse_transport::SGR: // mode: 1006
            return mouseTransportSGR(eventType, button, modifier, *pos.column + 1, *pos.line + 1, uiHandled);
        case mouse_transport::URXVT: // mode: 1015
            return mouseTransportURXVT(eventType, button, modifier, pos);
        case mouse_transport::SGRPixels: // mode: 1016
            return mouseTransportSGR(
                eventType, button, modifier, pixelPosition.x.value, pixelPosition.y.value, uiHandled);
    }

    return false;
}

bool input_generator::mouseTransportExtended(uint8_t button, uint8_t modifier, cell_location pos)
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

bool input_generator::mouseTransportX10(uint8_t button, uint8_t modifier, cell_location pos)
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

bool input_generator::mouseTransportSGR(
    mouse_event_type eventType, uint8_t button, uint8_t modifier, int x, int y, bool uiHandled)
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

    append(eventType != mouse_event_type::Release ? 'M' : 'm');

    return true;
}

bool input_generator::mouseTransportURXVT(mouse_event_type eventType,
                                          uint8_t button,
                                          uint8_t modifier,
                                          cell_location pos)
{
    if (eventType == mouse_event_type::Press)
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

bool input_generator::generateMousePress(
    modifier modifier, mouse_button button, cell_location pos, pixel_coordinate pixelPosition, bool uiHandled)
{
    auto const logged = [=](bool success) -> bool {
        if (success)
            InputLog()("Sending mouse press {} {} at {}.", button, modifier, pos);
        return success;
    };

    _currentMousePosition = pos;

    if (!_mouseProtocol.has_value())
        return false;

    switch (mouseWheelMode())
    {
        case mouse_wheel_mode::NormalCursorKeys:
            if (_passiveMouseTracking)
                break;
            switch (button)
            {
                case mouse_button::WheelUp: return logged(append("\033[A"));
                case mouse_button::WheelDown: return logged(append("\033[B"));
                default: break;
            }
            break;
        case mouse_wheel_mode::ApplicationCursorKeys:
            if (_passiveMouseTracking)
                break;
            switch (button)
            {
                case mouse_button::WheelUp: return logged(append("\033OA"));
                case mouse_button::WheelDown: return logged(append("\033OB"));
                default: break;
            }
            break;
        case mouse_wheel_mode::Default: break;
    }

    if (!isMouseWheel(button))
        if (!_currentlyPressedMouseButtons.count(button))
            _currentlyPressedMouseButtons.insert(button);

    return logged(generateMouse(
        mouse_event_type::Press, modifier, button, _currentMousePosition, pixelPosition, uiHandled));
}

bool input_generator::generateMouseRelease(
    modifier modifier, mouse_button button, cell_location pos, pixel_coordinate pixelPosition, bool uiHandled)
{
    auto const logged = [=](bool success) -> bool {
        if (success)
            InputLog()("Sending mouse release {} {} at {}.", button, modifier, pos);
        return success;
    };

    _currentMousePosition = pos;

    if (auto i = _currentlyPressedMouseButtons.find(button); i != _currentlyPressedMouseButtons.end())
        _currentlyPressedMouseButtons.erase(i);

    return logged(generateMouse(
        mouse_event_type::Release, modifier, button, _currentMousePosition, pixelPosition, uiHandled));
}

bool input_generator::generateMouseMove(modifier modifier,
                                        cell_location pos,
                                        pixel_coordinate pixelPosition,
                                        bool uiHandled)
{
    if (pos == _currentMousePosition && _mouseTransport != mouse_transport::SGRPixels)
        // Only generate a mouse move event if the coordinate of interest(!) has actually changed.
        return false;

    auto const logged = [&](bool success) -> bool {
        if (success)
        {
            InputLog()("[{}:{}] Sending mouse move at {} ({}:{}).",
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

    bool const report = (_mouseProtocol.value() == mouse_protocol::ButtonTracking && buttonsPressed)
                        || _mouseProtocol.value() == mouse_protocol::AnyEventTracking;

    if (report)
        return logged(generateMouse(
            mouse_event_type::Drag,
            modifier,
            buttonsPressed ? *_currentlyPressedMouseButtons.begin() // what if multiple are pressed?
                           : mouse_button::Release,
            pos,
            pixelPosition,
            uiHandled));

    return false;
}
// }}}

} // namespace terminal
