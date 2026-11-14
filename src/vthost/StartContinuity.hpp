// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace vthost
{

/// Whether a daemon starts afresh or carries on -- from a live upgrade's handover image, or after a crash
/// from what its keeper kept -- which decides what it does about what it cannot start as configured.
enum class StartContinuity : std::uint8_t
{
    Fresh = 0,  ///< It starts afresh: whatever it cannot start ends it, and the user is told.
    HandedOver, ///< It took over hosted programs: ending would hang them up, so it serves on without.
    /// Its keeper restarted it after a crash: as for HandedOver, ending would hang up what the keeper kept,
    /// so it serves on without.
    Recovered,
};

/// @param continuity How a daemon starts.
/// @return Whether it carries on from a previous daemon process, and so serves on without what it cannot
///         start rather than ending.
[[nodiscard]] constexpr bool carriesOn(StartContinuity continuity) noexcept
{
    return continuity != StartContinuity::Fresh;
}

} // namespace vthost
