// SPDX-License-Identifier: Apache-2.0
#include <vthost/Transport.hpp>

#include <core/Utf8Path.hpp>
#include <core/async/AsyncQueue.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/NetError.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/Tls.hpp>
#include <core/net/WithTimeout.hpp>

#include <atomic>
#include <cerrno>
#include <exception>
#include <format>
#include <tuple>
#include <utility>

#include <vthost/Logging.hpp>
#include <vthost/SocketPath.hpp>

namespace vthost
{

namespace
{
    /// @param error What a connect failed with.
    /// @return DaemonAbsent when it says nothing listens -- a refused connection, or a socket file that is
    ///         gone -- and Unreachable otherwise, decided while the typed error is in hand.
    [[nodiscard]] TransportError classifyConnectFailure(core::net::NetError const& error)
    {
        // ENOENT is ERROR_FILE_NOT_FOUND's number too, and WSAECONNREFUSED arrives as ConnRefused.
        auto const absent = error.code == core::net::NetErrorCode::ConnRefused || error.systemCode == ENOENT;
        return TransportError { .kind = absent ? TransportErrorKind::DaemonAbsent
                                               : TransportErrorKind::Unreachable,
                                .message = error.toString() };
    }

    /// @param error What a connect to a Unix socket failed with.
    /// @return As classifyConnectFailure(), and DaemonAbsent too for Windows' answer to a socket path that is
    ///         not there: WSAENETDOWN, which no AF_UNIX connect can otherwise mean, there being no network.
    [[nodiscard]] TransportError classifyUnixConnectFailure(core::net::NetError const& error)
    {
#ifdef _WIN32
        constexpr auto WindowsNetworkDown = 10050; // WSAENETDOWN
        if (error.systemCode == WindowsNetworkDown)
            return TransportError { .kind = TransportErrorKind::DaemonAbsent, .message = error.toString() };
#endif
        return classifyConnectFailure(error);
    }
} // namespace

struct UnixTransportFactory::EnsureRun
{
    /// @param loop Where a waiter that parked outside every executor's task is resumed.
    explicit EnsureRun(core::async::IExecutor& loop): result(loop, core::async::AsyncQueueOptions {}) {}

