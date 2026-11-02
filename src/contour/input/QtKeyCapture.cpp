// SPDX-License-Identifier: Apache-2.0
#include <contour/input/KeyMapping.hpp>
#include <contour/input/QtKeyCapture.hpp>

namespace contour::input
{

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
             Qt::KeyboardModifiers::fromInt(static_cast<int>(event.qtModifiers)),
             event.nativeScanCode,
             event.nativeVirtualKey,
             event.nativeModifiers,
             QString::fromStdU32String(event.text),
             event.type == vtbackend::KeyboardEventType::Repeat };
}

} // namespace contour::input
