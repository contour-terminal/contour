// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.hpp>
#include <vtpty/ProgramLookup.hpp>

#include <core/async/Task.hpp>
#include <core/async/WhenAll.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoAwaitable.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/IoResult.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/InMemoryTransport.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/NativeFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

#include <vthost/Askpass.hpp>
#include <vthost/HostIdentity.hpp>
#include <vthost/NativeSession.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/SshTransport.hpp>
#include <vthost/StdioPreamble.hpp>
#include <vthost/client/NativeClient.hpp>

using vthost::SshFailure;
using vthost::SshPromptMode;
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
    int terminated = 0;   ///< How often the child was ended while it ran: terminate() is idempotent.
    int runsForPolls = 0; ///< exitCode() reports nullopt for this many calls before the exit code.
    int polls = 0;
    std::vector<std::chrono::milliseconds> waits = {}; ///< Every wait exitCode() was asked to block for.
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

    [[nodiscard]] std::string stderrTail() const override
    {
        // A child that still runs has said nothing yet.
        return _state->polls <= _state->runsForPolls && _state->runsForPolls > 0 ? std::string {}
                                                                                 : _state->stderrTail;
    }
    [[nodiscard]] std::optional<int> exitCode(std::chrono::milliseconds wait) override
    {
        _state->waits.push_back(wait);
        if (++_state->polls <= _state->runsForPolls)
            return std::nullopt;
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
    std::vector<std::vector<std::string>> spawned;             ///< The argv of every spawn, in order.
    std::vector<vthost::SpawnEnvironment> spawnedEnvironments; ///< The environment of every spawn.
    std::optional<vthost::StdioSpawnError> failure;

    [[nodiscard]] std::expected<vthost::SpawnedStdioChild, vthost::StdioSpawnError> spawn(
        core::net::EventLoop& /*loop*/,
        std::span<std::string const> argv,
        vthost::SpawnEnvironment const& environment) override
    {
        spawned.emplace_back(argv.begin(), argv.end());
        spawnedEnvironments.push_back(environment);
        if (failure)
            return std::unexpected(*failure);
        REQUIRE(!prepared.empty());
        auto next = std::move(prepared.front());
        prepared.pop_front();
        return vthost::SpawnedStdioChild { .transport = std::move(next.transport),
                                           .process = std::make_unique<FakeStdioChild>(next.state) };
    }
};

/// What a test scripts for, and observes of, the askpass session a fake broker hands out.
struct FakeAskpassState
{
    bool wasCancelled = false;
    std::chrono::milliseconds promptTime { 0 };
    std::function<void()> wake; ///< What the transport asked to be told when a prompt is dismissed.

    /// The user dismisses a prompt: the attempt is cancelled, and the transport is told.
    void cancel()
    {
        wasCancelled = true;
        if (wake)
            std::exchange(wake, nullptr)();
    }
};

/// A session reporting what its state scripts.
class FakeAskpassSession final: public vthost::IAskpassSession
{
  public:
    explicit FakeAskpassSession(std::shared_ptr<FakeAskpassState> state): _state(std::move(state)) {}

    [[nodiscard]] vthost::SpawnEnvironment environment() const override
    {
        return { .set = { { "SSH_ASKPASS_REQUIRE", "force" } } };
    }
    void onCancelled(std::function<void()> wake) override
    {
        if (_state->wasCancelled)
            wake();
        else
            _state->wake = std::move(wake);
    }
    [[nodiscard]] bool wasCancelled() const noexcept override { return _state->wasCancelled; }
    [[nodiscard]] std::chrono::milliseconds promptTime() const noexcept override
    {
        return _state->promptTime;
    }

  private:
    std::shared_ptr<FakeAskpassState> _state;
};

/// A broker handing out sessions over one shared state, and counting them.
class FakeAskpassBroker final: public vthost::IAskpassBroker
{
  public:
    std::shared_ptr<FakeAskpassState> state = std::make_shared<FakeAskpassState>();
    int opened = 0;
    bool isOpenFailing = false;

