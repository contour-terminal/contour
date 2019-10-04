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
#include <ground/UTF8.h>

#include <algorithm>
#include <array>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>

using namespace std;

#define ESC "\x1B"
#define CSI "\x1B["
#define DEL "\x7F"
#define SS3 "\x4F"

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
        KeyMapping{Key::Enter, "\r"},
        KeyMapping{Key::Backspace, "\b"},
        KeyMapping{Key::Tab, "\t"},
        KeyMapping{Key::Escape, ESC},

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
        case Key::Enter: return "Enter";
        case Key::Backspace: return "Backspace";
        case Key::Tab: return "Tab";
        case Key::Escape: return "Escape";
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
        [&](MousePressEvent const& _mouse) { return false /*TODO: generate(_mouse.button, _mouse.modifier)*/; },
    }, _inputEvent);
}

bool InputGenerator::generate(char32_t _characterEvent, Modifier _modifier)
{
    if (_modifier.control() && _characterEvent == L' ')
        return emit("\x00");
    else if (_modifier.control() && tolower(_characterEvent) >= 'a' && tolower(_characterEvent) <= 'z')
        return emit(tolower(_characterEvent) - 'a' + 1);
    else if (!_modifier.control() && utf8::isASCII(_characterEvent))
        return emit(static_cast<char>(_characterEvent));
    else if (!_modifier)
        return emit(utf8::to_string(utf8::encode(_characterEvent)));
    else
        return false;
}

bool InputGenerator::generate(Key _key, Modifier _modifier)
{
    if (applicationCursorKeys())
        if (auto mapping = tryMap(mappings::applicationCursorKeys, _key); mapping)
            return emit(*mapping);

    if (applicationKeypad())
        if (auto mapping = tryMap(mappings::applicationNumpadKeys, _key); mapping)
            return emit(*mapping);

    if (_modifier)
    {
        if (auto mapping = tryMap(mappings::functionKeysWithModifiers, _key); mapping)
            return emit(fmt::format(*mapping, makeVirtualTerminalParam(_modifier)));
    }
    else
    {
        if (auto mapping = tryMap(mappings::standard, _key); mapping)
            return emit(*mapping);
    }

    return false;
}

void InputGenerator::swap(Sequence& _other)
{
    std::swap(pendingSequence_, _other);
}

inline bool InputGenerator::emit(std::string _sequence)
{
    pendingSequence_.insert(end(pendingSequence_), begin(_sequence), end(_sequence));
    return true;
}

inline bool InputGenerator::emit(std::string_view _sequence)
{
    pendingSequence_.insert(end(pendingSequence_), begin(_sequence), end(_sequence));
    return true;
}

inline bool InputGenerator::emit(char _asciiChar)
{
    pendingSequence_.push_back(_asciiChar);
    return true;
}

template <typename T, size_t N>
inline bool InputGenerator::emit(T(&_sequence)[N])
{
    pendingSequence_.insert(end(pendingSequence_), begin(_sequence), prev(end(_sequence)));
    return true;
}

optional<Modifier::Key> parseModifierKey(string const& _key)
{
    auto const key = toUpper(_key);
	if (key == "ALT")
		return Modifier::Key::Alt;
	if (key == "CONTROL")
		return Modifier::Key::Control;
	if (key == "SHIFT")
		return Modifier::Key::Shift;
	if (key == "META")
		return Modifier::Key::Meta;
	return nullopt;
}

