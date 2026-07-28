// SPDX-License-Identifier: Apache-2.0
#ifndef _WIN32

    #include <crispy/logsink.h>

    #include <catch2/catch_test_macros.hpp>

    #include <algorithm>
    #include <cerrno>
    #include <chrono>
    #include <cstddef>
    #include <cstdint>
    #include <filesystem>
    #include <memory>
    #include <span>
    #include <string>
    #include <vector>

    #include <unistd.h>

    #include <coro/Task.hpp>
    #include <coro/WhenAll.hpp>
    #include <net/AsyncBufferedReader.h>
    #include <net/EventLoop.h>
    #include <net/IListener.h>
    #include <net/PollEventSource.h>
    #include <net/Sockets.h>
    #include <net/testing/CoroTestSupport.h>
    #include <net/testing/TempDir.h>
    #include <vthost/ConnectionAcceptor.h>

using coro::Task;
using net::EventLoop;
using vthost::ConnectionAcceptor;

namespace
{

/// A handler that reads one line, records it, and echoes it back — the echo
/// makes the test deterministic: once the client read the echo, the handler
/// provably ran to that point.
Task<void> echoLineHandler(std::unique_ptr<net::ISocket> connection, std::vector<std::string>* seen)
{
    auto reader = net::AsyncBufferedReader { connection.get() };
    auto line = co_await reader.readLine();
    if (!line.has_value())
        co_return;
    seen->push_back(*line);

    auto const wire = *line + "\n";
    auto const bytes =
        std::span<std::byte const> { reinterpret_cast<std::byte const*>(wire.data()), wire.size() };
    std::ignore = co_await connection->write(bytes);
}

/// A handler that fails partway through serving, to prove the failure is reported.
///
/// It suspends first, so the exception leaves a RESUMED frame — the realistic shape. A throw
/// before the first await would propagate synchronously out of the spawn instead.
Task<void> throwingHandler(EventLoop* loop, std::unique_ptr<net::ISocket> connection)
{
    co_await loop->delay(std::chrono::milliseconds { 1 });
    connection.reset(); // closing here is what lets the client below see EOF
    throw std::runtime_error { "handler blew up" };
}

/// Connects and reads until the peer closes — a deterministic barrier for "the handler's flow ran
/// and unwound", rather than sleeping and hoping.
Task<void> clientAwaitingClosure(EventLoop* loop, std::string socketPath, bool* connected)
{
    auto socket = co_await net::connectUnix(loop, socketPath);
    *connected = socket.has_value();
    if (!socket.has_value())
        co_return;
    auto sock = std::move(*socket);
    auto reader = net::AsyncBufferedReader { sock.get() };
    std::ignore = co_await reader.readLine(); // ends at EOF: the handler never replies
}

/// A listener whose accept fails synchronously every time — the shape of fd
/// exhaustion (EMFILE/ENFILE), which never suspends the accepting coroutine.
struct ExhaustedListener final: net::IListener
{
    int* attempts;

    explicit ExhaustedListener(int* attemptsArg) noexcept: attempts(attemptsArg) {}

    coro::Task<net::AcceptResult> accept() override
    {
        ++*attempts;
        co_return std::unexpected(
            net::NetError { .code = net::NetErrorCode::Other, .systemCode = EMFILE, .context = "accept" });
    }

    [[nodiscard]] std::uint16_t localPort() const noexcept override { return 0; }
    void close() noexcept override {}
};

/// One client's round trip: connect, send a line, await the echo.
Task<void> clientRoundTrip(EventLoop* loop, std::string socketPath, std::string line, bool* echoed)
{
    auto connected = co_await net::connectUnix(loop, socketPath);
    REQUIRE(connected.has_value());
    auto sock = std::move(*connected);

    auto const wire = line + "\n";
    auto const bytes =
        std::span<std::byte const> { reinterpret_cast<std::byte const*>(wire.data()), wire.size() };
    REQUIRE((co_await sock->write(bytes)).has_value());

    auto reader = net::AsyncBufferedReader { sock.get() };
    auto const echo = co_await reader.readLine();
    *echoed = echo.has_value() && *echo == line;
}

/// Test-only connection handler: drains lines until the peer disconnects.
/// Used by the persistent-failure and close-before-serve tests which need a
/// real handler but don't care about the data.
coro::Task<void> drainConnection(std::unique_ptr<net::ISocket> connection)
{
    auto reader = net::AsyncBufferedReader { connection.get() };
    while (true)
    {
        auto const line = co_await reader.readLine();
        if (!line.has_value())
            co_return;
    }
}

} // namespace

