// SPDX-License-Identifier: Apache-2.0
#include <core/async/Task.hpp>
#include <core/async/WhenAll.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/SplitSocket.hpp>
#include <core/net/testing/BackendMatrix.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/InMemoryTransport.hpp>

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
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using core::async::Task;
using core::net::EventLoop;
using core::net::ISocket;
using core::net::testing::BackendMatrix;

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
    auto pair = core::net::testing::makeSocketPair(*loop);
    REQUIRE(pair.has_value());
    auto first = std::move(pair->first);
    auto second = std::move(pair->second);

    co_await core::async::whenAll(writeAll(first.get(), "ping", wroteOk),
                                  expectRead(second.get(), "ping", readOk));
}

/// The server flow: accept one connection, read a request, echo it back.
Task<void> echoServer(core::net::IListener* listener, bool* served)
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
///
/// Closes @p listener when it cannot connect. An arm that simply returns leaves its whenAll
/// sibling parked in accept() with nothing left to wake it, which turns the case's red into a
/// hang just as surely as a REQUIRE here would — the second shape of the same rule
/// (.agent/rules/testing.md).
Task<void> echoClient(EventLoop* loop, core::net::IListener* listener, bool* matched)
{
    auto connected = co_await core::net::connect(loop, "127.0.0.1", listener->boundPort());
    if (!connected.has_value())
    {
        listener->close();
        co_return;
    }
    auto sock = std::move(*connected);

    bool wroteOk = false;
    co_await writeAll(sock.get(), "hello", &wroteOk);
    if (!wroteOk)
        co_return; // the server already accepted; its arm sees this socket close and finishes
    co_await expectRead(sock.get(), "hello", matched);
}

/// Runs the loopback echo: server and client flows concurrently on one loop.
Task<void> loopbackEcho(EventLoop* loop, core::net::IListener* listener, bool* served, bool* matched)
{
    co_await core::async::whenAll(echoServer(listener, served), echoClient(loop, listener, matched));
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
    co_await core::async::whenAll(parkThenObserveClose(sock, resumedWithError), closeAfterParked(loop, sock));
}

/// Accepts @p count connections in sequence on one listener, counting them.
Task<void> acceptSequentially(core::net::IListener* listener, int count, int* accepted)
{
    while (*accepted < count)
    {
        auto conn = co_await listener->accept();
        if (!conn.has_value())
            co_return;
        ++*accepted;
        (*conn)->close();
    }
}

/// Connects to @p listener @p count times, one connection at a time.
Task<void> connectSequentially(EventLoop* loop, core::net::IListener* listener, int count, int* connected)
{
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, count))
    {
        auto sock = co_await core::net::connect(loop, "127.0.0.1", listener->boundPort());
        if (!sock.has_value())
        {
            listener->close(); // release the accept parked behind us rather than hang
            co_return;
        }
        ++*connected;
        (*sock)->close();
        co_await loop->delay(std::chrono::milliseconds { 20 }); // let the accept re-arm first
    }
}

/// Reads @p sock until it reports a clean EOF, recording that it did.
Task<void> readToEof(ISocket* sock, bool* sawEof)
{
    auto buffer = std::array<std::byte, 64> {};
    while (true)
    {
        auto const result = co_await sock->read(buffer);
        if (!result.has_value())
            co_return;
        if (*result == 0)
        {
            *sawEof = true;
            co_return;
        }
    }
}

} // namespace

TEST_CASE("InMemoryTransport round-trips bytes between connected endpoints", "[net]")
{
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue; // not available on this platform

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };

            auto wroteOk = false;
            auto readOk = false;
            loop.blockOn(pairRoundTrip(&loop, &wroteOk, &readOk));

            REQUIRE(wroteOk);
            REQUIRE(readOk);
        }
    }
}

