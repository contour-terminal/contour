// SPDX-License-Identifier: Apache-2.0
#include <vthost/Transport.hpp>

#include <core/Utf8Path.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/NetError.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/Tls.hpp>

#include <cerrno>
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
} // namespace

UnixTransportFactory::UnixTransportFactory(std::filesystem::path socketPath, EnsureDaemon ensureDaemon):
    _socketPath(std::move(socketPath)), _ensureDaemon(std::move(ensureDaemon))
{
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
        if (auto ensured = _ensureDaemon(); !ensured)
            co_return std::unexpected(std::move(ensured.error()));
    auto socket = co_await core::net::connectUnix(loop, core::pathToUtf8(nativeSocketPath(_socketPath)));
    if (!socket)
        co_return std::unexpected(classifyConnectFailure(socket.error()));
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
    auto tls = core::net::makeTlsClientContext(_endpoint.caPem, _endpoint.host);
    if (!tls)
        co_return std::unexpected(
            TransportError { .kind = TransportErrorKind::Unreachable, .message = tls.error() });
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
