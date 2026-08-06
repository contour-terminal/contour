// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/logstore.hpp>

namespace contour::display
{

auto inline const displayLog =
    logstore::category("gui.display", "Logs display driver details (e.g. OpenGL).");

} // namespace contour::display
