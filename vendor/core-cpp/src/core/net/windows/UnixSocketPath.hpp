// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Claiming an AF_UNIX socket path on Windows, for `IocpListener::bindUnix`. Private to this
/// module's Windows sources; it was shared with the readiness `WindowsListener` until 0.5.0
/// removed that transport (core-cpp#6).

// winsock2.h MUST precede windows.h / ws2tcpip.h, and afunix.h needs what they declare.
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <afunix.h>
// clang-format on

#include <core/net/NetError.hpp>

#include <expected>
#include <string_view>

namespace core::net::detail
{

/// Builds the AF_UNIX address for @p path and makes the path bindable: nothing is there, or a
/// stale socket file is, which is deleted. A live server keeps its path (@c NetErrorCode::AddressInUse),
/// and a file that is not a socket is never touched.
/// @param path The socket file path.
/// @return The address to bind, or why the path cannot be claimed; @c NetErrorCode::AddressError
///         for a path too long for `sun_path`.
[[nodiscard]] std::expected<sockaddr_un, NetError> claimUnixSocketPath(std::string_view path);

} // namespace core::net::detail
