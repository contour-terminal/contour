// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `IListener` — a server-side endpoint that accepts incoming connections as
/// `coro::Task<AcceptResult>`. Backs the `httpServe` builtin's accept loop.

#include <expected>
#include <memory>

#include <coro/Task.hpp>
#include <net/ISocket.h>
#include <net/IoResult.h>

namespace net
{

/// Result of an asynchronous accept: a newly-connected socket, or a @c NetError
/// (typically @c Cancelled when the listener is closed during shutdown).
using AcceptResult = std::expected<std::unique_ptr<ISocket>, NetError>;

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
    [[nodiscard]] virtual coro::Task<AcceptResult> accept() = 0;

    /// @return The local port the listener is bound to (0 if unbound), useful when
    ///         binding to port 0 to get an OS-assigned ephemeral port (tests).
    [[nodiscard]] virtual std::uint16_t localPort() const noexcept = 0;

    /// Stops accepting. A pending @c accept() resolves with @c NetErrorCode::Cancelled.
    virtual void close() noexcept = 0;
};

} // namespace net
