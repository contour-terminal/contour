// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.hpp>

#include <core/Utf8Path.hpp>
#include <core/log/LogSink.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IListener.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/WithTimeout.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <vthost/ConnectionAcceptor.hpp>
#include <vthost/DaemonSpawn.hpp>
#include <vthost/DaemonStarter.hpp>
#include <vthost/NativeSession.hpp>
#include <vthost/PlatformStreams.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/SocketPath.hpp>
#include <vthost/StdioAttach.hpp>
#include <vthost/StdioPreamble.hpp>
#include <vthost/proto/Pdu.hpp>
#include <vthost/proto/Wire.hpp>
#include <vthost/testing/MemoryByteStream.hpp>

namespace fs = std::filesystem;

using vthost::IByteStream;
using vthost::testing::makeMemoryStreamPair;

namespace
{

/// A directory for one test's socket, removed with it; terse, as sun_path is short.
struct SocketDirectory
{
    fs::path path = fs::temp_directory_path() / std::format("contour-s-{}", std::random_device {}());

    SocketDirectory() = default;
    ~SocketDirectory()
    {
        auto ec = std::error_code {};
        fs::remove_all(path, ec);
    }
    SocketDirectory(SocketDirectory const&) = delete;
    SocketDirectory& operator=(SocketDirectory const&) = delete;
    SocketDirectory(SocketDirectory&&) = delete;
    SocketDirectory& operator=(SocketDirectory&&) = delete;

    /// @return The control socket's path; the native one is beside it.
    [[nodiscard]] fs::path controlSocket() const { return path / "control"; }
};

void writeText(IByteStream& stream, std::string_view text)
{
    REQUIRE(stream.writeAll(std::as_bytes(std::span { text })).has_value());
}

/// Reads exactly @p count bytes, or fewer when the stream ends first.
[[nodiscard]] std::string readText(IByteStream& stream, std::size_t count)
{
    auto text = std::string {};
    auto buffer = std::vector<std::byte>(count);
    while (text.size() < count)
    {
        auto const read = stream.readSome(std::span { buffer }.first(count - text.size()));
        REQUIRE(read.has_value());
        if (*read == 0)
            break;
        for (auto const byte: std::span { buffer }.first(*read))
            text.push_back(static_cast<char>(byte));
    }
    return text;
}

/// Counts start() calls and answers them with @ref onStart.
class CountingStarter final: public vthost::DaemonStarter
{
  public:
    explicit CountingStarter(std::function<std::expected<void, std::string>()> onStart, int* calls):
        _onStart { std::move(onStart) }, _calls { calls }
    {
    }

    [[nodiscard]] std::expected<void, std::string> start() override
    {
        ++*_calls;
        return _onStart();
    }