    [[nodiscard]] std::expected<std::unique_ptr<vthost::IAskpassSession>, std::string> open(
        SshTarget const& /*target*/) override
    {
        ++opened;
        if (isOpenFailing)
            return std::unexpected { std::string { "no socket" } };
        return std::make_unique<FakeAskpassSession>(state);
    }
};

/// The ssh transport of a remote that never answers, for a loop with no I/O behind it.
///
/// A TestLoop runs on a NullBackend, and makeSocketPair's real sockets cannot be made on one everywhere: a
/// Windows socket needs the loop's completion port. An InMemorySocket can, but its parked read answers only
/// a close, never the flow's stop token, so a race against a deadline would wait on it for ever. This read
/// parks on the loop's own timer instead, which a stop cancels and which a manual clock never reaches.
class SilentSocket final: public core::net::ISocket
{
  public:
    explicit SilentSocket(core::net::EventLoop& loop): _loop(&loop) {}

    [[nodiscard]] core::net::IoAwaitable read(std::span<std::byte> /*buffer*/) override
    {
        return core::net::IoAwaitable { parkUntilStopped(_loop) };
    }
    [[nodiscard]] core::net::IoAwaitable write(std::span<std::byte const> buffer) override
    {
        return core::net::IoAwaitable { core::net::IoResult { buffer.size() } };
    }
    void close() noexcept override { _isClosed = true; }
    [[nodiscard]] bool isClosed() const noexcept override { return _isClosed; }

  private:
    [[nodiscard]] static core::async::Task<core::net::IoResult> parkUntilStopped(core::net::EventLoop* loop)
    {
        co_await loop->sleepUntil(loop->clock().now() + std::chrono::hours { 24 * 365 });
        co_return core::net::IoResult { 0 };
    }

    core::net::EventLoop* _loop;
    bool _isClosed = false;
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
    std::shared_ptr<FakeAskpassBroker> broker; ///< Null until a case wants prompts.
    std::shared_ptr<vthost::SshVersionProbe> probe = std::make_shared<vthost::SshVersionProbe>();
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

    /// Prepares the child of an `ssh -V` that printed @p banner to stderr and ended.
    void prepareVersionProbe(std::string banner)
    {
        auto [remote, state] = prepare({ .stderrTail = std::move(banner), .exitCode = 0 });
        remote.reset();
    }

    [[nodiscard]] vthost::SshTransportFactory factory(
        std::chrono::milliseconds timeout = vthost::SshConnectTimeout) const
    {
        return vthost::SshTransportFactory { target, spawner, broker, timeout, probe };
    }
};

