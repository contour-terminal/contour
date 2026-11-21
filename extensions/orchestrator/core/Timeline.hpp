// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <orchestrator/core/Run.hpp>

/// What the agents did over the last day, hour by hour, as the dashboard charts it: read from the runs'
/// histories and statistics, so it needs nothing the runs do not already keep.
namespace orchestrator
{

/// How many buckets a timeline has.
inline constexpr std::size_t TimelineBuckets = 24;

/// How long one bucket lasts: an hour.
inline constexpr uint64_t TimelineBucketMs = 3'600'000;

/// The last TimelineBuckets hours, oldest first; the last bucket ends now.
struct Timeline
{
    std::array<uint32_t, TimelineBuckets> active {};  ///< Agents active at some time in the hour.
    std::array<uint64_t, TimelineBuckets> tokens {};  ///< Output tokens, a run's spread evenly over the time
                                                      ///< it was active: its statistics are totals.
    std::array<uint32_t, TimelineBuckets> started {}; ///< Runs dispatched in the hour.
    std::array<uint32_t, TimelineBuckets> ended {};   ///< Runs that ended in the hour.
    uint32_t activeNow = 0;                           ///< Agents active now.

    [[nodiscard]] bool operator==(Timeline const&) const = default;
};

/// @param runs The runs, finished or not.
/// @param nowWallMs Now, in wall-clock milliseconds.
/// @return What they did over the last TimelineBuckets hours.
[[nodiscard]] Timeline timelineOf(std::span<Run const> runs, uint64_t nowWallMs);

} // namespace orchestrator
