// SPDX-License-Identifier: Apache-2.0
#include <core/Utf8Path.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/testing/InMemoryTransport.hpp>
#include <core/net/testing/SocketDecorator.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <expected>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <vthost/SocketPath.hpp>
#include <vthost/Transport.hpp>

using vthost::retryOf;
using vthost::TransportErrorKind;
using vthost::TransportRetry;

namespace
{

/// A socket that records its own destruction.
class RecordingSocket final: public core::net::testing::SocketDecorator
{
  public:
    RecordingSocket(core::net::ISocket& inner, std::vector<std::string>& events):
        SocketDecorator(inner), _events(events)
    {
    }
    ~RecordingSocket() override { _events.emplace_back("socket"); }

    RecordingSocket(RecordingSocket const&) = delete;
    RecordingSocket& operator=(RecordingSocket const&) = delete;
    RecordingSocket(RecordingSocket&&) = delete;
    RecordingSocket& operator=(RecordingSocket&&) = delete;

  private:
    std::vector<std::string>& _events;
};

} // namespace

TEST_CASE("only unreachable and protocol errors are retried", "[vthost][transport]")
{
    static_assert(retryOf(TransportErrorKind::Unreachable) == TransportRetry::Transient);
    static_assert(retryOf(TransportErrorKind::Protocol) == TransportRetry::Transient);
    static_assert(retryOf(TransportErrorKind::HostKeyUnknown) == TransportRetry::Permanent);
    static_assert(retryOf(TransportErrorKind::Refused) == TransportRetry::Permanent);
    static_assert(retryOf(TransportErrorKind::AuthenticationRequired) == TransportRetry::Permanent);
    static_assert(retryOf(TransportErrorKind::Cancelled) == TransportRetry::Permanent);
    // A daemon that went away comes back (C2): an absent one is retried too.
    static_assert(retryOf(TransportErrorKind::DaemonAbsent) == TransportRetry::Transient);
    static_assert(retryOf(TransportErrorKind::Misconfigured) == TransportRetry::Permanent);
}

TEST_CASE("a connection releases its lifetime only after its socket", "[vthost][transport]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto pair = *core::net::testing::makeSocketPair(loop);
    auto events = std::vector<std::string> {};
    {
        auto const connection = vthost::Connection {
            .lifetime =
                std::shared_ptr<void> { nullptr, [&events](void*) { events.emplace_back("lifetime"); } },
            .socket = std::make_unique<RecordingSocket>(*pair.second, events),
            .token = {},
        };
    }
    CHECK(events == std::vector<std::string> { "socket", "lifetime" });
}

// [afunix]: skips where AF_UNIX is missing. @see the tag's explanation in net/Socket_test.cpp.
TEST_CASE("the unix factory ensures the daemon before every attempt", "[vthost][transport][afunix]")
{
    auto const tmp = core::testing::ScopedTempDir { "contour-transport" };
    auto const controlSocket = tmp.path() / "sockets" / "control";
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    // Stands in for a serving daemon: reaching the listen backlog is a connected socket.
    auto listener = core::net::listenUnix(loop, core::pathToUtf8(vthost::nativeSocketPath(controlSocket)));
    if (!listener.has_value())
    {
        INFO("listenUnix failed: " << listener.error().toString());
        REQUIRE(listener.error().code == core::net::NetErrorCode::Unsupported);
        SKIP("AF_UNIX not supported on this platform");
    }

    auto ensured = 0;
    auto factory = vthost::UnixTransportFactory { controlSocket, [&ensured] {
                                                     ++ensured;
                                                     return std::expected<void, vthost::TransportError> {};
                                                 } };
    for ([[maybe_unused]] auto const attempt: { 1, 2 })
    {
        auto connection = loop.blockOn(factory.connect(loop));
        INFO((connection ? std::string {} : connection.error().message));
        REQUIRE(connection.has_value());
        CHECK((*connection)->token.empty());
    }
    CHECK(ensured == 2);
    CHECK(factory.describe().starts_with("unix:"));
}

TEST_CASE("the unix factory does not connect when the daemon cannot be ensured", "[vthost][transport]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto factory = vthost::UnixTransportFactory {
        "/nonexistent/contour/control",
        [] {
            return std::expected<void, vthost::TransportError> { std::unexpected { vthost::TransportError {
                .kind = TransportErrorKind::ProgramNotFound, .message = "no contour binary" } } };
        }
    };
    auto const connection = loop.blockOn(factory.connect(loop));
    REQUIRE_FALSE(connection.has_value());
    CHECK(connection.error().kind == TransportErrorKind::ProgramNotFound);
    CHECK(connection.error().message == "no contour binary");
}

