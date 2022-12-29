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

#include <unicode/convert.h>

#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

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

    constexpr Modifier(Key key): mask_ { static_cast<unsigned>(key) } {}

    constexpr Modifier() = default;
    constexpr Modifier(Modifier&&) = default;
    constexpr Modifier(Modifier const&) = default;
    constexpr Modifier& operator=(Modifier&&) = default;
    constexpr Modifier& operator=(Modifier const&) = default;

    [[nodiscard]] constexpr unsigned value() const noexcept { return mask_; }

    [[nodiscard]] constexpr bool none() const noexcept { return value() == 0; }
    [[nodiscard]] constexpr bool some() const noexcept { return value() != 0; }
    [[nodiscard]] constexpr bool shift() const noexcept { return value() & Shift; }
    [[nodiscard]] constexpr bool alt() const noexcept { return value() & Alt; }
    [[nodiscard]] constexpr bool control() const noexcept { return value() & Control; }
    [[nodiscard]] constexpr bool meta() const noexcept { return value() & Meta; }

    constexpr operator unsigned() const noexcept { return mask_; }

    [[nodiscard]] constexpr bool any() const noexcept { return mask_ != 0; }

    constexpr Modifier& operator|=(Modifier const& other) noexcept
    {
        mask_ |= other.mask_;
        return *this;
    }

    [[nodiscard]] constexpr Modifier with(Modifier const& other) const noexcept
    {
        return Modifier(static_cast<Key>(mask_ | other.mask_));
    }

    [[nodiscard]] constexpr Modifier without(Modifier const& other) const noexcept
    {
        return Modifier(static_cast<Key>(mask_ & ~other.mask_));
    }

    [[nodiscard]] bool contains(Modifier const& other) const noexcept
    {
        return (mask_ & other.mask_) == other.mask_;
    }

    constexpr void enable(Key key) noexcept { mask_ |= key; }

    constexpr void disable(Key key) noexcept { mask_ &= ~static_cast<unsigned>(key); }

  private:
    unsigned mask_ = 0;
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

    [[nodiscard]] bool normalCursorKeys() const noexcept { return cursorKeysMode_ == KeyMode::Normal; }
    [[nodiscard]] bool applicationCursorKeys() const noexcept { return !normalCursorKeys(); }

    [[nodiscard]] bool numericKeypad() const noexcept { return numpadKeysMode_ == KeyMode::Normal; }
    [[nodiscard]] bool applicationKeypad() const noexcept { return !numericKeypad(); }

    [[nodiscard]] bool bracketedPaste() const noexcept { return bracketedPaste_; }
    void setBracketedPaste(bool enable) { bracketedPaste_ = enable; }

    void setMouseProtocol(MouseProtocol mouseProtocol, bool enabled);
    [[nodiscard]] std::optional<MouseProtocol> mouseProtocol() const noexcept { return mouseProtocol_; }

    // Sets mouse event transport protocol (default, extended, xgr, urxvt)
    void setMouseTransport(MouseTransport mouseTransport);
    [[nodiscard]] MouseTransport mouseTransport() const noexcept { return mouseTransport_; }

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
    [[nodiscard]] MouseWheelMode mouseWheelMode() const noexcept { return mouseWheelMode_; }

    void setGenerateFocusEvents(bool enable) noexcept { generateFocusEvents_ = enable; }
    [[nodiscard]] bool generateFocusEvents() const noexcept { return generateFocusEvents_; }

    void setPassiveMouseTracking(bool v) noexcept { passiveMouseTracking_ = v; }
    [[nodiscard]] bool passiveMouseTracking() const noexcept { return passiveMouseTracking_; }

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
        return std::string_view(pendingSequence_.data() + consumedBytes_,
                                size_t(pendingSequence_.size() - size_t(consumedBytes_)));
    }

    void consume(int n)
    {
        consumedBytes_ += n;
        if (consumedBytes_ == static_cast<int>(pendingSequence_.size()))
        {
            consumedBytes_ = 0;
            pendingSequence_.clear();
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
    KeyMode cursorKeysMode_ = KeyMode::Normal;
    KeyMode numpadKeysMode_ = KeyMode::Normal;
    bool bracketedPaste_ = false;
    bool generateFocusEvents_ = false;
    std::optional<MouseProtocol> mouseProtocol_ = std::nullopt;
    bool passiveMouseTracking_ = false;
    MouseTransport mouseTransport_ = MouseTransport::Default;
    MouseWheelMode mouseWheelMode_ = MouseWheelMode::Default;
    Sequence pendingSequence_ {};
    int consumedBytes_ {};

    std::set<MouseButton> currentlyPressedMouseButtons_ {};
    CellLocation currentMousePosition_ {}; // current mouse position
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

namespace fmt // {{{
{

template <>
struct formatter<terminal::MouseProtocol>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }

    template <typename FormatContext>
    auto format(terminal::MouseProtocol value, FormatContext& ctx)
    {
        switch (value)
        {
            case terminal::MouseProtocol::X10: return fmt::format_to(ctx.out(), "X10");
            case terminal::MouseProtocol::HighlightTracking:
                return fmt::format_to(ctx.out(), "HighlightTracking");
            case terminal::MouseProtocol::ButtonTracking: return fmt::format_to(ctx.out(), "ButtonTracking");
            case terminal::MouseProtocol::NormalTracking: return fmt::format_to(ctx.out(), "NormalTracking");
            case terminal::MouseProtocol::AnyEventTracking:
                return fmt::format_to(ctx.out(), "AnyEventTracking");
        }
        return fmt::format_to(ctx.out(), "{}", unsigned(value));
    }
};

template <>
struct formatter<terminal::Modifier>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::Modifier modifier, FormatContext& ctx)
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
        return fmt::format_to(ctx.out(), "{}", s);
    }
};

