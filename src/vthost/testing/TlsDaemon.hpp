// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A TLS listener on loopback for tests of what dials a daemon: it completes the handshakes it is given and
/// keeps the connections open, presenting a self-signed identity whose fingerprint it can state.

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/Tls.hpp>
#include <core/net/WithTimeout.hpp>

#include <chrono>
#include <cstddef>
#include <format>
#include <future>
#include <memory>
#include <ranges>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <vthost/DaemonIdentity.hpp>
#include <vthost/DaemonSpawn.hpp>
#include <vthost/handover/HandoverImage.hpp>

namespace vthost::testing
{

/// A TLS listener on loopback that completes the handshakes it is given and keeps the connections open.
struct TlsDaemon
{
    explicit TlsDaemon(core::net::EventLoop& loop):
        listener(*core::net::listen(loop, "127.0.0.1", 0)),
        identity(*core::net::generateSelfSignedCertificate(
            { .commonName = "contour-daemon", .subjectNames = { "127.0.0.1" } })),
        context(*core::net::makeTlsServerContext(identity.certPem, identity.keyPem))
    {
    }

    /// Accepts @p count connections and finishes their TLS handshakes.
    core::async::Task<void> serve(core::net::EventLoop* loop, int count)
    {
        for ([[maybe_unused]] auto const _: std::views::iota(0, count))
        {
            auto accepted = co_await listener->accept();
            if (!accepted)
                co_return;
            auto socket = context->wrap(std::move(*accepted), *loop);
            std::ignore = co_await socket->handshakeIfNeeded();
            open.push_back(std::move(socket));
        }
    }

    /// Accepts one connection, finishes its handshake and reads what the client sends until it closes.
    /// @param bytes Set to how many application bytes arrived.
    core::async::Task<void> serveCounting(core::net::EventLoop* loop, std::size_t* bytes)
    {
        auto accepted = co_await listener->accept();
        if (!accepted)
            co_return;
        auto socket = context->wrap(std::move(*accepted), *loop);
        std::ignore = co_await socket->handshakeIfNeeded();
        auto buffer = std::vector<std::byte>(1024);
        while (true)
        {
            auto const count = co_await socket->read(buffer);
            if (!count || *count == 0)
                co_return;
            *bytes += *count;
        }
    }

    /// @return The daemon's fingerprint as `contour daemon status` prints it.
    [[nodiscard]] std::string fingerprint() const
    {
        return vthost::daemonFingerprint(
                   vthost::handover::TlsIdentity { .certPem = identity.certPem, .keyPem = identity.keyPem })
            .value();
    }

    [[nodiscard]] std::string hostPort() const { return std::format("127.0.0.1:{}", listener->boundPort()); }

    [[nodiscard]] vthost::TcpEndpoint endpoint() const
    {
        return vthost::TcpEndpoint { .host = "127.0.0.1", .port = listener->boundPort(), .token = "tok" };
    }

    std::unique_ptr<core::net::IListener> listener;
    core::net::CertKeyPem identity;
    std::shared_ptr<core::net::ITlsContext> context;
    std::vector<std::unique_ptr<core::net::ISocket>> open;
};

/// A @ref TlsDaemon on a loop of its own thread, accepting a given number of connections, as a daemon runs
/// beside the client a test drives.
class TlsDaemonThread
{
  public:
    /// What the daemon states about itself, read by the thread that dials it.
    struct Published
    {
        vthost::TcpEndpoint endpoint;
        std::string hostPort;
        std::string fingerprint;
    };

    /// @param accepts How many connections it accepts and handshakes.
    explicit TlsDaemonThread(int accepts):
        _thread { [this, accepts] {
            auto const backend = core::net::makeDefaultBackend();
            auto loop = core::net::EventLoop { *backend };
            auto daemon = TlsDaemon { loop };
            _published.set_value(Published { .endpoint = daemon.endpoint(),
                                             .hostPort = daemon.hostPort(),
                                             .fingerprint = daemon.fingerprint() });
            std::ignore = loop.blockOn(
                core::net::withTimeout(&loop, daemon.serve(&loop, accepts), std::chrono::seconds { 10 }));
        } }
    {
        _daemon = _published.get_future().get();
    }
    ~TlsDaemonThread() { _thread.join(); }

    TlsDaemonThread(TlsDaemonThread const&) = delete;
    TlsDaemonThread& operator=(TlsDaemonThread const&) = delete;
    TlsDaemonThread(TlsDaemonThread&&) = delete;
    TlsDaemonThread& operator=(TlsDaemonThread&&) = delete;

    /// @return What the daemon states about itself.
    [[nodiscard]] Published const& daemon() const noexcept { return _daemon; }

  private:
    std::promise<Published> _published;
    Published _daemon;
    std::thread _thread;
};

} // namespace vthost::testing
