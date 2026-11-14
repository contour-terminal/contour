// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What serves a window's sessions: this process, a daemon, or a host that is no daemon. The one answer the
/// location chip and the switcher both ask, so neither derives it from a name or a binding of its own.

#include <cstdint>

namespace contour::window
{

/// What serves a window's sessions, which decides what its chrome names and what its switcher can list.
enum class DaemonHosting : uint8_t
{
    InProcess = 0, ///< No binding: the window hosts its own sessions and shows the workspaces it owns.
    Daemon,        ///< The sessions live in a daemon: its chip names it, and its switcher lists every daemon.
    OtherHost,     ///< A host that is no daemon (a tmux attach): bound, but with no daemon to name or list.
};

} // namespace contour::window
