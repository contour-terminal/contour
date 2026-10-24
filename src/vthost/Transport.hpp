// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How a client reaches a daemon: the transport seam. A factory opens one fresh, ready connection per
/// call -- the local AF_UNIX socket, TCP+TLS, and (phase 5) ssh -- so whatever connects, reconnects or
/// probes a daemon is written once against `ITransportFactory`, never against an endpoint variant.

#include <core/async/Task.hpp>
#include <core/net/ISocket.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <vthost/Daemon.hpp>

namespace core::net
{
class EventLoop;
}

namespace vthost
{

/// Why a transport could not be opened. The values are agreed with phase 5 and only ever appended to.
enum class TransportErrorKind : uint8_t
{
    Unreachable = 0,            ///< The network or the socket failed in a way that may pass.
    Refused = 1,                ///< The other end will not have this client.
    AuthenticationRequired = 2, ///< A credential is needed that the user must supply first.
    HostKeyUnknown = 3,         ///< The remote host's key is not trusted yet.
    RemoteProgramMissing = 4,   ///< The remote host has no daemon program to run.
    ProgramNotFound = 5,        ///< A program the transport needs locally is missing.
    Protocol = 6,               ///< The other end answered, but not in the expected way.
    Cancelled = 7,              ///< The attempt was cancelled.
    DaemonAbsent = 8, ///< Nothing listens there: the socket file is gone, or the connection was refused.
    /// This end's own settings cannot be used (a CA that does not parse): the user must fix them first.
    Misconfigured = 9,
};

/// A failed transport.
struct TransportError
{
    TransportErrorKind kind = TransportErrorKind::Unreachable; ///< What a caller acts on.
    std::string message;                                       ///< For the user, shown verbatim.
};

/// Whether a failure may pass if the attempt is repeated.
enum class TransportRetry : uint8_t
{
    Permanent = 0, ///< Repeating cannot help until the user acts.
    Transient = 1, ///< A later attempt may succeed.
};

namespace detail
{
    /// One row of the retry table.
    struct TransportRetryRow
    {
        TransportErrorKind kind;
        TransportRetry retry;
    };

    /// Every kind's retry, one row each; a kind missing here is Permanent.
    inline constexpr auto TransportRetryTable = std::to_array<TransportRetryRow>({
        { .kind = TransportErrorKind::Unreachable, .retry = TransportRetry::Transient },
        { .kind = TransportErrorKind::Refused, .retry = TransportRetry::Permanent },
        { .kind = TransportErrorKind::AuthenticationRequired, .retry = TransportRetry::Permanent },
        { .kind = TransportErrorKind::HostKeyUnknown, .retry = TransportRetry::Permanent },
        { .kind = TransportErrorKind::RemoteProgramMissing, .retry = TransportRetry::Permanent },
        { .kind = TransportErrorKind::ProgramNotFound, .retry = TransportRetry::Permanent },
        { .kind = TransportErrorKind::Protocol, .retry = TransportRetry::Transient },
        { .kind = TransportErrorKind::Cancelled, .retry = TransportRetry::Permanent },
        // A daemon that crashed or restarts comes back: reconnecting is how a client finds it again.
        { .kind = TransportErrorKind::DaemonAbsent, .retry = TransportRetry::Transient },
        // A setting of this end's own reads the same on the next attempt.
        { .kind = TransportErrorKind::Misconfigured, .retry = TransportRetry::Permanent },
    });
} // namespace detail

/// @param kind A transport failure.
/// @return Whether repeating the attempt may help.
[[nodiscard]] constexpr TransportRetry retryOf(TransportErrorKind kind) noexcept
{
    for (auto const& row: detail::TransportRetryTable)
        if (row.kind == kind)
            return row.retry;
    return TransportRetry::Permanent;
}

/// Whether a transport proves who is at the other end, before a client sends it a token.
enum class PeerTrust : uint8_t
{
    Unverified = 0, ///< Encrypted, but the peer is not authenticated: TLS without a trust anchor.
    Verified,       ///< The peer proved its identity: TLS against a pinned CA, or ssh's host key.
    Local,          ///< This machine's socket, gated by its owner and permissions.
};

/// One open connection to a daemon.
struct Connection
{
    /// Whatever must outlive the socket (phase 5's ssh child). Declared FIRST so that it is destroyed
    /// LAST: the socket is closed and gone before what carries it is released.
    std::shared_ptr<void> lifetime;
    std::unique_ptr<core::net::ISocket> socket; ///< The ready, connected (and, over TCP, encrypted) socket.
    std::string token;                          ///< The ClientHello token; empty for unix and ssh.
};

/// Opens connections to one daemon.
class ITransportFactory
{
  public:
    virtual ~ITransportFactory() = default;

    /// Opens a fresh connection; called afresh for every attempt.
    /// @param loop The loop the connection runs on.
    /// @return The connection, or why there is none.
    [[nodiscard]] virtual core::async::Task<std::expected<std::unique_ptr<Connection>, TransportError>>
    connect(core::net::EventLoop& loop) = 0;

