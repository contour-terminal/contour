// SPDX-License-Identifier: Apache-2.0
#ifndef _WIN32

    #include <core/async/Task.hpp>
    #include <core/async/WhenAll.hpp>
    #include <core/log/LogSink.hpp>
    #include <core/net/AsyncBufferedReader.hpp>
    #include <core/net/EventLoop.hpp>
    #include <core/net/IListener.hpp>
    #include <core/net/IoBackend.hpp>
    #include <core/net/Sockets.hpp>
    #include <core/net/testing/CoroTestSupport.hpp>
    #include <core/net/testing/FailingListener.hpp>
    #include <core/net/testing/InMemorySocket.hpp>
    #include <core/testing/ScopedTempDir.hpp>

    #include <catch2/catch_test_macros.hpp>

    #include <sys/socket.h>
    #include <sys/un.h>

    #include <algorithm>
    #include <array>
    #include <cerrno>
    #include <chrono>
    #include <cstddef>
    #include <cstdint>
    #include <cstring>
    #include <filesystem>
    #include <functional>
    #include <memory>
    #include <span>
    #include <string>
    #include <utility>
    #include <vector>

    #include <unistd.h>

    #include <vthost/ConnectionAcceptor.hpp>
    #include <vthost/ListeningSocket.hpp>

using core::async::Task;
using core::net::EventLoop;
using vthost::ConnectionAcceptor;