/// @return Every SshFailure.
[[nodiscard]] auto allSshFailures()
{
    return std::views::iota(std::uint8_t { 0 },
                            static_cast<std::uint8_t>(std::to_underlying(SshFailure::Cancelled) + 1))
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

    CHECK(vthost::sshArguments({ .user = "me", .host = "homepc", .port = std::nullopt }, SshPromptMode::Batch)
          == expect({ "--", "me@homepc", "contour daemon --stdio" }));
    CHECK(vthost::sshArguments({ .user = "", .host = "homepc", .port = 2222 }, SshPromptMode::Batch)
          == expect({ "-p", "2222", "--", "homepc", "contour daemon --stdio" }));
    CHECK(vthost::sshArguments({ .user = "", .host = "fe80::1", .port = std::nullopt }, SshPromptMode::Batch)
          == expect({ "--", "fe80::1", "contour daemon --stdio" }));
    CHECK(vthost::sshArguments({ .user = "",
                                 .host = "homepc",
                                 .port = std::nullopt,
                                 .remoteCommand = "/opt/contour/bin/contour daemon --stdio" },
                               SshPromptMode::Batch)
              .back()
          == "/opt/contour/bin/contour daemon --stdio");
    // A destination that looks like an option still comes after the "--".
    auto const optionLike = vthost::sshArguments(
        { .user = "", .host = "-oProxyCommand=evil", .port = std::nullopt }, SshPromptMode::Batch);
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
        auto const error = vthost::sshTransportError(failure, { .target = target, .detail = "x" });
        CHECK(error.message.contains("homepc"));
        if (failure == SshFailure::HostKeyUnknown || failure == SshFailure::AuthenticationRequired)
            CHECK(error.message.contains(vthost::interactiveSshCommand(target)));
    }
    CHECK(vthost::sshTransportError(SshFailure::NoPreamble,
                                    { .target = target, .detail = "gone", .exitCode = 255 })
              .message
          == "ssh to ssh://me@homepc ended before Contour answered (exit code 255): gone");
    CHECK(
        vthost::sshTransportError(SshFailure::NoPreamble,
                                  { .target = target, .detail = "gone", .exitCode = vthost::ExitStatusLost })
            .message
        == "ssh to ssh://me@homepc ended before Contour answered (exit code unknown): gone");
    CHECK(
        vthost::sshTransportError(SshFailure::HostKeyChanged,
                                  { .target = { .user = "", .host = "homepc", .port = 2222 }, .detail = "x" })
            .message.contains("ssh-keygen -R '[homepc]:2222'"));
    // Its own kind: not the unknown key a user may accept, which a check reads as reachable.
    CHECK(
        vthost::sshTransportError(SshFailure::HostKeyChanged,
                                  { .target = { .user = "", .host = "homepc", .port = 2222 }, .detail = "x" })
            .kind
        == vthost::TransportErrorKind::HostKeyChanged);
}

TEST_CASE("sshArguments in askpass mode drops batch mode", "[vthost][ssh]")
{
    auto const target = SshTarget { .user = "me", .host = "homepc", .port = std::nullopt };
    auto const batch = vthost::sshArguments(target, SshPromptMode::Batch);
    auto const askpass = vthost::sshArguments(target, SshPromptMode::Askpass);
    CHECK(std::ranges::contains(batch, "BatchMode=yes"));
    CHECK_FALSE(std::ranges::contains(askpass, "BatchMode=yes"));
    CHECK(std::ranges::contains(askpass, "NumberOfPasswordPrompts=3"));
    CHECK(std::ranges::contains(askpass, "ConnectTimeout=10"));
    CHECK(askpass.back() == "contour daemon --stdio");
}

TEST_CASE("parseOpenSshVersion reads ssh -V", "[vthost][ssh]")
{
    using vthost::OpenSshVersion;
    CHECK(vthost::parseOpenSshVersion("OpenSSH_9.6p1 Ubuntu-3ubuntu13.5, OpenSSL 3.0.13 30 Jan 2024")
          == OpenSshVersion { 9, 6 });
    CHECK(vthost::parseOpenSshVersion("OpenSSH_8.4p1 Debian-5+deb11u3, OpenSSL 1.1.1w  11 Sep 2023")
          == OpenSshVersion { 8, 4 });
    CHECK(vthost::parseOpenSshVersion("OpenSSH_for_Windows_8.1p1, LibreSSL 3.0.2\r\n")
          == OpenSshVersion { 8, 1 });
    CHECK(vthost::parseOpenSshVersion("OpenSSH_10.0p2, OpenSSL 3.5.1 1 Jul 2025")
          == OpenSshVersion { 10, 0 });
    CHECK_FALSE(vthost::parseOpenSshVersion("dropbear v2022.83").has_value());
    CHECK_FALSE(vthost::parseOpenSshVersion("OpenSSH_garbage").has_value());
    CHECK_FALSE(vthost::parseOpenSshVersion("OpenSSH_-1.2").has_value());
    CHECK_FALSE(vthost::parseOpenSshVersion("OpenSSH_8.-4").has_value());
    CHECK_FALSE(vthost::parseOpenSshVersion("").has_value());
}