TEST_CASE("closing a socket resumes a reader parked on it instead of hanging", "[net]")
{
    // A reader parked on an idle socket that is then closed under it must resume with an error, not
    // hang. poll(2) reports POLLNVAL for the closed fd and Windows reports the now-invalid WSAEVENT
    // as failed; epoll and kqueue can report neither, so they rely on EventLoop::notifyHandleClosing.
    // Regression guard twice over: this deadlocked on Windows before the reactor routed the invalid
    // handle, and it deadlocked on epoll/kqueue for as long as this case hardcoded the poll backend —
    // which is exactly why it now runs against every backend.
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue; // not available on this platform

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto pair = core::net::testing::makeSocketPair(loop);
            REQUIRE(pair.has_value());

            auto resumedWithError = false;
            loop.blockOn(closeWhileParked(&loop, pair->second.get(), &resumedWithError));

            CHECK(resumedWithError); // it resumed at all (no hang) AND saw the close as an error
        }
    }
}

TEST_CASE("a listener keeps accepting across sequential connections", "[net]")
{
    // One listener, several connections one after another — the shape a daemon actually runs,
    // and the one every other case here stops short of: they accept once. On Windows the accept
    // path has to take its readiness indication off the shared event without destroying it
    // (@see core::net::consumeNetworkEvents and its own cases), and a listener that gets that
    // wrong does not fail, it goes SILENT: the second accept parks and nothing ever wakes it.
    //
    // Which is why the wait is BOUNDED here rather than left to the suite's timeout: a guard for a
    // hang that itself hangs reports "Timeout" after 1500 seconds and names nothing
    // (.agent/rules/testing.md: every wait is bounded and says what it waited for). The work is
    // three loopback connections with a 20ms pause between them, and both sections together
    // measure 0.147s against this 10s budget — about 70x, so it can only expire on the defect and
    // not on a slow machine.
    constexpr auto Connections = 3;
    constexpr auto Budget = std::chrono::milliseconds { 10000 };
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue; // not available on this platform

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto listener = core::net::listen(loop, "127.0.0.1", 0);
            REQUIRE(listener.has_value());

            auto accepted = 0;
            auto connected = 0;
            auto run = [](EventLoop* lp, core::net::IListener* l, int* a, int* c) -> Task<void> {
                co_await core::async::whenAll(acceptSequentially(l, Connections, a),
                                              connectSequentially(lp, l, Connections, c));
            };
            // The TIMER's own flag is the sentinel, not a "the work finished" one: whenAny
            // cancels the loser, and a cancelled accept RESOLVES with an error rather than
            // throwing, so the accept arm returns cleanly on the way out and a "finished" flag
            // would be set on the failing path too. This flag is reached only when the budget
            // really did expire first, which is exactly the condition being reported.
            auto timedOut = false;
            auto budget = [](EventLoop* lp, std::chrono::milliseconds limit, bool* expired) -> Task<void> {
                co_await lp->delay(limit);
                *expired = true;
            };
            loop.blockOn(core::net::testing::anyOf(run(&loop, listener->get(), &accepted, &connected),
                                                   budget(&loop, Budget, &timedOut)));

            INFO("waited " << Budget.count() << "ms for " << Connections
                           << " sequential accepts on this listener; it accepted " << accepted
                           << " and the client connected " << connected);
            REQUIRE_FALSE(timedOut); // true == the listener went silent: it stopped accepting
            CHECK(connected == Connections);
            CHECK(accepted == Connections);
        }
    }
}

TEST_CASE("a socket reports closed once a read observed the peer's EOF", "[net]")
{
    // ISocket::isClosed documents two halves — "close() was called" OR "the peer closed and a
    // read observed EOF" — and the production sockets latched only the first. A consumer
    // polling a connection whose peer hung up was therefore told it was still open, for ever.
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue; // not available on this platform

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto pair = core::net::testing::makeSocketPair(loop);
            REQUIRE(pair.has_value());

            CHECK_FALSE(pair->first->isClosed()); // a live connection, and nobody closed this end

            pair->second->close(); // the PEER hangs up; this end is untouched

            auto sawEof = false;
            loop.blockOn(readToEof(pair->first.get(), &sawEof));
            REQUIRE(sawEof);
            CHECK(pair->first->isClosed());
        }
    }
}