namespace
{

/// A handler that reads one line, records it, and echoes it back — the echo
/// makes the test deterministic: once the client read the echo, the handler
/// provably ran to that point.
Task<void> echoLineHandler(std::unique_ptr<core::net::ISocket> connection, std::vector<std::string>* seen)
{
    auto reader = core::net::AsyncBufferedReader { connection.get() };
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
Task<void> throwingHandler(EventLoop* loop, std::unique_ptr<core::net::ISocket> connection)
{
    co_await loop->delay(std::chrono::milliseconds { 1 });
    connection.reset(); // closing here is what lets the client below see EOF
    throw std::runtime_error { "handler blew up" };
}

/// Connects and reads until the peer closes — a deterministic barrier for "the handler's flow ran
/// and unwound", rather than sleeping and hoping.
Task<void> clientAwaitingClosure(EventLoop* loop, std::string socketPath, bool* connected)
{
    auto socket = co_await core::net::connectUnix(loop, socketPath);
    *connected = socket.has_value();
    if (!socket.has_value())
        co_return;
    auto sock = std::move(*socket);
    auto reader = core::net::AsyncBufferedReader { sock.get() };
    std::ignore = co_await reader.readLine(); // ends at EOF: the handler never replies
}

/// A listener whose accept fails synchronously every time, with one error — the shape of fd
/// exhaustion (EMFILE/ENFILE), which never suspends the accepting coroutine, and of a dead listener.
struct FailingAcceptListener final: core::net::IListener
{
    int* attempts;
    core::net::NetError error;
    bool* closed; ///< Set when the listener is closed; may be null.

    // Not noexcept: IListener's constructor allocates its close token's state.
    FailingAcceptListener(int* attemptsArg, core::net::NetError errorArg, bool* closedArg = nullptr):
        attempts(attemptsArg), error(std::move(errorArg)), closed(closedArg)
    {
    }

    core::async::Task<core::net::AcceptResult> accept() override
    {
        ++*attempts;
        co_return std::unexpected(error);
    }

    [[nodiscard]] std::uint16_t boundPort() const noexcept override { return 0; }

  protected:
    void doClose() noexcept override
    {
        if (closed != nullptr)
            *closed = true;
    }
};

/// A listener that has run out of descriptors for good: every accept answers EMFILE, which passes.
std::unique_ptr<core::net::IListener> exhaustedListener(int* attempts)
{
    return std::make_unique<FailingAcceptListener>(
        attempts,
        core::net::NetError {
            .code = core::net::NetErrorCode::ResourceExhausted, .systemCode = EMFILE, .context = "accept" });
}

/// One client's round trip: connect, send a line, await the echo.
Task<void> clientRoundTrip(EventLoop* loop, std::string socketPath, std::string line, bool* echoed)
{
    auto connected = co_await core::net::connectUnix(loop, socketPath);
    REQUIRE(connected.has_value());
    auto sock = std::move(*connected);

    auto const wire = line + "\n";
    auto const bytes =
        std::span<std::byte const> { reinterpret_cast<std::byte const*>(wire.data()), wire.size() };
    auto const written = co_await sock->write(bytes);
    REQUIRE(written.has_value());

    auto reader = core::net::AsyncBufferedReader { sock.get() };
    auto const echo = co_await reader.readLine();
    *echoed = echo.has_value() && *echo == line;
}

/// Test-only connection handler: drains lines until the peer disconnects.
/// Used by the persistent-failure and close-before-serve tests which need a
/// real handler but don't care about the data.
core::async::Task<void> drainConnection(std::unique_ptr<core::net::ISocket> connection)
{
    auto reader = core::net::AsyncBufferedReader { connection.get() };
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
    auto const tmp = core::testing::ScopedTempDir { "contour-muxsrv" };
    auto const socketPath = (tmp.path() / "sockets" / "default").string();

    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };

    auto listener = core::net::listenUnix(loop, socketPath);
    REQUIRE(listener.has_value());

    auto seen = std::vector<std::string> {};
    auto server =
        ConnectionAcceptor { loop,
                             "test",
                             std::move(*listener),
                             [&seen](vthost::ConnectionId const&, std::unique_ptr<core::net::ISocket> conn) {
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
        co_await core::async::whenAll(clientRoundTrip(lp, path, "alpha", a),
                                      clientRoundTrip(lp, path, "beta", b));
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
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };

    auto attempts = 0;
    auto server =
        ConnectionAcceptor { loop,
                             "test",
                             exhaustedListener(&attempts),
                             [](vthost::ConnectionId const&, std::unique_ptr<core::net::ISocket> conn) {
                                 return drainConnection(std::move(conn));
                             } };
    loop.spawn(server.serve());

    // Every accept fails without suspending; serve() must yield between attempts
    // or this blockOn would never get the loop back (the livelock this guards
    // against). Within 50ms only the initial attempt fits into the backoff.
    loop.blockOn(core::net::testing::sleepFor(&loop, std::chrono::milliseconds { 50 }));
    CHECK(attempts <= 2);
}

TEST_CASE("closing the server ends the accept loop", "[vthost][server]")
{
    auto const tmp = core::testing::ScopedTempDir { "contour-muxsrv" };
    auto const socketPath = (tmp.path() / "sockets" / "default").string();

    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };

    auto listener = core::net::listenUnix(loop, socketPath);
    REQUIRE(listener.has_value());

    auto server =
        ConnectionAcceptor { loop,
                             "test",
                             std::move(*listener),
                             [](vthost::ConnectionId const&, std::unique_ptr<core::net::ISocket> conn) {
                                 return drainConnection(std::move(conn));
                             } };

    // Close before serving: the first accept resolves cancelled and serve returns.
    server.close();
    loop.blockOn(server.serve());

    CHECK(server.acceptedCount() == 0);
}

namespace
{
/// @return A connected descriptor to the unix socket at @p path, from a plain blocking connect: the kernel
///         completes it into the listener's backlog whether or not anything accepts.
[[nodiscard]] int connectRaw(std::string const& path)
{
    auto const fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    auto address = sockaddr_un {};
    address.sun_family = AF_UNIX;
    REQUIRE(path.size() < sizeof(address.sun_path));
    std::memcpy(address.sun_path, path.data(), path.size());
    REQUIRE(::connect(fd, reinterpret_cast<sockaddr const*>(&address), sizeof(address)) == 0);
    return fd;
}

/// Turns @p loop for about @p duration.
void turnFor(EventLoop& loop, std::chrono::milliseconds duration)
{
    loop.blockOn(core::net::testing::sleepFor(&loop, duration));
}
} // namespace

TEST_CASE("a held acceptor leaves connections in the backlog, and takes them once resumed",
          "[vthost][acceptor][upgrade]")
{
    auto const tmp = core::testing::ScopedTempDir { "contour-muxsrv" };
    auto const socketPath = (tmp.path() / "sockets" / "default").string();
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto listener = core::net::listenUnix(loop, socketPath);
    REQUIRE(listener.has_value());
    auto server =
        ConnectionAcceptor { loop,
                             "test",
                             std::move(*listener),
                             [](vthost::ConnectionId const&, std::unique_ptr<core::net::ISocket> conn) {
                                 return drainConnection(std::move(conn));
                             } };
    loop.spawn(server.serve());
    turnFor(loop, std::chrono::milliseconds { 10 }); // parked in its accept

    server.holdAccepting();
    CHECK(server.admission() == vthost::Admission::Holding);
    auto const waiting = connectRaw(socketPath);
    turnFor(loop, std::chrono::milliseconds { 50 });
    CHECK(server.acceptedCount() == 0); // still bound: the connect went into the backlog

    server.resumeAccepting();
    CHECK(server.admission() == vthost::Admission::Accepting);
    turnFor(loop, std::chrono::milliseconds { 50 });
    CHECK(server.acceptedCount() == 1);

    // Held and resumed again: the accept loop is the same one.
    server.holdAccepting();
    turnFor(loop, std::chrono::milliseconds { 10 });
    server.resumeAccepting();
    auto const second = connectRaw(socketPath);
    turnFor(loop, std::chrono::milliseconds { 50 });
    CHECK(server.acceptedCount() == 2);

    ::close(waiting);
    ::close(second);
    server.close();
    turnFor(loop, std::chrono::milliseconds { 10 });
}

namespace
{
/// A listener whose one connection is accepted in the same turn the acceptor is held: the race a hold loses.
struct HoldingRaceListener final: core::net::IListener
{
    std::function<void()> onAccept;              ///< Runs as the connection is taken: the hold.
    std::unique_ptr<core::net::ISocket> pending; ///< The connection the next accept hands out.

