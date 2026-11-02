// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/input/NativeKeyEvent.hpp>

#include <QtGui/QKeyEvent>

namespace contour::input
{

/// Captures everything a Qt key event reports as plain data.
///
/// This is the one place the ambient keyboard state is read: on Windows, where Qt's native
/// modifier mask omits the CapsLock/NumLock toggles, `nativeModifiers` is replaced with the lock
/// bits (see nativeModifiersWithLockState()); elsewhere it is Qt's own value.
/// @param event The Qt event.
/// @param type Whether it is a press, repeat or release.
/// @return The captured event.
[[nodiscard]] NativeKeyEvent captureKeyEvent(QKeyEvent const& event, vtbackend::KeyboardEventType type);

/// Captures text committed by an input method as an unmodified, text-only event. Its native modifiers are
/// the lock state alone, sampled as captureKeyEvent() samples it (only Windows has any to sample; elsewhere
/// they are zero).
/// @param commit The committed text.
/// @return The captured event, with origin KeyEventOrigin::InputMethodCommit.
[[nodiscard]] NativeKeyEvent captureInputMethodCommit(QString const& commit);

/// Replays @p event through today's Qt path.
/// @param event The captured event.
/// @return A Qt event carrying the same key, modifiers, text and native fields.
[[nodiscard]] QKeyEvent toQKeyEvent(NativeKeyEvent const& event);

} // namespace contour::input
