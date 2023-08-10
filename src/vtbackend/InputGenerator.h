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
enum class mouse_protocol
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

class modifier
{
  public:
    enum key : unsigned
    {
        None = 0,
        Shift = 1,
        Alt = 2,
        Control = 4,
        Meta = 8,
    };

    constexpr modifier(key key): _mask { static_cast<unsigned>(key) } {}

    constexpr modifier() = default;
    constexpr modifier(modifier&&) = default;
    constexpr modifier(modifier const&) = default;
    constexpr modifier& operator=(modifier&&) = default;
    constexpr modifier& operator=(modifier const&) = default;

    [[nodiscard]] constexpr unsigned value() const noexcept { return _mask; }

    [[nodiscard]] constexpr bool none() const noexcept { return value() == 0; }
    [[nodiscard]] constexpr bool some() const noexcept { return value() != 0; }
    [[nodiscard]] constexpr bool shift() const noexcept { return value() & Shift; }
    [[nodiscard]] constexpr bool alt() const noexcept { return value() & Alt; }
    [[nodiscard]] constexpr bool control() const noexcept { return value() & Control; }
    [[nodiscard]] constexpr bool meta() const noexcept { return value() & Meta; }

    constexpr operator unsigned() const noexcept { return _mask; }

    [[nodiscard]] constexpr bool any() const noexcept { return _mask != 0; }

    constexpr modifier& operator|=(modifier const& other) noexcept
    {
        _mask |= other._mask;
        return *this;
    }

    [[nodiscard]] constexpr modifier with(modifier const& other) const noexcept
    {
        return modifier(static_cast<key>(_mask | other._mask));
    }

    [[nodiscard]] constexpr modifier without(modifier const& other) const noexcept
    {
        return modifier(static_cast<key>(_mask & ~other._mask));
    }

    [[nodiscard]] bool contains(modifier const& other) const noexcept
    {
        return (_mask & other._mask) == other._mask;
    }

    constexpr void enable(key key) noexcept { _mask |= key; }

    constexpr void disable(key key) noexcept { _mask &= ~static_cast<unsigned>(key); }

  private:
    unsigned _mask = 0;
};

constexpr modifier operator|(modifier a, modifier b) noexcept
{
    return modifier(static_cast<modifier::key>(a.value() | b.value()));
}

constexpr bool operator<(modifier lhs, modifier rhs) noexcept
{
    return lhs.value() < rhs.value();
}

constexpr bool operator==(modifier lhs, modifier rhs) noexcept
{
    return lhs.value() == rhs.value();
}

std::optional<modifier::key> parseModifierKey(std::string const& key);

constexpr bool operator!(modifier modifier) noexcept
{
    return modifier.none();
}

constexpr bool operator==(modifier lhs, modifier::key rhs) noexcept
{
    return static_cast<modifier::key>(lhs.value()) == rhs;
}

constexpr modifier operator+(modifier::key lhs, modifier::key rhs) noexcept
{
    return modifier(static_cast<modifier::key>(static_cast<unsigned>(lhs) | static_cast<unsigned>(rhs)));
}

/// @returns CSI parameter for given function key modifier
constexpr size_t makeVirtualTerminalParam(modifier modifier) noexcept
{
    return 1 + modifier.value();
}

std::string to_string(modifier modifier);

// }}}
// {{{ KeyInputEvent, Key
enum class key
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

std::string to_string(key key);

enum class key_mode
{
    Normal,
    Application
};
// }}}
// {{{ Mouse
enum class mouse_button
{
    Left,
    Right,
    Middle,
    Release, // Button was released and/or no button is pressed.
    WheelUp,
    WheelDown,
};

std::string to_string(mouse_button button);

enum class mouse_transport
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

class input_generator
{
  public:
    using sequence = std::vector<char>;

    /// Changes the input mode for cursor keys.
    void setCursorKeysMode(key_mode mode);

    /// Changes the input mode for numpad keys.
    void setNumpadKeysMode(key_mode mode);

    void setApplicationKeypadMode(bool enable);

    [[nodiscard]] bool normalCursorKeys() const noexcept { return _cursorKeysMode == key_mode::Normal; }
    [[nodiscard]] bool applicationCursorKeys() const noexcept { return !normalCursorKeys(); }

    [[nodiscard]] bool numericKeypad() const noexcept { return _numpadKeysMode == key_mode::Normal; }
    [[nodiscard]] bool applicationKeypad() const noexcept { return !numericKeypad(); }

    [[nodiscard]] bool bracketedPaste() const noexcept { return _bracketedPaste; }
    void setBracketedPaste(bool enable) { _bracketedPaste = enable; }

    void setMouseProtocol(mouse_protocol mouseProtocol, bool enabled);
    [[nodiscard]] std::optional<mouse_protocol> mouseProtocol() const noexcept { return _mouseProtocol; }