    explicit HoldingRaceListener(std::unique_ptr<core::net::ISocket> connection):
        pending { std::move(connection) }
    {
    }

    core::async::Task<core::net::AcceptResult> accept() override
    {
        if (!pending)
            co_return std::unexpected(core::net::NetError {
                .code = core::net::NetErrorCode::Cancelled, .systemCode = 0, .context = "accept" });
        if (onAccept)
            onAccept();
        co_return std::move(pending);
    }

    [[nodiscard]] std::uint16_t boundPort() const noexcept override { return 0; }

  protected:
    void doClose() noexcept override {}
};
} // namespace

TEST_CASE("a connection accepted as the acceptor is held is closed, not served",
          "[vthost][acceptor][upgrade]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto pair = core::net::testing::InMemorySocketPair::create();
    auto* const client = pair.client.get();
    auto owned = std::make_unique<HoldingRaceListener>(std::move(pair.server));
    auto* const listener = owned.get();
    auto served = 0;
    auto server = ConnectionAcceptor { loop,
                                       "test",
                                       std::move(owned),
                                       [&served](vthost::ConnectionId const&,
                                                 std::unique_ptr<core::net::ISocket> conn) {
                                           ++served;
                                           return drainConnection(std::move(conn));
                                       } };
    listener->onAccept = [&server] {
        server.holdAccepting();
    };
    loop.spawn(server.serve());
    turnFor(loop, std::chrono::milliseconds { 20 });

    CHECK(server.admission() == vthost::Admission::Holding);
    CHECK(served == 0);
    CHECK(server.acceptedCount() == 0);
    // The client reads the end of a connection nothing serves.
    auto const ended = loop.blockOn([](core::net::ISocket* socket) -> Task<bool> {
        auto buffer = std::array<std::byte, 16> {};
        auto const read = co_await socket->read(buffer);
        co_return !read.has_value() || *read == 0;
    }(client));
    CHECK(ended);

    server.close();
    turnFor(loop, std::chrono::milliseconds { 10 });
}

TEST_CASE("a held acceptor that is closed ends its accept loop", "[vthost][acceptor][upgrade]")
{
    auto const tmp = core::testing::ScopedTempDir { "contour-muxsrv" };
    auto const socketPath = (tmp.path() / "sockets" / "default").string();
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto listener = core::net::listenUnix(loop, socketPath);
    REQUIRE(listener.has_value());
    auto server =
        ConnectionAcceptor { loop,
                             "test",
                             std::move(*listener),
                             [](vthost::ConnectionId const&, std::unique_ptr<core::net::ISocket> conn) {
                                 return drainConnection(std::move(conn));
                             } };
    server.holdAccepting();
    auto const serveAndClose = [](EventLoop* lp, ConnectionAcceptor* acceptor) -> Task<void> {
        co_await core::async::whenAll(
            acceptor->serve(), [](EventLoop* l, ConnectionAcceptor* a) -> Task<void> {
                co_await core::net::testing::sleepFor(l, std::chrono::milliseconds { 10 });
                a->close();
            }(lp, acceptor));
    };
    loop.blockOn(serveAndClose(&loop, &server)); // returns only once serve() ended
    CHECK(server.acceptedCount() == 0);
}

TEST_CASE("an acceptor names the descriptor its unix listener listens on", "[vthost][acceptor][upgrade]")
{
    auto const tmp = core::testing::ScopedTempDir { "contour-muxsrv" };
    auto const socketPath = (tmp.path() / "sockets" / "default").string();
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto listener = core::net::listenUnix(loop, socketPath);
    REQUIRE(listener.has_value());
    auto server =
        ConnectionAcceptor { loop,
                             "control",
                             std::move(*listener),
                             [](vthost::ConnectionId const&, std::unique_ptr<core::net::ISocket> conn) {
                                 return drainConnection(std::move(conn));
                             } };
    CHECK(server.name() == "control");
    auto const handle = server.listeningHandle();
    REQUIRE_FALSE(handle.isNone());
    auto address = sockaddr_un {};
    auto length = static_cast<socklen_t>(sizeof(address));
    REQUIRE(::getsockname(handle.native(), reinterpret_cast<sockaddr*>(&address), &length) == 0);
    CHECK(std::string { address.sun_path } == socketPath);
    server.close();
}

TEST_CASE("a listening socket is told apart from every other descriptor, with and without SO_ACCEPTCONN",
          "[vthost][acceptor][upgrade]")
{
    // isListeningSocket asks SO_ACCEPTCONN where the system has it; detail::listensByAddress is what macOS,
    // which has not, falls back to. Both must agree on every kind of descriptor a daemon holds.
    auto const listener = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listener >= 0);
    REQUIRE(::listen(listener, 1) == 0);
    auto pair = std::array<int, 2> {};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair.data()) == 0);
    auto pipe = std::array<int, 2> {};
    REQUIRE(::pipe(pipe.data()) == 0);
    auto const unbound = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(unbound >= 0);

    CHECK(vthost::isListeningSocket(listener));
    CHECK(vthost::detail::listensByAddress(listener));
    for (auto const other: { pair[0], pipe[0], pipe[1], unbound })
    {
        INFO(other);
        CHECK_FALSE(vthost::isListeningSocket(other));
        CHECK_FALSE(vthost::detail::listensByAddress(other));
    }
    ::close(unbound);
    ::close(pipe[0]);
    ::close(pipe[1]);
    ::close(pair[0]);
    ::close(pair[1]);
    ::close(listener);
    CHECK_FALSE(vthost::isListeningSocket(listener));
    CHECK_FALSE(vthost::detail::listensByAddress(listener));
}

