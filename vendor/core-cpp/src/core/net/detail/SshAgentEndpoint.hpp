// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The platform half of the ssh-agent client: how an endpoint is dialled. Implemented in `posix/`
/// (a Unix socket) and `windows/` (a named pipe); the client itself is platform-free.
///
/// Private to core::net and the ssh-agent client: listed under SOURCES, not HEADERS.

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/NetError.hpp>

#include <expected>
#include <memory>
#include <string>

namespace core::net::detail
{

/// Opens one connection to the agent at @p endpoint.
/// @param loop The loop that drives the connection (a pointer: a coroutine's reference parameter
///        can dangle).
/// @param endpoint The `SSH_AUTH_SOCK` path, or on Windows the named pipe.
/// @return The connection; @c NetErrorCode::AddressError for an empty endpoint (or, on Windows,
///         one that is not UTF-8), else why the dial failed.
[[nodiscard]] async::Task<std::expected<std::unique_ptr<ISocket>, NetError>> connectAgentEndpoint(
    EventLoop* loop, std::string endpoint);

} // namespace core::net::detail
