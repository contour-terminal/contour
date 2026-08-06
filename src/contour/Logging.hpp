// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/Logging.hpp>

#include <crispy/logstore.hpp>

namespace contour
{

/// Startup timing, logged by the composition root and by the display it brings up. It belongs to
/// neither layer on its own, which is why it sits with main.cpp rather than inside one of them.
auto inline const startupLog = logstore::category("gui.startup", "Logs startup timing information.");

} // namespace contour
