// SPDX-License-Identifier: Apache-2.0
//
// The options every connected stream socket carries, dialled or accepted, on every backend.
//
// An accepted socket used to get close-on-exec and nothing else, while a dialled one also got
// TCP_NODELAY: a server's replies waited on Nagle for the client's ACK, and only the client's
// requests did not. `detail::applyStreamSocketOptions` is now the one place both paths go through,
// and these cases read the answer back from the kernel rather than trusting the call was made.
//
// The buffer sizes are asked for before the connection exists -- of the listening socket, and of
// a dialled one before its connect -- because the TCP window scale is announced in the handshake.
// The listener's own sizes are read back below, which is what tells that order from asking each
// accepted socket afterwards: both leave an accepted socket with the sizes.

#include <core/async/Task.hpp>
#include <core/async/WhenAll.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IConnector.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/SocketBuffers.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/ThreadedAddressResolver.hpp>
#include <core/net/detail/StreamSocketOptions.hpp>
#include <core/net/testing/BackendMatrix.hpp>
#include <core/net/testing/RawSockets.hpp>
#include <core/platform/Types.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

using core::async::Task;
using core::net::DialOptions;
using core::net::EventLoop;
using core::net::ISocket;
using core::net::KeepAlive;
using core::net::ListenOptions;
using core::net::SocketBufferSizes;
using core::net::detail::reportStreamSocketOptions;
using core::net::testing::BackendMatrix;
using core::net::testing::nativeHandleOf;
using core::net::testing::openRawTcpSocket;
using core::net::testing::RawSocket;

namespace
{

/// Large enough to differ from every default measured (Linux 16 KiB before a connection and
/// 128 KiB receive after it, Windows 64 KiB, macOS 128 KiB) and small enough to fit under Linux's
/// default `net.core.wmem_max`/`rmem_max` of 208 KiB, which caps an unprivileged request.
constexpr auto RequestedBuffer = std::size_t { 150'000 };

/// Accepts one connection on @p listener into @p accepted.
Task<void> acceptInto(core::net::IListener* listener, std::unique_ptr<ISocket>* accepted)
{
    auto result = co_await listener->accept();
    if (result.has_value())
        *accepted = std::move(*result);
}

/// Dials @p listener's port with @p options into @p dialled, closing the listener on failure so
/// the accept beside it is not left parked.
Task<void> dialInto(EventLoop* loop,
                    core::net::IListener* listener,
                    DialOptions options,
                    std::unique_ptr<ISocket>* dialled)
{
    auto result = co_await core::net::connect(
        loop, "127.0.0.1", listener->boundPort(), &core::net::defaultAsyncResolver(), options);
    if (!result.has_value())
    {
        listener->close();
        co_return;
    }
    *dialled = std::move(*result);
}

/// The two ends of one loopback connection.
struct Connection
{
    std::unique_ptr<ISocket> accepted;
    std::unique_ptr<ISocket> dialled;
};

/// Accepts and dials at once, so neither waits on the other.
Task<void> acceptAndDial(EventLoop* loop,
                         core::net::IListener* listener,
                         DialOptions dialOptions,
                         Connection* connection)
{
    co_await core::async::whenAll(acceptInto(listener, &connection->accepted),
                                  dialInto(loop, listener, dialOptions, &connection->dialled));
}

/// Connects a client to a fresh listener on @p loop.
Connection connectOnce(EventLoop& loop, ListenOptions listenOptions, DialOptions dialOptions)
{
    auto listener = core::net::listen(loop, listenOptions);
    REQUIRE(listener.has_value());
    auto connection = Connection {};
    loop.blockOn(acceptAndDial(&loop, listener->get(), dialOptions, &connection));
    REQUIRE(connection.accepted != nullptr);
    REQUIRE(connection.dialled != nullptr);
    return connection;
}

} // namespace

TEST_CASE("The helpers set TCP_NODELAY, size only what they are asked to, and touch nothing else",
          "[net][socket-options]")
{
    auto const untouched = RawSocket { openRawTcpSocket() };
    auto const configured = RawSocket { openRawTcpSocket() };
    REQUIRE(untouched.get() != core::platform::InvalidHandle);
    REQUIRE(configured.get() != core::platform::InvalidHandle);
    auto const before = reportStreamSocketOptions(untouched.get());

    SECTION("defaults")
    {
        core::net::detail::applyStreamSocketOptions(untouched.get(), KeepAlive::No);
        core::net::detail::applySocketBufferSizes(untouched.get(), SocketBufferSizes {});
        auto const after = reportStreamSocketOptions(untouched.get());
        CHECK(after.noDelay);
        CHECK(after.sendBuffer == before.sendBuffer);
        CHECK(after.receiveBuffer == before.receiveBuffer);
    }
    SECTION("sizes")
    {
        core::net::detail::applySocketBufferSizes(
            configured.get(), SocketBufferSizes { .send = RequestedBuffer, .receive = RequestedBuffer });
        auto const after = reportStreamSocketOptions(configured.get());
        CHECK(after.sendBuffer >= RequestedBuffer);
        CHECK(after.receiveBuffer >= RequestedBuffer);
        CHECK(after.sendBuffer != before.sendBuffer);
        CHECK(after.receiveBuffer != before.receiveBuffer);
    }
}

TEST_CASE("An accepted socket carries TCP_NODELAY, as a dialled one does", "[net][socket-options]")
{
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue; // not available on this platform

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto const connection = connectOnce(loop, ListenOptions { .host = "127.0.0.1" }, DialOptions {});

            auto const accepted = nativeHandleOf(*connection.accepted);
            auto const dialled = nativeHandleOf(*connection.dialled);
            REQUIRE(accepted != core::platform::InvalidHandle);
            REQUIRE(dialled != core::platform::InvalidHandle);
            CHECK(reportStreamSocketOptions(accepted).noDelay);
            CHECK(reportStreamSocketOptions(dialled).noDelay);
        }
    }
}

TEST_CASE("Buffer sizes asked of a listener reach every socket it accepts, and a dial's reach it",
          "[net][socket-options]")
{
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue; // not available on this platform

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto const sizes = SocketBufferSizes { .send = RequestedBuffer, .receive = RequestedBuffer };
            auto const connection = connectOnce(loop,
                                                ListenOptions { .host = "127.0.0.1", .buffers = sizes },
                                                DialOptions { .buffers = sizes });

            auto const accepted = reportStreamSocketOptions(nativeHandleOf(*connection.accepted));
            auto const dialled = reportStreamSocketOptions(nativeHandleOf(*connection.dialled));
            CHECK(accepted.sendBuffer >= RequestedBuffer);
            CHECK(accepted.receiveBuffer >= RequestedBuffer);
            CHECK(dialled.sendBuffer >= RequestedBuffer);
            CHECK(dialled.receiveBuffer >= RequestedBuffer);
        }
    }
}

TEST_CASE("Buffer sizes asked of a listener are on the listening socket, before any connection",
          "[net][socket-options]")
{
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue; // not available on this platform

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto const listener = core::net::listen(
                loop,
                ListenOptions {
                    .host = "127.0.0.1",
                    .buffers = SocketBufferSizes { .send = RequestedBuffer, .receive = RequestedBuffer } });
            REQUIRE(listener.has_value());

            auto const listening = nativeHandleOf(**listener);
            REQUIRE(listening != core::platform::InvalidHandle);
            auto const report = reportStreamSocketOptions(listening);
            CHECK(report.sendBuffer >= RequestedBuffer);
            CHECK(report.receiveBuffer >= RequestedBuffer);
        }
    }
}
