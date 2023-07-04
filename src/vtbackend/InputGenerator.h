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

#include <vtbackend/primitives.h>

#include <crispy/escape.h>
#include <crispy/overloaded.h>

#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <libunicode/convert.h>

namespace terminal
{

/// Mutualy exclusive mouse protocls.
enum class MouseProtocol
{
    /// Old X10 mouse protocol
    X10 = 9,
    /// Normal tracking mode, that's X10 with mouse release events and modifiers
    NormalTracking = 1000,
    /// Highlight mouse tracking
    HighlightTracking = 1001,
    /// Button-event tracking protocol.
    ButtonTracking = 1002,
    /// Like ButtonTracking plus motion events.
    AnyEventTracking = 1003,
};

class Modifier
{
  public:
    enum Key : unsigned
    {
        None = 0,
        Shift = 1,
        Alt = 2,
        Control = 4,
        Meta = 8,
    };

    constexpr Modifier(Key key): _mask { static_cast<unsigned>(key) } {}

    constexpr Modifier() = default;
    constexpr Modifier(Modifier&&) = default;
    constexpr Modifier(Modifier const&) = default;
    constexpr Modifier& operator=(Modifier&&) = default;
    constexpr Modifier& operator=(Modifier const&) = default;

    [[nodiscard]] constexpr unsigned value() const noexcept { return _mask; }

    [[nodiscard]] constexpr bool none() const noexcept { return value() == 0; }
    [[nodiscard]] constexpr bool some() const noexcept { return value() != 0; }
    [[nodiscard]] constexpr bool shift() const noexcept { return value() & Shift; }
    [[nodiscard]] constexpr bool alt() const noexcept { return value() & Alt; }
    [[nodiscard]] constexpr bool control() const noexcept { return value() & Control; }
    [[nodiscard]] constexpr bool meta() const noexcept { return value() & Meta; }

    constexpr operator unsigned() const noexcept { return _mask; }

    [[nodiscard]] constexpr bool any() const noexcept { return _mask != 0; }

    constexpr Modifier& operator|=(Modifier const& other) noexcept
    {
        _mask |= other._mask;
        return *this;
    }

    [[nodiscard]] constexpr Modifier with(Modifier const& other) const noexcept
    {
        return Modifier(static_cast<Key>(_mask | other._mask));
    }

    [[nodiscard]] constexpr Modifier without(Modifier const& other) const noexcept
    {
        return Modifier(static_cast<Key>(_mask & ~other._mask));
    }

    [[nodiscard]] bool contains(Modifier const& other) const noexcept
    {
        return (_mask & other._mask) == other._mask;
    }

    constexpr void enable(Key key) noexcept { _mask |= key; }

    constexpr void disable(Key key) noexcept { _mask &= ~static_cast<unsigned>(key); }

  private:
    unsigned _mask = 0;
};

constexpr Modifier operator|(Modifier a, Modifier b) noexcept
{
    return Modifier(static_cast<Modifier::Key>(a.value() | b.value()));
}

constexpr bool operator<(Modifier lhs, Modifier rhs) noexcept
{
    return lhs.value() < rhs.value();
}

constexpr bool operator==(Modifier lhs, Modifier rhs) noexcept
{
    return lhs.value() == rhs.value();
}

std::optional<Modifier::Key> parseModifierKey(std::string const& key);

constexpr bool operator!(Modifier modifier) noexcept
{
    return modifier.none();
}

constexpr bool operator==(Modifier lhs, Modifier::Key rhs) noexcept
{
    return static_cast<Modifier::Key>(lhs.value()) == rhs;
}

constexpr Modifier operator+(Modifier::Key lhs, Modifier::Key rhs) noexcept
{
    return Modifier(static_cast<Modifier::Key>(static_cast<unsigned>(lhs) | static_cast<unsigned>(rhs)));
}

/// @returns CSI parameter for given function key modifier
constexpr size_t makeVirtualTerminalParam(Modifier modifier) noexcept
{
    return 1 + modifier.value();
}

std::string to_string(Modifier modifier);

// }}}
// {{{ KeyInputEvent, Key
enum class Key
{
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
    // NOLINTBEGIN(readability-identifier-naming)
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
    // NOLINTEND(readability-identifier-naming)
};

std::string to_string(Key key);

enum class KeyMode
{
    Normal,
    Application
};
// }}}
// {{{ Mouse
enum class MouseButton
{
    Left,
    Right,
    Middle,
    Release, // Button was released and/or no button is pressed.
    WheelUp,
    WheelDown,
};

std::string to_string(MouseButton button);

enum class MouseTransport
{
    // CSI M Cb Cx Cy, with Cb, Cx, Cy incremented by 0x20
    Default,
    // CSI M Cb Coords, with Coords being UTF-8 encoded, Coords is a tuple, each value incremented by 0x20.
    Extended,
    // `CSI Cb Cx Cy M` and `CSI Cb Cx Cy m` (button release)
    SGR,
    // SGR-Pixels (1016), an xterm extension as of Patch #359 - 2020/08/17
    // This is just like SGR but reports pixels isntead of ANSI cursor positions.
    SGRPixels,
    // `CSI < Cb Cx Cy M` with Cb += 0x20
    URXVT,
};
// }}}

class InputGenerator
{
  public:
    using Sequence = std::vector<char>;

