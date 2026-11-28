// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a connection holding a daemon's raise role (proto::RaiseRole::Raise) does with what the panes it
/// does not mirror raised: a bell, a desktop notification, a command that finished -- decided and raised as a
/// pane out of sight would be. Shared by a window's own link (DaemonLink) and the observers of daemons no
/// window of the process is on (DaemonPresence).

#include <contour/config/BellConfig.hpp>
#include <contour/remote/DaemonLinkHost.hpp>
#include <contour/window/DaemonSection.hpp>
#include <contour/window/WorkspaceDirectory.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <vthost/proto/Pdu.hpp>

namespace contour::window
{
class WindowController;
}

namespace contour::remote
{

/// Whether a window rings for what was raised.
enum class RingBell : uint8_t
{
    No = 0, ///< Nothing asks for the bell.
    Yes,    ///< A pane rang, or a finish asks for the bell.
};

/// The daemon whose panes raised something.
struct RaisingDaemon
{
    window::DaemonRef daemon;        ///< The daemon: a finish notice is raised under an identifier naming it.
    window::DaemonInstance instance; ///< Its run, which that identifier names too.
    /// Empty for the daemon of the window that raises, whose notices read as they always did; else the
    /// daemon's name, which every notice's title then names.
    std::string name;
};

/// Notifies what the panes of @p directory that no window shows raised, as a hidden pane's would be decided.
/// @param raised What was raised, as NativeController::takeRaised() hands it out.
/// @param directory What the connection lists: a finish notice names the pane's tab from it.
/// @param notices What each is decided and raised with.
/// @param daemon The daemon that raised them. A pane's later finish replaces its earlier notice, and only
///        that pane's: panes of two daemons, or of two runs of one, have their own.
/// @return Whether the window that raises is to ring, once however many rang.
[[nodiscard]] RingBell raiseElsewhere(std::span<vthost::proto::PaneSummary const> raised,
                                      window::WorkspaceDirectory const& directory,
                                      ElsewhereNotices const& notices,
                                      RaisingDaemon const& daemon);

/// Rings the bell, and alerts, on @p window, as @p bell says: as the window does for a finish in one of its
/// own background tabs, there being no pane chrome to ring it.
/// @param window The window that rings.
/// @param bell How it rings.
void ringWindowBell(window::WindowController& window, config::Bell const& bell);

} // namespace contour::remote
