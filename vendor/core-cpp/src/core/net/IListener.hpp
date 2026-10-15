// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `IListener` — a server-side endpoint that accepts incoming connections as
/// `async::Task<AcceptResult>`. Backs the `httpServe` builtin's accept loop.

#include <core/async/StopToken.hpp>
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
///
/// **Closing is the base class's, and an implementation supplies only what its own close does**
/// (@c doClose). @c close() fires @c closeToken() first, and so does the destructor, so whoever
/// waits on something other than @c accept() -- an accept loop backing off, above all -- hears the
/// close at once rather than at its next accept, and hears a destruction it could not otherwise
/// detect without touching freed memory. A virtual `close()` each implementation remembered to
/// signal from could not promise either.
class IListener
{
  public:
    /// Allocates the close token's shared state, so it may throw @c std::bad_alloc: an
    /// implementation's constructor is not `noexcept`, or an allocation failure there would end the
    /// process rather than reach the caller that asked for the listener.
    IListener() = default;

    /// Fires @c closeToken() if @c close() did not, after the implementation's own destructor has
    /// run: a holder of the token learns that the listener is gone without touching it.
    virtual ~IListener() { _closing.request_stop(); }

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
    ///
    /// Fires @c closeToken(), then runs @c doClose() -- once: a second call does nothing.
    void close() noexcept
    {
        if (_closing.request_stop())
            doClose();
    }

    /// @return A token stopped when this listener is closed or destroyed. It holds shared state, so
    ///         it may be kept and asked after the listener is gone: an accept loop takes it before it
    ///         first accepts, races its backoff against it, and asks it before every accept, which
    ///         is what keeps it from calling into a listener its owner has destroyed.
    [[nodiscard]] async::StopToken closeToken() const noexcept { return _closing.get_token(); }

  protected:
    /// What closing does for this implementation: release the handle, and resolve a pending
    /// @c accept() with @c NetErrorCode::Cancelled. Called at most once, by @c close(), after
    /// @c closeToken() has fired. An implementation's destructor may call @c close() -- inside its
    /// own destructor the call still reaches its own @c doClose(), as `IocpListener`'s does -- or
    /// release the handle by code of its own, as `PosixListener`'s does when a parked accept must
    /// unwind rather than resume. The base destructor fires the token either way.
    virtual void doClose() noexcept = 0;

  private:
    async::StopSource _closing; ///< What @c closeToken() hands out.
};

} // namespace core::net
