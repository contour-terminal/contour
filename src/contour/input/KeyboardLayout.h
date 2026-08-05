// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>

namespace contour::input
{

/// Translates a windowing system's native key identifier into the vocabularies the keyboard
/// protocols transmit — neither of which it is.
///
/// What `QKeyEvent::nativeVirtualKey()` holds differs per platform, and only one of them is
/// codepoint-shaped: X11 and Wayland report a keysym, Win32 a virtual-key code, and macOS a
/// *positional* Carbon key code in which `kVK_ANSI_U` is 0x20 — the value of a space. Reading such
/// a code as a codepoint is how Ctrl+U once reached the application as Ctrl+Space.
///
/// This is an interface rather than a set of free functions because the answer comes from an ambient
/// operating-system resource: the keyboard layout the user has selected, which can change while
/// Contour is running. Tests substitute a fixed layout instead of inheriting the developer's.
class KeyboardLayout
{
  public:
    virtual ~KeyboardLayout() = default;

    /// @param nativeVirtualKey QKeyEvent::nativeVirtualKey() for the event in question. What that
    ///                         value means is the platform's business and this method's to know.
    /// @return The codepoint the key produces unmodified, or 0 when it cannot be determined —
    ///         in which case the caller falls back to deriving it from the character.
    [[nodiscard]] virtual char32_t unshiftedKeyOf(uint32_t nativeVirtualKey) const noexcept = 0;

    /// @param nativeVirtualKey QKeyEvent::nativeVirtualKey() for the event in question.
    /// @return The Win32 virtual-key code (`VK_*`) for the key, or 0 where there is none to give.
    ///         Only Windows has one; everywhere else this is 0, so that a native key identifier
    ///         cannot reach ConPTY's win32-input-mode disguised as a VK code.
    [[nodiscard]] virtual uint32_t win32VirtualKeyOf(uint32_t nativeVirtualKey) const noexcept = 0;
};

/// @return The KeyboardLayout implementation for the platform this build targets.
[[nodiscard]] std::unique_ptr<KeyboardLayout> makePlatformKeyboardLayout();

/// @return A KeyboardLayout that takes the native key identifier to already be codepoint-valued,
///         decoding the X11 `0x01000000 | codepoint` keysym form on the way. This is what every
///         platform except macOS currently gets from makePlatformKeyboardLayout().
///
/// It is right for the keys this path sees on a US layout and wrong beyond that, which is a known
/// gap rather than a claim: X11's legacy charset keysyms (`XK_Cyrillic_a` is 0x06C1) and Windows'
/// OEM punctuation codes (`VK_OEM_1` is 0xBA) are as positional as the macOS codes are, and X11
/// reports the *shifted* keysym, so Shift+3 reports '#' where the spec asks for '3'. Closing those
/// needs a keysym→UCS table and a VK table respectively — the same shape as the macOS one below.
[[nodiscard]] std::unique_ptr<KeyboardLayout> makePassthroughKeyboardLayout();

/// @return A KeyboardLayout that reads native key identifiers as macOS Carbon key codes (`kVK_*`)
///         and resolves them against a US ANSI keyboard, ignoring the user's actual layout.
///
/// This is the fallback the macOS layout composes when the live input source cannot answer, exposed
/// on its own so it can be constructed anywhere — including on Linux CI, where it is what lets the
/// macOS regression be tested at all. Reports 0 for keys that carry no character.
[[nodiscard]] std::unique_ptr<KeyboardLayout> makeMacUsAnsiKeyboardLayout();

} // namespace contour::input
