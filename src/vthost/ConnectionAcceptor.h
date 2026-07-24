// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ConnectionAcceptor` — the daemon's accept loop.
///
/// Deliberately protocol-agnostic: every accepted connection is handed to an
/// injected handler coroutine, so the tmux control-mode protocol (and later the
/// native cells+deltas protocol) plug in without touching the acceptor. Unlike
/// the strict request/response server this design was ported from, connections
/// are handled CONCURRENTLY: each one runs as its own spawned flow, with the
/// socket's ownership moved into that flow's frame.

#include <cstddef>
#include <functional>
#include <memory>

#include <coro/Task.hpp>
#include <net/EventLoop.h>
#include <net/IListener.h>
#include <net/ISocket.h>

namespace vthost
{

/// Handles one accepted client connection; owns the socket for its lifetime.
/// std::function (not move_only_function, which Apple's libc++ lacks): the
/// handler factories return copyable closures — only the SOCKET argument
/// moves, and that moves through the call just fine.
using ConnectionHandler = std::function<coro::Task<void>(std::unique_ptr<net::ISocket> connection)>;

/// Accepts connections on an injected listener and spawns one handler flow per
/// connection.
class ConnectionAcceptor
{
  public:
    /// @param loop The loop the accept flow and every connection flow run on.
    /// @param listener The bound endpoint to accept from (owned).
    /// @param handler Invoked once per accepted connection to produce its flow.
    ConnectionAcceptor(net::EventLoop& loop,
                       std::unique_ptr<net::IListener> listener,
                       ConnectionHandler handler);

    ConnectionAcceptor(ConnectionAcceptor const&) = delete;
    ConnectionAcceptor& operator=(ConnectionAcceptor const&) = delete;
    ConnectionAcceptor(ConnectionAcceptor&&) = delete;
    ConnectionAcceptor& operator=(ConnectionAcceptor&&) = delete;
    ~ConnectionAcceptor() = default;

    /// The accept loop: runs until the listener is closed or the loop stops.
    /// blockOn this (or spawn it) to serve.
    [[nodiscard]] coro::Task<void> serve();

    /// Stops accepting; a parked accept resolves as cancelled.
    void close() noexcept { _listener->close(); }

    /// @return The number of connections accepted so far (tests/diagnostics).
    [[nodiscard]] std::size_t acceptedCount() const noexcept { return _acceptedCount; }

  private:
    net::EventLoop& _loop;
    std::unique_ptr<net::IListener> _listener;
    ConnectionHandler _handler;
    std::size_t _acceptedCount = 0;
};
} // namespace vthost
