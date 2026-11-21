// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The reveal walk: how a tab a daemon-scoped extension holds -- its served tab, or an agent pane it opened
/// -- is brought in front of a window, on the host's loop.

#include <vthost/ConnectionId.hpp>
#include <vtworkspace/Primitives.hpp>

namespace vthost
{

class ServedTabHost;

/// Activates @p session's tab and tells the window on @p connection, which shows @p origin, to raise itself
/// and focus it. For a tab in another workspace, that window is asked to switch there first: refused, or
/// answered with the picker, nothing is activated; a window that already showed it and was focused instead is
/// the one told. A session the model no longer holds is not revealed. Loop thread.
/// @param host The host's loop-side calls.
/// @param session The tab's session.
/// @param connection The window to reveal it in.
/// @param origin The workspace that window shows.
void revealSession(ServedTabHost& host,
                   vtworkspace::SessionId session,
                   ConnectionId const& connection,
                   vtworkspace::WorkspaceId origin);

} // namespace vthost
