// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/LogStore.hpp>

namespace contour::input
{

auto inline const inputLog =
    logstore::Category("gui.input", "Logs input driver details (e.g. GUI input events).");

} // namespace contour::input