TEST_CASE("promptModeFor needs OpenSSH 8.4", "[vthost][ssh]")
{
    using vthost::OpenSshVersion;
    CHECK(vthost::promptModeFor(OpenSshVersion { 8, 1 }) == SshPromptMode::Batch);
    CHECK(vthost::promptModeFor(OpenSshVersion { 8, 3 }) == SshPromptMode::Batch);
    CHECK(vthost::promptModeFor(OpenSshVersion { 8, 4 }) == SshPromptMode::Askpass);
    CHECK(vthost::promptModeFor(OpenSshVersion { 9, 0 }) == SshPromptMode::Askpass);
    CHECK(vthost::promptModeFor(std::nullopt) == SshPromptMode::Batch);
}

TEST_CASE("ssh failure messages follow the prompt mode", "[vthost][ssh]")
{
    auto const target = SshTarget { .user = "me", .host = "homepc", .port = std::nullopt };
    for (auto const failure: { SshFailure::HostKeyUnknown, SshFailure::AuthenticationRequired })
    {
        auto const askpass = vthost::sshTransportError(
            failure, { .target = target, .detail = "denied", .mode = SshPromptMode::Askpass });
        CHECK_FALSE(askpass.message.contains(vthost::interactiveSshCommand(target)));
        CHECK_FALSE(askpass.message.contains("Run this once"));
        CHECK(askpass.message.contains("denied"));
        auto const old = vthost::sshTransportError(failure,
                                                   { .target = target,
                                                     .detail = "denied",
                                                     .mode = SshPromptMode::Batch,
                                                     .foundVersion = vthost::OpenSshVersion { 8, 1 } });
        CHECK(old.message.contains("Run this once"));
        CHECK(old.message.contains("8.1"));
        CHECK(old.message.contains("Win32-OpenSSH"));
        auto const unknown = vthost::sshTransportError(
            failure, { .target = target, .detail = "denied", .probed = vthost::SshVersionProbed::Yes });
        CHECK(unknown.message.contains("could not tell which ssh"));
        auto const plain = vthost::sshTransportError(failure, { .target = target, .detail = "denied" });
        CHECK_FALSE(plain.message.contains("Win32-OpenSSH"));
    }
    CHECK(vthost::sshTransportError(SshFailure::Cancelled, { .target = target }).kind
          == TransportErrorKind::Cancelled);
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
        auto const kind = vthost::sshTransportError(failure, { .target = target, .detail = "x" }).kind;
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
    CHECK(fixture.spawner->spawned.front() == vthost::sshArguments(fixture.target, SshPromptMode::Batch));
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

TEST_CASE("without a broker ssh runs in batch mode and no version is probed", "[vthost][ssh][askpass]")
{
    auto fixture = SshFixture {};
    auto [remote, state] = fixture.prepare();
    fixture.loop.blockOn(writeText(remote.get(), std::string { vthost::StdioPreambleReady }));

    auto factory = fixture.factory();
    auto const connection = fixture.loop.blockOn(factory.connect(fixture.loop));
    REQUIRE(connection.has_value());
    REQUIRE(fixture.spawner->spawned.size() == 1);
    CHECK(std::ranges::contains(fixture.spawner->spawned.front(), "BatchMode=yes"));
    CHECK(fixture.spawner->spawnedEnvironments.front().set.empty());
}

TEST_CASE("a broker and a new enough ssh run askpass, probing the version only once",
          "[vthost][ssh][askpass]")
{
    auto fixture = SshFixture {};
    fixture.broker = std::make_shared<FakeAskpassBroker>();
    fixture.prepareVersionProbe("OpenSSH_9.6p1 Ubuntu-3ubuntu13, OpenSSL 3.0.13 30 Jan 2024\n");

    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 2))
    {
        auto [remote, state] = fixture.prepare();
        fixture.loop.blockOn(writeText(remote.get(), std::string { vthost::StdioPreambleReady }));
        auto factory = fixture.factory();
        auto const connection = fixture.loop.blockOn(factory.connect(fixture.loop));
        INFO((connection ? std::string {} : connection.error().message));
        REQUIRE(connection.has_value());
    }

    // ssh -V once, then the two attempts.
    REQUIRE(fixture.spawner->spawned.size() == 3);
    CHECK(fixture.spawner->spawned[0] == std::vector<std::string> { "ssh", "-V" });
    CHECK(fixture.spawner->spawned[1] == vthost::sshArguments(fixture.target, SshPromptMode::Askpass));
    CHECK_FALSE(std::ranges::contains(fixture.spawner->spawned[1], "BatchMode=yes"));
    REQUIRE(fixture.spawner->spawnedEnvironments[1].set.size() == 1);
    CHECK(fixture.spawner->spawnedEnvironments[1].set.front().first == "SSH_ASKPASS_REQUIRE");
    CHECK(fixture.broker->opened == 2);
}