    // Sets mouse event transport protocol (default, extended, xgr, urxvt)
    void setMouseTransport(mouse_transport mouseTransport);
    [[nodiscard]] mouse_transport mouseTransport() const noexcept { return _mouseTransport; }

    enum class mouse_wheel_mode
    {
        // mouse wheel generates mouse wheel events as determined by mouse protocol + transport.
        Default,
        // mouse wheel generates normal cursor key events
        NormalCursorKeys,
        // mouse wheel generates application cursor key events
        ApplicationCursorKeys
    };

    void setMouseWheelMode(mouse_wheel_mode mode) noexcept;
    [[nodiscard]] mouse_wheel_mode mouseWheelMode() const noexcept { return _mouseWheelMode; }

    void setGenerateFocusEvents(bool enable) noexcept { _generateFocusEvents = enable; }
    [[nodiscard]] bool generateFocusEvents() const noexcept { return _generateFocusEvents; }

    void setPassiveMouseTracking(bool v) noexcept { _passiveMouseTracking = v; }
    [[nodiscard]] bool passiveMouseTracking() const noexcept { return _passiveMouseTracking; }

    bool generate(char32_t characterEvent, modifier modifier);
    bool generate(std::u32string const& characterEvent, modifier modifier);
    bool generate(key key, modifier modifier);
    void generatePaste(std::string_view const& text);
    bool generateMousePress(modifier modifier,
                            mouse_button button,
                            cell_location pos,
                            pixel_coordinate pixelPosition,
                            bool uiHandled);
    bool generateMouseMove(modifier modifier,
                           cell_location pos,
                           pixel_coordinate pixelPosition,
                           bool uiHandled);
    bool generateMouseRelease(modifier modifier,
                              mouse_button button,
                              cell_location pos,
                              pixel_coordinate pixelPosition,
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

    enum class mouse_event_type
    {
        Press,
        Drag,
        Release
    };

    /// Resets the input generator's state, as required by the RIS (hard reset) VT sequence.
    void reset();

  private:
    bool generateMouse(mouse_event_type eventType,
                       modifier modifier,
                       mouse_button button,
                       cell_location pos,
                       pixel_coordinate pixelPosition,
                       bool uiHandled);

    bool mouseTransport(mouse_event_type eventType,
                        uint8_t button,
                        uint8_t modifier,
                        cell_location pos,
                        pixel_coordinate pixelPosition,
                        bool uiHandled);

    bool mouseTransportX10(uint8_t button, uint8_t modifier, cell_location pos);
    bool mouseTransportExtended(uint8_t button, uint8_t modifier, cell_location pos);

    bool mouseTransportSGR(
        mouse_event_type type, uint8_t button, uint8_t modifier, int x, int y, bool uiHandled);

    bool mouseTransportURXVT(mouse_event_type type, uint8_t button, uint8_t modifier, cell_location pos);

    inline bool append(std::string_view sequence);
    inline bool append(char asciiChar);
    inline bool append(uint8_t byte);
    inline bool append(unsigned int asciiChar);

    // private fields
    //
    key_mode _cursorKeysMode = key_mode::Normal;
    key_mode _numpadKeysMode = key_mode::Normal;
    bool _bracketedPaste = false;
    bool _generateFocusEvents = false;
    std::optional<mouse_protocol> _mouseProtocol = std::nullopt;
    bool _passiveMouseTracking = false;
    mouse_transport _mouseTransport = mouse_transport::Default;
    mouse_wheel_mode _mouseWheelMode = mouse_wheel_mode::Default;
    sequence _pendingSequence {};
    int _consumedBytes {};

    std::set<mouse_button> _currentlyPressedMouseButtons {};
    cell_location _currentMousePosition {}; // current mouse position
};

inline std::string to_string(input_generator::mouse_event_type value)
{
    switch (value)
    {
        case input_generator::mouse_event_type::Press: return "Press";
        case input_generator::mouse_event_type::Drag: return "Drag";
        case input_generator::mouse_event_type::Release: return "Release";
    }
    return "???";
}

} // namespace terminal

// {{{ fmtlib custom formatter support