    /// @return Where this factory connects to, for log lines and failure messages.
    [[nodiscard]] virtual std::string describe() const = 0;

    /// @return Whether the peer is authenticated, known before connecting: a control client refuses an
    ///         Unverified transport, as an active man-in-the-middle would take its token.
    [[nodiscard]] virtual PeerTrust peerTrust() const noexcept = 0;
};

/// The local daemon over its AF_UNIX socket.
///
/// Threading: connect() runs on the loop's thread, as every factory's does, but the daemon start it begins
/// with does not -- it may wait seconds for a spawned daemon to serve, and the loop it would block is the
/// client's reactor, which must keep servicing timers, posted verbs and stop(). The start runs on a worker
/// thread of this factory's own, and its result is handed back to the loop.
class UnixTransportFactory final: public ITransportFactory
{
  public:
    /// Makes sure a daemon serves before an attempt connects; empty never starts one.
    using EnsureDaemon = std::function<std::expected<void, TransportError>()>;

    /// @param socketPath The daemon's control socket; the native socket beside it is connected.
    /// @param ensureDaemon Called before every attempt, on a worker thread rather than the loop's, and never
    ///        twice at once; empty for a caller that must never start a daemon (a status probe). An attempt
    ///        cancelled meanwhile stops waiting for it at once, the call itself running to its end.
    UnixTransportFactory(std::filesystem::path socketPath, EnsureDaemon ensureDaemon);

    /// Waits for a daemon start still under way: it cannot be interrupted, and it calls what this factory
    /// holds.
    ~UnixTransportFactory() override;

    UnixTransportFactory(UnixTransportFactory const&) = delete;
    UnixTransportFactory& operator=(UnixTransportFactory const&) = delete;
    UnixTransportFactory(UnixTransportFactory&&) = delete;
    UnixTransportFactory& operator=(UnixTransportFactory&&) = delete;

    [[nodiscard]] core::async::Task<std::expected<std::unique_ptr<Connection>, TransportError>> connect(
        core::net::EventLoop& loop) override;

    /// @return `unix:<socketPath>`.
    [[nodiscard]] std::string describe() const override;

    /// @return Local: the socket's owner check is the gate.
    [[nodiscard]] PeerTrust peerTrust() const noexcept override { return PeerTrust::Local; }

  private:
    /// The connect itself: a coroutine takes the loop by pointer, as its reference would outlive the call.
    [[nodiscard]] core::async::Task<std::expected<std::unique_ptr<Connection>, TransportError>> connectOn(
        core::net::EventLoop* loop);

    /// One daemon start: where its worker hands the result back to the loop.
    struct EnsureRun;

    /// One daemon start's worker, and the run it reports to.
    struct EnsureWorker
    {
        std::shared_ptr<EnsureRun> run;
        std::jthread thread;
    };

    /// Runs @c _ensureDaemon on a worker and waits for its result without blocking @p loop.
    /// @param loop The loop the attempt runs on.
    /// @return What @c _ensureDaemon answered.
    /// @throws core::async::OperationCancelled if the attempt is cancelled first.
    [[nodiscard]] core::async::Task<std::expected<void, TransportError>> ensureOffLoop(
        core::net::EventLoop* loop);

    std::filesystem::path _socketPath;
    EnsureDaemon _ensureDaemon;
    std::mutex _ensureMutex;  ///< Held by a worker while it calls @c _ensureDaemon.
    std::mutex _workersMutex; ///< Guards @c _workers.
    /// The workers started and not joined yet. Declared after what they call, so that it is destroyed --
    /// every thread joined -- first.
    std::vector<EnsureWorker> _workers;
};

/// A daemon over TCP, always TLS-encrypted, authenticated by the endpoint's token.
class TlsTransportFactory final: public ITransportFactory
{
  public:
    /// @param endpoint Where the daemon listens, its token, and the CA pinning its certificate (empty for
    ///        trust on first use).
    explicit TlsTransportFactory(TcpEndpoint endpoint);

    [[nodiscard]] core::async::Task<std::expected<std::unique_ptr<Connection>, TransportError>> connect(
        core::net::EventLoop& loop) override;

    /// @return `tls://host:port`.
    [[nodiscard]] std::string describe() const override;

    /// @return Verified when the endpoint pins a CA, else Unverified (trust on first use encrypts only).
    [[nodiscard]] PeerTrust peerTrust() const noexcept override
    {
        return _endpoint.caPem.empty() ? PeerTrust::Unverified : PeerTrust::Verified;
    }

  private:
    /// The connect itself: a coroutine takes the loop by pointer, as its reference would outlive the call.
    [[nodiscard]] core::async::Task<std::expected<std::unique_ptr<Connection>, TransportError>> connectOn(
        core::net::EventLoop* loop);

    TcpEndpoint _endpoint;
};

} // namespace vthost