TEST_CASE("two factories sharing one probe run ssh -V once", "[vthost][ssh][askpass]")
{
    auto fixture = SshFixture {};
    fixture.broker = std::make_shared<FakeAskpassBroker>();
    fixture.prepareVersionProbe("OpenSSH_9.6p1, OpenSSL 3.0.13\n");
    for ([[maybe_unused]] auto const host: std::views::iota(0, 2))
    {
        auto [remote, state] = fixture.prepare();
        fixture.loop.blockOn(writeText(remote.get(), std::string { vthost::StdioPreambleReady }));
        auto factory = fixture.factory();
        REQUIRE(fixture.loop.blockOn(factory.connect(fixture.loop)).has_value());
    }
    auto const probes =
        std::ranges::count(fixture.spawner->spawned, std::vector<std::string> { "ssh", "-V" });
    CHECK(probes == 1);
}

TEST_CASE("a broker that cannot open a session falls back to batch mode and connects",
          "[vthost][ssh][askpass]")
{
    auto fixture = SshFixture {};
    fixture.broker = std::make_shared<FakeAskpassBroker>();
    fixture.broker->isOpenFailing = true;
    fixture.prepareVersionProbe("OpenSSH_9.6p1, OpenSSL 3.0.13\n");
    auto [remote, state] = fixture.prepare();
    fixture.loop.blockOn(writeText(remote.get(), std::string { vthost::StdioPreambleReady }));

    auto factory = fixture.factory();
    auto const connection = fixture.loop.blockOn(factory.connect(fixture.loop));
    INFO((connection ? std::string {} : connection.error().message));
    REQUIRE(connection.has_value());
    REQUIRE(fixture.spawner->spawned.size() == 2);
    CHECK(fixture.spawner->spawned[1] == vthost::sshArguments(fixture.target, SshPromptMode::Batch));
    CHECK(fixture.spawner->spawnedEnvironments[1].set.empty());
}

namespace
{
/// A manual clock, a loop on it and a spawner whose `ssh -V` ends only after a few polls.
struct ProbeFixture
{
    core::platform::ManualClock clock;
    core::net::testing::TestLoop loop { clock };
    std::shared_ptr<FakeStdioSpawner> spawner = std::make_shared<FakeStdioSpawner>();
    std::shared_ptr<FakeChildState> child = std::make_shared<FakeChildState>(
        FakeChildState { .stderrTail = "OpenSSH_9.6p1, OpenSSL 3.0.13\n", .exitCode = 0, .runsForPolls = 3 });
    vthost::SshVersionProbe probe;
    std::vector<std::optional<vthost::OpenSshVersion>> answers;
    int finished = 0;

    ProbeFixture() { prepareChild(child); }

    /// Hands @p state out as the child of the next spawn.
    void prepareChild(std::shared_ptr<FakeChildState> const& state)
    {
        spawner->prepared.push_back({ .transport = std::make_unique<SilentSocket>(loop), .state = state });
    }

    void ask()
    {
        loop.spawn(askTask());
        loop.drain();
    }

    /// Lets time pass in small steps until every asker has its answer.
    void letTimePass()
    {
        for ([[maybe_unused]] auto const step: std::views::iota(0, 80))
        {
            clock.advance(10ms);
            loop.drain();
        }
    }

