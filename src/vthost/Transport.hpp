// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How a client reaches a daemon: the transport seam. A factory opens one fresh, ready connection per
/// call -- the local AF_UNIX socket, TCP+TLS, and (phase 5) ssh -- so whatever connects, reconnects or
/// probes a daemon is written once against `ITransportFactory`, never against an endpoint variant.

#include <core/async/Task.hpp>
#include <core/net/ISocket.hpp>
#include <core/platform/Clock.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <vthost/DaemonSpawn.hpp>
#include <vthost/KnownDaemons.hpp>

namespace core::net
{
class EventLoop;
}

namespace vthost::client
{
class IClientKeySource;
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
    /// The remote host's key differs from the one on record: a reinstalled machine, or someone in between.
    /// Unlike HostKeyUnknown, no answer from the user makes it safe to go on.
    HostKeyChanged = 10,
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

    /// Every kind's retry, one row each.
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
        { .kind = TransportErrorKind::HostKeyChanged, .retry = TransportRetry::Permanent },
    });

    static_assert(TransportRetryTable.size()
                      == static_cast<std::size_t>(TransportErrorKind::HostKeyChanged) + 1,
                  "TransportRetryTable needs a row for every TransportErrorKind");
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

/// One open connection to a daemon.
struct Connection
{
    /// Whatever must outlive the socket (phase 5's ssh child). Declared FIRST so that it is destroyed
    /// LAST: the socket is closed and gone before what carries it is released.
    std::shared_ptr<void> lifetime;
    std::unique_ptr<core::net::ISocket> socket; ///< The ready, connected (and, over TCP, encrypted) socket.
    std::string token;                          ///< The ClientHello token; empty for unix and ssh.
    /// What signs this client's answer to the daemon's AuthChallenge; null where the transport has none to
    /// offer (unix and ssh are not challenged). Shared with the factory that opened the connection.
    std::shared_ptr<client::IClientKeySource> keySource = {};
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

    /// Holds back starting a daemon until @p until: the daemon announced that it replaces itself in place
    /// (DaemonUpgrading), so its absence meanwhile is a gap to wait out, and a daemon started into it would
    /// be a second one. An attempt made during the hold connects without starting one, and reads an absent
    /// daemon as TransportErrorKind::DaemonAbsent. A later call replaces the hold; one with a time already
    /// past releases it.
    ///
    /// Not pure: only the local factory starts a daemon of its own, and the others ignore it. TLS and a
    /// test's transport start none. ssh does, on the remote host, through the relay's local daemon connector
    /// -- but that relay probes first (daemonAccepts), and the listener a daemon keeps across its exec
    /// answers the probe, so the relay starts nothing during an upgrade.
    /// @param until When the hold ends, on the clock of the loop the attempts run on.
    virtual void holdDaemonStart(core::platform::SteadyTimePoint /*until*/) {}
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

    /// Skips @c ensureDaemon in every attempt made before @p until, on the attempt's loop clock. Any thread.
    void holdDaemonStart(core::platform::SteadyTimePoint until) override;

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
    /// Until when no attempt starts a daemon (holdDaemonStart()), as ticks since the loop clock's epoch: the
    /// minimum holds nothing back.
    std::atomic<core::platform::SteadyDuration::rep> _holdUntil {
        core::platform::SteadyTimePoint::min().time_since_epoch().count()
    };
    std::mutex _ensureMutex;  ///< Held by a worker while it calls @c _ensureDaemon.
    std::mutex _workersMutex; ///< Guards @c _workers.
    /// The workers started and not joined yet. Declared after what they call, so that it is destroyed --
    /// every thread joined -- first.
    std::vector<EnsureWorker> _workers;
};