  private:
    std::function<std::expected<void, std::string>()> _onStart;
    int* _calls;
};

/// Tells @p listening that the loop serves. Spawned after the acceptor's serve(), it runs once that flow has
/// suspended in its first accept: the ready queue is first-in first-out, and whenAny() starts its children
/// inline.
core::async::Task<void> announceServing(std::promise<std::optional<std::string>>* listening)
{
    listening->set_value(std::nullopt);
    co_return;
}

/// Lets the loop run until @p stop is requested.
core::async::Task<void> idleUntil(core::net::EventLoop* loop, std::stop_token stop)
{
    while (!stop.stop_requested())
        co_await loop->delay(std::chrono::milliseconds { 1 });
}

/// A daemon's native endpoint, served on a thread of its own by a SessionHost of mock PTYs.
class ServedDaemon
{
  public:
    /// How long a connection here has for its hello: well within a case's patience, so that a client whose
    /// half-close the daemon never sees still ends (@see the case "a client's half-close made before the
    /// daemon accepts reaches it"), while far beyond what a healthy hello takes.
    static constexpr auto HelloTimeout = std::chrono::milliseconds { 2'000 };

    /// Binds the native socket beside @p controlSocket and returns once it accepts.
    ///
    /// "Accepts" is an accept PARKED on the listener, not just a bound one: a client could otherwise connect
    /// into the backlog, and half-close, before the loop has taken its first turn. On Windows, an AF_UNIX
    /// half-close made before the AcceptEx is posted can be lost to the accepted socket, so the session would
    /// wait for a hello that never comes, and the client for the session to end (@see the case "a client's
    /// half-close made before the daemon accepts reaches it").
    explicit ServedDaemon(fs::path const& controlSocket)
    {
        auto listening = std::promise<std::optional<std::string>> {};
        auto bound = listening.get_future();
        // The promise moves into the thread: set_value() may still be running there when get() returns here.
        _thread = std::jthread { [listening = std::move(listening),
                                  path = core::pathToUtf8(vthost::nativeSocketPath(controlSocket))](
                                     std::stop_token stop) mutable {
            auto const source = core::net::makeDefaultBackend();
            auto loop = core::net::EventLoop { *source };
            auto listener = core::net::listenUnix(loop, path);
            if (!listener)
            {
                listening.set_value(listener.error().toString());
                return;
            }
            auto host = vthost::SessionHost {
                loop,
                [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                    return std::make_unique<vtpty::MockPty>(size);
                },
                vtbackend::Settings {},
                core::defaultEnvironment(),
                vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual }
            };
            auto services = vthost::NativeSessionServices {};
            services.handshake.helloTimeout = HelloTimeout;
            auto acceptor =
                vthost::ConnectionAcceptor { loop,
                                             "native",
                                             std::move(*listener),
                                             vthost::makeNativeHandler(loop, host, std::move(services)) };
            loop.spawn(acceptor.serve());
            loop.spawn(announceServing(&listening));
            loop.blockOn(idleUntil(&loop, std::move(stop)));
            acceptor.close();
        } };
        auto const failure = bound.get();
        INFO((failure ? *failure : std::string {}));
        REQUIRE_FALSE(failure.has_value());
    }

