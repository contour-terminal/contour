// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace vthost
{

/// Whether a daemon starts afresh or carries on from a live upgrade's handover image, which decides what it
/// does about what it cannot start as configured.
enum class StartContinuity : std::uint8_t
{
    Fresh = 0,  ///< It starts afresh: whatever it cannot start ends it, and the user is told.
    HandedOver, ///< It took over hosted programs: ending would hang them up, so it serves on without.
};

} // namespace vthost