    /// Changes the input mode for cursor keys.
    void setCursorKeysMode(KeyMode mode);

    /// Changes the input mode for numpad keys.
    void setNumpadKeysMode(KeyMode mode);

    void setApplicationKeypadMode(bool enable);

    [[nodiscard]] bool normalCursorKeys() const noexcept { return _cursorKeysMode == KeyMode::Normal; }
    [[nodiscard]] bool applicationCursorKeys() const noexcept { return !normalCursorKeys(); }

    [[nodiscard]] bool numericKeypad() const noexcept { return _numpadKeysMode == KeyMode::Normal; }
    [[nodiscard]] bool applicationKeypad() const noexcept { return !numericKeypad(); }

    [[nodiscard]] bool bracketedPaste() const noexcept { return _bracketedPaste; }
    void setBracketedPaste(bool enable) { _bracketedPaste = enable; }

    void setMouseProtocol(MouseProtocol mouseProtocol, bool enabled);
    [[nodiscard]] std::optional<MouseProtocol> mouseProtocol() const noexcept { return _mouseProtocol; }

    // Sets mouse event transport protocol (default, extended, xgr, urxvt)
    void setMouseTransport(MouseTransport mouseTransport);
    [[nodiscard]] MouseTransport mouseTransport() const noexcept { return _mouseTransport; }

    enum class MouseWheelMode
    {
        // mouse wheel generates mouse wheel events as determined by mouse protocol + transport.
        Default,
        // mouse wheel generates normal cursor key events
        NormalCursorKeys,
        // mouse wheel generates application cursor key events
        ApplicationCursorKeys
    };

    void setMouseWheelMode(MouseWheelMode mode) noexcept;
    [[nodiscard]] MouseWheelMode mouseWheelMode() const noexcept { return _mouseWheelMode; }

    void setGenerateFocusEvents(bool enable) noexcept { _generateFocusEvents = enable; }
    [[nodiscard]] bool generateFocusEvents() const noexcept { return _generateFocusEvents; }

    void setPassiveMouseTracking(bool v) noexcept { _passiveMouseTracking = v; }
    [[nodiscard]] bool passiveMouseTracking() const noexcept { return _passiveMouseTracking; }

    bool generate(char32_t characterEvent, Modifier modifier);
    bool generate(std::u32string const& characterEvent, Modifier modifier);
    bool generate(Key key, Modifier modifier);
    void generatePaste(std::string_view const& text);
    bool generateMousePress(Modifier modifier,
                            MouseButton button,
                            CellLocation pos,
                            PixelCoordinate pixelPosition,
                            bool uiHandled);
    bool generateMouseMove(Modifier modifier,
                           CellLocation pos,
                           PixelCoordinate pixelPosition,
                           bool uiHandled);
    bool generateMouseRelease(Modifier modifier,
                              MouseButton button,
                              CellLocation pos,
                              PixelCoordinate pixelPosition,
                              bool uiHandled);

    bool generateFocusInEvent();
    bool generateFocusOutEvent();

    /// Generates raw input, usually used for sending reply VT sequences.
    bool generateRaw(std::string_view const& raw);

    /// Peeks into the generated output, returning it as string view.
    ///
    /// @return a view into the generated buffer sequence.
    [[nodiscard]] std::string_view peek() const noexcept
    {
        return std::string_view(_pendingSequence.data() + _consumedBytes,
                                size_t(_pendingSequence.size() - size_t(_consumedBytes)));
    }

