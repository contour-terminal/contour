// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.hpp>
#include <vtpty/ProgramLookup.hpp>

#include <core/async/Task.hpp>
#include <core/async/WhenAll.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/InMemoryTransport.hpp>
#include <core/platform/NativeFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <vthost/HostIdentity.hpp>
#include <vthost/NativeSession.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/SshTransport.hpp>
#include <vthost/StdioPreamble.hpp>
#include <vthost/client/NativeClient.hpp>

using vthost::SshFailure;
using vthost::SshTarget;
using vthost::TransportErrorKind;
using vthost::TransportRetry;
using namespace std::chrono_literals;

namespace
{

/// What a test scripts for, and observes of, one fake ssh child. Shared, as the child dies first.
struct FakeChildState
{
    std::string stderrTail;
    std::optional<int> exitCode;
    int terminated = 0; ///< How often the child was ended while it ran: terminate() is idempotent.
};

/// An ssh child that only reports what its state scripts.
class FakeStdioChild final: public vthost::IStdioChild
{
  public:
    explicit FakeStdioChild(std::shared_ptr<FakeChildState> state): _state(std::move(state)) {}
    ~FakeStdioChild() override { terminate(); }

    FakeStdioChild(FakeStdioChild const&) = delete;
    FakeStdioChild& operator=(FakeStdioChild const&) = delete;
    FakeStdioChild(FakeStdioChild&&) = delete;
    FakeStdioChild& operator=(FakeStdioChild&&) = delete;

    [[nodiscard]] std::string stderrTail() const override { return _state->stderrTail; }
    [[nodiscard]] std::optional<int> exitCode(std::chrono::milliseconds /*wait*/) override
    {
        return _state->exitCode;
    }
    void terminate() noexcept override
    {
        if (_isRunning)
            ++_state->terminated;
        _isRunning = false;
    }

  private:
    std::shared_ptr<FakeChildState> _state;
    bool _isRunning = true;
};

/// A spawner handing out prepared children in order; the test plays the remote on each one's far end.
class FakeStdioSpawner final: public vthost::IStdioSpawner
{
  public:
    /// One child the next spawn() hands out.
    struct Prepared
    {
        std::unique_ptr<core::net::ISocket> transport;
        std::shared_ptr<FakeChildState> state;
    };

    std::deque<Prepared> prepared;
    std::vector<std::vector<std::string>> spawned; ///< The argv of every spawn, in order.
    std::optional<vthost::StdioSpawnError> failure;

    [[nodiscard]] std::expected<vthost::SpawnedStdioChild, vthost::StdioSpawnError> spawn(
        core::net::EventLoop& /*loop*/, std::span<std::string const> argv) override
    {
        spawned.emplace_back(argv.begin(), argv.end());
        if (failure)
            return std::unexpected(*failure);
        REQUIRE(!prepared.empty());
        auto next = std::move(prepared.front());
        prepared.pop_front();
        return vthost::SpawnedStdioChild { .transport = std::move(next.transport),
                                           .process = std::make_unique<FakeStdioChild>(next.state) };
    }
};

/// Writes @p text to @p socket.
core::async::Task<void> writeText(core::net::ISocket* socket, std::string text)
{
    auto const written = co_await socket->write(std::as_bytes(std::span { text }));
    REQUIRE(written.has_value());
}

/// Connects through @p factory while @p remote writes @p text, which may be more than a socket buffer holds
/// (8 KiB on macOS): written ahead of the connect, it would wait forever for a reader.
/// @param out Receives what the connect returned.
core::async::Task<void> connectWhileWriting(
    core::net::EventLoop* loop,
    vthost::SshTransportFactory* factory,
    core::net::ISocket* remote,
    std::string text,
    std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>* out)
{
    auto const offer = [](core::net::ISocket* socket, std::string bytes) -> core::async::Task<void> {
        // The connect may stop reading, and close its end, before every byte is written.
        std::ignore = co_await socket->write(std::as_bytes(std::span { bytes }));
    };
    auto const connect = [](core::net::EventLoop* loop,
                            vthost::SshTransportFactory* factory,
                            std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>* out)
        -> core::async::Task<void> {
        *out = co_await factory->connect(*loop);
    };
    co_await core::async::whenAll(offer(remote, std::move(text)), connect(loop, factory, out));
}

/// Reads @p socket until it ends.
/// @return How many bytes arrived before it did.
core::async::Task<std::size_t> drain(core::net::ISocket* socket)
{
    auto buffer = std::vector<std::byte>(256);
    auto total = std::size_t { 0 };
    while (true)
    {
        auto const read = co_await socket->read(buffer);
        if (!read.has_value() || *read == 0)
            co_return total;
        total += *read;
    }
}

/// A loop, the fake spawner and a factory over it.
struct SshFixture
{
    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *source };
    std::shared_ptr<FakeStdioSpawner> spawner = std::make_shared<FakeStdioSpawner>();
    SshTarget target { .user = "me", .host = "homepc", .port = std::nullopt };

