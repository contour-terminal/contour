// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

/// Like @ref echoServer, but keeps accepting until one connection carries a real request.
/// A connection that closes without sending — a bind-time liveness probe is exactly that —
/// is drained and ignored rather than mistaken for the request under test.
Task<void> echoOnceDraining(net::IListener* listener, bool* served)
{
    while (!*served)
    {
        auto accepted = co_await listener->accept();
        if (!accepted.has_value())
            co_return;
        auto conn = std::move(*accepted);

        auto buffer = std::array<std::byte, 64> {};
        auto const got = co_await conn->read(buffer);
        if (!got.has_value() || *got == 0)
            continue; // a dropped/empty connection: keep waiting for a real request
        auto const echoed = co_await conn->write(std::span<std::byte const> { buffer }.subspan(0, *got));
        *served = echoed.has_value() && *echoed == *got;
    }
}

/// The client flow for a unix socket: connect to @p path, send a request, read the echo back.
Task<void> unixProbe(EventLoop* loop, std::string path, bool* matched)
{
    auto connected = co_await net::connectUnix(loop, path);
    if (!connected.has_value())
        co_return;
    auto sock = std::move(*connected);

    auto wroteOk = false;
    co_await writeAll(sock.get(), "probe", &wroteOk);
    if (!wroteOk)
        co_return;
    co_await expectRead(sock.get(), "probe", matched);
}

/// Whether the socket FILE at @p path is present.
///
/// Deliberately NOT std::filesystem::exists: a bound AF_UNIX socket on Windows is a reparse
/// point that exists() tries to follow and cannot, so it THROWS ERROR_CANT_ACCESS_FILE on
/// exactly the files this asks about. Reading the parent directory's entries answers from the
/// name alone, which is why WindowsListener::probeUnixSocketOwner reaches for FindFirstFileA
/// rather than a stat too.
[[nodiscard]] bool socketFileExists(std::filesystem::path const& path)
{
    auto ec = std::error_code {};
    for (auto const& entry: std::filesystem::directory_iterator { path.parent_path(), ec })
        if (entry.path().filename() == path.filename())
            return true;
    return false;
}

/// A unique, empty directory under the system temp dir for one test's socket files.
[[nodiscard]] std::filesystem::path makeSocketDir()
{
    return std::filesystem::temp_directory_path() / std::format("contour-net-{}", std::random_device {}());
}

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

// The [afunix] tag names the cases that SKIP where AF_UNIX is missing. Windows CI asserts this
// subset reports no skips: `windows-latest` is far past the 1803 that introduced AF_UNIX, so a
// skip there means the daemon's transport silently stopped being tested, not that the platform
// lacks it. Same reasoning as the [oracle] tag on the tmux interop tests.
TEST_CASE("unix-domain listen + connect echo a request", "[net][poll][afunix]")
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

