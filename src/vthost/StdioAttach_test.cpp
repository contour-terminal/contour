// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.hpp>

#include <core/Utf8Path.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Sockets.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
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
#include <vthost/Daemon.hpp>
#include <vthost/DaemonStarter.hpp>
#include <vthost/NativeSession.hpp>
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

/// Serves one native connection's whole flow.
core::async::Task<void> serveNative(core::net::EventLoop* loop,
                                    vthost::SessionHost* host,
                                    vthost::ConnectionId id,
                                    std::unique_ptr<core::net::ISocket> connection)
{
    auto session = vthost::NativeSession { *loop, *host, std::move(id), std::move(connection) };
    co_await session.run();
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
    /// Binds the native socket beside @p controlSocket and returns once it accepts.
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
            auto acceptor =
                vthost::ConnectionAcceptor { loop,
                                             "native",
                                             std::move(*listener),
                                             [&loop, &host](vthost::ConnectionId const& id,
                                                            std::unique_ptr<core::net::ISocket> connection) {
                                                 return serveNative(&loop, &host, id, std::move(connection));
                                             } };
            loop.spawn(acceptor.serve());
            listening.set_value(std::nullopt);
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
    auto attach =
        std::async(std::launch::async, [&]() { return vthost::runStdioAttach(client.a, connector); });

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
        auto attach =
            std::async(std::launch::async, [&]() { return vthost::runStdioAttach(client.a, connector); });

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
