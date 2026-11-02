// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/OptionAsAlt.hpp>

#include <vtbackend/input/InputGenerator.hpp>

#include <QtCore/QString>
#include <QtCore/Qt>
#include <QtCore/QtTypes>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <optional>

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

/// A Qt key that names a terminal key of its own, as opposed to one that produces a character.
struct NamedKeyRow
{
    Qt::Key qt;         ///< The Qt key code.
    vtbackend::Key key; ///< The terminal key it is.
};

/// The Qt keys that are terminal keys by name: editing and navigation keys, function keys, media keys, the
/// modifiers and the locks. Qt cannot tell a left modifier from a right one, so it maps to the left.
inline constexpr auto NamedKeyRows = std::array {
    NamedKeyRow { .qt = Qt::Key_Escape, .key = vtbackend::Key::Escape },
    NamedKeyRow { .qt = Qt::Key_Enter, .key = vtbackend::Key::Enter },
    NamedKeyRow { .qt = Qt::Key_Return, .key = vtbackend::Key::Enter },
    NamedKeyRow { .qt = Qt::Key_Tab, .key = vtbackend::Key::Tab },
    NamedKeyRow { .qt = Qt::Key_Backspace, .key = vtbackend::Key::Backspace },
    NamedKeyRow { .qt = Qt::Key_Insert, .key = vtbackend::Key::Insert },
    NamedKeyRow { .qt = Qt::Key_Delete, .key = vtbackend::Key::Delete },
    NamedKeyRow { .qt = Qt::Key_Right, .key = vtbackend::Key::RightArrow },
    NamedKeyRow { .qt = Qt::Key_Left, .key = vtbackend::Key::LeftArrow },
    NamedKeyRow { .qt = Qt::Key_Down, .key = vtbackend::Key::DownArrow },
    NamedKeyRow { .qt = Qt::Key_Up, .key = vtbackend::Key::UpArrow },
    NamedKeyRow { .qt = Qt::Key_PageDown, .key = vtbackend::Key::PageDown },
    NamedKeyRow { .qt = Qt::Key_PageUp, .key = vtbackend::Key::PageUp },
    NamedKeyRow { .qt = Qt::Key_Home, .key = vtbackend::Key::Home },
    NamedKeyRow { .qt = Qt::Key_End, .key = vtbackend::Key::End },
    NamedKeyRow { .qt = Qt::Key_F1, .key = vtbackend::Key::F1 },
    NamedKeyRow { .qt = Qt::Key_F2, .key = vtbackend::Key::F2 },
    NamedKeyRow { .qt = Qt::Key_F3, .key = vtbackend::Key::F3 },
    NamedKeyRow { .qt = Qt::Key_F4, .key = vtbackend::Key::F4 },
    NamedKeyRow { .qt = Qt::Key_F5, .key = vtbackend::Key::F5 },
    NamedKeyRow { .qt = Qt::Key_F6, .key = vtbackend::Key::F6 },
    NamedKeyRow { .qt = Qt::Key_F7, .key = vtbackend::Key::F7 },
    NamedKeyRow { .qt = Qt::Key_F8, .key = vtbackend::Key::F8 },
    NamedKeyRow { .qt = Qt::Key_F9, .key = vtbackend::Key::F9 },
    NamedKeyRow { .qt = Qt::Key_F10, .key = vtbackend::Key::F10 },
    NamedKeyRow { .qt = Qt::Key_F11, .key = vtbackend::Key::F11 },
    NamedKeyRow { .qt = Qt::Key_F12, .key = vtbackend::Key::F12 },
    NamedKeyRow { .qt = Qt::Key_F13, .key = vtbackend::Key::F13 },
    NamedKeyRow { .qt = Qt::Key_F14, .key = vtbackend::Key::F14 },
    NamedKeyRow { .qt = Qt::Key_F15, .key = vtbackend::Key::F15 },
    NamedKeyRow { .qt = Qt::Key_F16, .key = vtbackend::Key::F16 },
    NamedKeyRow { .qt = Qt::Key_F17, .key = vtbackend::Key::F17 },
    NamedKeyRow { .qt = Qt::Key_F18, .key = vtbackend::Key::F18 },
    NamedKeyRow { .qt = Qt::Key_F19, .key = vtbackend::Key::F19 },
    NamedKeyRow { .qt = Qt::Key_F20, .key = vtbackend::Key::F20 },
    NamedKeyRow { .qt = Qt::Key_F21, .key = vtbackend::Key::F21 },
    NamedKeyRow { .qt = Qt::Key_F22, .key = vtbackend::Key::F22 },
    NamedKeyRow { .qt = Qt::Key_F23, .key = vtbackend::Key::F23 },
    NamedKeyRow { .qt = Qt::Key_F24, .key = vtbackend::Key::F24 },
    NamedKeyRow { .qt = Qt::Key_F25, .key = vtbackend::Key::F25 },
    NamedKeyRow { .qt = Qt::Key_F26, .key = vtbackend::Key::F26 },
    NamedKeyRow { .qt = Qt::Key_F27, .key = vtbackend::Key::F27 },
    NamedKeyRow { .qt = Qt::Key_F28, .key = vtbackend::Key::F28 },
    NamedKeyRow { .qt = Qt::Key_F29, .key = vtbackend::Key::F29 },
    NamedKeyRow { .qt = Qt::Key_F30, .key = vtbackend::Key::F30 },
    NamedKeyRow { .qt = Qt::Key_F31, .key = vtbackend::Key::F31 },
    NamedKeyRow { .qt = Qt::Key_F32, .key = vtbackend::Key::F32 },
    NamedKeyRow { .qt = Qt::Key_F33, .key = vtbackend::Key::F33 },
    NamedKeyRow { .qt = Qt::Key_F34, .key = vtbackend::Key::F34 },
    NamedKeyRow { .qt = Qt::Key_F35, .key = vtbackend::Key::F35 },
    NamedKeyRow { .qt = Qt::Key_MediaPlay, .key = vtbackend::Key::MediaPlay },
    NamedKeyRow { .qt = Qt::Key_MediaStop, .key = vtbackend::Key::MediaStop },
    NamedKeyRow { .qt = Qt::Key_MediaPrevious, .key = vtbackend::Key::MediaPrevious },
    NamedKeyRow { .qt = Qt::Key_MediaNext, .key = vtbackend::Key::MediaNext },
    NamedKeyRow { .qt = Qt::Key_MediaPause, .key = vtbackend::Key::MediaPause },
    NamedKeyRow { .qt = Qt::Key_MediaTogglePlayPause, .key = vtbackend::Key::MediaTogglePlayPause },
    NamedKeyRow { .qt = Qt::Key_VolumeUp, .key = vtbackend::Key::VolumeUp },
    NamedKeyRow { .qt = Qt::Key_VolumeDown, .key = vtbackend::Key::VolumeDown },
    NamedKeyRow { .qt = Qt::Key_VolumeMute, .key = vtbackend::Key::VolumeMute },
    NamedKeyRow { .qt = Qt::Key_Shift,
                  .key = vtbackend::Key::LeftShift }, // NB: Qt cannot distinguish between left and right
    NamedKeyRow { .qt = Qt::Key_Control,
                  .key = vtbackend::Key::LeftControl }, // NB: Qt cannot distinguish between left and right
    NamedKeyRow { .qt = Qt::Key_Alt,
                  .key = vtbackend::Key::LeftAlt }, // NB: Qt cannot distinguish between left and right
    NamedKeyRow { .qt = Qt::Key_Meta,
                  .key = vtbackend::Key::LeftMeta }, // NB: Qt cannot distinguish between left and right
    NamedKeyRow { .qt = Qt::Key_Super_L, .key = vtbackend::Key::LeftSuper },
    NamedKeyRow { .qt = Qt::Key_Super_R, .key = vtbackend::Key::RightSuper },
    NamedKeyRow { .qt = Qt::Key_Hyper_L, .key = vtbackend::Key::LeftHyper },
    NamedKeyRow { .qt = Qt::Key_Hyper_R, .key = vtbackend::Key::RightHyper },
    NamedKeyRow { .qt = Qt::Key_CapsLock, .key = vtbackend::Key::CapsLock },
    NamedKeyRow { .qt = Qt::Key_ScrollLock, .key = vtbackend::Key::ScrollLock },
    NamedKeyRow { .qt = Qt::Key_NumLock, .key = vtbackend::Key::NumLock },
    NamedKeyRow { .qt = Qt::Key_Print, .key = vtbackend::Key::PrintScreen },
    NamedKeyRow { .qt = Qt::Key_Pause, .key = vtbackend::Key::Pause },
    NamedKeyRow { .qt = Qt::Key_Menu, .key = vtbackend::Key::Menu },
};

