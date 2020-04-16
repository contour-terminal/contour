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

#include <terminal/util/overloaded.h>
#include <terminal/Commands.h>
#include <terminal/Util.h>

#include <optional>
#include <set>
#include <string>
#include <utility>
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

    constexpr Modifier(Key _key) : mask_{static_cast<unsigned>(_key)} {}

    constexpr Modifier() = default;
    constexpr Modifier(Modifier&&) = default;
    constexpr Modifier(Modifier const&) = default;
    constexpr Modifier& operator=(Modifier&&) = default;
    constexpr Modifier& operator=(Modifier const&) = default;

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

    constexpr void enable(Key _key) noexcept
    {
        mask_ |= _key;
    }

    constexpr void disable(Key _key) noexcept
    {
        mask_ &= ~static_cast<unsigned>(_key);
    }

  private:
    unsigned mask_ = 0;
};

constexpr bool operator<(Modifier _lhs, Modifier _rhs) noexcept
{
    return _lhs.value() < _rhs.value();
}

constexpr bool operator==(Modifier _lhs, Modifier _rhs) noexcept
{
    return _lhs.value() == _rhs.value();
}

std::optional<Modifier::Key> parseModifierKey(std::string const& _key);

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
    return 1 + _modifier.value();
}

std::string to_string(Modifier _modifier);

enum class Key {
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

enum class KeyMode {
    Normal,
    Application
};

struct KeyInputEvent {
    Key key{};
    Modifier modifier{};
};

struct CharInputEvent {
    char32_t value{};
    Modifier modifier{};
};

enum class MouseButton {
    Left,
    Right,
    Middle,
    WheelUp,
    WheelDown,
};
std::string to_string(MouseButton _button);

struct MousePressEvent {
    MouseButton button;
    Modifier modifier{};
    cursor_pos_t row = 1;
    cursor_pos_t column = 1;
};

struct MouseMoveEvent {
    /// Row number in screen coordinates [1..rows]
    cursor_pos_t row;

    /// Column number in screen coordinates [1..cols]
    cursor_pos_t column;

    constexpr auto as_pair() const noexcept { return std::pair{ row, column }; }

