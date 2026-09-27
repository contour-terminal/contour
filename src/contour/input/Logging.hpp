// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/log/LogStore.hpp>

namespace contour::input
{

auto inline const inputLog =
    core::log::Category("gui.input", "Logs input driver details (e.g. GUI input events).");

} // namespace contour::input