/// A daemon over TCP, always TLS-encrypted. It authenticates itself by a CA the endpoint pins or by the
/// fingerprint of its certificate, pinned on first use; the client authenticates by the endpoint's token or
/// by an SSH key.
class TlsTransportFactory final: public ITransportFactory
{
  public:
    /// @param endpoint Where the daemon listens, its token, the CA pinning its certificate (empty for the
    ///        fingerprint to decide) and the fingerprint the user was told out of band, if any.
    /// @param knownDaemons The fingerprints pinned so far, and where a new one is kept. Must outlive the
    ///        factory.
    /// @param pinPrompt Asks the user to pin a daemon met for the first time; null when there is no one to
    ///        ask, which makes such a daemon acceptable only by its @c TcpEndpoint::expectedFingerprint.
    /// @param keySourceProvider Makes, on each attempt's loop, what signs the answers to the daemon's
    ///        AuthChallenge; empty for a client that has no key. A provider rather than a source, as a source
    ///        holds sockets and children that belong to the loop of the attempt, which a headless verb makes
    ///        only when it runs.
    TlsTransportFactory(
        TcpEndpoint endpoint,
        KnownDaemons& knownDaemons,
        std::shared_ptr<IPinPrompt> pinPrompt,
        std::function<std::shared_ptr<client::IClientKeySource>(core::net::EventLoop&)> keySourceProvider);

    [[nodiscard]] core::async::Task<std::expected<std::unique_ptr<Connection>, TransportError>> connect(
        core::net::EventLoop& loop) override;

    /// @return `tls://host:port`.
    [[nodiscard]] std::string describe() const override;

  private:
    /// How a daemon's certificate came to be trusted.
    enum class TrustBasis : uint8_t
    {
        Known = 0, ///< A CA, the fingerprint the user gave, a pin, or what the user trusted a moment ago.
        Prompted,  ///< The user was asked, said yes, and the daemon was pinned just now.
    };

    /// One TCP connect and TLS handshake, then the decision on whom it reached.
    struct Dialled
    {
        std::expected<std::unique_ptr<Connection>, TransportError> connection;
        /// What the user was just asked about and trusted; empty when nobody was asked.
        std::string promptedFingerprint {};
    };

    /// The connect itself: a coroutine takes the loop by pointer, as its reference would outlive the call.
    ///
    /// A daemon the user had to be asked about is dialled a second time once it is trusted: the first
    /// connection waited for the answer, perhaps longer than the daemon lets a connection wait to sign in,
    /// and the second is made with the pin in place, with nothing left to ask.
    [[nodiscard]] core::async::Task<std::expected<std::unique_ptr<Connection>, TransportError>> connectOn(
        core::net::EventLoop* loop);

    /// Dials once.
    /// @param loop The loop the connection runs on.
    /// @param trusted The fingerprint the user trusted on an earlier dial of this connect, if any; kept in
    ///        memory, so that a pin removed meanwhile (`forget-pin` from elsewhere) does not have the user
    ///        asked again.
    [[nodiscard]] core::async::Task<Dialled> dial(core::net::EventLoop* loop, std::string trusted);

    /// The TCP connect and the TLS handshake.
    [[nodiscard]] core::async::Task<std::expected<std::unique_ptr<core::net::ISocket>, TransportError>>
    handshaken(core::net::EventLoop* loop);

    /// Decides whether to trust the daemon whose certificate has @p fingerprint, pinning it when that is
    /// the decision. A pin store that cannot be read refuses (Misconfigured), naming the file, and so does
    /// one that cannot keep the pin of a daemon the user was asked about; a pin given by @c
    /// expectedFingerprint that cannot be kept is logged, and the connection goes on, as ssh does.
    /// @param fingerprint What the daemon presents.
    /// @param trusted What the user trusted on an earlier dial of this connect; empty for none.
    /// @return How it came to be trusted, else why not.
    [[nodiscard]] core::async::Task<std::expected<TrustBasis, TransportError>> decideTrust(
        std::string fingerprint, std::string trusted);

    /// @return `host:port`, as pins are keyed.
    [[nodiscard]] std::string hostPort() const;

    TcpEndpoint _endpoint;
    KnownDaemons& _knownDaemons;
    std::shared_ptr<IPinPrompt> _pinPrompt;
    std::function<std::shared_ptr<client::IClientKeySource>(core::net::EventLoop&)> _keySourceProvider;
};

} // namespace vthost
