// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/SystemPower.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>

namespace core::platform::detail
{

/// The boolean argument of logind's PrepareForSleep signal, named after its meaning.
enum class LogindSleepState : std::uint8_t
{
    Waking = 0, ///< PrepareForSleep(false): the system has resumed.
    Sleeping,   ///< PrepareForSleep(true): the system is about to sleep.
};

/// Maps the argument of PrepareForSleep to the event subscribers see.
/// @param state The signal's argument.
/// @return WillSleep for a system about to sleep, DidWake once it has resumed.
[[nodiscard]] constexpr SystemPowerEvent eventForPrepareForSleep(LogindSleepState state) noexcept
{
    switch (state)
    {
        case LogindSleepState::Sleeping: return SystemPowerEvent::WillSleep;
        case LogindSleepState::Waking: break;
    }
    return SystemPowerEvent::DidWake;
}

/// Converts sd_bus_get_timeout()'s absolute CLOCK_MONOTONIC deadline into a poll() timeout.
/// @param deadlineUsec The absolute deadline in microseconds; UINT64_MAX means none.
/// @param nowUsec The current CLOCK_MONOTONIC time in microseconds.
/// @return -1 for no deadline, otherwise the milliseconds left, rounded up and clamped to int.
[[nodiscard]] constexpr int pollTimeoutMilliseconds(std::uint64_t deadlineUsec,
                                                    std::uint64_t nowUsec) noexcept
{
    if (deadlineUsec == std::numeric_limits<std::uint64_t>::max())
        return -1;
    if (deadlineUsec <= nowUsec)
        return 0;
    auto const remaining = deadlineUsec - nowUsec;
    auto const milliseconds = (remaining / 1000) + (remaining % 1000 != 0 ? 1U : 0U);
    constexpr auto Cap = static_cast<std::uint64_t>(std::numeric_limits<int>::max());
    return static_cast<int>(milliseconds < Cap ? milliseconds : Cap);
}

/// The wait before the first attempt to reconnect to a system bus that was lost.
inline constexpr auto ReconnectFirstDelay = std::chrono::milliseconds { 1'000 };

/// The longest wait between two attempts to reconnect to the system bus.
inline constexpr auto ReconnectMaxDelay = std::chrono::milliseconds { 30'000 };

/// The capped exponential backoff of the Linux backend's reconnect: 1 s, doubling, at most 30 s.
/// @param attempt How many attempts have failed since the bus last worked; 0 for the first.
/// @return How long to wait before the next attempt.
[[nodiscard]] constexpr std::chrono::milliseconds reconnectDelay(std::uint32_t attempt) noexcept
{
    // ReconnectFirstDelay << 5 is already past the cap, and a larger shift could overflow.
    constexpr auto Doublings = std::uint32_t { 5 };
    if (attempt >= Doublings)
        return ReconnectMaxDelay;
    return std::min(ReconnectFirstDelay * (std::uint32_t { 1 } << attempt), ReconnectMaxDelay);
}

} // namespace core::platform::detail
