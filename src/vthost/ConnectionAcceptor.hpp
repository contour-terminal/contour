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

#include <vtpty/CrossingHandle.hpp>

#include <core/async/StopToken.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoResult.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <vthost/ConnectionId.hpp>

namespace vthost
{

/// Handles one accepted client connection; owns the socket for its lifetime.
/// std::function (not move_only_function, which Apple's libc++ lacks): the
/// handler factories return copyable closures — only the SOCKET argument
/// moves, and that moves through the call just fine.
using ConnectionHandler =
    std::function<core::async::Task<void>(ConnectionId id, std::unique_ptr<core::net::ISocket> connection)>;

/// Decides whether a repeated accept failure is worth a log line.
///
/// A listener wedged on EMFILE retries every 100ms forever; logging each attempt is 36,000
/// lines an hour, and a log that fills a disk is not a diagnostic. Pure by design — the failure
/// goes in, the decision comes out, no clock and no I/O — so the policy is testable without a
/// socket.
class AcceptFailureThrottle
{
  public:
    /// @param everyNth Log one line per this many consecutive IDENTICAL failures. Clamped to
    ///        at least 1, so shouldLog needs no divide-by-zero guard on every failure.
    explicit AcceptFailureThrottle(std::size_t everyNth = 50) noexcept:
        _everyNth(std::max(std::size_t { 1 }, everyNth))
    {
    }

    /// Records @p error and decides whether to report it.
    /// @param error The failure just observed.
    /// @return True for the first failure, for any change of (code, systemCode) — a NEW
    ///         problem is always news — and for every Nth repeat thereafter.
    [[nodiscard]] bool shouldLog(core::net::NetError const& error) noexcept;

    /// @return How many consecutive failures have been seen, for the message.
    [[nodiscard]] std::size_t consecutive() const noexcept { return _consecutive; }

  private:
    std::size_t _everyNth;
    core::net::NetErrorCode _lastCode = core::net::NetErrorCode::Ok;
    int _lastSystemCode = 0;
    std::size_t _consecutive = 0;
};

/// Whether a ConnectionAcceptor takes connections off its listener.
enum class Admission : std::uint8_t
{
    Accepting = 0, ///< Every connection is accepted and handed to the handler.
    /// None is: the listener stays open and bound, and connections wait in its backlog for whoever accepts
    /// next -- this acceptor once resumed, or the new image of a daemon upgraded live, which inherits it.
    Holding,
};

/// Accepts connections on an injected listener and spawns one handler flow per
/// connection.
class ConnectionAcceptor
{
  public:
    /// @param loop The loop the accept flow and every connection flow run on.
    /// @param name This endpoint's identity in diagnostics ("control", "native", "imsg",
    ///        "tmux-compat", "native-tcp"). Required and fixed at construction: an acceptor
    ///        serves exactly one endpoint for its whole life, and a nameless one would put
    ///        unattributable lines into a log covering up to five listeners.
    /// @param listener The bound endpoint to accept from (owned).
    /// @param handler Invoked once per accepted connection to produce its flow.
    /// @param failureThrottle How often a persistently failing accept is reported. Injected
    ///        rather than fixed, so the policy is the caller's and a test can drive it.
    ConnectionAcceptor(core::net::EventLoop& loop,
                       std::string name,
                       std::unique_ptr<core::net::IListener> listener,
                       ConnectionHandler handler,
                       AcceptFailureThrottle failureThrottle = AcceptFailureThrottle {});

    ConnectionAcceptor(ConnectionAcceptor const&) = delete;
    ConnectionAcceptor& operator=(ConnectionAcceptor const&) = delete;
    ConnectionAcceptor(ConnectionAcceptor&&) = delete;
    ConnectionAcceptor& operator=(ConnectionAcceptor&&) = delete;
    ~ConnectionAcceptor() = default;

    /// The accept loop: runs until the listener is closed, or found dead — then it is closed and the
    /// end logged — or the loop stops. Any other failed accept is backed off on, and accepting goes on.
    /// blockOn this (or spawn it) to serve.
    [[nodiscard]] core::async::Task<void> serve();

    /// Stops accepting; a parked accept resolves as cancelled.
    void close() noexcept;

    /// Stops taking connections off the listener without closing it: a parked accept is withdrawn, and
    /// connections wait in the backlog until resumeAccepting(). What a live upgrade does before it hands the
    /// listener to the new image. Idempotent. Loop thread.
    void holdAccepting();

    /// Takes connections off the listener again after holdAccepting(). Idempotent. Loop thread.
    void resumeAccepting();

    /// @return Whether connections are taken off the listener.
    [[nodiscard]] Admission admission() const noexcept { return _admission; }

    /// @return This endpoint's name, as the constructor was given it.
    [[nodiscard]] std::string const& name() const noexcept { return _name; }

    /// @return The listener's operating-system socket -- its descriptor on POSIX, its handle on Windows --
    ///         which a live upgrade hands to the daemon that takes over; none where it has none (an in-memory
    ///         listener).
    [[nodiscard]] vtpty::CrossingHandle listeningHandle() const;

    /// @return The number of connections accepted so far (tests/diagnostics).
    [[nodiscard]] std::size_t acceptedCount() const noexcept { return _acceptedCount; }

  private:
    core::net::EventLoop& _loop;
    std::string _name;
    std::unique_ptr<core::net::IListener> _listener;
    ConnectionHandler _handler;
    std::size_t _acceptedCount = 0;
    AcceptFailureThrottle _failureThrottle;
    Admission _admission = Admission::Accepting;
    /// Stopped by holdAccepting(), which withdraws a parked accept; renewed by resumeAccepting().
    core::async::StopSource _hold;
    /// Stopped by resumeAccepting() and close(), either of which ends a hold; renewed by holdAccepting().
    core::async::StopSource _release;
};
} // namespace vthost