    void consume(int n)
    {
        _consumedBytes += n;
        if (_consumedBytes == static_cast<int>(_pendingSequence.size()))
        {
            _consumedBytes = 0;
            _pendingSequence.clear();
        }
    }

    enum class MouseEventType
    {
        Press,
        Drag,
        Release
    };

    /// Resets the input generator's state, as required by the RIS (hard reset) VT sequence.
    void reset();

  private:
    bool generateMouse(MouseEventType eventType,
                       Modifier modifier,
                       MouseButton button,
                       CellLocation pos,
                       PixelCoordinate pixelPosition,
                       bool uiHandled);

    bool mouseTransport(MouseEventType eventType,
                        uint8_t button,
                        uint8_t modifier,
                        CellLocation pos,
                        PixelCoordinate pixelPosition,
                        bool uiHandled);

    bool mouseTransportX10(uint8_t button, uint8_t modifier, CellLocation pos);
    bool mouseTransportExtended(uint8_t button, uint8_t modifier, CellLocation pos);

    bool mouseTransportSGR(
        MouseEventType type, uint8_t button, uint8_t modifier, int x, int y, bool uiHandled);

    bool mouseTransportURXVT(MouseEventType type, uint8_t button, uint8_t modifier, CellLocation pos);

    inline bool append(std::string_view sequence);
    inline bool append(char asciiChar);
    inline bool append(uint8_t byte);
    inline bool append(unsigned int asciiChar);

    // private fields
    //
    KeyMode _cursorKeysMode = KeyMode::Normal;
    KeyMode _numpadKeysMode = KeyMode::Normal;
    bool _bracketedPaste = false;
    bool _generateFocusEvents = false;
    std::optional<MouseProtocol> _mouseProtocol = std::nullopt;
    bool _passiveMouseTracking = false;
    MouseTransport _mouseTransport = MouseTransport::Default;
    MouseWheelMode _mouseWheelMode = MouseWheelMode::Default;
    Sequence _pendingSequence {};
    int _consumedBytes {};

    std::set<MouseButton> _currentlyPressedMouseButtons {};
    CellLocation _currentMousePosition {}; // current mouse position
};

inline std::string to_string(InputGenerator::MouseEventType value)
{
    switch (value)
    {
        case InputGenerator::MouseEventType::Press: return "Press";
        case InputGenerator::MouseEventType::Drag: return "Drag";
        case InputGenerator::MouseEventType::Release: return "Release";
    }
    return "???";
}

} // namespace terminal

// {{{ fmtlib custom formatter support

