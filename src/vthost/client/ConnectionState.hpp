// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Where a supervised connection to a daemon stands: the vocabulary `ClientSupervisor` reports in, kept
/// apart from it so that a frontend that only shows the state (a pane saying "Reconnecting…") names no
/// networking.

#include <cstddef>
#include <cstdint>

namespace vthost::client
{

/// Where a supervised connection stands.
enum class ConnectionState : uint8_t
{
    Connecting = 0,   ///< The first connect is under way.
    Connected = 1,    ///< The daemon accepted the handshake.
    Reconnecting = 2, ///< A connection that was up is lost, and a new one is being sought.
    /// The first connect failed, or the user dismissed a login prompt of it; nothing is retried, the caller
    /// decides what follows.
    Failed = 3,
    Refused = 4, ///< A reconnect met a refusal or a permanent transport error; nothing is retried.
    Stopped = 5, ///< stop() ended it.
    /// The daemon announced it replaces itself in place (DaemonUpgrading) and closed the connection: the same
    /// daemon is waited for, and none is started meanwhile (ClientSupervisor's UpgradeHold).
    Upgrading = 6,
    /// A reconnect that would ask the user something (an ssh login) is held until a window of theirs is
    /// exposed; nothing is attempted meanwhile.
    NeedsAttention = 7,
};

/// Number of @c ConnectionState enumerators, derived from the last one; tables indexed by state pin to it.
inline constexpr std::size_t ConnectionStateCount =
    static_cast<std::size_t>(ConnectionState::NeedsAttention) + 1;

} // namespace vthost::client
