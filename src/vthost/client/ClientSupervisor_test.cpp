// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.hpp>

#include <core/Environment.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/InMemoryTransport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <vthost/ConnectionId.hpp>
#include <vthost/LoopDrain.hpp>
#include <vthost/NativeSession.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/Transport.hpp>
#include <vthost/client/ClientSupervisor.hpp>
#include <vthost/client/NativeClient.hpp>
#include <vthost/testing/TestHelpers.hpp>

using core::async::Task;
using vthost::Connection;
using vthost::TransportError;
using vthost::TransportErrorKind;
using vthost::client::backoffDelay;
using vthost::client::BackoffSchedule;
using vthost::client::ClientSupervisor;
using vthost::client::ConnectionState;
using vthost::client::NativeClient;
namespace proto = vthost::proto;
using namespace std::chrono_literals;

namespace
{

/// What one connect() attempt does.
using Step = std::function<std::expected<std::unique_ptr<Connection>, TransportError>(core::net::EventLoop&)>;

/// A transport whose attempts follow a script; past its end, the last step repeats.
class ScriptedTransport final: public vthost::ITransportFactory
{
  public:
    explicit ScriptedTransport(std::vector<Step> steps): _steps(std::move(steps)) {}

    [[nodiscard]] Task<std::expected<std::unique_ptr<Connection>, TransportError>> connect(
        core::net::EventLoop& loop) override
    {
        auto const index = std::min(_calls++, _steps.size() - 1);
        return settled(_steps[index](loop));
    }

    [[nodiscard]] std::string describe() const override { return "scripted:"; }

    /// @return How many attempts were made.
    [[nodiscard]] std::size_t calls() const noexcept { return _calls; }

  private:
    /// @return A task that is done at once, with @p result.
    static Task<std::expected<std::unique_ptr<Connection>, TransportError>> settled(
        std::expected<std::unique_ptr<Connection>, TransportError> result)
    {
        co_return result;
    }

    std::vector<Step> _steps;
    std::size_t _calls = 0;
};

/// A daemon on the test's loop: one SessionHost, serving a NativeSession per connection.
struct Daemon
{
    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *source };
    std::unique_ptr<vthost::SessionHost> host = makeHost();
    std::vector<std::unique_ptr<vthost::NativeSession>> sessions;
    std::vector<core::net::ISocket*> serverSockets; ///< Each connection's daemon end, to drop it.

    Daemon() { std::ignore = vthost::testing::newSession(*host); }

    ~Daemon()
    {
        // Ends the connection flows while the host they subscribe to still exists.
        auto const drain = vthost::LoopDrain { loop };
    }

    Daemon(Daemon const&) = delete;
    Daemon& operator=(Daemon const&) = delete;
    Daemon(Daemon&&) = delete;
    Daemon& operator=(Daemon&&) = delete;

    [[nodiscard]] std::unique_ptr<vthost::SessionHost> makeHost()
    {
        return std::make_unique<vthost::SessionHost>(
            loop,
            [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                return std::make_unique<vtpty::MockPty>(size);
            },
            vtbackend::Settings {},
            core::defaultEnvironment(),
            vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual });
    }

    /// @param services What the connection's handshake admits.
    /// @return An attempt that connects to this daemon.
    [[nodiscard]] Step serving(vthost::NativeSessionServices const& services = {})
    {
        return [this, services](core::net::EventLoop& clientLoop)
                   -> std::expected<std::unique_ptr<Connection>, TransportError> {
            auto pair = *core::net::testing::makeSocketPair(clientLoop);
            serverSockets.push_back(pair.first.get());
            sessions.push_back(std::make_unique<vthost::NativeSession>(
                loop,
                *host,
                vthost::ConnectionId { .endpoint = "test", .index = sessions.size() + 1 },
                std::move(pair.first),
                vthost::NativeSession::DefaultWriteQueueBytes,
                services));
            loop.spawn(serve(sessions.back().get()));
            return std::make_unique<Connection>(
                Connection { .lifetime = {}, .socket = std::move(pair.second), .token = {} });
        };
    }

    /// Drops the daemon's end of the latest connection, as a crashing daemon would.
    void dropConnection() const { serverSockets.back()->close(); }

    static Task<void> serve(vthost::NativeSession* session) { co_await session->run(); }
};

