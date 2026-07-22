// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>

#include <coro/Task.hpp>
#include <coro/WhenAll.hpp>
#include <net/EventLoop.h>
#include <net/IListener.h>
#include <net/ISocket.h>
#include <net/PollEventSource.h>
#include <net/Sockets.h>
#include <net/testing/InMemoryTransport.h>

using coro::Task;
using net::EventLoop;
using net::ISocket;
using net::PollEventSource;

namespace
{

/// Reads exactly @p expected.size() bytes from @p sock and checks they match.
Task<void> expectRead(ISocket* sock, std::string_view expected, bool* ok)
{
    auto buffer = std::array<std::byte, 64> {};
    std::size_t total = 0;
    while (total < expected.size())
    {
        auto const result = co_await sock->read(std::span<std::byte> { buffer }.subspan(total));
        if (!result.has_value() || *result == 0)
            break;
        total += *result;
    }
    *ok = total == expected.size() && std::memcmp(buffer.data(), expected.data(), expected.size()) == 0;
}

/// Writes @p data to @p sock.
Task<void> writeAll(ISocket* sock, std::string_view data, bool* ok)
{
    auto const bytes =
        std::span<std::byte const> { reinterpret_cast<std::byte const*>(data.data()), data.size() };
    auto const result = co_await sock->write(bytes);
    *ok = result.has_value() && *result == data.size();
}

/// Drives an InMemoryTransport round-trip: write on one end, read on the other.
Task<void> pairRoundTrip(EventLoop* loop, bool* wroteOk, bool* readOk)
{
    auto pair = net::testing::makeSocketPair(*loop);
    REQUIRE(pair.has_value());
    auto first = std::move(pair->first);
    auto second = std::move(pair->second);

    co_await coro::whenAll(writeAll(first.get(), "ping", wroteOk), expectRead(second.get(), "ping", readOk));
}

/// The server flow: accept one connection, read a request, echo it back.
Task<void> echoServer(net::IListener* listener, bool* served)
{
    auto accepted = co_await listener->accept();
    if (!accepted.has_value())
        co_return;
    auto conn = std::move(*accepted);

    auto buffer = std::array<std::byte, 64> {};
    auto const got = co_await conn->read(buffer);
    if (!got.has_value() || *got == 0)
        co_return;
    auto const echoed = co_await conn->write(std::span<std::byte const> { buffer }.subspan(0, *got));
    *served = echoed.has_value() && *echoed == *got;
}

/// The client flow: connect, send a request, read the echo, compare.
Task<void> echoClient(EventLoop* loop, std::uint16_t port, bool* matched)
{
    auto connected = co_await net::connect(loop, "127.0.0.1", port);
    if (!connected.has_value())
        co_return;
    auto sock = std::move(*connected);

    bool wroteOk = false;
    co_await writeAll(sock.get(), "hello", &wroteOk);
    if (!wroteOk)
        co_return;
    co_await expectRead(sock.get(), "hello", matched);
}

/// Runs the loopback echo: server and client flows concurrently on one loop.
Task<void> loopbackEcho(EventLoop* loop, net::IListener* listener, bool* served, bool* matched)
{
    co_await coro::whenAll(echoServer(listener, served), echoClient(loop, listener->localPort(), matched));
}

/// Parks reading an idle socket and records whether the read eventually resumed
/// with an error (rather than hanging forever).
Task<void> parkThenObserveClose(ISocket* sock, bool* resumedWithError)
{
    auto buffer = std::array<std::byte, 64> {};
    auto const result = co_await sock->read(buffer);
    *resumedWithError = !result.has_value();
}

/// Lets the reader reach its park, then closes the socket it is parked on.
Task<void> closeAfterParked(EventLoop* loop, ISocket* sock)
{
    co_await loop->delay(std::chrono::milliseconds { 20 });
    sock->close();
}

/// Runs the parked reader and the close concurrently on one loop.
Task<void> closeWhileParked(EventLoop* loop, ISocket* sock, bool* resumedWithError)
{
    co_await coro::whenAll(parkThenObserveClose(sock, resumedWithError), closeAfterParked(loop, sock));
}

} // namespace

