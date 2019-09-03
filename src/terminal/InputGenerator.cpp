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
#include <terminal/UTF8.h>

#include <algorithm>
#include <array>
#include <string_view>
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
        KeyMapping{Key::UpArrow, SS3 "A"},
        KeyMapping{Key::DownArrow, SS3 "B"},
        KeyMapping{Key::RightArrow, SS3 "C"},
        KeyMapping{Key::LeftArrow, SS3 "D"},
    };

    auto constexpr applicationNumpadKeys = array{
        KeyMapping{Key::Numpad_NumLock, SS3 "P"},
        KeyMapping{Key::Numpad_Divide, SS3 "Q"},
        KeyMapping{Key::Numpad_Multiply, SS3 "Q"},
        KeyMapping{Key::Numpad_Subtract, SS3 "Q"},
        KeyMapping{Key::Numpad_CapsLock, SS3 "m"},
        KeyMapping{Key::Numpad_Add, SS3 "l"},
        KeyMapping{Key::Numpad_Decimal, SS3 "n"},
        KeyMapping{Key::Numpad_Enter, SS3 "M"},
        //KeyMapping{Key::Numpad_Equal, SS3 "X"}, TODO: verify this mapping and this whole map
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


void InputGenerator::setCursorKeysMode(KeyMode _mode)
{
    cursorKeysMode_ = _mode;
}

void InputGenerator::setNumpadKeysMode(KeyMode _mode)
{
    numpadKeysMode_ = _mode;
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

void InputGenerator::swap(SequenceList& _other)
{
    std::swap(pendingSequences_, _other);
}

inline bool InputGenerator::emit(std::string _sequence)
{
    pendingSequences_.emplace_back(move(_sequence));
    return true;
}

inline bool InputGenerator::emit(std::string_view _sequence)
{
    pendingSequences_.emplace_back(_sequence);
    return true;
}

inline bool InputGenerator::emit(char _asciiChar)
{
    pendingSequences_.emplace_back(string(1, _asciiChar));
    return true;
}

template <typename T, size_t N>
inline bool InputGenerator::emit(T(&_sequence)[N])
{
    pendingSequences_.emplace_back(string(_sequence));
    return true;
}

} // namespace terminal
