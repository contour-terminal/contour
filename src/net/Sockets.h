// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Cross-platform factory functions for the async socket layer. Consumers use
/// these instead of including the per-platform implementation headers directly;
/// each resolves to the right backend (PosixSocket/Listener or
/// WindowsSocket/Listener) at compile time.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>
#include <vector>

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

/// Binds an AF_UNIX listener on the socket file @p path, hardening its parent
/// directory first (see UnixListener::bind for the exact policy).
/// @param loop The loop whose reactor drives accept readiness (not owned).
/// @param path The socket file path.
/// @param backlog The listen backlog.
/// @return The bound listener; @c NetErrorCode::Unsupported on Windows (for now).
[[nodiscard]] std::expected<std::unique_ptr<IListener>, NetError> listenUnix(EventLoop& loop,
                                                                             std::string_view path,
                                                                             int backlog = 128);

/// Connects to the AF_UNIX socket file @p path.
/// @param loop The loop whose reactor drives connect readiness (not owned; a
///        pointer, since coroutine reference parameters can dangle).
/// @param path The socket file path.
/// @return A task resolving to the connected socket; a @c NetError on failure,
///         @c NetErrorCode::Unsupported on Windows (for now).
[[nodiscard]] coro::Task<std::expected<std::unique_ptr<ISocket>, NetError>> connectUnix(
    EventLoop* loop, std::string_view path);

/// Appends one read chunk from @p socket to @p buffer — the accumulate step of
/// every binary-framed decode loop.
/// @param socket The transport to read from (not owned; a pointer, since
///        coroutine reference parameters can dangle).
/// @param buffer Receives the read bytes.
/// @return False on clean EOF or error; true when bytes were appended.
[[nodiscard]] coro::Task<bool> appendReadChunk(ISocket* socket, std::vector<std::byte>* buffer);

} // namespace net