    /// The worker pushes the one result; the attempt pops it, or stops waiting when it is cancelled.
    core::async::AsyncQueue<std::expected<void, TransportError>> result;
    std::atomic<bool> finished = false; ///< Set by the worker as its last act: it may be joined at once.
};

UnixTransportFactory::UnixTransportFactory(std::filesystem::path socketPath, EnsureDaemon ensureDaemon):
    _socketPath(std::move(socketPath)), _ensureDaemon(std::move(ensureDaemon))
{
}

UnixTransportFactory::~UnixTransportFactory() = default;

core::async::Task<std::expected<void, TransportError>> UnixTransportFactory::ensureOffLoop(
    core::net::EventLoop* loop)
{
    auto run = std::make_shared<EnsureRun>(*loop);
    {
        auto const lock = std::lock_guard { _workersMutex };
        // The workers that finished are joined here, so no more threads are kept than starts are under way.
        std::erase_if(_workers, [](EnsureWorker const& worker) { return worker.run->finished.load(); });
        _workers.push_back(EnsureWorker {
            .run = run,
            .thread = std::jthread { [this, run] {
                auto result = std::expected<void, TransportError> {};
                try
                {
                    auto const lock = std::lock_guard { _ensureMutex };
                    result = _ensureDaemon();
                }
                catch (std::exception const& error)
                {
                    result = std::unexpected(
                        TransportError { .kind = TransportErrorKind::Unreachable, .message = error.what() });
                }
                catch (...)
                {
                    // Nothing may leave a jthread: an escaped exception is std::terminate.
                    result = std::unexpected(TransportError { .kind = TransportErrorKind::Unreachable,
                                                              .message = "the daemon start threw" });
                }
                // Into a queue nobody waits on any more, if the attempt was cancelled meanwhile.
                std::ignore = run->result.push(std::move(result));
                run->finished = true;
            } },
        });
    }
    // Parked on the loop, which goes on serving everything else; the attempt's cancellation ends the wait.
    auto result = co_await run->result.pop();
    if (!result)
        co_return std::unexpected(TransportError { .kind = TransportErrorKind::Cancelled,
                                                   .message = "the daemon start was abandoned" });
    co_return std::move(*result);
}

core::async::Task<std::expected<std::unique_ptr<Connection>, TransportError>> UnixTransportFactory::connect(
    core::net::EventLoop& loop)
{
    return connectOn(&loop);
}

core::async::Task<std::expected<std::unique_ptr<Connection>, TransportError>> UnixTransportFactory::connectOn(
    core::net::EventLoop* loop)
{
    // Held back while a daemon that announced its upgrade comes back: it is waited for, never doubled.
    auto const held = loop->clock().now().time_since_epoch().count() < _holdUntil.load();
    if (_ensureDaemon && !held)
        if (auto ensured = co_await ensureOffLoop(loop); !ensured)
            co_return std::unexpected(std::move(ensured.error()));
    auto socket = co_await core::net::connectUnix(loop, core::pathToUtf8(nativeSocketPath(_socketPath)));
    if (!socket)
        co_return std::unexpected(classifyUnixConnectFailure(socket.error()));
    co_return std::make_unique<Connection>(
        Connection { .lifetime = {}, .socket = std::move(*socket), .token = {} });
}

void UnixTransportFactory::holdDaemonStart(core::platform::SteadyTimePoint until)
{
    _holdUntil = until.time_since_epoch().count();
}

std::string UnixTransportFactory::describe() const
{
    return std::format("unix:{}", core::pathToUtf8(_socketPath));
}

TlsTransportFactory::TlsTransportFactory(
    TcpEndpoint endpoint,
    KnownDaemons& knownDaemons,
    std::shared_ptr<IPinPrompt> pinPrompt,
    std::function<std::shared_ptr<client::IClientKeySource>(core::net::EventLoop&)> keySourceProvider):
    _endpoint(std::move(endpoint)),
    _knownDaemons(knownDaemons),
    _pinPrompt(std::move(pinPrompt)),
    _keySourceProvider(std::move(keySourceProvider))
{
}

std::string TlsTransportFactory::hostPort() const
{
    return pinKey(_endpoint.host, _endpoint.port);
}

core::async::Task<std::expected<std::unique_ptr<Connection>, TransportError>> TlsTransportFactory::connect(
    core::net::EventLoop& loop)
{
    return connectOn(&loop);
}

core::async::Task<std::expected<TlsTransportFactory::TrustBasis, TransportError>> TlsTransportFactory::
    decideTrust(std::string fingerprint, std::string trusted)
{
    auto const where = hostPort();
    // What the user was told out of band decides first: a pin that happens to match must not stand in for it,
    // nor one that differs be taken as the user's word.
    if (!_endpoint.expectedFingerprint.empty() && _endpoint.expectedFingerprint != fingerprint)
        co_return std::unexpected(TransportError {
            .kind = TransportErrorKind::Refused,
            .message = std::format("The daemon at {} presents {}, not the {} that was expected.",
                                   where,
                                   fingerprint,
                                   _endpoint.expectedFingerprint) });
    auto const changed = [&](std::string const& expected) {
        // No answer of the user's makes this safe: it is a reinstalled daemon or someone in between, and
        // only the user's own `forget-pin` tells the two apart.
        return std::unexpected(TransportError {
            .kind = TransportErrorKind::HostKeyChanged,
            .message = std::format("The daemon at {0} presents {1}, but {2} is pinned. If that daemon was "
                                   "reinstalled, run `contour daemon forget-pin {0}` and connect again.",
                                   where,
                                   fingerprint,
                                   expected) });
    };
    // A store that cannot be read is refused outright, never taken for "no pin": that would ask the user
    // about every daemon, a changed one too, and teach them to say yes.
    auto const unusable = [&](std::string const& why) {
        return std::unexpected(
            TransportError { .kind = TransportErrorKind::Misconfigured,
                             .message = std::format("The pinned daemons cannot be checked: {}. Repair or "
                                                    "remove the file and connect again.",
                                                    why) });
    };
    auto const pinned = _knownDaemons.pinOf(where);
    if (!pinned)
        co_return unusable(pinned.error());
    if (*pinned)
    {
        if (**pinned == fingerprint)
            co_return TrustBasis::Known;
        co_return changed(**pinned);
    }
    // Trusted a moment ago, by this very connect, and since forgotten: the same daemon is still the one
    // trusted.
    if (!trusted.empty())
    {
        if (trusted == fingerprint)
            co_return TrustBasis::Known;
        co_return changed(trusted);
    }
    // What the store says when it cannot keep a pin names the file.
    auto const pin = [&]() {
        return _knownDaemons.pin(where, fingerprint);
    };
    if (!_endpoint.expectedFingerprint.empty())
    {
        // The user's own word verified this daemon, as ssh's does a host it was told: not keeping it means
        // only that the word is needed again next time.
        if (auto const kept = pin(); !kept)
            // On the error category, which is on by default: the user is to see that the word is not kept.
            errorLog()("warning: the daemon at {} is not remembered: {}", where, kept.error());
        co_return TrustBasis::Known;
    }
    if (!_pinPrompt)
        co_return std::unexpected(TransportError {
            .kind = TransportErrorKind::HostKeyUnknown,
            .message = std::format("The daemon at {} presents {} and is not known yet: pass "
                                   "--daemon-fingerprint SHA256:\u2026; `contour daemon status` on that "
                                   "machine prints it.",
                                   where,
                                   fingerprint) });
    if (co_await _pinPrompt->confirm(where, fingerprint) != PinDecision::Trust)
        co_return std::unexpected(
            TransportError { .kind = TransportErrorKind::Cancelled,
                             .message = std::format("the daemon at {} was not trusted", where) });
    // A daemon the user was asked about must be kept, or the next connect asks again: refused, with why.
    if (auto const kept = pin(); !kept)
        co_return std::unexpected(TransportError {
            .kind = TransportErrorKind::Misconfigured,
            .message = std::format("The daemon at {} was trusted but cannot be remembered: {}. Make '{}' "
                                   "writable and connect again.",
                                   where,
                                   kept.error(),
                                   core::pathToUtf8(_knownDaemons.file().parent_path())) });
    co_return TrustBasis::Prompted;
}

core::async::Task<std::expected<std::unique_ptr<core::net::ISocket>, TransportError>> TlsTransportFactory::
    handshaken(core::net::EventLoop* loop)
{
    auto socket = co_await core::net::connect(loop, _endpoint.host, _endpoint.port);
    if (!socket)
        co_return std::unexpected(classifyConnectFailure(socket.error()));
    // The host we asked for is also the name the certificate must carry: a pinned CA proves who signed,
    // not who was signed for.
    // A context that cannot be made is this end's configuration (the pinned CA), not the network: retrying
    // cannot help. The handshake's own setup failing below may pass.
    auto tls = core::net::makeTlsClientContext(_endpoint.caPem, _endpoint.host);
    if (!tls)
        co_return std::unexpected(
            TransportError { .kind = TransportErrorKind::Misconfigured, .message = tls.error() });
    auto encrypted = (*tls)->wrap(std::move(*socket), *loop);
    if (!encrypted)
        co_return std::unexpected(TransportError { .kind = TransportErrorKind::Unreachable,
                                                   .message = "TLS handshake setup failed" });
    // Eagerly, so that who answered is known before a token or a signature goes anywhere.
    if (auto const done = co_await encrypted->handshakeIfNeeded(); !done)
        co_return std::unexpected(TransportError {
            .kind = TransportErrorKind::Unreachable,
            .message =
                std::format("the TLS handshake with {} failed: {}", hostPort(), done.error().toString()) });
    co_return std::move(encrypted);
}

core::async::Task<TlsTransportFactory::Dialled> TlsTransportFactory::dial(core::net::EventLoop* loop,
                                                                          std::string trusted)
{
    // Bounded here, and only here: what the user is asked afterwards must not run against a clock that
    // cannot see the question.
    auto bounded = co_await core::net::withTimeout(loop, handshaken(loop), _endpoint.dialTimeout);
    if (!bounded)
        co_return Dialled { .connection = std::unexpected(TransportError {
                                .kind = TransportErrorKind::Unreachable,
                                .message = std::format("the daemon at {} did not answer within {}",
                                                       hostPort(),
                                                       _endpoint.dialTimeout) }) };
    if (!*bounded)
        co_return Dialled { .connection = std::unexpected(std::move(bounded->error())) };
    auto encrypted = std::move(**bounded);
    auto basis = TrustBasis::Known;
    auto fingerprint = std::string {};
    if (_endpoint.caPem.empty())
    {
        auto const* const session = core::net::tlsSessionOf(*encrypted);
        auto const digest = session != nullptr ? session->peerCertificateDigest() : std::nullopt;
        if (!digest)
            co_return Dialled { .connection = std::unexpected(
                                    TransportError { .kind = TransportErrorKind::Protocol,
                                                     .message = "the daemon presented no certificate" }) };
        fingerprint = core::net::sshStyleFingerprint(*digest);
        auto const decided = co_await decideTrust(fingerprint, std::move(trusted));
        if (!decided)
            co_return Dialled { .connection = std::unexpected(decided.error()) };
        basis = *decided;
    }
    co_return Dialled { .connection = std::make_unique<Connection>(Connection {
                            .lifetime = {},
                            .socket = std::move(encrypted),
                            .token = _endpoint.token,
                            .keySource = _keySourceProvider ? _keySourceProvider(*loop) : nullptr }),
                        .promptedFingerprint =
                            basis == TrustBasis::Prompted ? std::move(fingerprint) : std::string {} };
}

core::async::Task<std::expected<std::unique_ptr<Connection>, TransportError>> TlsTransportFactory::connectOn(
    core::net::EventLoop* loop)
{
    auto first = co_await dial(loop, {});
    if (first.promptedFingerprint.empty())
        co_return std::move(first.connection);
    // The user was asked and said yes, and the connection that waited for the answer is not to be relied on:
    // the daemon closes one that does not sign in soon enough. Drop it, and dial the now trusted daemon.
    (*first.connection).reset();
    auto second = co_await dial(loop, std::move(first.promptedFingerprint));
    co_return std::move(second.connection);
}

std::string TlsTransportFactory::describe() const
{
    return std::format("tls://{}", hostPort());
}

} // namespace vthost
