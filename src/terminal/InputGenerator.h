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
#pragma once

#include <terminal/Sequencer.h> // MouseProtocol
#include <terminal/Coordinate.h>

#include <crispy/overloaded.h>
#include <crispy/escape.h>
#include <unicode/convert.h>

#include <optional>
#include <set>
#include <string>
#include <string_view>
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

    constexpr bool any() const noexcept { return mask_ != 0; }

    constexpr Modifier& operator|=(Modifier const& _other) noexcept
    {
        mask_ |= _other.mask_;
        return *this;
    }

    constexpr Modifier with(Modifier const& _other) const noexcept
    {
        return Modifier(static_cast<Key>(mask_ | _other.mask_));
    }

    constexpr Modifier without(Modifier const& _other) const noexcept
    {
        return Modifier(static_cast<Key>(mask_ & ~_other.mask_));
    }

    bool contains(Modifier const& _other) const noexcept
    {
        return (mask_ & _other.mask_) == _other.mask_;
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
    F13,
    F14,
    F15,
    F16,
    F17,
    F18,
    F19,
    F20,

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

constexpr bool operator==(KeyInputEvent _lhs, KeyInputEvent _rhs) noexcept
{
    return _lhs.key == _rhs.key &&
           _lhs.modifier == _rhs.modifier;
}

constexpr bool operator!=(KeyInputEvent _lhs, KeyInputEvent _rhs) noexcept
{
    return !(_lhs == _rhs);
}

struct CharInputEvent {
    char32_t value{};
    Modifier modifier{};
};

constexpr bool operator==(CharInputEvent _lhs, CharInputEvent _rhs) noexcept
{
    return _lhs.value == _rhs.value &&
           _lhs.modifier == _rhs.modifier;
}

constexpr bool operator!=(CharInputEvent _lhs, CharInputEvent _rhs) noexcept
{
    return !(_lhs == _rhs);
}

enum class MouseButton {
    Left,
    Right,
    Middle,
    Release, // Button was released and/or no button is pressed.
    WheelUp,
    WheelDown,
};
std::string to_string(MouseButton _button);

struct MousePressEvent {
    MouseButton button;
    Modifier modifier{};
    int row = 1;
    int column = 1;
};

constexpr bool operator==(MousePressEvent a, MousePressEvent b) noexcept
{
    return a.button == b.button
        && a.modifier == b.modifier
        && a.column == b.column
        && a.row == b.row;
}

constexpr bool operator!=(MousePressEvent a, MousePressEvent b) noexcept
{
    return !(a == b);
}

struct MouseMoveEvent {
    /// Row number in screen coordinates [1..rows]
    int row;

    /// Column number in screen coordinates [1..cols]
    int column;

    Modifier modifier{};

    constexpr auto as_pair() const noexcept { return std::pair{ row, column }; }

    constexpr auto coordinates() const noexcept { return Coordinate{ row, column }; }
};

struct MouseReleaseEvent {
    MouseButton button;
    Modifier modifier{};
    int row = 1;
    int column = 1;
};

struct FocusInEvent {};
struct FocusOutEvent {};

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

    /// Generates input sequence for a pressed character.
    bool generate(char32_t _characterEvent, Modifier _modifier);
    bool generate(std::u32string const& _characterEvent, Modifier _modifier);

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

    /// Generates raw input, usually used for sending reply VT sequences.
    bool generate(std::string_view const& _raw);

    /// Swaps out the generated input control sequences.
    void swap(Sequence& _other);

    enum class MouseEventType { Press, Drag, Release };

    /// Resets the input generator's state, as required by the RIS (hard reset) VT sequence.
    void reset();

  private:
    bool generateMouse(MouseButton _button,
                       Modifier _modifier,
                       int _row,
                       int _column,
                       MouseEventType _eventType);

    bool mouseTransport(uint8_t _button, uint8_t _modifier, int _row, int _column, MouseEventType _type);
    bool mouseTransportX10(uint8_t _button, uint8_t _modifier, int _row, int _column);
    bool mouseTransportSGR(uint8_t _button, uint8_t _modifier, int _row, int _column, MouseEventType _type);
    bool mouseTransportURXVT(uint8_t _button, uint8_t _modifier, int _row, int _column, MouseEventType _type);

    inline bool append(std::string_view _sequence);
    inline bool append(char _asciiChar);
    inline bool append(uint8_t _byte);
    inline bool append(unsigned int _asciiChar);

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
    Coordinate currentMousePosition_{0, 0}; // current mouse position
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
    struct formatter<terminal::Modifier> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::Modifier _modifier, FormatContext& _ctx)
        {
            std::string s;
            auto const advance = [&](bool _cond, std::string_view _text) {
                if (!_cond)
                    return;
                if (!s.empty())
                    s += ',';
                s += _text;
            };
            advance(_modifier.alt(), "Alt");
            advance(_modifier.shift(), "Shift");
            advance(_modifier.control(), "Control");
            advance(_modifier.meta(), "Meta");
            if (s.empty())
                s = "None";
            return format_to(_ctx.out(), "{}", s);
        }
    };

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
                    return format_to(_ctx.out(), "NormalCursorKeys");
                case terminal::InputGenerator::MouseWheelMode::ApplicationCursorKeys:
                    return format_to(_ctx.out(), "ApplicationCursorKeys");
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

    template <>
    struct formatter<terminal::MouseButton> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::MouseButton _value, FormatContext& _ctx)
        {
            switch (_value)
            {
                case terminal::MouseButton::Left: return format_to(_ctx.out(), "Left");
                case terminal::MouseButton::Right: return format_to(_ctx.out(), "Right");
                case terminal::MouseButton::Middle: return format_to(_ctx.out(), "Middle");
                case terminal::MouseButton::Release: return format_to(_ctx.out(), "Release");
                case terminal::MouseButton::WheelUp: return format_to(_ctx.out(), "WheelUp");
                case terminal::MouseButton::WheelDown: return format_to(_ctx.out(), "WheelDown");
            }
            return format_to(_ctx.out(), "<{}>", unsigned(_value));
        }
    };

    template <>
    struct formatter<terminal::MouseTransport> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::MouseTransport _value, FormatContext& _ctx)
        {
            switch (_value)
            {
                case terminal::MouseTransport::Default: return format_to(_ctx.out(), "Default");
                case terminal::MouseTransport::Extended: return format_to(_ctx.out(), "Extended");
                case terminal::MouseTransport::SGR: return format_to(_ctx.out(), "SGR");
                case terminal::MouseTransport::URXVT: return format_to(_ctx.out(), "URXVT");
            }
            return format_to(_ctx.out(), "<{}>", unsigned(_value));
        }
    };

    template <>
    struct formatter<terminal::InputGenerator::MouseEventType> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::InputGenerator::MouseEventType _value, FormatContext& _ctx)
        {
            switch (_value)
            {
                case terminal::InputGenerator::MouseEventType::Press: return format_to(_ctx.out(), "Press");
                case terminal::InputGenerator::MouseEventType::Drag: return format_to(_ctx.out(), "Drag");
                case terminal::InputGenerator::MouseEventType::Release: return format_to(_ctx.out(), "Release");
            }
            return format_to(_ctx.out(), "<{}>", unsigned(_value));
        }
    };

    template <>
    struct formatter<terminal::MousePressEvent> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::MousePressEvent const& ev, FormatContext& _ctx)
        {
            return format_to(_ctx.out(),
                ev.modifier.any()
                    ? "Mouse button press {}+{} ({}:{})"
                    : "Mouse button press {} ({}:{})",
                ev.button,
                ev.modifier,
                ev.row,
                ev.column
            );
        }
    };

    template <>
    struct formatter<terminal::MouseMoveEvent> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::MouseMoveEvent ev, FormatContext& _ctx)
        {
            return format_to(_ctx.out(),
                ev.modifier.any()
                    ? "MouseMoveEvent to {}:{} +{}"
                    : "MouseMoveEvent to {}:{}",
                ev.row,
                ev.column,
                ev.modifier
            );
        }
    };

    template <>
    struct formatter<terminal::MouseReleaseEvent> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::MouseReleaseEvent ev, FormatContext& _ctx)
        {
            return format_to(_ctx.out(),
                "MouseReleaseEvent {} at {}:{} {}",
                ev.button,
                ev.row,
                ev.column,
                ev.modifier
            );
        }
    };

    template <>
    struct formatter<terminal::KeyInputEvent> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::KeyInputEvent _event, FormatContext& _ctx)
        {
            return format_to(_ctx.out(),
                _event.modifier.any()
                    ? "{}+{}"
                    : "{}",
                _event.key, _event.modifier
            );
        }
    };

    template <>
    struct formatter<terminal::CharInputEvent> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::CharInputEvent _event, FormatContext& _ctx)
        {
            auto const u8str = unicode::convert_to<char>(_event.value);
            if (_event.modifier.any())
                return format_to(_ctx.out(), "\"{}\"+{}", crispy::escape(u8str), _event.modifier);
            else
                return format_to(_ctx.out(), "\"{}\"", crispy::escape(u8str), _event.modifier);
        }
    };

    template <>
    struct formatter<terminal::FocusInEvent> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::FocusInEvent, FormatContext& _ctx)
        {
            return format_to(_ctx.out(), "FocusInEvent");
        }
    };

    template <>
    struct formatter<terminal::FocusOutEvent> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::FocusOutEvent, FormatContext& _ctx)
        {
            return format_to(_ctx.out(), "FocusOutEvent");
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
        size_t operator()(terminal::CharInputEvent const& _input) const noexcept {
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
} // }}}