    /// Prepares one child and @return the remote's end of its transport.
    [[nodiscard]] std::pair<std::unique_ptr<core::net::ISocket>, std::shared_ptr<FakeChildState>> prepare(
        FakeChildState script = {})
    {
        auto pair = *core::net::testing::makeSocketPair(loop);
        auto state = std::make_shared<FakeChildState>(std::move(script));
        spawner->prepared.push_back({ .transport = std::move(pair.second), .state = state });
        return { std::move(pair.first), state };
    }

    [[nodiscard]] vthost::SshTransportFactory factory(
        std::chrono::milliseconds timeout = vthost::SshConnectTimeout) const
    {
        return vthost::SshTransportFactory { target, spawner, timeout };
    }
};

/// @return Every SshFailure.
[[nodiscard]] auto allSshFailures()
{
    return std::views::iota(std::uint8_t { 0 },
                            static_cast<std::uint8_t>(std::to_underlying(SshFailure::NoPreamble) + 1))
           | std::views::transform([](std::uint8_t value) { return static_cast<SshFailure>(value); });
}

} // namespace

TEST_CASE("sshArguments spells batch mode and ends options before the destination", "[vthost][ssh]")
{
    auto const options = std::vector<std::string> {
        "ssh", "-T",
        "-o",  "BatchMode=yes",
        "-o",  "ConnectTimeout=10",
        "-o",  "ServerAliveInterval=15",
        "-o",  "ServerAliveCountMax=3",
    };
    auto const expect = [&](std::vector<std::string> const& tail) {
        auto argv = options;
        argv.insert(argv.end(), tail.begin(), tail.end());
        return argv;
    };

    CHECK(vthost::sshArguments({ .user = "me", .host = "homepc", .port = std::nullopt })
          == expect({ "--", "me@homepc", "contour daemon --stdio" }));
    CHECK(vthost::sshArguments({ .user = "", .host = "homepc", .port = 2222 })
          == expect({ "-p", "2222", "--", "homepc", "contour daemon --stdio" }));
    CHECK(vthost::sshArguments({ .user = "", .host = "fe80::1", .port = std::nullopt })
          == expect({ "--", "fe80::1", "contour daemon --stdio" }));
    CHECK(vthost::sshArguments({ .user = "",
                                 .host = "homepc",
                                 .port = std::nullopt,
                                 .remoteCommand = "/opt/contour/bin/contour daemon --stdio" })
              .back()
          == "/opt/contour/bin/contour daemon --stdio");
    // A destination that looks like an option still comes after the "--".
    auto const optionLike =
        vthost::sshArguments({ .user = "", .host = "-oProxyCommand=evil", .port = std::nullopt });
    CHECK(optionLike.at(optionLike.size() - 3) == "--");
}

TEST_CASE("interactiveSshCommand names what to run once", "[vthost][ssh]")
{
    CHECK(vthost::interactiveSshCommand({ .user = "me", .host = "homepc", .port = std::nullopt })
          == "ssh me@homepc");
    CHECK(vthost::interactiveSshCommand({ .user = "", .host = "homepc", .port = 2222 })
          == "ssh -p 2222 homepc");
}