template <>
struct formatter<terminal::InputGenerator::MouseWheelMode>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::InputGenerator::MouseWheelMode value, FormatContext& ctx)
    {
        switch (value)
        {
            case terminal::InputGenerator::MouseWheelMode::Default:
                return fmt::format_to(ctx.out(), "Default");
            case terminal::InputGenerator::MouseWheelMode::NormalCursorKeys:
                return fmt::format_to(ctx.out(), "NormalCursorKeys");
            case terminal::InputGenerator::MouseWheelMode::ApplicationCursorKeys:
                return fmt::format_to(ctx.out(), "ApplicationCursorKeys");
        }
        return fmt::format_to(ctx.out(), "<{}>", unsigned(value));
    }
};

template <>
struct formatter<terminal::KeyMode>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::KeyMode value, FormatContext& ctx)
    {
        switch (value)
        {
            case terminal::KeyMode::Application: return fmt::format_to(ctx.out(), "Application");
            case terminal::KeyMode::Normal: return fmt::format_to(ctx.out(), "Normal");
        }
        return fmt::format_to(ctx.out(), "<{}>", unsigned(value));
    }
};

template <>
struct formatter<terminal::MouseButton>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::MouseButton value, FormatContext& ctx)
    {
        switch (value)
        {
            case terminal::MouseButton::Left: return fmt::format_to(ctx.out(), "Left");
            case terminal::MouseButton::Right: return fmt::format_to(ctx.out(), "Right");
            case terminal::MouseButton::Middle: return fmt::format_to(ctx.out(), "Middle");
            case terminal::MouseButton::Release: return fmt::format_to(ctx.out(), "Release");
            case terminal::MouseButton::WheelUp: return fmt::format_to(ctx.out(), "WheelUp");
            case terminal::MouseButton::WheelDown: return fmt::format_to(ctx.out(), "WheelDown");
        }
        return fmt::format_to(ctx.out(), "<{}>", unsigned(value));
    }
};

template <>
struct formatter<terminal::MouseTransport>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::MouseTransport value, FormatContext& ctx)
    {
        switch (value)
        {
            case terminal::MouseTransport::Default: return fmt::format_to(ctx.out(), "Default");
            case terminal::MouseTransport::Extended: return fmt::format_to(ctx.out(), "Extended");
            case terminal::MouseTransport::SGR: return fmt::format_to(ctx.out(), "SGR");
            case terminal::MouseTransport::URXVT: return fmt::format_to(ctx.out(), "URXVT");
            case terminal::MouseTransport::SGRPixels: return fmt::format_to(ctx.out(), "SGR-Pixels");
        }
        return fmt::format_to(ctx.out(), "<{}>", unsigned(value));
    }
};