    constexpr auto coordinates() const noexcept { return Coordinate{ row, column }; }
};

struct MouseReleaseEvent {
    MouseButton button;
    Modifier modifier{};
    cursor_pos_t row = 1;
    cursor_pos_t column = 1;
};

struct FocusInEvent {};
struct FocusOutEvent {};

using InputEvent = std::variant<
    KeyInputEvent,
    CharInputEvent,
    MousePressEvent,
    MouseMoveEvent,
    MouseReleaseEvent,
    FocusInEvent,
    FocusOutEvent
>;

using MouseEvent = std::variant<
    MousePressEvent,
    MouseMoveEvent,
    MouseReleaseEvent
>;

constexpr Modifier modifier(InputEvent _event) noexcept
{
    return std::visit(overloaded{
        [](KeyInputEvent _keyInput) -> Modifier { return _keyInput.modifier; },
        [](CharInputEvent _charInput) -> Modifier { return _charInput.modifier; },
        [](MousePressEvent _mousePress) -> Modifier { return _mousePress.modifier; },
        [](MouseMoveEvent) -> Modifier { return Modifier::None; },
        [](MouseReleaseEvent) -> Modifier { return Modifier::None; },
        [](FocusInEvent) -> Modifier { return Modifier::None; },
        [](FocusOutEvent) -> Modifier { return Modifier::None; },
    }, _event);
}

constexpr bool operator<(InputEvent const& _lhs, InputEvent const& _rhs) noexcept
{
    if (modifier(_lhs) < modifier(_rhs))
        return true;

    if (_lhs.index() < _rhs.index())
        return true;

    if (_lhs.index() == _rhs.index() && modifier(_lhs) == modifier(_rhs))
    {
        if (std::holds_alternative<KeyInputEvent>(_lhs))
            return std::get<KeyInputEvent>(_lhs).key < std::get<KeyInputEvent>(_rhs).key;
        if (std::holds_alternative<CharInputEvent>(_lhs))
            return std::get<CharInputEvent>(_lhs).value < std::get<CharInputEvent>(_rhs).value;
        if (std::holds_alternative<MousePressEvent>(_lhs))
            return std::get<MousePressEvent>(_lhs).button < std::get<MousePressEvent>(_rhs).button;
        if (std::holds_alternative<MouseMoveEvent>(_lhs))
            return std::get<MouseMoveEvent>(_lhs).as_pair() < std::get<MouseMoveEvent>(_rhs).as_pair();
        if (std::holds_alternative<MouseReleaseEvent>(_lhs))
            return std::get<MouseReleaseEvent>(_lhs).button < std::get<MouseReleaseEvent>(_rhs).button;
        if (std::holds_alternative<FocusInEvent>(_lhs))
            return true;
        if (std::holds_alternative<FocusOutEvent>(_lhs))
            return true;
    }

    return false;
}

constexpr bool operator==(InputEvent const& _lhs, InputEvent const& _rhs) noexcept
{
    if (modifier(_lhs) == modifier(_rhs))
    {
        if (std::holds_alternative<KeyInputEvent>(_lhs) && std::holds_alternative<KeyInputEvent>(_rhs))
            return std::get<KeyInputEvent>(_lhs).key == std::get<KeyInputEvent>(_rhs).key;
        if (std::holds_alternative<CharInputEvent>(_lhs) && std::holds_alternative<CharInputEvent>(_rhs))
            return std::get<CharInputEvent>(_lhs).value == std::get<CharInputEvent>(_rhs).value;
        if (std::holds_alternative<MousePressEvent>(_lhs) && std::holds_alternative<MousePressEvent>(_rhs))
            return std::get<MousePressEvent>(_lhs).button == std::get<MousePressEvent>(_rhs).button;
        if (std::holds_alternative<FocusInEvent>(_lhs) && std::holds_alternative<FocusInEvent>(_rhs))
            return true;
        if (std::holds_alternative<FocusOutEvent>(_lhs) && std::holds_alternative<FocusOutEvent>(_rhs))
            return true;
    }
    return false;
}

enum class MouseTransport {
    // CSI M Cb Cx Cy, with Cb, Cx, Cy incremented by 0x20
    Default,
    // CSI M Cb Coords, with Coords being UTF-8 encoded, Coords is a tuple, each value incremented by 0x20.
    Extended,
    // `CSI Cb Cx Cy M` and `CSI Cb Cx Cy m` (button release)
    SGR,
    // `CSI < Cb Cx Cy M` with Cb += 0x20
    URXVT,
};

inline std::string to_string(MouseTransport _value)
{
    switch (_value)
    {
        case MouseTransport::Default:
            return "Default";
        case MouseTransport::Extended:
            return "Extended";
        case MouseTransport::SGR:
            return "SGR";
        case MouseTransport::URXVT:
            return "URXVT";
    }
    return "<Unknown MouseTransport>";
}

class InputGenerator {
  public:
    using Sequence = std::vector<char>;

    /// Changes the input mode for cursor keys.
    void setCursorKeysMode(KeyMode _mode);

    /// Changes the input mode for numpad keys.
    void setNumpadKeysMode(KeyMode _mode);

    void setApplicationKeypadMode(bool _enable);

    bool normalCursorKeys() const noexcept { return cursorKeysMode_ == KeyMode::Normal; }
    bool applicationCursorKeys() const noexcept { return !normalCursorKeys(); }

    bool numericKeypad() const noexcept { return numpadKeysMode_ == KeyMode::Normal; }
    bool applicationKeypad() const noexcept { return !numericKeypad(); }

    bool bracketedPaste() const noexcept { return bracketedPaste_; }
    void setBracketedPaste(bool _enable) { bracketedPaste_ = _enable; }

