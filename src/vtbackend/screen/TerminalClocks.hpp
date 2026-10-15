// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/SteadyClockRef.hpp>
#include <vtbackend/screen/LocalTime.hpp>

#include <core/platform/Clock.hpp>

namespace vtbackend
{

/// The clocks a Terminal reads; injected so tests drive time deterministically.
///
/// Two, because they answer different questions: the steady clock measures how long a command ran (an NTP
/// step must not make a build take minus three seconds), the wall clock says when something happened in
/// the user's terms (a row's birth time, a block's start). Both are BORROWED: whoever builds the Terminal
/// keeps them alive for its whole life, and both members refuse a temporary at compile time.
///
/// Beside them sits the time zone those calendar times are read in: ambient process state too, and as
/// mandatory -- it has no default, and refuses a null conversion at compile time.
struct TerminalClocks
{
    /// Monotonic time, for durations.
    SteadyClockRef steady;

    /// Calendar time, for the times a user reads.
    core::platform::WallClockRef wall;

    /// Converts a wall-clock instant to local civil time for display -- the gutter's timestamps, the
    /// block tooltip. The time zone is ambient process state, so it is injected like the clocks are.
    LocalTimeConverter localTime;

    /// The production clocks (process-wide defaults from core::platform).
    /// @return Clocks reading std::chrono::steady_clock and std::chrono::system_clock, read in the
    ///         process's own time zone.
    [[nodiscard]] static TerminalClocks system() noexcept
    {
        return TerminalClocks { .steady = core::platform::defaultSteadyClock(),
                                .wall = core::platform::defaultSystemWallClock(),
                                .localTime = &systemLocalTime };
    }
};

} // namespace vtbackend