  private:
    std::jthread _thread;
};

/// The relay of a case against a ServedDaemon: its deadline outlasts the daemon's hello budget, so that a
/// daemon that never sees the client is still the side that ends the connection, as in production; and it
/// bounds every attach of the case, so that none waits for ever on a daemon that does not answer.
constexpr auto AttachRelay =
    vthost::RelayConfig { .daemonReplyDeadline = std::chrono::milliseconds { 10'000 } };

/// Encodes one ClientHello as a client's first frame.
[[nodiscard]] std::vector<std::byte> clientHelloFrame()
{
    auto request = vthost::proto::Writer {};
    vthost::proto::encodePdu(request, 1, vthost::proto::DecodedPdu { vthost::proto::ClientHello {} });
    return { request.view().begin(), request.view().end() };
}

/// Reads frames from @p stream until one is a ServerHello, or the stream ends.
/// @return Whether a ServerHello arrived.
[[nodiscard]] bool receivesServerHello(IByteStream& stream)
{
    auto pending = std::vector<std::byte> {};
    auto buffer = std::vector<std::byte>(4096);
    while (true)
    {
        auto const read = stream.readSome(buffer);
        if (!read || *read == 0)
            return false;
        pending.insert(pending.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(*read));
        while (true)
        {
            auto const frame = vthost::proto::decodePdu(pending);
            if (!frame)
                break;
            if (std::holds_alternative<vthost::proto::ServerHello>(frame->pdu))
                return true;
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(frame->consumed));
        }
    }
}

/// What the accepted end of a connection read first.
enum class FirstRead : std::uint8_t
{
    Nothing = 0, ///< Nothing within the deadline: the read is still parked.
    EndOfStream, ///< The peer's half-close.
    Bytes,       ///< Data.
    Failed,      ///< The accept or the read failed.
};

/// Accepts one connection on @p listener, and reads it once, for at most @p deadline.
core::async::Task<void> acceptAndReadOnce(core::net::EventLoop* loop,
                                          core::net::IListener* listener,
                                          std::chrono::milliseconds deadline,
                                          FirstRead* result)
{
    *result = FirstRead::Failed;
    auto accepted = co_await listener->accept();
    if (!accepted)
        co_return;
    auto* const socket = accepted->get();
    auto const read = [](core::net::ISocket* socket, FirstRead* result) -> core::async::Task<void> {
        auto buffer = std::array<std::byte, 16> {};
        auto const count = co_await socket->read(buffer);
        if (!count)
            *result = FirstRead::Failed;
        else if (*count == 0)
            *result = FirstRead::EndOfStream;
        else
            *result = FirstRead::Bytes;
    };
    *result = FirstRead::Nothing;
    std::ignore = co_await core::net::withTimeout(loop, read(socket, result), deadline);
}

} // namespace

TEST_CASE("stdio attach writes the ready preamble and nothing else before the client speaks",
          "[vthost][stdio]")
{
    auto const client = makeMemoryStreamPair();
    auto const daemon = makeMemoryStreamPair();
    auto attach = std::async(std::launch::async, [&]() {
        return vthost::runStdioAttach(
            client.a, [&]() -> std::expected<std::shared_ptr<IByteStream>, std::string> { return daemon.a; });
    });

    CHECK(readText(*client.b, vthost::StdioPreambleReady.size()) == vthost::StdioPreambleReady);
    auto more = std::async(std::launch::async, [&]() { return readText(*client.b, 1); });
    CHECK(more.wait_for(std::chrono::milliseconds { 100 }) == std::future_status::timeout);

    writeText(*client.b, "ping");
    CHECK(readText(*daemon.b, 4) == "ping");

    daemon.b->closeWrite();
    CHECK(attach.get() == EXIT_SUCCESS);
    CHECK(more.get().empty()); // the client's stream ended with the daemon's, and nothing came before
    client.b->closeWrite();    // lets the relay's client→daemon thread finish
}

TEST_CASE("stdio attach gives up, by name, on a daemon that sends nothing", "[vthost][stdio]")
{
    auto capture = core::log::ScopedCapture {};
    auto const client = makeMemoryStreamPair();
    auto const daemon = makeMemoryStreamPair();
    auto attach = std::async(std::launch::async, [&]() {
        return vthost::runStdioAttach(
            client.a,
            [&]() -> std::expected<std::shared_ptr<IByteStream>, std::string> { return daemon.a; },
            vthost::RelayConfig { .daemonReplyDeadline = std::chrono::milliseconds { 50 } });
    });

    CHECK(readText(*client.b, vthost::StdioPreambleReady.size()) == vthost::StdioPreambleReady);
    writeText(*client.b, "hello");
    CHECK(readText(*daemon.b, 5) == "hello"); // delivered, and never answered

    CHECK(attach.get() == EXIT_FAILURE);
    CHECK(readText(*client.b, 1).empty()); // the client's stream ends, with no stray bytes
    CHECK(capture.contains("daemon sent nothing within 0.05s"));

    client.b->closeWrite(); // lets the relay's client→daemon thread finish
    daemon.b->closeWrite(); // lets its daemon→client thread finish
}

TEST_CASE("stdio attach reports a connector failure as an error preamble", "[vthost][stdio]")
{
    auto const client = makeMemoryStreamPair();
    auto const result =
        vthost::runStdioAttach(client.a, []() -> std::expected<std::shared_ptr<IByteStream>, std::string> {
            return std::unexpected { std::string { "daemon did not start within 5 seconds." } };
        });

    CHECK(result == EXIT_FAILURE);
    auto const expected =
        std::string_view { "contour-stdio 1 error: daemon did not start within 5 seconds.\n" };
    CHECK(readText(*client.b, expected.size() + 1) == expected);
}

TEST_CASE("stdio attach reaches a real daemon", "[vthost][stdio][afunix]")
{
    auto const directory = SocketDirectory {};
    auto const daemon = ServedDaemon { directory.controlSocket() };
    auto const spawner = vthost::makeNativeDaemonSpawner();
    auto starts = 0;
    auto connector = vthost::makeLocalDaemonConnector(
        directory.controlSocket(),
        *spawner,
        std::make_unique<CountingStarter>([]() -> std::expected<void, std::string> { return {}; }, &starts));

    auto const client = makeMemoryStreamPair();
    auto attach = std::async(std::launch::async,
                             [&]() { return vthost::runStdioAttach(client.a, connector, AttachRelay); });

    REQUIRE(readText(*client.b, vthost::StdioPreambleReady.size()) == vthost::StdioPreambleReady);
    REQUIRE(client.b->writeAll(clientHelloFrame()).has_value());
    CHECK(receivesServerHello(*client.b));
    CHECK(starts == 0); // a serving daemon is not started again

    client.b->closeWrite(); // the daemon sees the client leave, and closes its side
    CHECK(attach.get() == EXIT_SUCCESS);
}

TEST_CASE("stdio attach starts the daemon through the starter when none serves", "[vthost][stdio][afunix]")
{
    auto const directory = SocketDirectory {};
    auto const spawner = vthost::makeNativeDaemonSpawner();
    auto starts = 0;

    SECTION("a starter that binds the daemon")
    {
        auto daemon = std::optional<ServedDaemon> {};
        auto connector = vthost::makeLocalDaemonConnector(directory.controlSocket(),
                                                          *spawner,
                                                          std::make_unique<CountingStarter>(
                                                              [&]() -> std::expected<void, std::string> {
                                                                  daemon.emplace(directory.controlSocket());
                                                                  return {};
                                                              },
                                                              &starts));

        auto const client = makeMemoryStreamPair();
        auto attach = std::async(std::launch::async,
                                 [&]() { return vthost::runStdioAttach(client.a, connector, AttachRelay); });

        CHECK(readText(*client.b, vthost::StdioPreambleReady.size()) == vthost::StdioPreambleReady);
        CHECK(starts == 1);
        client.b->closeWrite();
        CHECK(attach.get() == EXIT_SUCCESS);
    }

    SECTION("a starter that fails")
    {
        auto connector = vthost::makeLocalDaemonConnector(
            directory.controlSocket(),
            *spawner,
            std::make_unique<CountingStarter>(
                []() -> std::expected<void, std::string> {
                    return std::unexpected { std::string { "daemon did not start within 5 seconds." } };
                },
                &starts));

        auto const client = makeMemoryStreamPair();
        CHECK(vthost::runStdioAttach(client.a, connector) == EXIT_FAILURE);
        CHECK(starts == 1);
        auto const expected =
            std::string_view { "contour-stdio 1 error: daemon did not start within 5 seconds.\n" };
        CHECK(readText(*client.b, expected.size() + 1) == expected);
    }
}

// What the stdio relay rests on: the daemon learns that the client is gone from the client's half-close, and
// ends the connection, which ends the relay. Here the half-close is made while the connection still waits in
// the listener's backlog, before any accept is posted -- the order a scheduler can produce whenever a client
// is quicker than the daemon's first loop turn. A daemon that never sees that half-close would leave the
// client waiting for ever. [!mayfail] while it is not known to hold on Windows' AF_UNIX: it reports there,
// rather than failing the suite. @see ServedDaemon, which no longer lets its own cases reach this order.
TEST_CASE("a client's half-close made before the daemon accepts reaches it",
          "[vthost][stdio][afunix][!mayfail]")
{
    auto const directory = SocketDirectory {};
    auto const path = vthost::nativeSocketPath(directory.controlSocket());
    auto const backend = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *backend };
    auto listener = core::net::listenUnix(loop, core::pathToUtf8(path));
    REQUIRE(listener.has_value());

    auto const client = vthost::connectUnixStream(path);
    REQUIRE(client.has_value());
    (*client)->closeWrite(); // into the backlog: nothing has accepted it yet

    auto firstRead = FirstRead::Failed;
    loop.blockOn(acceptAndReadOnce(&loop, listener->get(), std::chrono::seconds { 5 }, &firstRead));
    CHECK(firstRead == FirstRead::EndOfStream);
}