TEST_CASE("the accepting endpoint is named in the connection log", "[vthost][acceptor]")
{
    // The daemon serves up to five listeners at once, so "accepted a connection" without a name
    // is barely a diagnostic. The identity also has to survive into the handler's frame.
    auto capture = core::log::ScopedCapture { "vthost.conn" };

    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto const tmp = core::testing::ScopedTempDir { "contour-muxsrv" };
    auto const socketPath = (tmp.path() / "sockets" / "named").string();
    auto listener = core::net::listenUnix(loop, socketPath);
    REQUIRE(listener.has_value());

    auto seen = std::vector<std::string> {};
    auto server =
        ConnectionAcceptor { loop,
                             "native",
                             std::move(*listener),
                             [&seen](vthost::ConnectionId const&, std::unique_ptr<core::net::ISocket> conn) {
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
    auto capture = core::log::ScopedCapture {};

    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto attempts = 0;
    auto server =
        ConnectionAcceptor { loop,
                             "native",
                             exhaustedListener(&attempts),
                             [](vthost::ConnectionId const&, std::unique_ptr<core::net::ISocket> conn) {
                                 return drainConnection(std::move(conn));
                             } };
    loop.spawn(server.serve());
    loop.blockOn(core::net::testing::sleepFor(&loop, std::chrono::milliseconds { 250 }));

    CHECK(attempts >= 2);
    CHECK(capture.contains("native: accept failed"));
    CHECK(capture.contains("errno 24")); // EMFILE
    // Throttled: many retries, but not one line each.
    CHECK(capture.count("accept failed") < static_cast<std::size_t>(attempts));
}

TEST_CASE("a dead listener ends the accept loop, closed, and says so", "[vthost][acceptor]")
{
    // A listening handle that is gone fails every accept the same way, for good. Backed off on as a
    // passing shortage is, it was retried every 100 ms forever, and the daemon looked like it served.
    auto capture = core::log::ScopedCapture {};

    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto attempts = 0;
    auto closed = false;
    auto deadHandle = core::net::NetError { .code = core::net::NetErrorCode::BadHandle,
                                            .systemCode = EBADF,
                                            .context = "accept" };
    auto server = ConnectionAcceptor {
        loop,
        "native",
        std::make_unique<FailingAcceptListener>(&attempts, std::move(deadHandle), &closed),
        [](vthost::ConnectionId const&, std::unique_ptr<core::net::ISocket> conn) {
            return drainConnection(std::move(conn));
        }
    };
    loop.spawn(server.serve());
    loop.blockOn(core::net::testing::sleepFor(&loop, std::chrono::milliseconds { 250 }));

    CHECK(attempts == 1);
    CHECK(closed); // refusing a client, rather than queueing it for an accept that never comes
    CHECK(capture.contains("native: accept loop ended"));
    CHECK(capture.contains("errno 9")); // EBADF
}

TEST_CASE("a passing accept failure is backed off on, and accepting goes on", "[vthost][acceptor]")
{
    // EMFILE says the process ran short of descriptors, which passes: the loop waits, accepts again, and
    // serves the client that queued meanwhile.
    auto const tmp = core::testing::ScopedTempDir { "contour-muxsrv" };
    auto const socketPath = (tmp.path() / "sockets" / "exhausted").string();

    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto listener = core::net::listenUnix(loop, socketPath);
    REQUIRE(listener.has_value());

    auto seen = std::vector<std::string> {};
    auto server = ConnectionAcceptor {
        loop,
        "native",
        std::make_unique<core::net::testing::FailingListener>(
            std::move(*listener),
            core::net::testing::repeatedFailures(core::net::NetErrorCode::ResourceExhausted, 2)),
        [&seen](vthost::ConnectionId const&, std::unique_ptr<core::net::ISocket> conn) {
            return echoLineHandler(std::move(conn), &seen);
        }
    };
    loop.spawn(server.serve());

    auto echoed = false;
    loop.blockOn(clientRoundTrip(&loop, socketPath, "alpha", &echoed));

    CHECK(echoed);
    CHECK(server.acceptedCount() == 1);
    server.close();
}

TEST_CASE("an exception escaping a connection handler is reported, not reaped", "[vthost][acceptor]")
{
    // A connection flow is spawned as a ROOT task, and the loop reaps finished roots by destroying
    // their frames — so core::async::Task's captured exception_ptr went to the grave unread. The daemon
    // survived (which is right: one bad peer must not take it down) but said nothing at all, which
    // is how a real bug would hide. Surviving is still the behaviour; the silence is not.
    auto capture = core::log::ScopedCapture {};

    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto const tmp = core::testing::ScopedTempDir { "contour-muxsrv" };
    auto const socketPath = (tmp.path() / "sockets" / "throwing").string();
    auto listener = core::net::listenUnix(loop, socketPath);
    REQUIRE(listener.has_value());

    auto server =
        ConnectionAcceptor { loop,
                             "native",
                             std::move(*listener),
                             [&loop](vthost::ConnectionId const&, std::unique_ptr<core::net::ISocket> conn) {
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
    auto const emfile = core::net::NetError { .code = core::net::NetErrorCode::ResourceExhausted,
                                              .systemCode = EMFILE,
                                              .context = "accept" };

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
        auto const refused = core::net::NetError { .code = core::net::NetErrorCode::ConnRefused,
                                                   .systemCode = ECONNREFUSED,
                                                   .context = "accept" };
        CHECK(throttle.shouldLog(refused));
        CHECK(throttle.consecutive() == 1);
    }
}

#endif // !_WIN32