  private:
    [[nodiscard]] core::async::Task<void> askTask()
    {
        answers.push_back(co_await probe.version(&loop, spawner.get()));
        ++finished;
    }
};
} // namespace

TEST_CASE("the ssh -V probe waits for the child without blocking the loop", "[vthost][ssh][askpass]")
{
    auto fixture = ProbeFixture {};
    fixture.ask();
    CHECK(fixture.finished == 0); // parked on the timer, not blocked in exitCode

    fixture.letTimePass();
    REQUIRE(fixture.finished == 1);
    CHECK(fixture.answers.front() == vthost::OpenSshVersion { 9, 6 });
    REQUIRE_FALSE(fixture.child->waits.empty());
    CHECK(std::ranges::all_of(fixture.child->waits, [](auto wait) { return wait == 0ms; }));
}

TEST_CASE("a probe that got no banner is not remembered", "[vthost][ssh][askpass]")
{
    auto fixture = ProbeFixture {};
    // The first ssh -V ends before its stderr arrives, as on a loaded machine.
    fixture.child->stderrTail.clear();
    fixture.child->runsForPolls = 0;
    fixture.prepareChild(std::make_shared<FakeChildState>(
        FakeChildState { .stderrTail = "OpenSSH_9.6p1, OpenSSL 3.0.13\n", .exitCode = 0 }));

    fixture.ask();
    fixture.letTimePass();
    REQUIRE(fixture.finished == 1);
    CHECK_FALSE(fixture.answers.back().has_value());

    fixture.ask();
    fixture.letTimePass();
    REQUIRE(fixture.finished == 2);
    CHECK(fixture.answers.back() == vthost::OpenSshVersion { 9, 6 });
    CHECK(fixture.spawner->spawned.size() == 2);
}

TEST_CASE("a banner that is no OpenSSH's is remembered", "[vthost][ssh][askpass]")
{
    auto fixture = ProbeFixture {};
    fixture.child->stderrTail = "dropbear v2022.83\n";
    fixture.child->runsForPolls = 0;

    fixture.ask();
    fixture.letTimePass();
    fixture.ask();
    fixture.letTimePass();
    REQUIRE(fixture.finished == 2);
    CHECK_FALSE(fixture.answers.back().has_value());
    CHECK(fixture.spawner->spawned.size() == 1);
}

TEST_CASE("concurrent first callers share one ssh -V probe", "[vthost][ssh][askpass]")
{
    auto fixture = ProbeFixture {};
    fixture.ask();
    fixture.ask();
    fixture.letTimePass();
    REQUIRE(fixture.finished == 2);
    CHECK(fixture.answers.at(0) == fixture.answers.at(1));
    CHECK(fixture.spawner->spawned.size() == 1);
}

TEST_CASE("an old ssh runs in batch mode with a broker and never opens a session", "[vthost][ssh][askpass]")
{
    auto fixture = SshFixture {};
    fixture.broker = std::make_shared<FakeAskpassBroker>();
    fixture.prepareVersionProbe("OpenSSH_for_Windows_8.1p1, LibreSSL 3.0.2\r\n");
    auto [remote, state] =
        fixture.prepare({ .stderrTail = "me@homepc: Permission denied (publickey).\n", .exitCode = 255 });
    remote.reset();

    auto factory = fixture.factory();
    auto const connection = fixture.loop.blockOn(factory.connect(fixture.loop));
    REQUIRE_FALSE(connection.has_value());
    REQUIRE(fixture.spawner->spawned.size() == 2);
    CHECK(std::ranges::contains(fixture.spawner->spawned[1], "BatchMode=yes"));
    CHECK(fixture.broker->opened == 0);
    CHECK(connection.error().message.contains("Win32-OpenSSH"));
}