    void setMouseProtocol(MouseProtocol _mouseProtocol, bool _enabled);
    std::optional<MouseProtocol> mouseProtocol() const noexcept { return mouseProtocol_; }

    // Sets mouse event transport protocol (default, extended, xgr, urxvt)
    void setMouseTransport(MouseTransport _mouseTransport);
    MouseTransport mouseTransport() const noexcept { return mouseTransport_; }

    enum class MouseWheelMode {
        // mouse wheel generates mouse wheel events as determined by mouse protocol + transport.
        Default,
        // mouse wheel generates normal cursor key events
        NormalCursorKeys,
        // mouse wheel generates application cursor key events
        ApplicationCursorKeys
    };

    void setMouseWheelMode(MouseWheelMode _mode) noexcept;
    MouseWheelMode mouseWheelMode() const noexcept { return mouseWheelMode_; }

    void setGenerateFocusEvents(bool _enable) noexcept { generateFocusEvents_ = _enable; }
    bool generateFocusEvents() const noexcept { return generateFocusEvents_; };

    /// Generates input sequences for given input event.
    bool generate(InputEvent const& _inputEvent);

    /// Generates input sequence for a pressed character.
    bool generate(char32_t _characterEvent, Modifier _modifier);

    /// Generates input sequence for a pressed special key.
    bool generate(Key _key, Modifier _modifier);

    /// Generates input sequence for bracketed paste text.
    void generatePaste(std::string_view const& _text);

    /// Generates input sequence for a mouse button press event.
    bool generate(MousePressEvent const& _mousePress);

    /// Generates input sequence for a mouse button release event.
    bool generate(MouseReleaseEvent const& _mousePress);

    /// Generates input sequence for a mouse move event.
    bool generate(MouseMoveEvent const& _mouseMove);

    bool generate(FocusInEvent const&);
    bool generate(FocusOutEvent const&);

    /// Swaps out the generated input control sequences.
    void swap(Sequence& _other);

    enum class MouseEventType { Press, Drag, Release };

  private:
    bool generateMouse(MouseButton _button,
                       Modifier _modifier,
                       cursor_pos_t _row,
                       cursor_pos_t _column,
                       MouseEventType _eventType);

    inline bool append(std::string _sequence);
    inline bool append(std::string_view _sequence);
    inline bool append(char _asciiChar);
    inline bool append(uint8_t _byte);
    inline bool append(unsigned int _asciiChar);
    template <typename T, size_t N> inline bool append(T (&_sequence)[N]);

  private:
    KeyMode cursorKeysMode_ = KeyMode::Normal;
    KeyMode numpadKeysMode_ = KeyMode::Normal;
    bool bracketedPaste_ = false;
    bool generateFocusEvents_ = false;
    std::optional<MouseProtocol> mouseProtocol_ = std::nullopt;
    MouseTransport mouseTransport_ = MouseTransport::Default;
    MouseWheelMode mouseWheelMode_ = MouseWheelMode::Default;
    Sequence pendingSequence_{};

    std::set<MouseButton> currentlyPressedMouseButtons_{};
    terminal::Coordinate currentMousePosition_{0, 0}; // current mouse position
};

inline std::string to_string(InputGenerator::MouseEventType _value)
{
    switch (_value)
    {
        case InputGenerator::MouseEventType::Press: return "Press";
        case InputGenerator::MouseEventType::Drag: return "Drag";
        case InputGenerator::MouseEventType::Release: return "Release";
    }
    return "???";
}

}  // namespace terminal

