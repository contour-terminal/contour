// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/logstore.h>

namespace contour::input
{

auto inline const inputLog =
    logstore::category("gui.input", "Logs input driver details (e.g. GUI input events).");

} // namespace contour::input
