// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Which window an app-wide popup (the switcher, the connect picker's sheet) opens over.

#include <QtGui/QWindow>

namespace contour::window
{

/// The window a popup opens over, as its transient parent.
///
/// The focused window, unless that is the popup itself: reopened while it has the focus (the workspace chip
/// clicked again, the shortcut pressed twice), it stays over the window it is over already, rather than be
/// made its own parent -- which Qt refuses with a warning.
/// @param focused The window with the focus (QGuiApplication::focusWindow()), or null.
/// @param popup   The popup being opened.
/// @return The window to open @p popup over, or null for none.
[[nodiscard]] inline QWindow* popupParentFor(QWindow* focused, QWindow const& popup) noexcept
{
    return focused == &popup ? popup.transientParent() : focused;
}

} // namespace contour::window