TEST_CASE("describeSshTarget spells an ssh URL", "[vthost][ssh]")
{
    CHECK(vthost::describeSshTarget({ .user = "me", .host = "homepc", .port = 2222 })
          == "ssh://me@homepc:2222");
    CHECK(vthost::describeSshTarget({ .user = "", .host = "homepc", .port = std::nullopt })
          == "ssh://homepc");
    CHECK(vthost::describeSshTarget({ .user = "", .host = "fe80::1", .port = 22 }) == "ssh://[fe80::1]:22");
}

TEST_CASE("classifySshFailure", "[vthost][ssh]")
{
    struct Row
    {
        std::string_view stderrTail;
        std::optional<int> exitCode;
        SshFailure expected;
    };
    auto const rows = std::to_array<Row>({
        { .stderrTail = "Host key verification failed.",
          .exitCode = 255,
          .expected = SshFailure::HostKeyUnknown },
        { .stderrTail = "@@@ WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED! @@@",
          .exitCode = 255,
          .expected = SshFailure::HostKeyChanged },
        { .stderrTail = "me@homepc: Permission denied (publickey).",
          .exitCode = 255,
          .expected = SshFailure::AuthenticationRequired },
        { .stderrTail = "Permission denied, please try again.",
          .exitCode = 255,
          .expected = SshFailure::AuthenticationRequired },
        { .stderrTail = "bash: line 1: /opt/contour/bin/contour: Permission denied",
          .exitCode = 126,
          .expected = SshFailure::NoPreamble },
        { .stderrTail = "Received disconnect from 10.0.0.2 port 22:2: Too many authentication failures",
          .exitCode = 255,
          .expected = SshFailure::AuthenticationRequired },
        { .stderrTail = "ssh: Could not resolve hostname homepc: Name or service not known",
          .exitCode = 255,
          .expected = SshFailure::HostUnreachable },
        { .stderrTail = "ssh: connect to host homepc port 22: Connection refused",
          .exitCode = 255,
          .expected = SshFailure::HostUnreachable },
        { .stderrTail = "ssh: connect to host homepc port 22: Connection timed out",
          .exitCode = 255,
          .expected = SshFailure::HostUnreachable },
        { .stderrTail = "ssh: connect to host homepc port 22: No route to host",
          .exitCode = 255,
          .expected = SshFailure::HostUnreachable },
        { .stderrTail = "bash: line 1: contour: command not found",
          .exitCode = 127,
          .expected = SshFailure::RemoteContourMissing },
        { .stderrTail = "'contour' is not recognized as an internal or external command,",
          .exitCode = 1,
          .expected = SshFailure::RemoteContourMissing },
        { .stderrTail = "Unexpected argument \"--stdio\".",
          .exitCode = 1,
          .expected = SshFailure::RemoteContourTooOld },
        { .stderrTail = "", .exitCode = 127, .expected = SshFailure::RemoteContourMissing },
        { .stderrTail = "", .exitCode = 255, .expected = SshFailure::NoPreamble },
        { .stderrTail = "", .exitCode = std::nullopt, .expected = SshFailure::NoPreamble },
        { .stderrTail = "", .exitCode = vthost::ExitStatusLost, .expected = SshFailure::NoPreamble },
    });
    for (auto const& row: rows)
    {
        INFO(row.stderrTail);
        CHECK(vthost::classifySshFailure(row.stderrTail, row.exitCode) == row.expected);
    }
}

TEST_CASE("every ssh failure has a message naming the host", "[vthost][ssh]")
{
    auto const target = SshTarget { .user = "me", .host = "homepc", .port = std::nullopt };
    for (auto const failure: allSshFailures())
    {
        INFO(static_cast<int>(failure));
        auto const error = vthost::sshTransportError(failure, target, "x");
        CHECK(error.message.contains("homepc"));
        if (failure == SshFailure::HostKeyUnknown || failure == SshFailure::AuthenticationRequired)
            CHECK(error.message.contains(vthost::interactiveSshCommand(target)));
    }
    CHECK(vthost::sshTransportError(SshFailure::NoPreamble, target, "gone", 255).message
          == "ssh to ssh://me@homepc ended before Contour answered (exit code 255): gone");
    CHECK(vthost::sshTransportError(SshFailure::NoPreamble, target, "gone", vthost::ExitStatusLost).message
          == "ssh to ssh://me@homepc ended before Contour answered (exit code unknown): gone");
    CHECK(vthost::sshTransportError(
              SshFailure::HostKeyChanged, { .user = "", .host = "homepc", .port = 2222 }, "x")
              .message.contains("ssh-keygen -R '[homepc]:2222'"));
}

