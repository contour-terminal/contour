// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/log/LogStore.hpp>

namespace contour::session
{

auto inline const sessionLog = core::log::Category("gui.session", "VT terminal session logs");
auto inline const managerLog = core::log::Category("gui.session_manager", "Sessions manager logs");

} // namespace contour::session
