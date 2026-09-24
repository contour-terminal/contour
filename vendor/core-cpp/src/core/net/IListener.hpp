// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `IListener` — a server-side endpoint that accepts incoming connections as
/// `async::Task<AcceptResult>`. Backs the `httpServe` builtin's accept loop.

#include <core/async/Task.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoResult.hpp>

#include <expected>
#include <memory>

namespace core::net
{

/// Result of an asynchronous accept: a newly-connected socket, or a @c NetError
/// (typically @c Cancelled when the listener is closed during shutdown).
///
/// **An alias of @c SocketResult rather than a second spelling of the same type.** Accept and
/// connect answer the same question and their results are the same type, so a helper that
/// consumes one consumes the other — and writing the expansion twice invites the two to drift.
using AcceptResult = SocketResult;

/// A server endpoint producing connected @c ISockets.
class IListener
{
  public:
    IListener() = default;
    virtual ~IListener() = default;

    IListener(IListener const&) = delete;
    IListener& operator=(IListener const&) = delete;
    IListener(IListener&&) = delete;
    IListener& operator=(IListener&&) = delete;

    /// Accepts the next incoming connection, parking the caller until one arrives.
    /// @return A task resolving to the accepted socket, or a @c NetError
    ///         (@c Cancelled if the listener is closed while accepting).
    [[nodiscard]] virtual async::Task<AcceptResult> accept() = 0;

    /// @return The port the listener is actually bound to (0 if it has none — an AF_UNIX
    ///         listener, or an unbound one).
    ///
    /// **The port it GOT, not the port it asked for**, which is the whole reason to ask: a bind
    /// to port 0 means "pick a free one", so the number an operator, a log line or a test needs
    /// is the kernel's answer. Spelled `localPort` in contour; see the CHANGELOG's Breaking
    /// section for the migration.
    [[nodiscard]] virtual std::uint16_t boundPort() const noexcept = 0;

    /// Stops accepting. A pending @c accept() resolves with @c NetErrorCode::Cancelled.
    virtual void close() noexcept = 0;
};

} // namespace core::net
