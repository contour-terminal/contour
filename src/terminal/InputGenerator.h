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
#pragma once

#include <string>
#include <variant>
#include <vector>

namespace terminal {

class Modifier {
  public:
    enum Key : unsigned {
        None = 0,
        Shift = 1,
        Alt = 2,
        Control = 4,
        Meta = 8,
    };

    constexpr Modifier() : mask_{} {}
    constexpr Modifier(Key _key) : mask_{static_cast<unsigned>(_key)} {}

    constexpr unsigned value() const noexcept { return mask_; }

    constexpr bool none() const noexcept { return value() == 0; }
    constexpr bool some() const noexcept { return value() != 0; }
    constexpr bool shift() const noexcept { return value() & Shift; }
    constexpr bool alt() const noexcept { return value() & Alt; }
    constexpr bool control() const noexcept { return value() & Control; }
    constexpr bool meta() const noexcept { return value() & Meta; }

    constexpr operator unsigned () const noexcept { return mask_; }

    constexpr Modifier& operator|=(Modifier const& _other) noexcept
    {
        mask_ |= _other.mask_;
        return *this;
    }

  private:
    unsigned mask_;
};

constexpr bool operator!(Modifier _modifier) noexcept
{
    return _modifier.none();
}

constexpr bool operator==(Modifier _lhs, Modifier::Key _rhs) noexcept
{
    return static_cast<Modifier::Key>(_lhs.value()) == _rhs;
}

constexpr Modifier operator+(Modifier::Key _lhs, Modifier::Key _rhs) noexcept
{
    return Modifier(static_cast<Modifier::Key>(static_cast<unsigned>(_lhs) | static_cast<unsigned>(_rhs)));
}

/// @returns CSI parameter for given function key modifier
constexpr size_t makeVirtualTerminalParam(Modifier _modifier) noexcept
{
    switch (_modifier) {
        case Modifier::Key::Shift:
            return 2;
        case Modifier::Key::Alt:
            return 3;
        case Modifier::Key::Shift + Modifier::Key::Alt:
            return 4;
        case Modifier::Key::Control:
            return 5;
        case Modifier::Key::Shift + Modifier::Key::Control:
            return 6;
        case Modifier::Key::Alt + Modifier::Key::Control:
            return 7;
        case Modifier::Key::Shift + Modifier::Key::Alt + Modifier::Key::Control:
            return 8;
        case Modifier::Key::Meta:
            return 9;
        case Modifier::Key::Meta + Modifier::Key::Shift:
            return 10;
        case Modifier::Key::Meta + Modifier::Key::Alt:
            return 11;
        case Modifier::Key::Meta + Modifier::Key::Alt + Modifier::Key::Shift:
            return 12;
        case Modifier::Key::Meta + Modifier::Key::Control:
            return 13;
        case Modifier::Key::Meta + Modifier::Key::Control + Modifier::Key::Shift:
            return 14;
        case Modifier::Key::Meta + Modifier::Key::Control + Modifier::Key::Alt:
            return 15;
        case Modifier::Key::Meta + Modifier::Key::Control + Modifier::Key::Alt + Modifier::Key::Shift:
            return 16;
        default:
            return 0;
    }
}

std::string to_string(Modifier _modifier);

enum class Key {
    // C0 keys
    Enter,
    Backspace,
    Tab,
    Escape,

    // function keys
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,

    // cursor keys
    DownArrow,
    LeftArrow,
    RightArrow,
    UpArrow,

    // 6-key editing pad
    Insert,
    Delete,
    Home,
    End,
    PageUp,
    PageDown,

    // numpad keys
    Numpad_NumLock,
    Numpad_Divide,
    Numpad_Multiply,
    Numpad_Subtract,
    Numpad_CapsLock,
    Numpad_Add,
    Numpad_Decimal,
    Numpad_Enter,
    Numpad_Equal,
    Numpad_0,
    Numpad_1,
    Numpad_2,
    Numpad_3,
    Numpad_4,
    Numpad_5,
    Numpad_6,
    Numpad_7,
    Numpad_8,
    Numpad_9,
};

std::string to_string(Key _key);

struct MouseMoveEvent {
    int row;
    int column;
};

enum class MouseButtonEvent {
    Left,
    Right,
    Middle,
    WheelUp,
    WheelDown,
};

using MouseEvent = std::variant<MouseButtonEvent, MouseMoveEvent>;

enum class KeyMode {
    Normal,
    Application
};

class InputGenerator {
  public:
    using Sequence = std::vector<char>;

    /// Changes the input mode for cursor keys.
    void setCursorKeysMode(KeyMode _mode);

    /// Changes the input mode for numpad keys.
    void setNumpadKeysMode(KeyMode _mode);

    /// Generates input sequence for a pressed character.
    bool generate(char32_t _characterEvent, Modifier _modifier);

    /// Generates input sequence for a pressed special key.
    bool generate(Key _key, Modifier _modifier);

    /// Generates input sequence for a mouse button press event.
    //TODO: void generate(MouseButtonEvent _mouseButton, Modifier _modifier);

    /// Generates input sequence for a mouse move event.
    //TODO: void generate(MouseMoveEvent _mouseMove);

    /// Swaps out the generated input control sequences.
    void swap(Sequence& _other);

    bool normalCursorKeys() const noexcept { return cursorKeysMode_ == KeyMode::Normal; }
    bool applicationCursorKeys() const noexcept { return !normalCursorKeys(); }

    bool numericKeypad() const noexcept { return numpadKeysMode_ == KeyMode::Normal; }
    bool applicationKeypad() const noexcept { return !numericKeypad(); }

  private:
    inline bool emit(std::string _sequence);
    inline bool emit(std::string_view _sequence);
    inline bool emit(char _asciiChar);
    template <typename T, size_t N> inline bool emit(T (&_sequence)[N]);

  private:
    KeyMode cursorKeysMode_ = KeyMode::Normal;
    KeyMode numpadKeysMode_ = KeyMode::Normal;
    Sequence pendingSequence_{};
};

}  // namespace terminal