TEST_CASE("closing a unix listener removes its socket file", "[net][poll][afunix]")
{
    // The daemon's contract (docs/internals/vthost.md, "Daemon lifetime"): a closed listener
    // leaves no path behind, so the next start's liveness probe finds nothing and binds fresh
    // rather than reclaiming a corpse. Windows used to keep the file — WindowsListener held no
    // path at all — which is what this pins.
    auto source = PollEventSource {};
    auto loop = EventLoop { source };

    auto const dir = makeSocketDir();
    auto const path = (dir / "closing.sock").string();
    auto listener = net::listenUnix(loop, path);
    if (!listener.has_value())
    {
        REQUIRE(listener.error().code == net::NetErrorCode::Unsupported);
        SKIP("AF_UNIX not supported on this platform");
    }
    REQUIRE(socketFileExists(path));

    (*listener)->close();
    REQUIRE_FALSE(socketFileExists(path));

    auto ec = std::error_code {};
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("a live server on the path is not hijacked", "[net][poll][afunix]")
{
    // A second bind must be REFUSED rather than unlink the live socket out from under the
    // incumbent, which would keep all its sessions but become unreachable forever. The refusal
    // is what a second `contour daemon` on one label hits.
    //
    // Cross-platform on purpose: the POSIX twin in UnixSocket_test.cpp cannot run here, and the
    // Windows probe (WindowsListener::probeUnixSocketOwner) had no coverage at all.
    auto source = PollEventSource {};
    auto loop = EventLoop { source };

    auto const dir = makeSocketDir();
    auto const path = (dir / "default").string();
    auto first = net::listenUnix(loop, path);
    if (!first.has_value())
    {
        REQUIRE(first.error().code == net::NetErrorCode::Unsupported);
        SKIP("AF_UNIX not supported on this platform");
    }

    auto second = net::listenUnix(loop, path);
    REQUIRE_FALSE(second.has_value());
    REQUIRE(second.error().code == net::NetErrorCode::AddressInUse);
    REQUIRE(socketFileExists(path)); // the incumbent's file, left intact

    // The incumbent still serves: a client connects and gets its probe echoed back. The refused
    // bind's own liveness probe left a dropped connection queued ahead of it, which is why the
    // server flow here drains rather than treating the first accept as the request.
    auto served = false;
    auto matched = false;
    auto run = [](net::IListener* listener, EventLoop* lp, std::string p, bool* s, bool* m) -> Task<void> {
        co_await coro::whenAll(echoOnceDraining(listener, s), unixProbe(lp, std::move(p), m));
    };
    loop.blockOn(run(first->get(), &loop, path, &served, &matched));

    REQUIRE(served);
    REQUIRE(matched);

    auto ec = std::error_code {};
    std::filesystem::remove_all(dir, ec);
}

namespace
{

/// Writes @p total bytes over @p sock, retrying past every partial write.
Task<void> writeBulk(ISocket* sock, std::size_t total, bool* ok)
{
    auto const chunk = std::vector<std::byte>(std::size_t { 64 } * 1024, std::byte { 0x5A });
    auto sent = std::size_t { 0 };
    while (sent < total)
    {
        auto const take = std::min(chunk.size(), total - sent);
        auto const n = co_await sock->write(std::span<std::byte const> { chunk.data(), take });
        if (!n)
            co_return;
        sent += *n;
    }
    *ok = sent == total;
}

/// Reads until @p total bytes have arrived (or the peer hangs up).
Task<void> readBulk(ISocket* sock, std::size_t total, std::size_t* got)
{
    auto buffer = std::vector<std::byte>(std::size_t { 64 } * 1024);
    while (*got < total)
    {
        auto const n = co_await sock->read(buffer);
        if (!n || *n == 0)
            co_return;
        *got += *n;
    }
}

/// One connection carrying a large payload in BOTH directions at once, from concurrent read and
/// write coroutines on the same socket — the shape an attached client runs (a read pump plus
/// WriteQueue::drain) and the only one that puts both directions under backpressure together.
Task<void> duplexBulk(EventLoop* loop,
                      net::IListener* listener,
                      std::size_t payload,
                      std::size_t* serverGot,
                      std::size_t* clientGot,
                      bool* serverSent,
                      bool* clientSent)
{
    auto server = [](net::IListener* l, std::size_t bytes, std::size_t* got, bool* sent) -> Task<void> {
        auto accepted = co_await l->accept();
        if (!accepted)
            co_return;
        auto conn = std::move(*accepted);
        co_await coro::whenAll(writeBulk(conn.get(), bytes, sent), readBulk(conn.get(), bytes, got));
    };
    auto client = [](EventLoop* innerLoop, uint16_t port, std::size_t bytes, std::size_t* got, bool* sent)
        -> Task<void> {
        auto connected = co_await net::connect(innerLoop, "127.0.0.1", port);
        if (!connected)
            co_return;
        auto sock = std::move(*connected);
        co_await coro::whenAll(writeBulk(sock.get(), bytes, sent), readBulk(sock.get(), bytes, got));
    };
    co_await coro::whenAll(server(listener, payload, serverGot, serverSent),
                           client(loop, listener->localPort(), payload, clientGot, clientSent));
}

} // namespace

// A reader and a writer sharing ONE socket, both under backpressure. On Windows the two directions
// share a single WSAEVENT (WSAEventSelect permits no more), and the reader used to WSAResetEvent it
// before every recv — throwing away the FD_WRITE raised for a parked writer, which Winsock does not
// repeat until another send returns WSAEWOULDBLOCK. The write queue then stalled for good and the
// attached window froze. Indications are latched per direction now, so neither side can destroy the
// other's wake-up.
//
// A regression does not fail here — it HANGS, and the suite's per-test timeout reports it. That is
// the nature of a lost wake-up, and matches how the TLS deadlock case is covered.
TEST_CASE("a concurrent reader and writer on one socket both make progress", "[net][poll]")
{
    auto source = PollEventSource {};
    auto loop = EventLoop { source };

    auto listener = net::listen(loop, "127.0.0.1", 0);
    REQUIRE(listener.has_value());

    // Comfortably past any socket send buffer, so both directions really do block.
    constexpr auto Payload = std::size_t { 4 } * 1024 * 1024;
    auto serverGot = std::size_t { 0 };
    auto clientGot = std::size_t { 0 };
    auto serverSent = false;
    auto clientSent = false;
    loop.blockOn(
        duplexBulk(&loop, listener->get(), Payload, &serverGot, &clientGot, &serverSent, &clientSent));

    CHECK(serverSent);
    CHECK(clientSent);
    CHECK(serverGot == Payload);
    CHECK(clientGot == Payload);
}
