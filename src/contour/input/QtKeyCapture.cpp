// SPDX-License-Identifier: Apache-2.0
#include <contour/input/KeyMapping.hpp>
#include <contour/input/KeyboardPlatform.hpp>
#include <contour/input/QtKeyCapture.hpp>

#ifdef _WIN32
    #include <windows.h>
#endif

namespace contour::input
{

uint32_t nativeModifiersWithLockState([[maybe_unused]] uint32_t qtNativeModifiers) noexcept
{
#ifdef _WIN32
    auto bits = uint32_t { 0 };
    if ((GetKeyState(VK_CAPITAL) & 0x0001) != 0)
        bits |= Win32CapsLockBit;
    if ((GetKeyState(VK_NUMLOCK) & 0x0001) != 0)
        bits |= Win32NumLockBit;
    return bits;
#else
    return qtNativeModifiers;
#endif
}

NativeKeyEvent captureKeyEvent(QKeyEvent const& event, vtbackend::KeyboardEventType type)
{
    return NativeKeyEvent {
        .qtKey = event.key(),
        .text = event.text().toStdU32String(),
        .qtModifiers = static_cast<uint32_t>(event.modifiers().toInt()),
        .nativeModifiers = nativeModifiersWithLockState(event.nativeModifiers()),
        .nativeVirtualKey = event.nativeVirtualKey(),
        .nativeScanCode = event.nativeScanCode(),
        .type = type,
        .origin = KeyEventOrigin::Keyboard,
    };
}

NativeKeyEvent captureInputMethodCommit(QString const& commit)
{
    // The lock state is sampled as for a key: on Windows the toggles come from the OS, and a commit carries
    // them as the text a key typed would.
    return {
        .text = commit.toStdU32String(),
        .nativeModifiers = nativeModifiersWithLockState(0),
        .origin = KeyEventOrigin::InputMethodCommit,
    };
}

QKeyEvent toQKeyEvent(NativeKeyEvent const& event)
{
    auto const qtType =
        event.type == vtbackend::KeyboardEventType::Release ? QEvent::KeyRelease : QEvent::KeyPress;
    return { qtType,
             event.qtKey,
             qtModifiersOf(event.qtModifiers),
             event.nativeScanCode,
             event.nativeVirtualKey,
             event.nativeModifiers,
             QString::fromStdU32String(event.text),
             event.type == vtbackend::KeyboardEventType::Repeat };
}

} // namespace contour::input
