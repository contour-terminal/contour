// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `detail::fromWinsockError` — the one table a Winsock or Win32 error from a socket operation is
/// classified by on Windows.
///
/// **One table, because a second copy lacks a row** (`.agent/rules/async-and-net.md`, "A platform
/// socket error is classified in one table"): the dial, the completion socket and the completion
/// listener all reach it, and it answers every `WSAE*` code through `detail::classifySocketError`,
/// the table the datagram and blocking transports use. It adds only what a completion or a dial
/// meets and a `WSAE*` table cannot hold: `ERROR_OPERATION_ABORTED` is `Cancelled`, and a missing
/// family or protocol is `Unsupported`. Defined in `windows/DialPrimitives.cpp`, where the dial's
/// copy of it used to live alone.

#include <core/net/NetError.hpp>

#include <string>

namespace core::net::detail
{

/// Classifies @p error, a `WSAGetLastError()`, `GetLastError()` or @c completionError value.
/// @param error The system error.
/// @param context What was being attempted, for the message.
/// @return The classified error, carrying @p error as its system code.
[[nodiscard]] NetError fromWinsockError(int error, std::string context);

} // namespace core::net::detail
