// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtGui/QWindow>

namespace contour::window
{

/// Brings a shown window back in front: out of a minimised state, raised, and given the focus.
///
/// The minimised flag is cleared from the state set rather than through showNormal(), which would also
/// clear a maximised (or full-screen) state beneath it: a window minimised from maximised comes back
/// maximised.
/// @param window The window; one that is hidden stays hidden.
/// @pre GUI thread.
inline void raiseAndActivate(QWindow& window)
{
    auto states = window.windowStates();
    states.setFlag(Qt::WindowMinimized, false);
    window.setWindowStates(states);
    window.raise();
    window.requestActivate();
}

} // namespace contour::window