TEST_CASE("a split socket is closed once its read half observed EOF", "[net]")
{
    // SplitSocket::isClosed is "closed once either half is", built straight on the answer
    // above — so a combined transport whose read half saw the peer hang up must report closed
    // too, though nothing on either half was closed from this side.
    auto source = core::net::makeDefaultBackend();
    REQUIRE(source != nullptr);
    auto loop = EventLoop { *source };
    auto readPair = core::net::testing::makeSocketPair(loop);
    auto writePair = core::net::testing::makeSocketPair(loop);
    REQUIRE(readPair.has_value());
    REQUIRE(writePair.has_value());

    auto split = core::net::combineHalves(std::move(readPair->first), std::move(writePair->first));
    CHECK_FALSE(split->isClosed());

    readPair->second->close(); // the read half's peer hangs up

    auto sawEof = false;
    loop.blockOn(readToEof(split.get(), &sawEof));
    REQUIRE(sawEof);
    CHECK(split->isClosed());
}

TEST_CASE("listen + connect + accept echo a request over loopback", "[net]")
{
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue; // not available on this platform

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };

            auto listener = core::net::listen(loop, "127.0.0.1", 0);
            REQUIRE(listener.has_value());
            REQUIRE((*listener)->boundPort() != 0);

            auto served = false;
            auto matched = false;
            loop.blockOn(loopbackEcho(&loop, listener->get(), &served, &matched));

            REQUIRE(served);
            REQUIRE(matched);
        }
    }
}

namespace
{

/// Like @ref echoServer, but keeps accepting until one connection carries a real request.
/// A connection that closes without sending — a bind-time liveness probe is exactly that —
/// is drained and ignored rather than mistaken for the request under test.
Task<void> echoOnceDraining(core::net::IListener* listener, bool* served)
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
///
/// Closes @p listener when it gives up, because its whenAll sibling is parked in accept() and
/// whenAll cancels nobody: an arm that returns early without a stop turns a red into a hang.
Task<void> unixProbe(EventLoop* loop, core::net::IListener* listener, std::string path, bool* matched)
{
    auto connected = co_await core::net::connectUnix(loop, path);
    if (!connected.has_value())
    {
        listener->close();
        co_return;
    }
    auto sock = std::move(*connected);

    auto wroteOk = false;
    co_await writeAll(sock.get(), "probe", &wroteOk);
    if (!wroteOk)
    {
        listener->close();
        co_return;
    }
    co_await expectRead(sock.get(), "probe", matched);
}

/// Whether the socket FILE at @p path is present.
///
/// Deliberately NOT std::filesystem::exists: a bound AF_UNIX socket on Windows is a reparse
/// point that exists() tries to follow and cannot, so it THROWS ERROR_CANT_ACCESS_FILE on
/// exactly the files this asks about. Reading the parent directory's entries answers from the
/// name alone, which is why the Windows path claim (`windows/UnixSocketPath.cpp`) reaches for
/// FindFirstFileA rather than a stat too.
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
Task<void> unixEcho(
    EventLoop* loop, core::net::IListener* listener, std::string path, bool* served, bool* matched)
{
    // No REQUIRE inside a whenAll arm: whenAll deliberately does not cancel its siblings when
    // one throws, and the sibling here is parked in accept() with nobody left to close the
    // listener — so a failed assertion HUNG the suite instead of failing it
    // (`.agent/rules/testing.md`: a REQUIRE above a stop turns a red into a hang). The arm
    // records what it saw and releases its sibling; the case asserts once whenAll has returned.
    auto client =
        [](EventLoop* innerLoop, core::net::IListener* acceptor, std::string target, bool* ok) -> Task<void> {
        auto socket = co_await core::net::connectUnix(innerLoop, target);
        if (!socket.has_value())
        {
            acceptor->close(); // nothing will ever arrive: release the accept parked behind us
            co_return;
        }
        auto const request = std::string_view { "unix-ping" };
        // Not discarded: a failed write left this arm walking into read() and parking there, and
        // its sibling parked in its own read() waiting for bytes that were never sent — two parked
        // arms and no stop, the same hang by another route. Returning is enough HERE, unlike in
        // connectAndProbe's twin: returning destroys this socket, and echoServer co_returns on the
        // EOF that produces rather than looping back into accept(). The sibling's shape is what
        // makes the difference.
        if (auto const wrote = co_await (*socket)->write(std::as_bytes(std::span { request }));
            !wrote.has_value())
            co_return;
        auto buffer = std::array<std::byte, 32> {};
        auto const n = co_await (*socket)->read(buffer);
        if (!n.has_value())
            co_return; // same: the EOF this return produces is what ends echoServer
        *ok = std::string_view { reinterpret_cast<char const*>(buffer.data()), *n } == "unix-ping";
    };
    co_await core::async::whenAll(echoServer(listener, served), client(loop, listener, path, matched));
}

} // namespace

