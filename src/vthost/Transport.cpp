// SPDX-License-Identifier: Apache-2.0
#include <vthost/Transport.hpp>

#include <core/Utf8Path.hpp>
#include <core/async/AsyncQueue.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/NetError.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/Tls.hpp>

#include <atomic>
#include <cerrno>
#include <exception>
#include <format>
#include <utility>

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
    if (_ensureDaemon)
        if (auto ensured = co_await ensureOffLoop(loop); !ensured)
            co_return std::unexpected(std::move(ensured.error()));
    auto socket = co_await core::net::connectUnix(loop, core::pathToUtf8(nativeSocketPath(_socketPath)));
    if (!socket)
        co_return std::unexpected(classifyUnixConnectFailure(socket.error()));
    co_return std::make_unique<Connection>(
        Connection { .lifetime = {}, .socket = std::move(*socket), .token = {} });
}

std::string UnixTransportFactory::describe() const
{
    return std::format("unix:{}", core::pathToUtf8(_socketPath));
}

TlsTransportFactory::TlsTransportFactory(TcpEndpoint endpoint): _endpoint(std::move(endpoint))
{
}

core::async::Task<std::expected<std::unique_ptr<Connection>, TransportError>> TlsTransportFactory::connect(
    core::net::EventLoop& loop)
{
    return connectOn(&loop);
}

core::async::Task<std::expected<std::unique_ptr<Connection>, TransportError>> TlsTransportFactory::connectOn(
    core::net::EventLoop* loop)
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
    co_return std::make_unique<Connection>(
        Connection { .lifetime = {}, .socket = std::move(encrypted), .token = _endpoint.token });
}

std::string TlsTransportFactory::describe() const
{
    return std::format("tls://{}:{}", _endpoint.host, _endpoint.port);
}

} // namespace vthost
