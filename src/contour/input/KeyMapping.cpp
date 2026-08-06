// SPDX-License-Identifier: Apache-2.0

#ifdef _WIN32
    #include <windows.h>
#endif

#include <contour/input/KeyMapping.hpp>

namespace contour::input
{

namespace
{
#ifdef _WIN32
    // Bit positions Contour uses to carry the CapsLock/NumLock toggle state into makeModifiers()
    // through its `nativeModifiers` argument on Windows, where Qt's native modifier mask omits lock
    // toggles. (X11 encodes these as 0x02/0x10 and macOS as 0x00010000, read in their own branches.)
    constexpr quint32 Win32CapsLockBit = 0x0001;
    constexpr quint32 Win32NumLockBit = 0x0002;
#endif
} // namespace

quint32 nativeModifiersWithLockState([[maybe_unused]] quint32 qtNativeModifiers) noexcept
{
#ifdef _WIN32
    quint32 bits = 0;
    if ((GetKeyState(VK_CAPITAL) & 0x0001) != 0)
        bits |= Win32CapsLockBit;
    if ((GetKeyState(VK_NUMLOCK) & 0x0001) != 0)
        bits |= Win32NumLockBit;
    return bits;
#else
    return qtNativeModifiers;
#endif
}

vtbackend::KeyboardModifiers makeModifiers(Qt::KeyboardModifiers qtModifiers,
                                           quint32 nativeModifiers,
                                           [[maybe_unused]] bool stripAltGr)
{
    using vtbackend::LockKey;
    using vtbackend::Modifier;
    using vtbackend::Modifiers;

    Modifiers chord {};

    // Standard modifiers from Qt
    if (qtModifiers & Qt::AltModifier)
        chord |= Modifier::Alt;
    if (qtModifiers & Qt::ShiftModifier)
        chord |= Modifier::Shift;
    if (qtModifiers & Qt::ControlModifier)
        chord |= Modifier::Control;
    if (qtModifiers & Qt::MetaModifier)
        chord |= Modifier::Super;

    vtbackend::LockKeys locks {};

#ifdef _WIN32
    // Windows: Handle AltGr (Ctrl+Alt combination).
    // In Win32 Input Mode, we keep the raw modifier state so ConPTY receives
    // the correct dwControlKeyState flags.
    if (stripAltGr)
    {
        auto constexpr AltGrEquivalent = Modifiers { Modifier::Alt, Modifier::Control };
        if (chord.contains(AltGrEquivalent))
            chord = chord.without(AltGrEquivalent);
    }

    // Windows: CapsLock/NumLock are not part of Qt's native modifier mask, so the Qt event boundary
    // (nativeModifiersWithLockState) folds the live toggle state into these bits before calling us.
    // Reading it from the argument here — rather than querying GetKeyState directly — keeps this
    // mapping pure and testable, matching the X11/macOS branches which also read lock state from
    // nativeModifiers.
    if ((nativeModifiers & Win32CapsLockBit) != 0)
        locks |= LockKey::CapsLock;
    if ((nativeModifiers & Win32NumLockBit) != 0)
        locks |= LockKey::NumLock;

#elifdef __APPLE__
    // macOS: NSEventModifierFlagCapsLock = 0x00010000
    constexpr quint32 MacCapsLock = 0x00010000;
    if (nativeModifiers & MacCapsLock)
        locks |= LockKey::CapsLock;
    // NumLock doesn't exist on standard macOS keyboards

#else
    // Linux (X11/Wayland): XCB/XKB modifier masks
    // CapsLock = XCB_MOD_MASK_LOCK (bit 1, value 0x02) - fixed by X11 protocol
    // NumLock = XCB_MOD_MASK_2 (bit 4, value 0x10) - conventional mapping
    constexpr quint32 XcbCapsLock = 0x02;
    constexpr quint32 XcbNumLock = 0x10;

    if (nativeModifiers & XcbCapsLock)
        locks |= LockKey::CapsLock;
    if (nativeModifiers & XcbNumLock)
        locks |= LockKey::NumLock;
#endif

    return { chord, locks };
}

char32_t unshiftedCodepoint(char32_t ch) noexcept
{
    // The US-ASCII shifted -> base inverse of the shift level, kept consistent with the CharMappings
    // table in SessionInput.cpp (which reports the *shifted* symbol for a Shift+key chord). A row per
    // shifted symbol, so extending it is data, not logic.
    switch (ch)
    {
        case U')': return U'0';
        case U'!': return U'1';
        case U'@': return U'2';
        case U'#': return U'3';
        case U'$': return U'4';
        case U'%': return U'5';
        case U'^': return U'6';
        case U'&': return U'7';
        case U'*': return U'8';
        case U'(': return U'9';
        case U'<': return U',';
        case U'>': return U'.';
        case U'?': return U'/';
        case U':': return U';';
        case U'"': return U'\'';
        case U'{': return U'[';
        case U'}': return U']';
        case U'|': return U'\\';
        case U'~': return U'`';
        case U'_': return U'-';
        case U'+': return U'=';
        default: return ch;
    }
}

} // namespace contour::input
