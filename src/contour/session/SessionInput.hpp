// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/input/KeyboardLayout.hpp>

#include <vtbackend/InputGenerator.hpp>

#include <QtGui/QKeyEvent>

namespace contour::session
{

class TerminalSession;

/// Translates a Qt key event into terminal input and hands it to @p session.
///
/// The translation itself lives a layer down in contour::input, which knows nothing about sessions.
/// What this layer adds is the delivery: which session receives the input, and the session-state
/// questions (Win32 input mode, the active keyboard protocol) that decide how it is encoded.
///
/// @param keyEvent The Qt event.
/// @param eventType Whether this is a press, a repeat or a release.
/// @param session The session to send the resulting input to.
/// @param keyboardLayout Resolves the event's native key identifier to the codepoint the key
///                       carries unmodified. Passed in rather than queried, because it reads the
///                       user's active input source -- an ambient resource tests must be able to
///                       substitute.
/// @return Whether the event was translated into input; false leaves it for Qt to route on.
bool sendKeyEvent(QKeyEvent* keyEvent,
                  vtbackend::KeyboardEventType eventType,
                  TerminalSession& session,
                  input::KeyboardLayout const& keyboardLayout);
void sendWheelEvent(QWheelEvent* event, TerminalSession& session);
void sendMousePressEvent(QMouseEvent* event, TerminalSession& session);
void sendMouseMoveEvent(QMouseEvent* event, TerminalSession& session);
void sendMouseMoveEvent(QHoverEvent* event, TerminalSession& session);
void sendMouseReleaseEvent(QMouseEvent* event, TerminalSession& session);

/// Result of computing auto-scroll parameters from the mouse position.
struct AutoScrollInfo
{
    int direction = 0;    ///< -1 = up (into history), 0 = inactive, +1 = down
    int linesPerTick = 0; ///< Number of lines to scroll per timer tick.
};

/// Computes auto-scroll direction and speed based on mouse pixel position vs content area bounds.
///
/// @return AutoScrollInfo with direction and speed; direction == 0 means mouse is inside content area.
AutoScrollInfo computeAutoScrollInfo(QMouseEvent const* event, TerminalSession const& session) noexcept;

} // namespace contour::session
