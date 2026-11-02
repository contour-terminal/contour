// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/input/XkbKeymap.hpp>

#include <cstdint>
#include <memory>

namespace contour::input
{

/// What the windowing system calls a physical key: the two native identifiers a key event carries.
///
/// Which of them means anything is the platform's business: X11 and Wayland report a keysym in
/// `virtualKey`, the XKB keycode in `scanCode` and the active group in `group`; Win32 a virtual-key code and
/// a scan code; macOS a Carbon key code in `virtualKey`.
struct NativeKeyCode
{
    uint32_t virtualKey = 0; ///< QKeyEvent::nativeVirtualKey().
    uint32_t scanCode = 0;   ///< QKeyEvent::nativeScanCode().
    uint32_t group = 0;      ///< X11/Wayland: the XKB group (layout index) in force; the platform decodes it.
};

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

    /// @param key The native identifiers of the event in question. What they mean is the platform's
    ///            business and this method's to know.
    /// @return The codepoint the key produces unmodified, or 0 when it cannot be determined —
    ///         in which case the caller falls back to deriving it from the character.
    [[nodiscard]] virtual char32_t unshiftedKeyOf(NativeKeyCode key) const noexcept = 0;

    /// @param key The native identifiers of the event in question.
    /// @return The Win32 virtual-key code (`VK_*`) for the key, or 0 where there is none to give.
    ///         Only Windows has one; everywhere else this is 0, so that a native key identifier
    ///         cannot reach ConPTY's win32-input-mode disguised as a VK code.
    [[nodiscard]] virtual uint32_t win32VirtualKeyOf(NativeKeyCode key) const noexcept = 0;
};

/// @return The KeyboardLayout implementation for the platform this build targets.
[[nodiscard]] std::unique_ptr<KeyboardLayout> makePlatformKeyboardLayout();

/// @param keymap What the keymap in force says about a key; may be null.
/// @return A KeyboardLayout for X11 and Wayland, where a key event reports the keysym it produced -- the
///         *shifted* one when Shift was held -- and its XKB keycode.
///
/// The unshifted key is what @p keymap says the key's keycode produces at shift level 0, read through the
/// keysym table (keysymToCodepoint()). Without a keymap, or where it has nothing for the key, the event's
/// own keysym is read through the same table -- right for any key held without Shift, and for a shifted one
/// only where the shifted and unshifted characters coincide.
[[nodiscard]] std::unique_ptr<KeyboardLayout> makeXkbKeyboardLayout(std::unique_ptr<XkbKeymap> keymap);

/// @return A KeyboardLayout that reads native key identifiers as Windows virtual-key codes (`VK_*`) and
///         resolves them against a US keyboard, ignoring the user's actual layout; the VK code itself is
///         what win32VirtualKeyOf() reports.
///
/// This is the fallback the Windows layout composes when the live layout cannot answer, exposed on its own
/// so it can be constructed anywhere -- including on Linux CI, where it is what lets the Windows rules be
/// tested at all. Reports 0 for keys that carry no character.
[[nodiscard]] std::unique_ptr<KeyboardLayout> makeWin32UsKeyboardLayout();

/// @return A KeyboardLayout that reads native key identifiers as macOS Carbon key codes (`kVK_*`)
///         and resolves them against a US ANSI keyboard, ignoring the user's actual layout.
///
/// This is the fallback the macOS layout composes when the live input source cannot answer, exposed
/// on its own so it can be constructed anywhere — including on Linux CI, where it is what lets the
/// macOS regression be tested at all. Reports 0 for keys that carry no character.
[[nodiscard]] std::unique_ptr<KeyboardLayout> makeMacUsAnsiKeyboardLayout();

} // namespace contour::input
