// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/Logging.hpp>

#include <crispy/LogStore.hpp>

namespace contour
{

/// Startup timing, logged by the composition root and by the display it brings up. It belongs to
/// neither layer on its own, which is why it sits with main.cpp rather than inside one of them.
auto inline const startupLog = logstore::Category("gui.startup", "Logs startup timing information.");

/// Desktop notifications, logged by the notifier that decides what to send and by the D-Bus
/// transport that sends it. Shared for the same reason as startupLog: it belongs to neither on its
/// own, and a single category keeps one name for the user to switch on.
auto inline const notifierLog = logstore::Category("gui.notifier", "Desktop notification backend");

/// xdg-desktop-portal method calls, logged by the one asynchronous caller every portal-speaking
/// class shares. Its own category rather than each caller's, because what it reports -- a call that
/// the portal refused or never answered -- is a fact about the portal, and the message names the
/// interface and method so the notification and launcher cases stay tellable apart.
auto inline const portalLog = logstore::Category("gui.portal", "xdg-desktop-portal method calls");

/// Opening a URL, file or folder in the desktop's handler. Its own category because the interesting
/// events -- a portal that refused, a fallback that was taken -- arrive ASYNCHRONOUSLY, long after
/// the action that caused them returned, and are the only trace such a failure leaves.
auto inline const launcherLog = logstore::Category("gui.launcher", "Opening URLs in the desktop's handler");

} // namespace contour