template <>
struct fmt::formatter<terminal::mouse_protocol>: formatter<std::string_view>
{
    auto format(terminal::mouse_protocol value, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case terminal::mouse_protocol::X10: name = "X10"; break;
            case terminal::mouse_protocol::HighlightTracking: name = "HighlightTracking"; break;
            case terminal::mouse_protocol::ButtonTracking: name = "ButtonTracking"; break;
            case terminal::mouse_protocol::NormalTracking: name = "NormalTracking"; break;
            case terminal::mouse_protocol::AnyEventTracking: name = "AnyEventTracking"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<terminal::modifier>: formatter<std::string>
{
    auto format(terminal::modifier modifier, format_context& ctx) -> format_context::iterator
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
struct fmt::formatter<terminal::input_generator::mouse_wheel_mode>: formatter<std::string_view>
{
    auto format(terminal::input_generator::mouse_wheel_mode value, format_context& ctx)
        -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case terminal::input_generator::mouse_wheel_mode::Default: name = "Default"; break;
            case terminal::input_generator::mouse_wheel_mode::NormalCursorKeys:
                name = "NormalCursorKeys";
                break;
            case terminal::input_generator::mouse_wheel_mode::ApplicationCursorKeys:
                name = "ApplicationCursorKeys";
                break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<terminal::key_mode>: public formatter<std::string_view>
{
    auto format(terminal::key_mode value, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case terminal::key_mode::Normal: name = "Normal"; break;
            case terminal::key_mode::Application: name = "Application"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<terminal::mouse_button>: formatter<std::string_view>
{
    auto format(terminal::mouse_button value, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case terminal::mouse_button::Left: name = "Left"; break;
            case terminal::mouse_button::Right: name = "Right"; break;
            case terminal::mouse_button::Middle: name = "Middle"; break;
            case terminal::mouse_button::Release: name = "Release"; break;
            case terminal::mouse_button::WheelUp: name = "WheelUp"; break;
            case terminal::mouse_button::WheelDown: name = "WheelDown"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<terminal::mouse_transport>: formatter<std::string_view>
{
    auto format(terminal::mouse_transport value, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case terminal::mouse_transport::Default: name = "Default"; break;
            case terminal::mouse_transport::Extended: name = "Extended"; break;
            case terminal::mouse_transport::SGR: name = "SGR"; break;
            case terminal::mouse_transport::URXVT: name = "URXVT"; break;
            case terminal::mouse_transport::SGRPixels: name = "SGR-Pixels"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<terminal::key>: formatter<std::string_view>
{
    auto format(terminal::key value, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case terminal::key::F1: name = "F1"; break;
            case terminal::key::F2: name = "F2"; break;
            case terminal::key::F3: name = "F3"; break;
            case terminal::key::F4: name = "F4"; break;
            case terminal::key::F5: name = "F5"; break;
            case terminal::key::F6: name = "F6"; break;
            case terminal::key::F7: name = "F7"; break;
            case terminal::key::F8: name = "F8"; break;
            case terminal::key::F9: name = "F9"; break;
            case terminal::key::F10: name = "F10"; break;
            case terminal::key::F11: name = "F11"; break;
            case terminal::key::F12: name = "F12"; break;
            case terminal::key::F13: name = "F13"; break;
            case terminal::key::F14: name = "F14"; break;
            case terminal::key::F15: name = "F15"; break;
            case terminal::key::F16: name = "F16"; break;
            case terminal::key::F17: name = "F17"; break;
            case terminal::key::F18: name = "F18"; break;
            case terminal::key::F19: name = "F19"; break;
            case terminal::key::F20: name = "F20"; break;
            case terminal::key::DownArrow: name = "DownArrow"; break;
            case terminal::key::LeftArrow: name = "LeftArrow"; break;
            case terminal::key::RightArrow: name = "RightArrow"; break;
            case terminal::key::UpArrow: name = "UpArrow"; break;
            case terminal::key::Insert: name = "Insert"; break;
            case terminal::key::Delete: name = "Delete"; break;
            case terminal::key::Home: name = "Home"; break;
            case terminal::key::End: name = "End"; break;
            case terminal::key::PageUp: name = "PageUp"; break;
            case terminal::key::PageDown: name = "PageDown"; break;
            case terminal::key::Numpad_NumLock: name = "Numpad_NumLock"; break;
            case terminal::key::Numpad_Divide: name = "Numpad_Divide"; break;
            case terminal::key::Numpad_Multiply: name = "Numpad_Multiply"; break;
            case terminal::key::Numpad_Subtract: name = "Numpad_Subtract"; break;
            case terminal::key::Numpad_CapsLock: name = "Numpad_CapsLock"; break;
            case terminal::key::Numpad_Add: name = "Numpad_Add"; break;
            case terminal::key::Numpad_Decimal: name = "Numpad_Decimal"; break;
            case terminal::key::Numpad_Enter: name = "Numpad_Enter"; break;
            case terminal::key::Numpad_Equal: name = "Numpad_Equal"; break;
            case terminal::key::Numpad_0: name = "Numpad_0"; break;
            case terminal::key::Numpad_1: name = "Numpad_1"; break;
            case terminal::key::Numpad_2: name = "Numpad_2"; break;
            case terminal::key::Numpad_3: name = "Numpad_3"; break;
            case terminal::key::Numpad_4: name = "Numpad_4"; break;
            case terminal::key::Numpad_5: name = "Numpad_5"; break;
            case terminal::key::Numpad_6: name = "Numpad_6"; break;
            case terminal::key::Numpad_7: name = "Numpad_7"; break;
            case terminal::key::Numpad_8: name = "Numpad_8"; break;
            case terminal::key::Numpad_9: name = "Numpad_9"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};
// }}}