/// @return An attempt that fails with @p kind and @p message.
[[nodiscard]] Step failing(TransportErrorKind kind, std::string const& message)
{
    return
        [kind, message](core::net::EventLoop&) -> std::expected<std::unique_ptr<Connection>, TransportError> {
            return std::unexpected(TransportError { .kind = kind, .message = message });
        };
}

/// The services of a daemon that speaks only protocol versions this client does not.
[[nodiscard]] vthost::NativeSessionServices disjointVersions()
{
    auto services = vthost::NativeSessionServices {};
    services.handshake.versions = proto::VersionRange { .min = 6, .max = 8 };
    return services;
}

/// One state the supervisor entered.
struct Entered
{
    ConnectionState state;
    std::string reason;
};

/// A supervisor over a scripted transport, recording what it does.
struct Supervised
{
    Supervised(Daemon& daemon,
               std::vector<Step> steps,
               BackoffSchedule backoff = { .first = 1ms, .ceiling = 4ms },
               proto::AttachIntent firstIntent = { .kind = proto::AttachIntentKind::Launch, .windows = {} }):
        loop(daemon.loop), transport(std::make_shared<ScriptedTransport>(std::move(steps)))
    {
        supervisor.emplace(
            loop,
            transport,
            [this](std::unique_ptr<Connection> connection, proto::AttachIntent intent) {
                intents.push_back(intent);
                return std::make_unique<NativeClient>(
                    loop,
                    std::move(connection->socket),
                    NativeClient::HandshakeOptions {
                        .token = connection->token,
                        .capabilities = proto::CapabilitySet::of({ proto::Capability::AttachIntent }),
                        .attachIntent = std::move(intent) },
                    vtbackend::CommandBlockStoreLimits {},
                    NativeClient::UpdateHandler {},
                    NativeClient::ImageHandler {},
                    NativeClient::SessionEventHandler {},
                    NativeClient::LayoutHandler {});
            },
            std::move(firstIntent),
            backoff,
            [this](ConnectionState state, std::string const& reason) {
                states.push_back(Entered { .state = state, .reason = reason });
            });
    }

    core::net::EventLoop& loop;
    std::shared_ptr<ScriptedTransport> transport;
    std::optional<ClientSupervisor> supervisor;
    std::vector<Entered> states;
    std::vector<proto::AttachIntent> intents;

    /// @return How many times @p state was entered.
    [[nodiscard]] std::size_t count(ConnectionState state) const
    {
        return static_cast<std::size_t>(std::ranges::count(states, state, &Entered::state));
    }

    /// @return The states entered, in order.
    [[nodiscard]] std::vector<ConnectionState> sequence() const
    {
        auto out = std::vector<ConnectionState> {};
        for (auto const& entered: states)
            out.push_back(entered.state);
        return out;
    }

    /// Waits until @p state was entered @p times times.
    [[nodiscard]] Task<bool> reached(ConnectionState state, std::size_t times = 1)
    {
        co_return co_await core::net::testing::waitUntil(
            &loop, [this, state, times] { return count(state) >= times; });
    }

    /// Runs the supervisor beside @p driver until both ended.
    void drive(Task<void> driver)
    {
        loop.blockOn(core::net::testing::allOf(supervisor->run(), std::move(driver)));
    }
};

} // namespace

