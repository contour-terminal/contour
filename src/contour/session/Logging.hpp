// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/logstore.hpp>

namespace contour::session
{

auto inline const sessionLog = logstore::Category("gui.session", "VT terminal session logs");
auto inline const managerLog = logstore::Category("gui.session_manager", "Sessions manager logs");

} // namespace contour::session
