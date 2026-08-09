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

} // namespace contour