TEST_CASE("ConnectionAcceptor serves concurrent connections through the injected handler", "[vthost][server]")
{
    auto const tmp = net::testing::TempDir { "contour-muxsrv" };
    auto const socketPath = (tmp.path() / "sockets" / "default").string();

    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };

    auto listener = net::listenUnix(loop, socketPath);
    REQUIRE(listener.has_value());

    auto seen = std::vector<std::string> {};
    auto server =
        ConnectionAcceptor { loop,
                             "test",
                             std::move(*listener),
                             [&seen](vthost::ConnectionId const&, std::unique_ptr<net::ISocket> conn) {
                                 return echoLineHandler(std::move(conn), &seen);
                             } };
    loop.spawn(server.serve());

    // TWO clients in flight at once: with the ported-from server's sequential
    // accept this would deadlock (the second connect never gets served while
    // the first handler is still parked); the spawn-per-connection design
    // serves both within one blockOn.
    auto firstEchoed = false;
    auto secondEchoed = false;
    auto runBoth = [](EventLoop* lp, std::string path, bool* a, bool* b) -> Task<void> {
        co_await coro::whenAll(clientRoundTrip(lp, path, "alpha", a), clientRoundTrip(lp, path, "beta", b));
    };
    loop.blockOn(runBoth(&loop, socketPath, &firstEchoed, &secondEchoed));

    CHECK(firstEchoed);
    CHECK(secondEchoed);
    CHECK(server.acceptedCount() == 2);
    REQUIRE(seen.size() == 2);
    CHECK(std::ranges::count(seen, "alpha") == 1);
    CHECK(std::ranges::count(seen, "beta") == 1);

    server.close(); // the parked accept resolves as cancelled; ~EventLoop reaps
}

TEST_CASE("persistent accept failures back off instead of starving the loop", "[vthost][server]")
{
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };

    auto attempts = 0;
    auto server = ConnectionAcceptor { loop,
                                       "test",
                                       std::make_unique<ExhaustedListener>(&attempts),
                                       [](vthost::ConnectionId const&, std::unique_ptr<net::ISocket> conn) {
                                           return drainConnection(std::move(conn));
                                       } };
    loop.spawn(server.serve());

    // Every accept fails without suspending; serve() must yield between attempts
    // or this blockOn would never get the loop back (the livelock this guards
    // against). Within 50ms only the initial attempt fits into the backoff.
    loop.blockOn(net::testing::sleepFor(&loop, std::chrono::milliseconds { 50 }));
    CHECK(attempts <= 2);
}

TEST_CASE("closing the server ends the accept loop", "[vthost][server]")
{
    auto const tmp = net::testing::TempDir { "contour-muxsrv" };
    auto const socketPath = (tmp.path() / "sockets" / "default").string();

    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };

    auto listener = net::listenUnix(loop, socketPath);
    REQUIRE(listener.has_value());

    auto server = ConnectionAcceptor { loop,
                                       "test",
                                       std::move(*listener),
                                       [](vthost::ConnectionId const&, std::unique_ptr<net::ISocket> conn) {
                                           return drainConnection(std::move(conn));
                                       } };

    // Close before serving: the first accept resolves cancelled and serve returns.
    server.close();
    loop.blockOn(server.serve());

    CHECK(server.acceptedCount() == 0);
}

TEST_CASE("the accepting endpoint is named in the connection log", "[vthost][acceptor]")
{
    // The daemon serves up to five listeners at once, so "accepted a connection" without a name
    // is barely a diagnostic. The identity also has to survive into the handler's frame.
    auto capture = logstore::scoped_capture { "vthost.conn" };

    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };
    auto const tmp = net::testing::TempDir { "contour-muxsrv" };
    auto const socketPath = (tmp.path() / "sockets" / "named").string();
    auto listener = net::listenUnix(loop, socketPath);
    REQUIRE(listener.has_value());

    auto seen = std::vector<std::string> {};
    auto server =
        ConnectionAcceptor { loop,
                             "native",
                             std::move(*listener),
                             [&seen](vthost::ConnectionId const&, std::unique_ptr<net::ISocket> conn) {
                                 return echoLineHandler(std::move(conn), &seen);
                             } };
    loop.spawn(server.serve());

    auto echoed = false;
    loop.blockOn(clientRoundTrip(&loop, socketPath, "alpha", &echoed));
    CHECK(echoed);

    CHECK(capture.contains("native#1: accepted"));
}