template <>
struct formatter<terminal::Key>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    constexpr auto format(terminal::Key value, FormatContext& ctx) const
    {
        switch (value)
        {
            case terminal::Key::F1: return fmt::format_to(ctx.out(), "F1");
            case terminal::Key::F2: return fmt::format_to(ctx.out(), "F2");
            case terminal::Key::F3: return fmt::format_to(ctx.out(), "F3");
            case terminal::Key::F4: return fmt::format_to(ctx.out(), "F4");
            case terminal::Key::F5: return fmt::format_to(ctx.out(), "F5");
            case terminal::Key::F6: return fmt::format_to(ctx.out(), "F6");
            case terminal::Key::F7: return fmt::format_to(ctx.out(), "F7");
            case terminal::Key::F8: return fmt::format_to(ctx.out(), "F8");
            case terminal::Key::F9: return fmt::format_to(ctx.out(), "F9");
            case terminal::Key::F10: return fmt::format_to(ctx.out(), "F10");
            case terminal::Key::F11: return fmt::format_to(ctx.out(), "F11");
            case terminal::Key::F12: return fmt::format_to(ctx.out(), "F12");
            case terminal::Key::F13: return fmt::format_to(ctx.out(), "F13");
            case terminal::Key::F14: return fmt::format_to(ctx.out(), "F14");
            case terminal::Key::F15: return fmt::format_to(ctx.out(), "F15");
            case terminal::Key::F16: return fmt::format_to(ctx.out(), "F16");
            case terminal::Key::F17: return fmt::format_to(ctx.out(), "F17");
            case terminal::Key::F18: return fmt::format_to(ctx.out(), "F18");
            case terminal::Key::F19: return fmt::format_to(ctx.out(), "F19");
            case terminal::Key::F20: return fmt::format_to(ctx.out(), "F20");
            case terminal::Key::DownArrow: return fmt::format_to(ctx.out(), "DownArrow");
            case terminal::Key::LeftArrow: return fmt::format_to(ctx.out(), "LeftArrow");
            case terminal::Key::RightArrow: return fmt::format_to(ctx.out(), "RightArrow");
            case terminal::Key::UpArrow: return fmt::format_to(ctx.out(), "UpArrow");
            case terminal::Key::Insert: return fmt::format_to(ctx.out(), "Insert");
            case terminal::Key::Delete: return fmt::format_to(ctx.out(), "Delete");
            case terminal::Key::Home: return fmt::format_to(ctx.out(), "Home");
            case terminal::Key::End: return fmt::format_to(ctx.out(), "End");
            case terminal::Key::PageUp: return fmt::format_to(ctx.out(), "PageUp");
            case terminal::Key::PageDown: return fmt::format_to(ctx.out(), "PageDown");
            case terminal::Key::Numpad_NumLock: return fmt::format_to(ctx.out(), "Numpad_NumLock");
            case terminal::Key::Numpad_Divide: return fmt::format_to(ctx.out(), "Numpad_Divide");
            case terminal::Key::Numpad_Multiply: return fmt::format_to(ctx.out(), "Numpad_Multiply");
            case terminal::Key::Numpad_Subtract: return fmt::format_to(ctx.out(), "Numpad_Subtract");
            case terminal::Key::Numpad_CapsLock: return fmt::format_to(ctx.out(), "Numpad_CapsLock");
            case terminal::Key::Numpad_Add: return fmt::format_to(ctx.out(), "Numpad_Add");
            case terminal::Key::Numpad_Decimal: return fmt::format_to(ctx.out(), "Numpad_Decimal");
            case terminal::Key::Numpad_Enter: return fmt::format_to(ctx.out(), "Numpad_Enter");
            case terminal::Key::Numpad_Equal: return fmt::format_to(ctx.out(), "Numpad_Equal");
            case terminal::Key::Numpad_0: return fmt::format_to(ctx.out(), "Numpad_0");
            case terminal::Key::Numpad_1: return fmt::format_to(ctx.out(), "Numpad_1");
            case terminal::Key::Numpad_2: return fmt::format_to(ctx.out(), "Numpad_2");
            case terminal::Key::Numpad_3: return fmt::format_to(ctx.out(), "Numpad_3");
            case terminal::Key::Numpad_4: return fmt::format_to(ctx.out(), "Numpad_4");
            case terminal::Key::Numpad_5: return fmt::format_to(ctx.out(), "Numpad_5");
            case terminal::Key::Numpad_6: return fmt::format_to(ctx.out(), "Numpad_6");
            case terminal::Key::Numpad_7: return fmt::format_to(ctx.out(), "Numpad_7");
            case terminal::Key::Numpad_8: return fmt::format_to(ctx.out(), "Numpad_8");
            case terminal::Key::Numpad_9: return fmt::format_to(ctx.out(), "Numpad_9");
        }

        return fmt::format_to(ctx.out(), "{}", (unsigned) value);
    }
};
} // namespace fmt
