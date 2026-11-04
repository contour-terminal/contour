// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/SystemPower.hpp>

#include <cstdint>
#include <optional>

namespace core::platform::detail
{

/// The system power messages a sleep/wake backend that must acknowledge sleep reacts to, named after the
/// ones macOS (IOKit) delivers.
enum class PowerMessage : std::uint8_t
{
    Other = 0,      ///< A message the backend does not act on.
    CanSystemSleep, ///< The system asks whether it may idle-sleep.
    WillSleep,      ///< The system is going to sleep.
    WillNotSleep,   ///< An idle sleep that was asked about (CanSystemSleep) will not happen.
    HasPoweredOn,   ///< The system has woken up.
};

/// Whether a message must be answered to let the system go on.
enum class PowerAcknowledgement : std::uint8_t
{
    No = 0,
    Yes,
};

/// What a backend does for one power message.
struct PowerMessageAction
{
    std::optional<SystemPowerEvent> event; ///< What to dispatch to the subscribers, if anything.
    PowerAcknowledgement acknowledge = PowerAcknowledgement::No; ///< Whether to allow the power change.

    [[nodiscard]] constexpr bool operator==(PowerMessageAction const&) const noexcept = default;
};

/// Decides what to do for a power message. The event is dispatched before the acknowledgement, so the
/// subscribers see WillSleep while the machine is still awake. WillNotSleep reports nothing: per
/// <IOKit/IOMessage.h> it only ever follows CanSystemSleep, which reported nothing either, so there is no
/// WillSleep for it to take back.
/// @param message The message received.
/// @return The event to dispatch and whether to acknowledge.
[[nodiscard]] constexpr PowerMessageAction planPowerMessage(PowerMessage message) noexcept
{
    switch (message)
    {
        case PowerMessage::CanSystemSleep: return { std::nullopt, PowerAcknowledgement::Yes };
        case PowerMessage::WillSleep: return { SystemPowerEvent::WillSleep, PowerAcknowledgement::Yes };
        case PowerMessage::WillNotSleep: return {};
        case PowerMessage::HasPoweredOn: return { SystemPowerEvent::DidWake, PowerAcknowledgement::No };
        case PowerMessage::Other: break;
    }
    return {};
}

} // namespace core::platform::detail
