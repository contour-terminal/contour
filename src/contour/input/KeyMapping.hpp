// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/input/InputGenerator.hpp>

#include <QtCore/Qt>
#include <QtCore/QtTypes>

#include <cctype>

namespace contour::input
{

/// Whether @p key is a modifier key rather than a key that produces input on its own.
/// @param key The Qt key code.
/// @return True for Alt, Control, Shift and Meta.
[[nodiscard]] constexpr bool isModifier(Qt::Key key)
{
    switch (key)
    {
        case Qt::Key_Alt:
        case Qt::Key_Control:
        case Qt::Key_Shift:
        case Qt::Key_Meta: return true;
        default: return false;
    }
}

/// The character an A-Z key produces under @p mods, or 0 for any other key.
/// @param key The Qt key code.
/// @param mods The modifiers held; only Shift is consulted.
/// @return The upper- or lower-case letter, or 0 when @p key is not a letter.
[[nodiscard]] constexpr char32_t makeChar(Qt::Key key, Qt::KeyboardModifiers mods)
{
    auto const value = static_cast<int>(key);
    if (value >= 'A' && value <= 'Z')
    {
        if (mods & Qt::ShiftModifier)
            return static_cast<char32_t>(value);
        else
            return static_cast<char32_t>(std::tolower(value));
    }
    return 0;
}

/// Returns the native modifier mask to hand makeModifiers() for a key event, with the CapsLock/
/// NumLock toggle state folded in. This is the single OS-boundary seam for lock state: on Windows
/// it reads the live toggle state from the OS (GetKeyState) — which Qt does not surface in its
/// native modifier mask — and encodes it into the Win32 lock bits; on X11/Wayland/macOS the lock
/// state already rides in Qt's native mask, so it is returned unchanged. Isolating the ambient
/// query here keeps makeModifiers() a pure function of its arguments, and therefore unit-testable
/// without depending on the machine's live lock state.
/// @param qtNativeModifiers Qt's native modifier mask for the event (QKeyEvent::nativeModifiers()).
/// @return The native modifier mask to pass to makeModifiers().
[[nodiscard]] quint32 nativeModifiersWithLockState(quint32 qtNativeModifiers) noexcept;

/// Creates the VT keyboard modifier state from Qt modifiers and native platform modifiers.
/// The chord is taken from the Qt modifiers, the CapsLock and NumLock state from the
/// platform-specific native modifiers.
/// @param qtModifiers Standard Qt keyboard modifiers
/// @param nativeModifiers Platform-specific value from QKeyEvent::nativeModifiers()
/// @param stripAltGr When true (default), removes the Ctrl+Alt combination on Windows
///                   that represents AltGr. Set to false for Win32 Input Mode which
///                   needs the raw modifier state.
/// @return The chord being held, plus the latched lock keys.
[[nodiscard]] vtbackend::KeyboardModifiers makeModifiers(Qt::KeyboardModifiers qtModifiers,
                                                         quint32 nativeModifiers = 0,
                                                         bool stripAltGr = true);

/// Maps a US-ASCII "shifted" character back to the base character its physical key produces without
/// Shift (e.g. '<' -> ',', '?' -> '/', '_' -> '-', '@' -> '2'). Returns @p ch unchanged when it is not
/// a shifted symbol.
///
/// Keyboard shortcuts are written with the base key label (e.g. `Ctrl+Shift+,`) and stored as the base
/// character, but a Shift+punctuation chord is delivered by Qt as the *shifted* symbol (comma+Shift is
/// reported as '<' on a US layout). Matching the delivered shifted codepoint against the base char a
/// binding is stored under is what lets such shortcuts fire as the user intended.
/// @param ch The (possibly shifted) input codepoint.
/// @return The un-shifted base codepoint, or @p ch if it is not a recognized shifted symbol.
[[nodiscard]] char32_t unshiftedCodepoint(char32_t ch) noexcept;

} // namespace contour::input
