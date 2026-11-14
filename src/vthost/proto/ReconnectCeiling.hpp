// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The numbers a client's reconnect and a daemon's reconnect grace agree on.

#include <chrono>

namespace vthost::proto
{

/// The longest wait between two reconnect attempts of a client (client::BackoffSchedule::ceiling).
inline constexpr auto ReconnectCeiling = std::chrono::milliseconds { 5000 };

/// How long a daemon may take to answer a ClientHello before the client gives that attempt up
/// (client::LivenessPolicy::handshakeDeadline): long enough for a daemon busy restoring its sessions, short
/// enough that a connection gone half-open mid-handshake is retried instead of waited on for good.
inline constexpr auto HandshakeDeadline = std::chrono::milliseconds { 10000 };

/// How long a daemon takes a window whose connection dropped to still be reconnecting
/// (SessionHostConfig::reconnectGrace). The window's last retry starts up to one ReconnectCeiling after the
/// drop and then takes up to one HandshakeDeadline to connect and attach, so the hold covers both: shorter,
/// and a new window could take the workspace between the retry's start and its attach, leaving two windows
/// on it. A resume ends the hold (the window is attached again), and a Goodbye ends it too.
inline constexpr auto ReconnectGrace = ReconnectCeiling + HandshakeDeadline;

} // namespace vthost::proto