optional<variant<Key, char32_t>> parseKeyOrChar(string const& _name)
{
    if (auto const key = parseKey(_name); key.has_value())
        return key.value();

    if (_name.size() == 1)
    {
        if (isdigit(_name[0]))
            return static_cast<char32_t>(_name[0]);

        if (isalpha(_name[0]))
            return static_cast<char32_t>(tolower(_name[0]));
    }

    auto constexpr namedChars = array{
        pair{"APOSTROPHE"sv, '\''},
        pair{"ADD"sv, '+'},
        pair{"BACKSLASH"sv, 'x'},
        pair{"COMMA"sv, ','},
        pair{"DECIMAL"sv, '.'},
        pair{"DIVIDE"sv, '/'},
        pair{"EQUAL"sv, '='},
        pair{"LEFT_BRACKET"sv, '['},
        pair{"MINUS"sv, '-'},
        pair{"MULTIPLY"sv, '*'},
        pair{"PERIOD"sv, '.'},
        pair{"RIGHT_BRACKET"sv, ']'},
        pair{"SEMICOLON"sv, ';'},
        pair{"SLASH"sv, '/'},
        pair{"SUBTRACT"sv, '-'},
        pair{"SPACE"sv, ' '}
    };

    auto const name = toUpper(_name);
    for (auto const& mapping: namedChars)
        if (name == mapping.first)
            return mapping.second;

    return nullopt;
}

optional<Key> parseKey(string const& _name)
{
	auto static constexpr mappings = array{
		pair{ "Enter"sv, Key::Enter },
		pair{ "Backspace"sv, Key::Backspace },
		pair{ "Tab"sv, Key::Tab },
		pair{ "Escape"sv, Key::Escape },
		pair{ "F1"sv, Key::F1 },
		pair{ "F2"sv, Key::F2 },
		pair{ "F3"sv, Key::F3 },
		pair{ "F4"sv, Key::F4 },
		pair{ "F5"sv, Key::F5 },
		pair{ "F6"sv, Key::F6 },
		pair{ "F7"sv, Key::F7 },
		pair{ "F8"sv, Key::F8 },
		pair{ "F9"sv, Key::F9 },
		pair{ "F10"sv, Key::F10 },
		pair{ "F11"sv, Key::F11 },
		pair{ "F12"sv, Key::F12 },
		pair{ "DownArrow"sv, Key::DownArrow },
		pair{ "LeftArrow"sv, Key::LeftArrow },
		pair{ "RightArrow"sv, Key::RightArrow },
		pair{ "UpArrow"sv, Key::UpArrow },
		pair{ "Insert"sv, Key::Insert },
		pair{ "Delete"sv, Key::Delete },
		pair{ "Home"sv, Key::Home },
		pair{ "End"sv, Key::End },
		pair{ "PageUp"sv, Key::PageUp },
		pair{ "PageDown"sv, Key::PageDown },
		pair{ "Numpad_NumLock"sv, Key::Numpad_NumLock },
		pair{ "Numpad_Divide"sv, Key::Numpad_Divide },
		pair{ "Numpad_Multiply"sv, Key::Numpad_Multiply },
		pair{ "Numpad_Subtract"sv, Key::Numpad_Subtract },
		pair{ "Numpad_CapsLock"sv, Key::Numpad_CapsLock },
		pair{ "Numpad_Add"sv, Key::Numpad_Add },
		pair{ "Numpad_Decimal"sv, Key::Numpad_Decimal },
		pair{ "Numpad_Enter"sv, Key::Numpad_Enter },
		pair{ "Numpad_Equal"sv, Key::Numpad_Equal },
		pair{ "Numpad_0"sv, Key::Numpad_0 },
		pair{ "Numpad_1"sv, Key::Numpad_1 },
		pair{ "Numpad_2"sv, Key::Numpad_2 },
		pair{ "Numpad_3"sv, Key::Numpad_3 },
		pair{ "Numpad_4"sv, Key::Numpad_4 },
		pair{ "Numpad_5"sv, Key::Numpad_5 },
		pair{ "Numpad_6"sv, Key::Numpad_6 },
		pair{ "Numpad_7"sv, Key::Numpad_7 },
		pair{ "Numpad_8"sv, Key::Numpad_8 },
		pair{ "Numpad_9"sv, Key::Numpad_9 }
    };

    auto const name = toLower(_name);

    for (auto const mapping: mappings)
        if (name == toLower(mapping.first))
            return mapping.second;

    return nullopt;
}

} // namespace terminal