TEST_CASE("a probe that fails or does not parse means batch mode", "[vthost][ssh][askpass]")
{
    auto fixture = SshFixture {};
    fixture.broker = std::make_shared<FakeAskpassBroker>();
    fixture.prepareVersionProbe("something else entirely\n");
    auto [remote, state] = fixture.prepare();
    fixture.loop.blockOn(writeText(remote.get(), std::string { vthost::StdioPreambleReady }));

    auto factory = fixture.factory();
    REQUIRE(fixture.loop.blockOn(factory.connect(fixture.loop)).has_value());
    REQUIRE(fixture.spawner->spawned.size() == 2);
    CHECK(std::ranges::contains(fixture.spawner->spawned[1], "BatchMode=yes"));
    CHECK(fixture.broker->opened == 0);
}

TEST_CASE("a cancelled prompt is a cancelled login whatever ssh printed", "[vthost][ssh][askpass]")
{
    auto fixture = SshFixture {};
    fixture.broker = std::make_shared<FakeAskpassBroker>();
    fixture.prepareVersionProbe("OpenSSH_9.6p1, OpenSSL 3.0.13\n");
    auto [remote, state] =
        fixture.prepare({ .stderrTail = "me@homepc: Permission denied (publickey).\n", .exitCode = 255 });
    remote.reset();
    fixture.broker->state->wasCancelled = true;

    auto factory = fixture.factory();
    auto const connection = fixture.loop.blockOn(factory.connect(fixture.loop));
    REQUIRE_FALSE(connection.has_value());
    CHECK(connection.error().kind == TransportErrorKind::Cancelled);
    CHECK_FALSE(connection.error().message.contains(vthost::interactiveSshCommand(fixture.target)));
    CHECK(connection.error().message.contains("cancelled"));
}

namespace
{
/// A manual clock, a loop on it, and a silent ssh behind a factory with a 30 s budget.
struct PausedDeadlineFixture
{
    core::platform::ManualClock clock;
    core::net::testing::TestLoop loop { clock };
    std::shared_ptr<FakeStdioSpawner> spawner = std::make_shared<FakeStdioSpawner>();
    std::shared_ptr<FakeAskpassBroker> broker = std::make_shared<FakeAskpassBroker>();
    std::shared_ptr<FakeChildState> child = std::make_shared<FakeChildState>();
    std::optional<std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>> result;
    std::optional<vthost::SshTransportFactory> factory;

    PausedDeadlineFixture()
    {
        spawner->prepared.push_back({ .transport = std::make_unique<SilentSocket>(loop), .state = child });
        factory.emplace(SshTarget { .user = "me", .host = "homepc", .port = std::nullopt },
                        spawner,
                        broker,
                        30s,
                        std::make_shared<vthost::SshVersionProbe>());
    }

    /// The probe's answer: a modern ssh, from a child that has already ended.
    void answerVersionProbe()
    {
        spawner->prepared.push_front(
            { .transport = std::make_unique<SilentSocket>(loop),
              .state = std::make_shared<FakeChildState>(
                  FakeChildState { .stderrTail = "OpenSSH_9.6p1, OpenSSL 3.0.13\n", .exitCode = 0 }) });
    }

    void start()
    {
        loop.spawn(run());
        loop.drain();
        // The ssh -V probe parks on the loop's timer, which only the clock moves.
        for ([[maybe_unused]] auto const step: std::views::iota(0, 5))
        {
            clock.advance(10ms);
            loop.drain();
        }
    }

    [[nodiscard]] core::async::Task<void> run() { result = co_await factory->connect(loop); }
};
} // namespace

TEST_CASE("the connect deadline stands still while a prompt is open", "[vthost][ssh][askpass]")
{
    auto fixture = PausedDeadlineFixture {};
    fixture.answerVersionProbe();
    fixture.start();
    fixture.broker->state->promptTime = 40s;

    fixture.clock.advance(35s);
    fixture.loop.drain();
    CHECK_FALSE(fixture.result.has_value()); // 30 s budget + 40 s of prompts

    fixture.clock.advance(36s); // 71 s in
    fixture.loop.drain();
    REQUIRE(fixture.result.has_value());
    REQUIRE_FALSE(fixture.result->has_value());
    CHECK(fixture.result->error().kind == TransportErrorKind::Unreachable);
    CHECK(fixture.child->terminated == 1);
}