TEST_CASE("ssh failures retry only when retrying can help", "[vthost][ssh]")
{
    auto const target = SshTarget { .user = "me", .host = "homepc", .port = std::nullopt };
    for (auto const failure: allSshFailures())
    {
        INFO(static_cast<int>(failure));
        auto const expected = failure == SshFailure::HostUnreachable || failure == SshFailure::NoPreamble
                                  ? TransportRetry::Transient
                                  : TransportRetry::Permanent;
        auto const kind = vthost::sshTransportError(failure, target, "x").kind;
        CHECK(vthost::retryOf(kind) == expected);
        // The remote relay starts its daemon itself: an absent one is never this transport's answer.
        CHECK(kind != TransportErrorKind::DaemonAbsent);
    }
}

TEST_CASE("ssh connect succeeds after login noise", "[vthost][ssh]")
{
    auto fixture = SshFixture {};
    auto [remote, state] = fixture.prepare();
    fixture.loop.blockOn(writeText(remote.get(), std::format("Welcome\n{}", vthost::StdioPreambleReady)));

    auto factory = fixture.factory();
    auto connection = fixture.loop.blockOn(factory.connect(fixture.loop));
    INFO((connection ? std::string {} : connection.error().message));
    REQUIRE(connection.has_value());
    CHECK((*connection)->token.empty());
    CHECK((*connection)->socket != nullptr);
    CHECK(state->terminated == 0);
    REQUIRE(fixture.spawner->spawned.size() == 1);
    CHECK(fixture.spawner->spawned.front() == vthost::sshArguments(fixture.target));
    CHECK(factory.describe() == "ssh://me@homepc");
    // ssh verifies the host key, so a control client may send this transport its token.
    CHECK(factory.peerTrust() == vthost::PeerTrust::Verified);
}

TEST_CASE("ssh connect names a refused login and the command to run once", "[vthost][ssh]")
{
    auto fixture = SshFixture {};
    auto [remote, state] =
        fixture.prepare({ .stderrTail = "me@homepc: Permission denied (publickey).\n", .exitCode = 255 });
    remote.reset();

    auto factory = fixture.factory();
    auto const connection = fixture.loop.blockOn(factory.connect(fixture.loop));
    REQUIRE_FALSE(connection.has_value());
    CHECK(connection.error().kind == TransportErrorKind::AuthenticationRequired);
    CHECK(connection.error().message.contains("ssh me@homepc"));
    CHECK(state->terminated == 1);
}

TEST_CASE("ssh connect reports what ssh said when it could not reach the host", "[vthost][ssh]")
{
    auto fixture = SshFixture {};
    auto [remote, state] = fixture.prepare(
        { .stderrTail = "debug\nssh: connect to host homepc port 22: Connection refused\r\n\n",
          .exitCode = 255 });
    remote.reset();

    auto factory = fixture.factory();
    auto const connection = fixture.loop.blockOn(factory.connect(fixture.loop));
    REQUIRE_FALSE(connection.has_value());
    CHECK(connection.error().kind == TransportErrorKind::Unreachable);
    CHECK(connection.error().message
          == "ssh could not reach ssh://me@homepc: ssh: connect to host homepc port 22: Connection refused");
    CHECK(state->terminated == 1);
}

