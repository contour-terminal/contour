// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Clock.hpp>

#include <chrono>
#include <optional>

namespace core::tui
{

/// Computes remaining milliseconds from a start time point and a timeout duration.
///
/// @param clock The clock @p start was read from, and the one "now" is read from.
/// @param start The time point when the timer started.
/// @param timeout The total timeout duration.
/// @return 0 if the timeout has expired, otherwise the remaining milliseconds.
template <typename Duration>
[[nodiscard]] int remainingMs(IClock const& clock, SteadyTimePoint start, Duration timeout)
{
    auto const remaining = timeout - (clock.now() - start);
    if (remaining <= std::chrono::milliseconds::zero())
        return 0;
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count());
}

/// Computes remaining milliseconds for an optional timer.
///
/// @param clock The clock @p start was read from, and the one "now" is read from.
/// @param start The optional time point when the timer started.
/// @param timeout The total timeout duration.
/// @return -1 if the timer is not active, 0 if expired, otherwise the remaining milliseconds.
template <typename Duration>
[[nodiscard]] int remainingMs(IClock const& clock,
                              std::optional<SteadyTimePoint> const& start,
                              Duration timeout)
{
    if (!start)
        return -1;
    return remainingMs(clock, *start, timeout);
}

} // namespace core::tui