TEST_CASE("backoff doubles from the first delay up to the ceiling", "[vthost][reconnect]")
{
    constexpr auto Schedule = BackoffSchedule { .first = 250ms, .ceiling = 5000ms };
    static_assert(backoffDelay(Schedule, 0) == 0ms);
    static_assert(backoffDelay(Schedule, 1) == 250ms);
    static_assert(backoffDelay(Schedule, 2) == 500ms);
    static_assert(backoffDelay(Schedule, 3) == 1000ms);
    static_assert(backoffDelay(Schedule, 6) == 5000ms);
    static_assert(backoffDelay(Schedule, 4000000000U) == 5000ms);
    static_assert(backoffDelay({ .first = 0ms, .ceiling = 5000ms }, 7) == 0ms);
    static_assert(backoffDelay({ .first = 900ms, .ceiling = 500ms }, 1) == 500ms);
}

TEST_CASE("a first connect that fails ends Failed without retrying", "[vthost][reconnect]")
{
    auto daemon = Daemon {};
    auto h = Supervised { daemon, { failing(TransportErrorKind::DaemonAbsent, "no such file") } };
    daemon.loop.blockOn(h.supervisor->run());

    CHECK(h.sequence() == std::vector { ConnectionState::Connecting, ConnectionState::Failed });
    CHECK(h.states.back().reason == "no such file");
    CHECK(h.transport->calls() == 1);
}

TEST_CASE("a first connect the daemon refuses ends Failed", "[vthost][reconnect]")
{
    auto daemon = Daemon {};
    auto h = Supervised { daemon, { daemon.serving(disjointVersions()) } };
    daemon.loop.blockOn(h.supervisor->run());

    CHECK(h.sequence() == std::vector { ConnectionState::Connecting, ConnectionState::Failed });
    CHECK(h.states.back().reason.contains("the daemon speaks protocol"));
    CHECK(h.transport->calls() == 1);
}

TEST_CASE("a lost connection reconnects with Resume of the followed windows", "[vthost][reconnect]")
{
    auto daemon = Daemon {};
    auto h = Supervised { daemon, { daemon.serving() } };
    h.drive([](Supervised* h, Daemon* daemon) -> Task<void> {
        REQUIRE(co_await h->reached(ConnectionState::Connected));
        daemon->dropConnection();
        REQUIRE(co_await h->reached(ConnectionState::Connected, 2));
        h->supervisor->stop();
    }(&h, &daemon));

    CHECK(h.sequence()
          == std::vector { ConnectionState::Connecting,
                           ConnectionState::Connected,
                           ConnectionState::Reconnecting,
                           ConnectionState::Connected,
                           ConnectionState::Stopped });
    REQUIRE(h.intents.size() == 2);
    CHECK(h.intents[0].kind == proto::AttachIntentKind::Launch);
    CHECK(h.intents[1]
          == proto::AttachIntent { .kind = proto::AttachIntentKind::Resume,
                                   .windows = { daemon.host->windowId().value } });
}

TEST_CASE("a client that follows every window keeps following every window", "[vthost][reconnect]")
{
    // Resume would narrow it to the windows it had; a window another client creates later must reach it.
    auto daemon = Daemon {};
    auto h = Supervised { daemon,
                          { daemon.serving() },
                          { .first = 1ms, .ceiling = 4ms },
                          { .kind = proto::AttachIntentKind::AllWindows, .windows = {} } };
    h.drive([](Supervised* h, Daemon* daemon) -> Task<void> {
        REQUIRE(co_await h->reached(ConnectionState::Connected));
        daemon->dropConnection();
        REQUIRE(co_await h->reached(ConnectionState::Connected, 2));
        h->supervisor->stop();
    }(&h, &daemon));

    REQUIRE(h.intents.size() == 2);
    CHECK(h.intents[1] == proto::AttachIntent { .kind = proto::AttachIntentKind::AllWindows, .windows = {} });
}