TEST_CASE("ssh connect passes the relay's error through", "[vthost][ssh]")
{
    auto fixture = SshFixture {};
    auto [remote, state] = fixture.prepare();
    fixture.loop.blockOn(
        writeText(remote.get(), vthost::stdioPreambleError("daemon did not start within 5 seconds.")));

    auto factory = fixture.factory();
    auto const connection = fixture.loop.blockOn(factory.connect(fixture.loop));
    REQUIRE_FALSE(connection.has_value());
    CHECK(connection.error().kind == TransportErrorKind::Refused);
    CHECK(connection.error().message.contains("daemon did not start within 5 seconds."));
    CHECK(state->terminated == 1);
}

TEST_CASE("ssh connect refuses bytes after the preamble", "[vthost][ssh]")
{
    auto fixture = SshFixture {};
    auto [remote, state] = fixture.prepare();
    fixture.loop.blockOn(writeText(remote.get(), std::format("{}junk", vthost::StdioPreambleReady)));

    auto factory = fixture.factory();
    auto const connection = fixture.loop.blockOn(factory.connect(fixture.loop));
    REQUIRE_FALSE(connection.has_value());
    CHECK(connection.error().kind == TransportErrorKind::Protocol);
    CHECK(connection.error().message == "unexpected bytes after the stdio preamble from ssh://me@homepc");
    CHECK(state->terminated == 1);
}

TEST_CASE("ssh connect names a login shell that prints too much", "[vthost][ssh]")
{
    auto fixture = SshFixture {};
    auto [remote, state] = fixture.prepare();

    auto factory = fixture.factory();
    auto connection = std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError> {};
    fixture.loop.blockOn(connectWhileWriting(&fixture.loop,
                                             &factory,
                                             remote.get(),
                                             std::string(vthost::StdioPreambleScanLimit + 1, 'x'),
                                             &connection));
    REQUIRE_FALSE(connection.has_value());
    CHECK(connection.error().kind == TransportErrorKind::Refused);
    CHECK(connection.error().message.contains(".bashrc"));
    CHECK(state->terminated == 1);
}

TEST_CASE("ssh connect gives up on a silent remote", "[vthost][ssh]")
{
    auto fixture = SshFixture {};
    auto [remote, state] = fixture.prepare();

    auto factory = fixture.factory(50ms);
    auto const connection = fixture.loop.blockOn(factory.connect(fixture.loop));
    REQUIRE_FALSE(connection.has_value());
    CHECK(connection.error().kind == TransportErrorKind::Unreachable);
    CHECK(connection.error().message == "ssh://me@homepc did not answer within 1 seconds");
    CHECK(state->terminated == 1);
}

TEST_CASE("ssh connect names a missing ssh client", "[vthost][ssh]")
{
    auto fixture = SshFixture {};
    fixture.spawner->failure = vthost::StdioSpawnError { .kind = vthost::SpawnFailure::ProgramNotFound,
                                                         .message = "ssh: not found" };

    auto factory = fixture.factory();
    auto const connection = fixture.loop.blockOn(factory.connect(fixture.loop));
    REQUIRE_FALSE(connection.has_value());
    CHECK(connection.error().kind == TransportErrorKind::ProgramNotFound);
    CHECK(connection.error().message == "ssh was not found on PATH; install the OpenSSH client");
}

TEST_CASE("each connect spawns afresh and a dropped connection reaps its ssh", "[vthost][ssh]")
{
    auto fixture = SshFixture {};
    auto [firstRemote, firstState] = fixture.prepare();
    auto [secondRemote, secondState] = fixture.prepare();
    fixture.loop.blockOn(writeText(firstRemote.get(), std::string { vthost::StdioPreambleReady }));
    fixture.loop.blockOn(writeText(secondRemote.get(), std::string { vthost::StdioPreambleReady }));

    auto factory = fixture.factory();
    auto first = fixture.loop.blockOn(factory.connect(fixture.loop));
    REQUIRE(first.has_value());

    // The connection drops (sleep, Wi-Fi): the remote end goes away and the socket reads EOF.
    firstRemote.reset();
    CHECK(fixture.loop.blockOn(drain((*first)->socket.get())) == 0);
    CHECK(firstState->terminated == 0);
    first->reset();
    CHECK(firstState->terminated == 1);

    // A reconnect calls connect() again, which spawns a new ssh.
    auto second = fixture.loop.blockOn(factory.connect(fixture.loop));
    REQUIRE(second.has_value());
    CHECK(fixture.spawner->spawned.size() == 2);
    CHECK(secondState->terminated == 0);
}

