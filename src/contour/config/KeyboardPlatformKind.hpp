// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

namespace contour::config
{

/// Which windowing system's key-event vocabulary a key event speaks.
///
/// The three differ in what `QKeyEvent::nativeVirtualKey()` and `nativeModifiers()` hold, so the
/// input pipeline selects its per-platform behaviour by this kind rather than by `#ifdef`.
/// Zero is `Xkb`, the default of every build that is neither macOS nor Windows.
enum class KeyboardPlatformKind : uint8_t
{
    Xkb = 0, //!< X11 and Wayland: keysyms and XCB modifier masks.
    Win32,   //!< Windows: virtual-key codes, lock state sampled from the OS.
    Mac,     //!< macOS: positional Carbon key codes and NSEvent modifier flags.
};

/// The number of KeyboardPlatformKind enumerators.
inline constexpr std::size_t KeyboardPlatformKindCount = 3;

/// The kind this build targets.
inline constexpr KeyboardPlatformKind BuildKeyboardPlatformKind =
#if defined(__APPLE__)
    KeyboardPlatformKind::Mac;
#elif defined(_WIN32)
    KeyboardPlatformKind::Win32;
#else
    KeyboardPlatformKind::Xkb;
#endif

} // namespace contour::config