TEST_CASE("the unix factory contains a daemon start that throws a non-std type", "[vthost][transport]")
{
    // An exception leaving the worker thread would be std::terminate.
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto factory =
        vthost::UnixTransportFactory { "/nonexistent/contour/control",
                                       []() -> std::expected<void, vthost::TransportError> { throw 42; } };
    auto const connection = loop.blockOn(factory.connect(loop));
    REQUIRE_FALSE(connection.has_value());
    CHECK(connection.error().kind == TransportErrorKind::Unreachable);
    CHECK(connection.error().message == "the daemon start threw");
}

TEST_CASE("the unix factory starts the daemon off the loop's thread", "[vthost][transport][threading]")
{
    // The start may wait seconds for a spawned daemon; the loop it would block is a client's reactor.
    using namespace std::chrono_literals;
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto const loopThread = std::this_thread::get_id();
    auto ensuredOn = std::thread::id {};
    auto loopTurned = false;
    auto turned = std::promise<void> {};
    auto factory = vthost::UnixTransportFactory {
        "/nonexistent/contour/control",
        [&] {
            ensuredOn = std::this_thread::get_id();
            // Blocks until the loop runs what it is posted, which it could not if this ran on it.
            loop.post([&turned] { turned.set_value(); });
            loopTurned = turned.get_future().wait_for(5s) == std::future_status::ready;
            return std::expected<void, vthost::TransportError> { std::unexpected { vthost::TransportError {
                .kind = TransportErrorKind::ProgramNotFound, .message = "no contour binary" } } };
        }
    };
    auto const connection = loop.blockOn(factory.connect(loop));

    CHECK(ensuredOn != loopThread);
    CHECK(loopTurned);
    REQUIRE_FALSE(connection.has_value());
    CHECK(connection.error().kind == TransportErrorKind::ProgramNotFound);
    CHECK(connection.error().message == "no contour binary");
}

TEST_CASE("a unix socket nobody serves reads as an absent daemon", "[vthost][transport][afunix]")
{
    auto const tmp = core::testing::ScopedTempDir { "contour-transport" };
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto factory = vthost::UnixTransportFactory { tmp.path() / "nobody", nullptr };
    auto const connection = loop.blockOn(factory.connect(loop));
    REQUIRE_FALSE(connection.has_value());
    CHECK(connection.error().kind == TransportErrorKind::DaemonAbsent);
}

TEST_CASE("a CA the TLS factory cannot use is not retried", "[vthost][transport]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto listener = core::net::listen(loop, "127.0.0.1", 0);
    REQUIRE(listener.has_value());
    auto factory = vthost::TlsTransportFactory { vthost::TcpEndpoint {
        .host = "127.0.0.1", .port = (*listener)->boundPort(), .token = {}, .caPem = "not a certificate" } };
    auto const connection = loop.blockOn(factory.connect(loop));
    REQUIRE_FALSE(connection.has_value());
    CHECK(connection.error().kind == TransportErrorKind::Misconfigured);
    CHECK(retryOf(connection.error().kind) == TransportRetry::Permanent);
}

TEST_CASE("the TLS factory names its endpoint", "[vthost][transport]")
{
    auto const factory = vthost::TlsTransportFactory { vthost::TcpEndpoint {
        .host = "127.0.0.1", .port = 4711, .token = "secret", .caPem = {} } };
    CHECK(factory.describe() == "tls://127.0.0.1:4711");
}

TEST_CASE("transports report their peer trust", "[vthost][transport][grants]")
{
    CHECK(vthost::UnixTransportFactory { "/run/contour/socket", {} }.peerTrust() == vthost::PeerTrust::Local);
    CHECK(vthost::TlsTransportFactory {
              vthost::TcpEndpoint { .host = "h", .port = 7297, .token = "t", .caPem = {} } }
              .peerTrust()
          == vthost::PeerTrust::Unverified);
    CHECK(vthost::TlsTransportFactory {
              vthost::TcpEndpoint {
                  .host = "h", .port = 7297, .token = "t", .caPem = "-----BEGIN CERTIFICATE-----" } }
              .peerTrust()
          == vthost::PeerTrust::Verified);
}