// The [afunix] tag names the cases that SKIP where AF_UNIX is missing. Windows CI asserts this
// subset reports no skips: `windows-latest` is far past the 1803 that introduced AF_UNIX, so a
// skip there means the daemon's transport silently stopped being tested, not that the platform
// lacks it. Same reasoning as the [oracle] tag on the tmux interop tests.
TEST_CASE("unix-domain listen + connect echo a request", "[net][afunix]")
{
    // Runtime-gated: on platforms without AF_UNIX support this documents the
    // Unsupported answer instead (never a crash). On Windows this is the
    // afunix.h path's coverage.
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue; // not available on this platform

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };

            auto const path = (std::filesystem::temp_directory_path()
                               / std::format("contour-net-{}", std::random_device {}()) / "echo.sock")
                                  .string();
            auto listener = core::net::listenUnix(loop, path);
            if (!listener.has_value())
            {
                REQUIRE(listener.error().code == core::net::NetErrorCode::Unsupported);
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
    }
}

TEST_CASE("closing a unix listener removes its socket file", "[net][afunix]")
{
    // The daemon's contract (docs/internals/vthost.md, "Daemon lifetime"): a closed listener
    // leaves no path behind, so the next start's liveness probe finds nothing and binds fresh
    // rather than reclaiming a corpse. Windows used to keep the file — WindowsListener held no
    // path at all — which is what this pins.
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue; // not available on this platform

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };

            auto const dir = makeSocketDir();
            auto const path = (dir / "closing.sock").string();
            auto listener = core::net::listenUnix(loop, path);
            if (!listener.has_value())
            {
                REQUIRE(listener.error().code == core::net::NetErrorCode::Unsupported);
                SKIP("AF_UNIX not supported on this platform");
            }
            REQUIRE(socketFileExists(path));

            (*listener)->close();
            REQUIRE_FALSE(socketFileExists(path));

            auto ec = std::error_code {};
            std::filesystem::remove_all(dir, ec);
        }
    }
}

