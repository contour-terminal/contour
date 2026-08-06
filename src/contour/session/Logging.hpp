// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/logstore.hpp>

namespace contour::session
{

auto inline const sessionLog = logstore::category("gui.session", "VT terminal session logs");
auto inline const managerLog = logstore::category("gui.session_manager", "Sessions manager logs");

} // namespace contour::session