/// The terminal key a Qt key is, when it is one by name.
/// @param qtKey The Qt key code.
/// @return The key; nullopt for a key that produces a character, which the char mappings match instead.
[[nodiscard]] constexpr std::optional<vtbackend::Key> namedKeyOf(int qtKey) noexcept
{
    auto const row =
        std::ranges::find(NamedKeyRows, qtKey, [](auto const& r) { return static_cast<int>(r.qt); });
    return row != NamedKeyRows.end() ? std::optional { row->key } : std::nullopt;
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

/// What a Ctrl+Alt combination means for one event. Windows reports the AltGr key to Qt as
/// Ctrl+Alt, so the same two modifiers are either a chord the user pressed or a keyboard-layout
/// level that selected the character. AltGr is zero because it is makeModifiers' default.
enum class CtrlAltRole : uint8_t
{
    AltGr, ///< Ctrl and Alt selected the AltGr level; they are not modifiers of the result.
    Chord, ///< Ctrl and Alt are modifiers, reported as such.
};

/// Classifies a key event's Ctrl+Alt the way Windows itself does: the AltGr level yields
/// graphical characters, while a genuine Ctrl+Alt chord yields none (or a control character).
/// This is Windows Terminal's heuristic for the same ambiguity.
/// @param qtModifiers The event's Qt modifiers.
/// @param text The text the platform produced for the event (QKeyEvent::text()).
/// @return AltGr when Ctrl and Alt are both held and @p text is non-empty and wholly graphical,
///         else Chord.
[[nodiscard]] CtrlAltRole ctrlAltRoleOf(Qt::KeyboardModifiers qtModifiers, QString const& text) noexcept;

/// The chord a key or mouse event holds, as Qt reports it: Alt, Shift, Control, and Meta as Super.
/// Lock keys and the keypad flag are not part of a chord.
/// @param qtModifiers The event's Qt modifiers.
/// @return The chord.
[[nodiscard]] vtbackend::Modifiers chordOf(Qt::KeyboardModifiers qtModifiers) noexcept;

/// The bit of X11's core modifier mask (`XCB_MOD_MASK_LOCK`) set while CapsLock is latched. Fixed by the X11
/// protocol; Wayland's Qt plugin reports the same mask.
inline constexpr quint32 XcbCapsLockBit = 0x02;

/// The bit of X11's core modifier mask (`XCB_MOD_MASK_2`) set while NumLock is latched -- the conventional
/// mapping, which every desktop keymap follows.
inline constexpr quint32 XcbNumLockBit = 0x10;

/// The lock keys an X11 or Wayland key event says are latched.
/// @param nativeModifiers The value of QKeyEvent::nativeModifiers().
/// @return CapsLock and NumLock, as their XCB mask bits say.
[[nodiscard]] constexpr vtbackend::LockKeys xcbLockKeysOf(quint32 nativeModifiers) noexcept
{
    auto locks = vtbackend::LockKeys {};
    if ((nativeModifiers & XcbCapsLockBit) != 0)
        locks |= vtbackend::LockKey::CapsLock;
    if ((nativeModifiers & XcbNumLockBit) != 0)
        locks |= vtbackend::LockKey::NumLock;
    return locks;
}

/// The bit of macOS's modifier flags (`NSEventModifierFlagCapsLock`) set while CapsLock is latched. A Mac
/// keyboard has no NumLock.
inline constexpr quint32 MacCapsLockBit = 0x00010000;

/// The lock keys a macOS key event says are latched.
/// @param nativeModifiers The value of QKeyEvent::nativeModifiers().
/// @return CapsLock when its flag is set.
[[nodiscard]] constexpr vtbackend::LockKeys macLockKeysOf(quint32 nativeModifiers) noexcept
{
    auto locks = vtbackend::LockKeys {};
    if ((nativeModifiers & MacCapsLockBit) != 0)
        locks |= vtbackend::LockKey::CapsLock;
    return locks;
}

/// Creates the VT keyboard modifier state from Qt modifiers and native platform modifiers.
/// The chord is taken from the Qt modifiers, the CapsLock and NumLock state from the
/// platform-specific native modifiers.
/// @param qtModifiers Standard Qt keyboard modifiers
/// @param nativeModifiers Platform-specific value from QKeyEvent::nativeModifiers()
/// @param ctrlAltRole AltGr (the default) strips a held Ctrl+Alt from the chord on Windows; Chord
///                    keeps it. Ignored elsewhere, where AltGr is a key of its own. Key events pass
///                    ctrlAltRoleOf(); only mouse events still take the default.
/// @return The chord being held, plus the latched lock keys.
[[nodiscard]] vtbackend::KeyboardModifiers makeModifiers(Qt::KeyboardModifiers qtModifiers,
                                                         quint32 nativeModifiers = 0,
                                                         CtrlAltRole ctrlAltRole = CtrlAltRole::AltGr);

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
