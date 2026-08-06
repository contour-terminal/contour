// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/LogStore.hpp>

namespace contour::display
{

auto inline const displayLog =
    logstore::Category("gui.display", "Logs display driver details (e.g. OpenGL).");

} // namespace contour::display
