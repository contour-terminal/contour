// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a remote::DaemonLink needs from the application; light, so that the composition root can implement
/// it without including the link.

#include <contour/window/NoticeQueue.hpp>

#include <cstdint>

#include <boxed-cpp/boxed.hpp>
#include <vtworkspace/Primitives.hpp>

class QScreen;

namespace contour::remote
{

class DaemonLink;

namespace detail::tags
{
    struct DaemonLinkId
    {
    };
} // namespace detail::tags

/// Names a link among the application's.
using DaemonLinkId = boxed::boxed<uint32_t, detail::tags::DaemonLinkId>;

/// What a link needs from the application: OS windows opened and closed, notices posted, and the local
/// daemon restarted or upgraded by the process resources the application owns.
class IDaemonLinkHost
{
  public:
    virtual ~IDaemonLinkHost() = default;

    IDaemonLinkHost() = default;
    IDaemonLinkHost(IDaemonLinkHost const&) = delete;
    IDaemonLinkHost& operator=(IDaemonLinkHost const&) = delete;
    IDaemonLinkHost(IDaemonLinkHost&&) = delete;
    IDaemonLinkHost& operator=(IDaemonLinkHost&&) = delete;

    /// Opens an OS window for the daemon workspace @p link has staged. The window claims it as it loads, by
    /// calling DaemonLink::bindPendingWindow() before this returns (the QML load is synchronous).
    /// @param link The link the window is for.
    /// @param screen Where the window opens; null for the default placement.
    virtual void openWindowFor(DaemonLink& link, QScreen* screen) = 0;

    /// Closes the OS window that shows @p window, if it is still open.
    /// @param window The local workspace of the window to close.
    virtual void closeWindow(vtworkspace::WorkspaceId window) = 0;

    /// Posts @p notice to the windows @p link serves.
    /// @param link The link the notice is about.
    /// @param notice What to show.
    virtual void postNotice(DaemonLink& link, window::Notice notice) = 0;

    /// The user asked, through @p link's drift badge, to upgrade its daemon in place. Asked only of a link
    /// whose daemon is window::DaemonLocality::Local; the outcome is reported to DaemonLink::drift().
    /// @param link The link whose daemon to upgrade.
    virtual void upgradeDaemon(DaemonLink& link) = 0;

    /// The user asked, through @p link's drift badge, to restart its daemon on this release. Asked only of a
    /// link whose daemon is window::DaemonLocality::Local; the outcome is reported to DaemonLink::drift().
    /// @param link The link whose daemon to restart.
    virtual void restartDaemon(DaemonLink& link) = 0;

    /// @p link stopped because the last window it served closed, and asks to be destroyed. Not at once: the
    /// application destroys it from its event loop, once its controller binds no pane any more, for the
    /// ptys of the closed windows' sessions unregister from it as they go (DaemonLink::bindsPanes).
    /// @param link The link that ended.
    virtual void linkEnded(DaemonLink& link) = 0;
};

} // namespace contour::remote
