// SPDX-License-Identifier: Apache-2.0
#ifndef _WIN32

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
    #include <vthost/ConnectionAcceptor.h>

using coro::Task;
using net::EventLoop;
using vthost::ConnectionAcceptor;

namespace
{

namespace fs = std::filesystem;

/// A unique per-test directory under the system temp dir, removed on destruction.
struct TempDir
{
    fs::path path;

    TempDir()
    {
        auto templ = (fs::temp_directory_path() / "contour-muxsrv-XXXXXX").string();
        REQUIRE(::mkdtemp(templ.data()) != nullptr);
        path = templ;
    }

    ~TempDir()
    {
        auto ec = std::error_code {};
        fs::remove_all(path, ec);
    }

    TempDir(TempDir const&) = delete;
    TempDir& operator=(TempDir const&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
};

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

} // namespace

TEST_CASE("ConnectionAcceptor serves concurrent connections through the injected handler", "[vthost][server]")
{
    auto const tmp = TempDir {};
    auto const socketPath = (tmp.path / "sockets" / "default").string();

    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };

    auto listener = net::listenUnix(loop, socketPath);
    REQUIRE(listener.has_value());

    auto seen = std::vector<std::string> {};
    auto server =
        ConnectionAcceptor { loop, std::move(*listener), [&seen](std::unique_ptr<net::ISocket> conn) {
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
    auto server =
        ConnectionAcceptor { loop, std::make_unique<ExhaustedListener>(&attempts), vthost::drainConnection };
    loop.spawn(server.serve());

    // Every accept fails without suspending; serve() must yield between attempts
    // or this blockOn would never get the loop back (the livelock this guards
    // against). Within 50ms only the initial attempt fits into the backoff.
    loop.blockOn(net::testing::sleepFor(&loop, std::chrono::milliseconds { 50 }));
    CHECK(attempts <= 2);
}

TEST_CASE("closing the server ends the accept loop", "[vthost][server]")
{
    auto const tmp = TempDir {};
    auto const socketPath = (tmp.path / "sockets" / "default").string();

    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };

    auto listener = net::listenUnix(loop, socketPath);
    REQUIRE(listener.has_value());

    auto server = ConnectionAcceptor { loop, std::move(*listener), vthost::drainConnection };

    // Close before serving: the first accept resolves cancelled and serve returns.
    server.close();
    loop.blockOn(server.serve());

    CHECK(server.acceptedCount() == 0);
}

#endif // !_WIN32
