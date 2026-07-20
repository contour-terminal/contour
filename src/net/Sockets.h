// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Cross-platform factory functions for the async socket layer. Consumers use
/// these instead of including the per-platform implementation headers directly;
/// each resolves to the right backend (PosixSocket/Listener or
/// WindowsSocket/Listener) at compile time.

#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>

#include <coro/Task.hpp>
#include <net/EventLoop.h>
#include <net/IListener.h>
#include <net/ISocket.h>
#include <net/IoResult.h>

namespace net
{

/// Binds a TCP listener on @p host : @p port driven by @p loop's reactor.
/// @param loop The loop whose reactor drives accept readiness (not owned).
/// @param host The bind address ("127.0.0.1", "0.0.0.0", "::").
/// @param port The bind port; 0 requests an OS-assigned ephemeral port.
/// @param backlog The listen backlog.
/// @return The bound listener, or a @c NetError on failure.
[[nodiscard]] std::expected<std::unique_ptr<IListener>, NetError> listen(EventLoop& loop,
                                                                         std::string_view host,
                                                                         std::uint16_t port,
                                                                         int backlog = 128);

/// Connects a TCP client socket to @p host : @p port, parking the caller until the
/// connection completes.
/// @param loop The loop whose reactor drives connect readiness (not owned; a
///        pointer, since coroutine reference parameters can dangle).
/// @param host The remote host ("127.0.0.1", a hostname).
/// @param port The remote port.
/// @return A task resolving to the connected socket, or a @c NetError on failure.
[[nodiscard]] coro::Task<std::expected<std::unique_ptr<ISocket>, NetError>> connect(EventLoop* loop,
                                                                                    std::string_view host,
                                                                                    std::uint16_t port);

} // namespace net