TEST_CASE("a live server on the path is not hijacked", "[net][afunix]")
{
    // A second bind must be REFUSED rather than unlink the live socket out from under the
    // incumbent, which would keep all its sessions but become unreachable forever. The refusal
    // is what a second `contour daemon` on one label hits.
    //
    // Cross-platform on purpose: the POSIX twin in UnixSocket_test.cpp cannot run here, and the
    // Windows probe (WindowsListener::probeUnixSocketOwner) had no coverage at all.
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue; // not available on this platform

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };

            auto const dir = makeSocketDir();
            auto const path = (dir / "default").string();
            auto first = core::net::listenUnix(loop, path);
            if (!first.has_value())
            {
                REQUIRE(first.error().code == core::net::NetErrorCode::Unsupported);
                SKIP("AF_UNIX not supported on this platform");
            }

            auto second = core::net::listenUnix(loop, path);
            REQUIRE_FALSE(second.has_value());
            REQUIRE(second.error().code == core::net::NetErrorCode::AddressInUse);
            REQUIRE(socketFileExists(path)); // the incumbent's file, left intact

            // The incumbent still serves: a client connects and gets its probe echoed back. The refused
            // bind's own liveness probe left a dropped connection queued ahead of it, which is why the
            // server flow here drains rather than treating the first accept as the request.
            auto served = false;
            auto matched = false;
            auto run = [](core::net::IListener* listener, EventLoop* lp, std::string p, bool* s, bool* m)
                -> Task<void> {
                co_await core::async::whenAll(echoOnceDraining(listener, s),
                                              unixProbe(lp, listener, std::move(p), m));
            };
            loop.blockOn(run(first->get(), &loop, path, &served, &matched));

            REQUIRE(served);
            REQUIRE(matched);

            auto ec = std::error_code {};
            std::filesystem::remove_all(dir, ec);
        }
    }
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
                      core::net::IListener* listener,
                      std::size_t payload,
                      std::size_t* serverGot,
                      std::size_t* clientGot,
                      bool* serverSent,
                      bool* clientSent)
{
    auto server = [](core::net::IListener* l, std::size_t bytes, std::size_t* got, bool* sent) -> Task<void> {
        auto accepted = co_await l->accept();
        if (!accepted)
            co_return;
        auto conn = std::move(*accepted);
        co_await core::async::whenAll(writeBulk(conn.get(), bytes, sent), readBulk(conn.get(), bytes, got));
    };
    // The client closes the listener when it cannot connect, for the same reason unixProbe
    // does: its sibling is parked in accept() and whenAll cancels nobody, so an arm that
    // simply returns leaves the case hanging rather than failing.
    auto client = [](EventLoop* innerLoop,
                     core::net::IListener* acceptor,
                     std::size_t bytes,
                     std::size_t* got,
                     bool* sent) -> Task<void> {
        auto connected = co_await core::net::connect(innerLoop, "127.0.0.1", acceptor->boundPort());
        if (!connected)
        {
            acceptor->close();
            co_return;
        }
        auto sock = std::move(*connected);
        co_await core::async::whenAll(writeBulk(sock.get(), bytes, sent), readBulk(sock.get(), bytes, got));
    };
    co_await core::async::whenAll(server(listener, payload, serverGot, serverSent),
                                  client(loop, listener, payload, clientGot, clientSent));
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
TEST_CASE("a concurrent reader and writer on one socket both make progress", "[net]")
{
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue; // not available on this platform

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };

            auto listener = core::net::listen(loop, "127.0.0.1", 0);
            REQUIRE(listener.has_value());

            // Comfortably past any socket send buffer, so both directions really do block.
            constexpr auto Payload = std::size_t { 4 } * 1024 * 1024;
            auto serverGot = std::size_t { 0 };
            auto clientGot = std::size_t { 0 };
            auto serverSent = false;
            auto clientSent = false;
            loop.blockOn(duplexBulk(
                &loop, listener->get(), Payload, &serverGot, &clientGot, &serverSent, &clientSent));

            CHECK(serverSent);
            CHECK(clientSent);
            CHECK(serverGot == Payload);
            CHECK(clientGot == Payload);
        }
    }
}

TEST_CASE("a second listener on a bound port is refused by default", "[net][listen]")
{
    // The default is exclusive, and it has to stay so: a port a second process can bind is a port
    // whose connections it can take. Every backend, because each builds its own listener: the WFMO
    // one bound with SO_REUSEADDR until this case asked it.
    for (auto const& backend: BackendMatrix)
    {
        auto firstBackend = core::net::makeBackend(backend.kind);
        auto secondBackend = core::net::makeBackend(backend.kind);
        if (!firstBackend || !secondBackend)
            continue; // not available on this platform

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto firstLoop = EventLoop { *firstBackend };
            auto secondLoop = EventLoop { *secondBackend };

            auto first = core::net::listen(firstLoop, core::net::ListenOptions { .host = "127.0.0.1" });
            REQUIRE(first.has_value());
            auto const port = (*first)->boundPort();

            auto second =
                core::net::listen(secondLoop, core::net::ListenOptions { .host = "127.0.0.1", .port = port });
            REQUIRE_FALSE(second.has_value());
            CHECK(second.error().code == core::net::NetErrorCode::AddressInUse);
        }
    }
}
