// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/SystemPower.hpp>

#include <cstdint>
#include <optional>

namespace core::platform::detail
{

/// The PBT_* power-broadcast types the Windows backend reacts to. Plain integers mirroring the values of
/// <winuser.h>, so the decision below is table-testable on every platform without that header.
inline constexpr std::uint32_t PowerBroadcastSuspend = 0x0004;         ///< PBT_APMSUSPEND
inline constexpr std::uint32_t PowerBroadcastResumeSuspend = 0x0007;   ///< PBT_APMRESUMESUSPEND
inline constexpr std::uint32_t PowerBroadcastResumeAutomatic = 0x0012; ///< PBT_APMRESUMEAUTOMATIC

/// Decides which event a Windows power broadcast stands for. PBT_APMRESUMESUSPEND is ignored on purpose:
/// it follows PBT_APMRESUMEAUTOMATIC only when a user is present, so reacting to both would report one
/// wake twice.
/// @param type The Type argument of the suspend/resume callback.
/// @return The event to dispatch, or nothing for a broadcast the backend does not act on.
[[nodiscard]] constexpr std::optional<SystemPowerEvent> eventForPowerBroadcast(std::uint32_t type) noexcept
{
    switch (type)
    {
        case PowerBroadcastSuspend: return SystemPowerEvent::WillSleep;
        case PowerBroadcastResumeAutomatic: return SystemPowerEvent::DidWake;
        default: return std::nullopt;
    }
}

} // namespace core::platform::detail