TEST_CASE("InMemoryTransport round-trips bytes between connected endpoints", "[net]")
{
    auto source = PollEventSource {};
    auto loop = EventLoop { source };

    auto wroteOk = false;
    auto readOk = false;
    loop.blockOn(pairRoundTrip(&loop, &wroteOk, &readOk));

    REQUIRE(wroteOk);
    REQUIRE(readOk);
}

TEST_CASE("closing a socket resumes a reader parked on it instead of hanging", "[net][poll]")
{
    // A reader parked on an idle socket that is then closed under it must resume with an error, not
    // hang. On POSIX poll(2) reports POLLNVAL for the closed fd; on Windows the reactor must route the
    // now-invalid WSAEVENT the same way. Regression guard: this deadlocked on Windows before the fix,
    // taking the whole disconnect-while-parked path (attach clients, control clients) down with it.
    auto source = PollEventSource {};
    auto loop = EventLoop { source };
    auto pair = net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto resumedWithError = false;
    loop.blockOn(closeWhileParked(&loop, pair->second.get(), &resumedWithError));

    CHECK(resumedWithError); // it resumed at all (no hang) AND saw the close as an error
}

TEST_CASE("listen + connect + accept echo a request over loopback", "[net][poll]")
{
    auto source = PollEventSource {};
    auto loop = EventLoop { source };

    auto listener = net::listen(loop, "127.0.0.1", 0);
    REQUIRE(listener.has_value());
    REQUIRE((*listener)->localPort() != 0);

    auto served = false;
    auto matched = false;
    loop.blockOn(loopbackEcho(&loop, listener->get(), &served, &matched));

    REQUIRE(served);
    REQUIRE(matched);
}

namespace
{

/// The unix-socket echo: connect by PATH rather than port.
Task<void> unixEcho(EventLoop* loop, net::IListener* listener, std::string path, bool* served, bool* matched)
{
    auto client = [](EventLoop* innerLoop, std::string target, bool* ok) -> Task<void> {
        auto socket = co_await net::connectUnix(innerLoop, target);
        REQUIRE(socket.has_value());
        auto const request = std::string_view { "unix-ping" };
        std::ignore = co_await (*socket)->write(std::as_bytes(std::span { request }));
        auto buffer = std::array<std::byte, 32> {};
        auto const n = co_await (*socket)->read(buffer);
        REQUIRE(n.has_value());
        *ok = std::string_view { reinterpret_cast<char const*>(buffer.data()), *n } == "unix-ping";
    };
    co_await coro::whenAll(echoServer(listener, served), client(loop, path, matched));
}

} // namespace

TEST_CASE("unix-domain listen + connect echo a request", "[net][poll]")
{
    // Runtime-gated: on platforms without AF_UNIX support this documents the
    // Unsupported answer instead (never a crash). On Windows this is the
    // afunix.h path's coverage.
    auto source = PollEventSource {};
    auto loop = EventLoop { source };

    auto const path = (std::filesystem::temp_directory_path()
                       / std::format("contour-net-{}", std::random_device {}()) / "echo.sock")
                          .string();
    auto listener = net::listenUnix(loop, path);
    if (!listener.has_value())
    {
        REQUIRE(listener.error().code == net::NetErrorCode::Unsupported);
        SKIP("AF_UNIX not supported on this platform");
    }

    auto served = false;
    auto matched = false;
    loop.blockOn(unixEcho(&loop, listener->get(), path, &served, &matched));
    REQUIRE(served);
    REQUIRE(matched);

    auto ec = std::error_code {};
    std::filesystem::remove_all(std::filesystem::path { path }.parent_path(), ec);
}