template <>
struct fmt::formatter<terminal::MouseProtocol>: formatter<std::string_view>
{
    auto format(terminal::MouseProtocol value, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case terminal::MouseProtocol::X10: name = "X10"; break;
            case terminal::MouseProtocol::HighlightTracking: name = "HighlightTracking"; break;
            case terminal::MouseProtocol::ButtonTracking: name = "ButtonTracking"; break;
            case terminal::MouseProtocol::NormalTracking: name = "NormalTracking"; break;
            case terminal::MouseProtocol::AnyEventTracking: name = "AnyEventTracking"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<terminal::Modifier>: formatter<std::string>
{
    auto format(terminal::Modifier modifier, format_context& ctx) -> format_context::iterator
    {
        std::string s;
        auto const advance = [&](bool cond, std::string_view text) {
            if (!cond)
                return;
            if (!s.empty())
                s += ',';
            s += text;
        };
        advance(modifier.alt(), "Alt");
        advance(modifier.shift(), "Shift");
        advance(modifier.control(), "Control");
        advance(modifier.meta(), "Meta");
        if (s.empty())
            s = "None";
        return formatter<std::string>::format(s, ctx);
    }
};

template <>
struct fmt::formatter<terminal::InputGenerator::MouseWheelMode>: formatter<std::string_view>
{
    auto format(terminal::InputGenerator::MouseWheelMode value, format_context& ctx)
        -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case terminal::InputGenerator::MouseWheelMode::Default: name = "Default"; break;
            case terminal::InputGenerator::MouseWheelMode::NormalCursorKeys: name = "NormalCursorKeys"; break;
            case terminal::InputGenerator::MouseWheelMode::ApplicationCursorKeys:
                name = "ApplicationCursorKeys";
                break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<terminal::KeyMode>: public formatter<std::string_view>
{
    auto format(terminal::KeyMode value, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case terminal::KeyMode::Normal: name = "Normal"; break;
            case terminal::KeyMode::Application: name = "Application"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<terminal::MouseButton>: formatter<std::string_view>
{
    auto format(terminal::MouseButton value, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case terminal::MouseButton::Left: name = "Left"; break;
            case terminal::MouseButton::Right: name = "Right"; break;
            case terminal::MouseButton::Middle: name = "Middle"; break;
            case terminal::MouseButton::Release: name = "Release"; break;
            case terminal::MouseButton::WheelUp: name = "WheelUp"; break;
            case terminal::MouseButton::WheelDown: name = "WheelDown"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<terminal::MouseTransport>: formatter<std::string_view>
{
    auto format(terminal::MouseTransport value, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case terminal::MouseTransport::Default: name = "Default"; break;
            case terminal::MouseTransport::Extended: name = "Extended"; break;
            case terminal::MouseTransport::SGR: name = "SGR"; break;
            case terminal::MouseTransport::URXVT: name = "URXVT"; break;
            case terminal::MouseTransport::SGRPixels: name = "SGR-Pixels"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<terminal::Key>: formatter<std::string_view>
{
    auto format(terminal::Key value, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case terminal::Key::F1: name = "F1"; break;
            case terminal::Key::F2: name = "F2"; break;
            case terminal::Key::F3: name = "F3"; break;
            case terminal::Key::F4: name = "F4"; break;
            case terminal::Key::F5: name = "F5"; break;
            case terminal::Key::F6: name = "F6"; break;
            case terminal::Key::F7: name = "F7"; break;
            case terminal::Key::F8: name = "F8"; break;
            case terminal::Key::F9: name = "F9"; break;
            case terminal::Key::F10: name = "F10"; break;
            case terminal::Key::F11: name = "F11"; break;
            case terminal::Key::F12: name = "F12"; break;
            case terminal::Key::F13: name = "F13"; break;
            case terminal::Key::F14: name = "F14"; break;
            case terminal::Key::F15: name = "F15"; break;
            case terminal::Key::F16: name = "F16"; break;
            case terminal::Key::F17: name = "F17"; break;
            case terminal::Key::F18: name = "F18"; break;
            case terminal::Key::F19: name = "F19"; break;
            case terminal::Key::F20: name = "F20"; break;
            case terminal::Key::DownArrow: name = "DownArrow"; break;
            case terminal::Key::LeftArrow: name = "LeftArrow"; break;
            case terminal::Key::RightArrow: name = "RightArrow"; break;
            case terminal::Key::UpArrow: name = "UpArrow"; break;
            case terminal::Key::Insert: name = "Insert"; break;
            case terminal::Key::Delete: name = "Delete"; break;
            case terminal::Key::Home: name = "Home"; break;
            case terminal::Key::End: name = "End"; break;
            case terminal::Key::PageUp: name = "PageUp"; break;
            case terminal::Key::PageDown: name = "PageDown"; break;
            case terminal::Key::Numpad_NumLock: name = "Numpad_NumLock"; break;
            case terminal::Key::Numpad_Divide: name = "Numpad_Divide"; break;
            case terminal::Key::Numpad_Multiply: name = "Numpad_Multiply"; break;
            case terminal::Key::Numpad_Subtract: name = "Numpad_Subtract"; break;
            case terminal::Key::Numpad_CapsLock: name = "Numpad_CapsLock"; break;
            case terminal::Key::Numpad_Add: name = "Numpad_Add"; break;
            case terminal::Key::Numpad_Decimal: name = "Numpad_Decimal"; break;
            case terminal::Key::Numpad_Enter: name = "Numpad_Enter"; break;
            case terminal::Key::Numpad_Equal: name = "Numpad_Equal"; break;
            case terminal::Key::Numpad_0: name = "Numpad_0"; break;
            case terminal::Key::Numpad_1: name = "Numpad_1"; break;
            case terminal::Key::Numpad_2: name = "Numpad_2"; break;
            case terminal::Key::Numpad_3: name = "Numpad_3"; break;
            case terminal::Key::Numpad_4: name = "Numpad_4"; break;
            case terminal::Key::Numpad_5: name = "Numpad_5"; break;
            case terminal::Key::Numpad_6: name = "Numpad_6"; break;
            case terminal::Key::Numpad_7: name = "Numpad_7"; break;
            case terminal::Key::Numpad_8: name = "Numpad_8"; break;
            case terminal::Key::Numpad_9: name = "Numpad_9"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};
// }}}