TEST_CASE("ssh is ended as soon as the user dismisses a prompt, not when its own clock runs out",
          "[vthost][ssh][askpass]")
{
    auto fixture = PausedDeadlineFixture {};
    fixture.answerVersionProbe();
    fixture.start();
    fixture.loop.drain();
    REQUIRE_FALSE(fixture.result.has_value());
    CHECK(fixture.child->terminated == 0);

    // A prompt is dismissed: no time passes, and ssh goes, so that it asks nothing further.
    fixture.broker->state->cancel();
    fixture.loop.drain();
    REQUIRE(fixture.result.has_value());
    REQUIRE_FALSE(fixture.result->has_value());
    CHECK(fixture.result->error().kind == TransportErrorKind::Cancelled);
    CHECK(fixture.child->terminated == 1);
}

TEST_CASE("a prompt dismissed before the transport listens ends the attempt too", "[vthost][ssh][askpass]")
{
    auto fixture = PausedDeadlineFixture {};
    fixture.answerVersionProbe();
    fixture.broker->state->wasCancelled = true;
    fixture.start();
    fixture.loop.drain();
    REQUIRE(fixture.result.has_value());
    REQUIRE_FALSE(fixture.result->has_value());
    CHECK(fixture.result->error().kind == TransportErrorKind::Cancelled);
    CHECK(fixture.child->terminated == 1);
}

TEST_CASE("a cancelled prompt wins over the timeout's words", "[vthost][ssh][askpass]")
{
    auto fixture = PausedDeadlineFixture {};
    fixture.answerVersionProbe();
    fixture.start();
    fixture.broker->state->wasCancelled = true;

    fixture.clock.advance(31s);
    fixture.loop.drain();
    REQUIRE(fixture.result.has_value());
    REQUIRE_FALSE(fixture.result->has_value());
    CHECK(fixture.result->error().kind == TransportErrorKind::Cancelled);
    CHECK_FALSE(fixture.result->error().message.contains("did not answer"));
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

TEST_CASE("the real ssh -V names its OpenSSH version", "[vthost][ssh][spawn]")
{
    auto const searchPath = vtpty::execSearchPath(std::nullopt);
    auto nativeFileSystem = core::platform::NativeFileSystem { core::platform::nativeRename() };
    auto const search =
        vtpty::ProgramSearch { .path = searchPath, .workingDirectory = {}, .fileSystem = nativeFileSystem };
    if (!vtpty::findProgram("ssh", search))
        SKIP("no ssh client on PATH");
    auto const spawner = vthost::makeSystemStdioSpawner(search);
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto probe = vthost::SshVersionProbe {};

    // Under whatever load the machine is under: the banner must be read, not guessed at.
    auto const found = loop.blockOn(probe.version(&loop, spawner.get()));
    REQUIRE(found.has_value());
    CHECK(found->major >= 1);
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
    auto factory = vthost::SshTransportFactory { SshTarget { .user = "", .host = "127.0.0.1", .port = 1 },
                                                 spawner,
                                                 nullptr,
                                                 vthost::SshConnectTimeout,
                                                 std::make_shared<vthost::SshVersionProbe>() };

    auto const connection = loop.blockOn(factory.connect(loop));
    REQUIRE_FALSE(connection.has_value());
    if (connection.error().kind == TransportErrorKind::ProgramNotFound)
        SKIP("no ssh client on PATH");
    INFO(connection.error().message);
    CHECK(connection.error().kind == TransportErrorKind::Unreachable);
    CHECK(connection.error().message.starts_with("ssh could not reach ssh://127.0.0.1:1: "));
    CHECK(connection.error().message.contains("Connection refused"));
}

TEST_CASE("an ssh transport may prompt exactly when it has a broker", "[vthost][ssh][askpass]")
{
    auto fixture = SshFixture {};
    CHECK_FALSE(fixture.factory().mayPrompt());
    fixture.broker = std::make_shared<FakeAskpassBroker>();
    CHECK(fixture.factory().mayPrompt());
}