TEST_CASE("a persistently failing accept is reported, not silent", "[vthost][acceptor]")
{
    // Before this, fd exhaustion was invisible: the acceptor retried every 100ms forever and
    // said nothing, so a daemon that had stopped serving looked identical to an idle one.
    auto capture = logstore::scoped_capture {};

    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };
    auto attempts = 0;
    auto server = ConnectionAcceptor { loop,
                                       "native",
                                       std::make_unique<ExhaustedListener>(&attempts),
                                       [](vthost::ConnectionId const&, std::unique_ptr<net::ISocket> conn) {
                                           return drainConnection(std::move(conn));
                                       } };
    loop.spawn(server.serve());
    loop.blockOn(net::testing::sleepFor(&loop, std::chrono::milliseconds { 250 }));

    CHECK(attempts >= 2);
    CHECK(capture.contains("native: accept failed"));
    CHECK(capture.contains("errno 24")); // EMFILE
    // Throttled: many retries, but not one line each.
    CHECK(capture.count("accept failed") < static_cast<std::size_t>(attempts));
}

TEST_CASE("an exception escaping a connection handler is reported, not reaped", "[vthost][acceptor]")
{
    // A connection flow is spawned as a ROOT task, and the loop reaps finished roots by destroying
    // their frames — so coro::Task's captured exception_ptr went to the grave unread. The daemon
    // survived (which is right: one bad peer must not take it down) but said nothing at all, which
    // is how a real bug would hide. Surviving is still the behaviour; the silence is not.
    auto capture = logstore::scoped_capture {};

    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };
    auto const tmp = net::testing::TempDir { "contour-muxsrv" };
    auto const socketPath = (tmp.path() / "sockets" / "throwing").string();
    auto listener = net::listenUnix(loop, socketPath);
    REQUIRE(listener.has_value());

    auto server =
        ConnectionAcceptor { loop,
                             "native",
                             std::move(*listener),
                             [&loop](vthost::ConnectionId const&, std::unique_ptr<net::ISocket> conn) {
                                 return throwingHandler(&loop, std::move(conn));
                             } };
    loop.spawn(server.serve());

    auto connected = false;
    loop.blockOn(clientAwaitingClosure(&loop, socketPath, &connected));

    CHECK(connected);
    CHECK(capture.contains("native#1: connection flow aborted"));
    CHECK(capture.contains("handler blew up")); // the reason, not just the fact
}

TEST_CASE("AcceptFailureThrottle reports the first failure and then every Nth", "[vthost][acceptor]")
{
    auto const emfile =
        net::NetError { .code = net::NetErrorCode::Other, .systemCode = EMFILE, .context = "accept" };

    SECTION("an identical failure is rate-limited")
    {
        auto throttle = vthost::AcceptFailureThrottle { 2 };
        CHECK(throttle.shouldLog(emfile)); // the first is always news
        CHECK_FALSE(throttle.shouldLog(emfile));
        CHECK(throttle.shouldLog(emfile)); // every 2nd thereafter
        CHECK(throttle.consecutive() == 3);
    }

    SECTION("a changed failure is reported immediately")
    {
        auto throttle = vthost::AcceptFailureThrottle { 1000 };
        CHECK(throttle.shouldLog(emfile));
        CHECK_FALSE(throttle.shouldLog(emfile));
        // A different problem is a different story, however often the old one repeated.
        auto const refused = net::NetError { .code = net::NetErrorCode::ConnRefused,
                                             .systemCode = ECONNREFUSED,
                                             .context = "accept" };
        CHECK(throttle.shouldLog(refused));
        CHECK(throttle.consecutive() == 1);
    }
}

#endif // !_WIN32
