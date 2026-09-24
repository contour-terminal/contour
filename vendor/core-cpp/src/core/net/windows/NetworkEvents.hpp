// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The one way this module takes a Winsock readiness indication off an event.

// clang-format off
#include <winsock2.h>
#include <windows.h>
// clang-format on

namespace core::net
{

/// Consumes the network-event indications Winsock has recorded for @p socket on @p event,
/// resetting the event object in the same step.
///
/// `WSAResetEvent` is NOT a substitute, and this exists so that no call site reaches for it.
/// Winsock records an indication once and signals the event once; it raises that indication
/// again only after the call that re-enables it (accept for FD_ACCEPT, recv for FD_READ, a send
/// that returns WSAEWOULDBLOCK for FD_WRITE). A reset therefore clears the EVENT and leaves the
/// RECORD standing, so an indication that arrived between a failing syscall and the reset is
/// lost for good, and whatever parks on the event afterwards never wakes. For a listener that
/// means it stops accepting — for this connection and every later one, since the record is
/// still set. `WSAEnumNetworkEvents` clears both at once and hands back what it took, so the
/// caller can act on an indication instead of parking on an event that can no longer signal.
///
/// @param socket The socket the event was associated with by `WSAEventSelect`.
/// @param event The event associated with @p socket.
/// @return The FD_* bitmask consumed; 0 when nothing was recorded, when either argument is
///         invalid, or when the enumeration itself failed (in which case the caller parks, and
///         the ordinary readiness path resolves it).
[[nodiscard]] long consumeNetworkEvents(SOCKET socket, WSAEVENT event) noexcept;

} // namespace core::net
