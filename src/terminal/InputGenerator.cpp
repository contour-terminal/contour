/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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
#include <terminal/util/UTF8.h>

#include <algorithm>
#include <array>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>

using namespace std;

namespace terminal {

namespace {
    template <typename String>
    inline std::string toLower(String const& _value)
    {
        std::string result;
        result.reserve(_value.size());
        std::transform(
            begin(_value),
            end(_value),
            back_inserter(result),
            [](auto ch) { return std::tolower(ch); }
        );
        return result;
    }

    template <typename String>
    inline std::string toUpper(String const& _value)
    {
        std::string result;
        result.reserve(_value.size());
        std::transform(
            begin(_value),
            end(_value),
            back_inserter(result),
            [](auto ch) { return std::toupper(ch); }
        );
        return result;
    }
}

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
    #define SS3 "\x4F"

    // the modifier parameter is going to be replaced via fmt::format()
    auto constexpr functionKeysWithModifiers = array{
        // Note, that F1..F4 is using CSI too instead of ESC when used with modifier keys.
        KeyMapping{Key::F1, CSI "1;{}P"},
        KeyMapping{Key::F2, CSI "1;{}Q"},
        KeyMapping{Key::F3, CSI "1;{}R"},
        KeyMapping{Key::F4, CSI "1;{}S"},
        KeyMapping{Key::F5, CSI "15;{}~"},
        KeyMapping{Key::F6, CSI "17;{}~"},
        KeyMapping{Key::F7, CSI "18;{}~"},
        KeyMapping{Key::F8, CSI "19;{}~"},
        KeyMapping{Key::F9, CSI "20;{}~"},
        KeyMapping{Key::F10, CSI "21;{}~"},
        KeyMapping{Key::F11, CSI "23;{}~"},
        KeyMapping{Key::F12, CSI "24;{}~"},

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

    auto constexpr standard = array{
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
        KeyMapping{Key::F1, ESC "0P"},
        KeyMapping{Key::F2, ESC "0Q"},
        KeyMapping{Key::F3, ESC "0R"},
        KeyMapping{Key::F4, ESC "0S"},
        KeyMapping{Key::F5, CSI "15~"},
        KeyMapping{Key::F6, CSI "17~"},
        KeyMapping{Key::F7, CSI "18~"},
        KeyMapping{Key::F8, CSI "19~"},
        KeyMapping{Key::F9, CSI "20~"},
        KeyMapping{Key::F10, CSI "21~"},
        KeyMapping{Key::F11, CSI "23~"},
        KeyMapping{Key::F12, CSI "24~"},
    };

    /// Cursor key mappings in when cursor key application mode is set.
    auto constexpr applicationCursorKeys = array{
        KeyMapping{Key::UpArrow, ESC SS3 "A"},
        KeyMapping{Key::DownArrow, ESC SS3 "B"},
        KeyMapping{Key::RightArrow, ESC SS3 "C"},
        KeyMapping{Key::LeftArrow, ESC SS3 "D"},
    };

    auto constexpr applicationNumpadKeys = array{
        KeyMapping{Key::Numpad_NumLock, ESC SS3 "P"},
        KeyMapping{Key::Numpad_Divide, ESC SS3 "Q"},
        KeyMapping{Key::Numpad_Multiply, ESC SS3 "Q"},
        KeyMapping{Key::Numpad_Subtract, ESC SS3 "Q"},
        KeyMapping{Key::Numpad_CapsLock, ESC SS3 "m"},
        KeyMapping{Key::Numpad_Add, ESC SS3 "l"},
        KeyMapping{Key::Numpad_Decimal, ESC SS3 "n"},
        KeyMapping{Key::Numpad_Enter, ESC SS3 "M"},
        KeyMapping{Key::Numpad_Equal, ESC SS3 "X"},
        KeyMapping{Key::Numpad_0, ESC SS3 "p"},
        KeyMapping{Key::Numpad_1, ESC SS3 "q"},
        KeyMapping{Key::Numpad_2, ESC SS3 "r"},
        KeyMapping{Key::Numpad_3, ESC SS3 "s"},
        KeyMapping{Key::Numpad_4, ESC SS3 "t"},
        KeyMapping{Key::Numpad_5, ESC SS3 "u"},
        KeyMapping{Key::Numpad_6, ESC SS3 "v"},
        KeyMapping{Key::Numpad_7, ESC SS3 "w"},
        KeyMapping{Key::Numpad_8, ESC SS3 "x"},
        KeyMapping{Key::Numpad_9, ESC SS3 "y"},
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
        case MouseButton::WheelUp: return "WheelUp"s;
        case MouseButton::WheelDown: return "WheelDown"s;
    }
    return ""; // should never be reached
}

void InputGenerator::setCursorKeysMode(KeyMode _mode)
{
    cursorKeysMode_ = _mode;
}

void InputGenerator::setNumpadKeysMode(KeyMode _mode)
{
    numpadKeysMode_ = _mode;
}

void InputGenerator::setApplicationKeypadMode(bool _enable)
{
    // cerr << "InputGenerator.setApplicationKeypadMode: "
    //      << (_enable ? "enable" : "disable") << endl;

    if (_enable)
        numpadKeysMode_ = KeyMode::Application;
    else
        numpadKeysMode_ = KeyMode::Normal; // aka. Numeric
}

bool InputGenerator::generate(InputEvent const& _inputEvent)
{
    return visit(overloaded{
        [&](KeyInputEvent const& _key) { return generate(_key.key, _key.modifier); },
        [&](CharInputEvent const& _chr) { return generate(_chr.value, _chr.modifier); },
		// TODO: the mouse input events should only generate input and return true *iff* requested
		//       by the connected application, returning false immediately otherwise.
        [&](MousePressEvent const& /*_mousePress*/) { return false /*TODO: generate(_mouse.button, _mouse.modifier)*/; },
        [&](MouseMoveEvent const& /*_mouseMove*/) { return false; /* TODO */ },
        [&](MouseReleaseEvent const& /*_mouseRelease*/) { return false; /* TODO */ },
    }, _inputEvent);
}

bool InputGenerator::generate(char32_t _characterEvent, Modifier _modifier)
{
    char const chr = static_cast<char>(_characterEvent);

    if (_characterEvent < 32 || (!_modifier.control() && utf8::isASCII(_characterEvent)))
        return append(chr); // raw C0 code
    else if (_modifier == Modifier::Control && _characterEvent == L' ')
        return append("\x00");
    else if (_modifier == Modifier::Control && tolower(chr) >= 'a' && tolower(chr) <= 'z')
        return append(static_cast<char>(tolower(chr) - 'a' + 1));
    else if (_modifier.control() && _characterEvent >= '[' && _characterEvent <= '_')
        return append(static_cast<char>(chr - 'A' + 1)); // remaining C0 characters 0x1B .. 0x1F
    else if (!_modifier || _modifier == Modifier::Shift)
        return append(utf8::to_string(utf8::encode(_characterEvent)));
    else
        return false;
}

bool InputGenerator::generate(Key _key, Modifier _modifier)
{
    if (applicationCursorKeys())
        if (auto mapping = tryMap(mappings::applicationCursorKeys, _key); mapping)
            return append(*mapping);

    if (applicationKeypad())
        if (auto mapping = tryMap(mappings::applicationNumpadKeys, _key); mapping)
            return append(*mapping);

    if (_modifier)
    {
        if (auto mapping = tryMap(mappings::functionKeysWithModifiers, _key); mapping)
            return append(fmt::format(*mapping, makeVirtualTerminalParam(_modifier)));
    }
    else
    {
        if (auto mapping = tryMap(mappings::standard, _key); mapping)
            return append(*mapping);
    }

    return false;
}

void InputGenerator::generatePaste(std::string_view const& _text)
{
    if (bracketedPaste_)
        append("\033[200~"sv);

    append(_text);

    if (bracketedPaste_)
        append("\033[201~"sv);
}

void InputGenerator::swap(Sequence& _other)
{
    std::swap(pendingSequence_, _other);
    printf("InputGenerator.transmit(\"%s\")\n", escape(begin(_other), end(_other)).c_str());
}

inline bool InputGenerator::append(std::string _sequence)
{
    pendingSequence_.insert(end(pendingSequence_), begin(_sequence), end(_sequence));
    return true;
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

template <typename T, size_t N>
inline bool InputGenerator::append(T(&_sequence)[N])
{
    pendingSequence_.insert(end(pendingSequence_), begin(_sequence), prev(end(_sequence)));
    return true;
}

} // namespace terminal
