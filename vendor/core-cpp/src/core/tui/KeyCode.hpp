// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace core::tui
{

/// @brief Key codes for keyboard events.
///
/// Printable characters use their Unicode codepoint directly (cast to KeyCode).
/// Non-printable keys use values in the 0x10000+ range to avoid collision.
/// Kitty-specific keys use values in the 0x20000+ range.
enum class KeyCode : std::uint32_t
{
    /// Sentinel for "no key" (also the null Unicode codepoint). Used when a
    /// KeyEvent/KeyChord carries its value in the codepoint field instead.
    None = 0,

    // Non-printable keys (above Unicode BMP to avoid collision with codepoints)
    Enter = 0x10000,
    Tab,
    Backspace,
    Delete,
    Escape,
    Up,
    Down,
    Left,
    Right,
    Home,
    End,
    PageUp,
    PageDown,
    Insert,
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

    // ========================================================================
    // Kitty keyboard protocol special keys (0x20000+ range)
    // These correspond to Kitty's Private Use Area keycodes (57344-63743)
    // ========================================================================

    // Lock/System keys
    CapsLock = 0x20000,
    ScrollLock,
    NumLock,
    PrintScreen,
    Pause,
    Menu,

    // Extended function keys (F13-F35)
    F13,
    F14,
    F15,
    F16,
    F17,
    F18,
    F19,
    F20,
    F21,
    F22,
    F23,
    F24,
    F25,
    F26,
    F27,
    F28,
    F29,
    F30,
    F31,
    F32,
    F33,
    F34,
    F35,

    // Keypad keys
    Kp0,
    Kp1,
    Kp2,
    Kp3,
    Kp4,
    Kp5,
    Kp6,
    Kp7,
    Kp8,
    Kp9,
    KpDecimal,
    KpDivide,
    KpMultiply,
    KpSubtract,
    KpAdd,
    KpEnter,
    KpEqual,
    KpSeparator,
    KpLeft,
    KpRight,
    KpUp,
    KpDown,
    KpPageUp,
    KpPageDown,
    KpHome,
    KpEnd,
    KpInsert,
    KpDelete,
    KpBegin,

    // Media keys
    MediaPlay,
    MediaPause,
    MediaPlayPause,
    MediaReverse,
    MediaStop,
    MediaFastForward,
    MediaRewind,
    MediaTrackNext,
    MediaTrackPrevious,
    MediaRecord,
    LowerVolume,
    RaiseVolume,
    MuteVolume,

    // Modifier keys (when reported as key events, not as modifiers)
    LeftShift,
    LeftControl,
    LeftAlt,
    LeftSuper,
    LeftHyper,
    LeftMeta,
    RightShift,
    RightControl,
    RightAlt,
    RightSuper,
    RightHyper,
    RightMeta,
    IsoLevel3Shift,
    IsoLevel5Shift,
};

/// @brief Checks whether a key code represents a modifier-only or lock key.
///
/// These keys (Ctrl, Alt, Shift, Meta, CapsLock, etc.) produce no text and
/// have no editing action when pressed alone. Reported by the Kitty keyboard
/// protocol (flag 8) as discrete key events.
[[nodiscard]] constexpr auto isModifierOnlyKey(KeyCode key) noexcept -> bool
{
    return (key >= KeyCode::CapsLock && key <= KeyCode::Menu)                // Lock/system keys
           || (key >= KeyCode::LeftShift && key <= KeyCode::IsoLevel5Shift); // Modifier keys
}

/// @brief Checks whether a key code represents a printable Unicode character.
/// @param key The key code to test.
/// @return True if the key code is a printable codepoint.
///
/// Excludes Kitty's Private Use Area (57344-63743) which is used for special keys
/// like CapsLock, NumLock, modifier keys, keypad keys, and media keys.
[[nodiscard]] constexpr auto isPrintable(KeyCode key) noexcept -> bool
{
    auto const value = static_cast<std::uint32_t>(key);
    // Printable if: >= 32 (space) AND below special key range (0x10000)
    // AND not in Kitty PUA range (57344-63743)
    return value >= 32 && value < 0x10000 && (value < 57344 || value > 63743);
}

/// @brief Converts a Unicode codepoint to a KeyCode.
/// @param codepoint The Unicode codepoint.
/// @return The corresponding KeyCode.
[[nodiscard]] constexpr auto keyCodeFromCodepoint(char32_t codepoint) noexcept -> KeyCode
{
    return static_cast<KeyCode>(codepoint);
}

/// @brief Extracts the Unicode codepoint from a printable KeyCode.
/// @param key The key code.
/// @return The Unicode codepoint, or 0 if not printable.
[[nodiscard]] constexpr auto codepointFromKeyCode(KeyCode key) noexcept -> char32_t
{
    return isPrintable(key) ? static_cast<char32_t>(key) : 0;
}

/// @brief Returns the text character a numpad key produces, or 0 for non-character numpad keys.
[[nodiscard]] constexpr auto numpadCodepoint(KeyCode key) noexcept -> char32_t
{
    switch (key)
    {
        case KeyCode::Kp0: return U'0';
        case KeyCode::Kp1: return U'1';
        case KeyCode::Kp2: return U'2';
        case KeyCode::Kp3: return U'3';
        case KeyCode::Kp4: return U'4';
        case KeyCode::Kp5: return U'5';
        case KeyCode::Kp6: return U'6';
        case KeyCode::Kp7: return U'7';
        case KeyCode::Kp8: return U'8';
        case KeyCode::Kp9: return U'9';
        case KeyCode::KpDecimal: return U'.';
        case KeyCode::KpDivide: return U'/';
        case KeyCode::KpMultiply: return U'*';
        case KeyCode::KpSubtract: return U'-';
        case KeyCode::KpAdd: return U'+';
        case KeyCode::KpEqual: return U'=';
        case KeyCode::KpSeparator: return U',';
        default: return 0;
    }
}

/// @brief Checks whether a numpad key is an operator that always produces text regardless of NumLock.
[[nodiscard]] constexpr auto isNumpadOperator(KeyCode key) noexcept -> bool
{
    switch (key)
    {
        case KeyCode::KpDivide:
        case KeyCode::KpMultiply:
        case KeyCode::KpSubtract:
        case KeyCode::KpAdd:
        case KeyCode::KpEqual:
        case KeyCode::KpSeparator: return true;
        default: return false;
    }
}

/// @brief Checks whether a key code can produce text input (Unicode printable or numpad character key).
[[nodiscard]] constexpr auto isTextProducingKey(KeyCode key) noexcept -> bool
{
    return isPrintable(key) || numpadCodepoint(key) != 0;
}

} // namespace core::tui