namespace fmt { // {{{
    template <>
    struct formatter<terminal::InputGenerator::MouseWheelMode> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::InputGenerator::MouseWheelMode _value, FormatContext& _ctx)
        {
            switch (_value)
            {
                case terminal::InputGenerator::MouseWheelMode::Default:
                    return format_to(_ctx.out(), "Default");
                case terminal::InputGenerator::MouseWheelMode::NormalCursorKeys:
                    return format_to(_ctx.out(), "Normal");
                case terminal::InputGenerator::MouseWheelMode::ApplicationCursorKeys:
                    return format_to(_ctx.out(), "Application");
            }
            return format_to(_ctx.out(), "<{}>", unsigned(_value));
        }
    };

    template <>
    struct formatter<terminal::KeyMode> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::KeyMode _value, FormatContext& _ctx)
        {
            switch (_value)
            {
                case terminal::KeyMode::Application:
                    return format_to(_ctx.out(), "Application");
                case terminal::KeyMode::Normal:
                    return format_to(_ctx.out(), "Normal");
            }
            return format_to(_ctx.out(), "<{}>", unsigned(_value));
        }
    };
} // }}}

namespace std { // {{{
    template<>
    struct hash<terminal::KeyInputEvent> {
        constexpr size_t operator()(terminal::KeyInputEvent const& _input) const noexcept {
            return (1 << 16) | _input.modifier << 8 | (static_cast<unsigned>(_input.key) & 0xFF);
        }
    };

    template<>
    struct hash<terminal::CharInputEvent> {
        constexpr size_t operator()(terminal::CharInputEvent const& _input) const noexcept {
            return (2 << 16) | _input.modifier << 8 | (static_cast<unsigned>(_input.value) & 0xFF);
        }
    };

    template<>
    struct hash<terminal::MousePressEvent> {
        constexpr size_t operator()(terminal::MousePressEvent const& _input) const noexcept {
            return (3 << 16) | _input.modifier << 8 | (static_cast<unsigned>(_input.button) & 0xFF);
        }
    };

    template<>
    struct hash<terminal::MouseMoveEvent> {
        constexpr size_t operator()(terminal::MouseMoveEvent const& _input) const noexcept {
            return (4 << 16) | (_input.row << 8) | (_input.column & 0xFF);
        }
    };

    template<>
    struct hash<terminal::MouseReleaseEvent> {
        constexpr size_t operator()(terminal::MouseReleaseEvent const& _input) const noexcept {
            return (5 << 16) | (static_cast<unsigned>(_input.button) & 0xFF);
        }
    };

    template<>
    struct hash<terminal::FocusInEvent> {
        constexpr size_t operator()(terminal::FocusInEvent const&) const noexcept {
            return (6 << 16) | 1;
        }
    };

    template<>
    struct hash<terminal::FocusOutEvent> {
        constexpr size_t operator()(terminal::FocusOutEvent const&) const noexcept {
            return (6 << 16) | 2;
        }
    };

    template<>
    struct hash<terminal::MouseEvent> {
        constexpr size_t operator()(terminal::MouseEvent const& _input) const noexcept {
            return visit(overloaded{
                [](terminal::MousePressEvent ev) { return hash<terminal::MousePressEvent>{}(ev); },
                [](terminal::MouseMoveEvent ev) { return hash<terminal::MouseMoveEvent>{}(ev); },
                [](terminal::MouseReleaseEvent ev) { return hash<terminal::MouseReleaseEvent>{}(ev); },
            }, _input);
        }
    };

    template<>
    struct hash<terminal::InputEvent> {
        constexpr size_t operator()(terminal::InputEvent const& _input) const noexcept {
            return visit(overloaded{
                [](terminal::KeyInputEvent ev) { return hash<terminal::KeyInputEvent>{}(ev); },
                [](terminal::CharInputEvent ev) { return hash<terminal::CharInputEvent>{}(ev); },
                [](terminal::MousePressEvent ev) { return hash<terminal::MousePressEvent>{}(ev); },
                [](terminal::MouseMoveEvent ev) { return hash<terminal::MouseMoveEvent>{}(ev); },
                [](terminal::MouseReleaseEvent ev) { return hash<terminal::MouseReleaseEvent>{}(ev); },
                [](terminal::FocusInEvent ev) { return hash<terminal::FocusInEvent>{}(ev); },
                [](terminal::FocusOutEvent ev) { return hash<terminal::FocusOutEvent>{}(ev); },
            }, _input);
        }
    };
} // }}}