namespace
{
/// Waits until @p client knows its daemon's identity, then detaches it.
core::async::Task<void> awaitIdentity(core::net::EventLoop* loop, vthost::client::NativeClient* client)
{
    co_await core::net::testing::waitUntil(loop, [client] { return !client->screens().empty(); });
    client->detach();
}
} // namespace

TEST_CASE("an ssh attach keeps a remote daemon's paths foreign", "[vthost][ssh]")
{
    using vthost::client::NativeClient;
    auto fixture = SshFixture {};
    auto const daemon =
        vtbackend::HostIdentity { .machineId = "3deb5353d3ba43d08201c136a47ead7b", .hostname = "zeta" };
    auto host =
        vthost::SessionHost { fixture.loop,
                              [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                  return std::make_unique<vtpty::MockPty>(size);
                              },
                              vtbackend::Settings {},
                              core::defaultEnvironment(),
                              vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                                          .sizePolicy = vthost::ClientSizePolicy::Latest,
                                                          .startupLayout = vtworkspace::Layout {},
                                                          .identity = daemon } };
    host.createTab();

    // The remote relay: the preamble first, then the native server on the same byte stream.
    auto [remote, state] = fixture.prepare();
    fixture.loop.blockOn(writeText(remote.get(), std::string { vthost::StdioPreambleReady }));
    auto factory = fixture.factory();
    auto connection = fixture.loop.blockOn(factory.connect(fixture.loop));
    REQUIRE(connection.has_value());

    auto server = vthost::NativeSession {
        fixture.loop, host, vthost::ConnectionId { .endpoint = "ssh", .index = 1 }, std::move(remote)
    };
    auto client = NativeClient { fixture.loop,
                                 std::move((*connection)->socket),
                                 NativeClient::HandshakeOptions {},
                                 vtbackend::CommandBlockStoreLimits {},
                                 NativeClient::UpdateHandler {},
                                 NativeClient::ImageHandler {},
                                 NativeClient::SessionEventHandler {},
                                 NativeClient::LayoutHandler {} };
    fixture.loop.blockOn(
        core::net::testing::allOf(server.run(), client.run(), awaitIdentity(&fixture.loop, &client)));

    auto const self =
        vtbackend::LocalIdentity { .machineId = "0123456789abcdef0123456789abcdef", .hostname = "eta" };
    CHECK(client.daemonIdentity() == vthost::appSpecificIdentity(daemon.view()));
    CHECK(vthost::daemonMachineOf(client.daemonIdentity(), self) == vthost::DaemonMachine::Other);
}

TEST_CASE("a real ssh to a port nobody serves reads as an unreachable host", "[vthost][ssh][spawn]")
{
    // The real spawner and the real ssh client: what ssh prints and exits with is classified as the
    // fakes above assume. Port 1 (tcpmux) has no listener, so the connect is refused at once.
    auto const searchPath = vtpty::execSearchPath(std::nullopt);
    auto nativeFileSystem = core::platform::NativeFileSystem { core::platform::nativeRename() };
    auto const spawner =
        std::shared_ptr<vthost::IStdioSpawner> { vthost::makeSystemStdioSpawner(vtpty::ProgramSearch {
            .path = searchPath, .workingDirectory = {}, .fileSystem = nativeFileSystem }) };
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto factory =
        vthost::SshTransportFactory { SshTarget { .user = "", .host = "127.0.0.1", .port = 1 }, spawner };

    auto const connection = loop.blockOn(factory.connect(loop));
    REQUIRE_FALSE(connection.has_value());
    if (connection.error().kind == TransportErrorKind::ProgramNotFound)
        SKIP("no ssh client on PATH");
    INFO(connection.error().message);
    CHECK(connection.error().kind == TransportErrorKind::Unreachable);
    CHECK(connection.error().message.starts_with("ssh could not reach ssh://127.0.0.1:1: "));
    CHECK(connection.error().message.contains("Connection refused"));
}