TEST_CASE("a window created during the connection is resumed too", "[vthost][reconnect]")
{
    auto daemon = Daemon {};
    auto h = Supervised { daemon, { daemon.serving() } };
    h.drive([](Supervised* h, Daemon* daemon) -> Task<void> {
        REQUIRE(co_await h->reached(ConnectionState::Connected));
        h->supervisor->current()->createWindow({});
        REQUIRE(co_await core::net::testing::waitUntil(
            &h->loop, [h] { return h->supervisor->current()->followedWindows().size() == 2; }));
        daemon->dropConnection();
        REQUIRE(co_await h->reached(ConnectionState::Connected, 2));
        h->supervisor->stop();
    }(&h, &daemon));

    REQUIRE(h.intents.size() == 2);
    CHECK(h.intents[1].kind == proto::AttachIntentKind::Resume);
    CHECK(h.intents[1].windows.size() == 2);
}

TEST_CASE("transport failures during reconnect keep retrying", "[vthost][reconnect]")
{
    auto daemon = Daemon {};
    auto h = Supervised { daemon,
                          { daemon.serving(),
                            failing(TransportErrorKind::DaemonAbsent, "gone"),
                            failing(TransportErrorKind::Unreachable, "still gone"),
                            daemon.serving() } };
    h.drive([](Supervised* h, Daemon* daemon) -> Task<void> {
        REQUIRE(co_await h->reached(ConnectionState::Connected));
        daemon->dropConnection();
        REQUIRE(co_await h->reached(ConnectionState::Connected, 2));
        h->supervisor->stop();
    }(&h, &daemon));

    CHECK(h.count(ConnectionState::Reconnecting) == 1);
    CHECK(h.count(ConnectionState::Connected) == 2);
    CHECK(h.transport->calls() == 4); // the first connect, then three during the reconnect
}

TEST_CASE("a refusal after reconnect stops retrying and names both versions", "[vthost][reconnect]")
{
    auto daemon = Daemon {};
    auto h = Supervised { daemon, { daemon.serving(), daemon.serving(disjointVersions()) } };
    h.drive([](Supervised* h, Daemon* daemon) -> Task<void> {
        REQUIRE(co_await h->reached(ConnectionState::Connected));
        daemon->dropConnection();
        // No stop(): the refusal ends run() by itself.
        co_return;
    }(&h, &daemon));

    REQUIRE_FALSE(h.states.empty());
    CHECK(h.states.back().state == ConnectionState::Refused);
    CHECK(h.states.back().reason.contains("the daemon speaks protocol"));
    CHECK(h.transport->calls() == 2);
}

TEST_CASE("stop during backoff ends run promptly", "[vthost][reconnect]")
{
    auto daemon = Daemon {};
    auto h = Supervised { daemon, { daemon.serving() }, { .first = 10s, .ceiling = 10s } };
    auto const began = std::chrono::steady_clock::now();
    h.drive([](Supervised* h, Daemon* daemon) -> Task<void> {
        REQUIRE(co_await h->reached(ConnectionState::Connected));
        daemon->dropConnection();
        REQUIRE(co_await h->reached(ConnectionState::Reconnecting));
        co_await h->loop.delay(50ms);
        h->supervisor->stop();
    }(&h, &daemon));

    CHECK(std::chrono::steady_clock::now() - began < 1s);
    CHECK(h.states.back().state == ConnectionState::Stopped);
    CHECK(h.transport->calls() == 1);
}

TEST_CASE("a permanent transport error during reconnect stops retrying", "[vthost][reconnect]")
{
    auto daemon = Daemon {};
    auto h = Supervised { daemon,
                          { daemon.serving(),
                            failing(TransportErrorKind::AuthenticationRequired, "run `ssh home` once") } };
    h.drive([](Supervised* h, Daemon* daemon) -> Task<void> {
        REQUIRE(co_await h->reached(ConnectionState::Connected));
        daemon->dropConnection();
        co_return;
    }(&h, &daemon));

    CHECK(h.sequence()
          == std::vector { ConnectionState::Connecting,
                           ConnectionState::Connected,
                           ConnectionState::Reconnecting,
                           ConnectionState::Refused });
    CHECK(h.states.back().reason == "run `ssh home` once");
    CHECK(h.transport->calls() == 2);
}
