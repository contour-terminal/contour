// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.hpp>

#include <core/Environment.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/InMemoryTransport.hpp>
#include <core/platform/Clock.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <vthost/ConnectionId.hpp>
#include <vthost/LoopDrain.hpp>
#include <vthost/NativeSession.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/Transport.hpp>
#include <vthost/client/ClientSupervisor.hpp>
#include <vthost/client/Liveness.hpp>
#include <vthost/client/NativeClient.hpp>
#include <vthost/testing/FakeDaemonControl.hpp>
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
    [[nodiscard]] vthost::PeerTrust peerTrust() const noexcept override { return vthost::PeerTrust::Local; }

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
    /// @param clock What the loop -- the daemon's and the client's alike -- tells time by.
    explicit Daemon(core::platform::IClock& clock = core::platform::defaultSteadyClock()): clock(clock)
    {
        std::ignore = vthost::testing::newSession(*host);
    }

    core::platform::IClock& clock;
    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *source, clock };
    std::unique_ptr<vthost::SessionHost> host = makeHost();
    std::vector<std::unique_ptr<vthost::NativeSession>> sessions;
    std::vector<core::net::ISocket*> serverSockets; ///< Each connection's daemon end, to drop it.
    std::vector<std::unique_ptr<core::net::ISocket>> unanswered; ///< The daemon ends silent() holds open.

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

    /// @return An attempt whose connection is accepted and then never answered: the daemon's end is held
    ///         open, and nothing reads or writes it.
    [[nodiscard]] Step silent()
    {
        return [this](core::net::EventLoop& clientLoop)
                   -> std::expected<std::unique_ptr<Connection>, TransportError> {
            auto pair = *core::net::testing::makeSocketPair(clientLoop);
            unanswered.push_back(std::move(pair.first));
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
               proto::AttachIntent firstIntent = { .kind = proto::AttachIntentKind::Default, .workspace = 0 },
               vthost::client::LivenessPolicy liveness = {}):
        loop(daemon.loop), transport(std::make_shared<ScriptedTransport>(std::move(steps)))
    {
        supervisor.emplace(
            loop,
            transport,
            [this, liveness](std::unique_ptr<Connection> connection, proto::AttachIntent intent) {
                intents.push_back(intent);
                return std::make_unique<NativeClient>(
                    loop,
                    std::move(connection->socket),
                    NativeClient::HandshakeOptions {
                        .token = connection->token,
                        .capabilities = proto::CapabilitySet::of({ proto::Capability::AttachIntent,
                                                                   proto::Capability::DaemonFollow,
                                                                   proto::Capability::LayoutBatch,
                                                                   proto::Capability::LiveUpgrade }),
                        .attachIntent = intent },
                    vtbackend::CommandBlockStoreLimits {},
                    NativeClient::UpdateHandler {},
                    NativeClient::ImageHandler {},
                    NativeClient::SessionEventHandler {},
                    NativeClient::LayoutHandler {},
                    NativeClient::PermissionHandler {},
                    liveness);
            },
            firstIntent,
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

TEST_CASE("a first connect the user cancels ends Failed, saying so", "[vthost][reconnect]")
{
    // A dismissed login prompt: the transport's Cancelled, with stop() never called.
    auto daemon = Daemon {};
    auto h =
        Supervised { daemon, { failing(TransportErrorKind::Cancelled, "The ssh login was cancelled.") } };
    daemon.loop.blockOn(h.supervisor->run());

    CHECK(h.sequence() == std::vector { ConnectionState::Connecting, ConnectionState::Failed });
    CHECK(h.states.back().reason == "The ssh login was cancelled.");
    CHECK(h.transport->calls() == 1);
}

TEST_CASE("a reconnect the user cancels is not retried, and says so", "[vthost][reconnect]")
{
    auto daemon = Daemon {};
    auto h = Supervised {
        daemon, { daemon.serving(), failing(TransportErrorKind::Cancelled, "The ssh login was cancelled.") }
    };
    h.drive([](Supervised* h, Daemon* daemon) -> Task<void> {
        auto const connected = co_await h->reached(ConnectionState::Connected);
        if (!connected)
        {
            h->supervisor->stop();
            co_return;
        }
        daemon->dropConnection();
        // The supervisor ends on its own, as nothing is left to retry.
        co_await h->reached(ConnectionState::Refused);
    }(&h, &daemon));

    CHECK(h.states.back().state == ConnectionState::Refused);
    CHECK(h.states.back().reason == "The ssh login was cancelled.");
    CHECK(h.transport->calls() == 2);
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

TEST_CASE("a lost connection reconnects with Resume of the selected workspace", "[vthost][reconnect]")
{
    auto daemon = Daemon {};
    auto h = Supervised { daemon, { daemon.serving() } };
    h.drive([](Supervised* h, Daemon* daemon) -> Task<void> {
        auto const connected = co_await h->reached(ConnectionState::Connected);
        REQUIRE(connected);
        daemon->dropConnection();
        auto const reconnected = co_await h->reached(ConnectionState::Connected, 2);
        REQUIRE(reconnected);
        h->supervisor->stop();
    }(&h, &daemon));

    CHECK(h.sequence()
          == std::vector { ConnectionState::Connecting,
                           ConnectionState::Connected,
                           ConnectionState::Reconnecting,
                           ConnectionState::Connected,
                           ConnectionState::Stopped });
    REQUIRE(h.intents.size() == 2);
    CHECK(h.intents[0].kind == proto::AttachIntentKind::Default);
    CHECK(h.intents[1]
          == proto::AttachIntent { .kind = proto::AttachIntentKind::Resume,
                                   .workspace = daemon.host->workspaceId().value });
}

TEST_CASE("a workspace selected during the connection is resumed, not the one attached to",
          "[vthost][reconnect]")
{
    auto daemon = Daemon {};
    auto h = Supervised { daemon, { daemon.serving() } };
    auto const first = daemon.host->workspaceId().value;
    auto const second = daemon.host->createWorkspace()->id().value;
    h.drive([](Supervised* h, Daemon* daemon, uint64_t second) -> Task<void> {
        auto const connected = co_await h->reached(ConnectionState::Connected);
        REQUIRE(connected);
        auto const serial = h->supervisor->current()->selectWorkspace(second);
        REQUIRE(serial.has_value());
        // Held once the daemon states it, with the batch that answers.
        auto const stated = co_await core::net::testing::waitUntil(
            &h->loop, [h, second] { return h->supervisor->current()->selectedWorkspace() == second; });
        REQUIRE(stated);
        daemon->dropConnection();
        auto const reconnected = co_await h->reached(ConnectionState::Connected, 2);
        REQUIRE(reconnected);
        h->supervisor->stop();
    }(&h, &daemon, second));

    REQUIRE(h.intents.size() == 2);
    CHECK(h.intents[1]
          == proto::AttachIntent { .kind = proto::AttachIntentKind::Resume, .workspace = second });
    CHECK(second != first);
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
        auto const connected = co_await h->reached(ConnectionState::Connected);
        REQUIRE(connected);
        daemon->dropConnection();
        auto const reconnected = co_await h->reached(ConnectionState::Connected, 2);
        REQUIRE(reconnected);
        h->supervisor->stop();
    }(&h, &daemon));

    CHECK(h.count(ConnectionState::Reconnecting) == 1);
    CHECK(h.count(ConnectionState::Connected) == 2);
    CHECK(h.transport->calls() == 4); // the first connect, then three during the reconnect
}

TEST_CASE("a reconnect the daemon accepts and never answers is retried", "[vthost][reconnect]")
{
    // Half-open mid-handshake: without a deadline the attempt would wait for the hello's answer for good.
    auto daemon = Daemon {};
    auto h = Supervised { daemon,
                          { daemon.serving(), daemon.silent(), daemon.serving() },
                          { .first = 1ms, .ceiling = 4ms },
                          { .kind = proto::AttachIntentKind::Default, .workspace = 0 },
                          { .idleBeforeProbe = 5s, .deadAfter = 15s, .handshakeDeadline = 50ms } };
    h.drive([](Supervised* h, Daemon* daemon) -> Task<void> {
        auto const connected = co_await h->reached(ConnectionState::Connected);
        REQUIRE(connected);
        daemon->dropConnection();
        auto const reconnected = co_await h->reached(ConnectionState::Connected, 2);
        REQUIRE(reconnected);
        h->supervisor->stop();
    }(&h, &daemon));

    CHECK(h.count(ConnectionState::Reconnecting) == 1);
    CHECK(h.transport->calls() == 3); // the first connect, the unanswered one, and the one that took
    CHECK(h.states.back().state == ConnectionState::Stopped);
}

TEST_CASE("a refusal after reconnect stops retrying and names both versions", "[vthost][reconnect]")
{
    auto daemon = Daemon {};
    auto h = Supervised { daemon, { daemon.serving(), daemon.serving(disjointVersions()) } };
    h.drive([](Supervised* h, Daemon* daemon) -> Task<void> {
        auto const connected = co_await h->reached(ConnectionState::Connected);
        REQUIRE(connected);
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
        auto const connected = co_await h->reached(ConnectionState::Connected);
        REQUIRE(connected);
        daemon->dropConnection();
        auto const reconnecting = co_await h->reached(ConnectionState::Reconnecting);
        REQUIRE(reconnecting);
        co_await h->loop.delay(50ms);
        h->supervisor->stop();
    }(&h, &daemon));

    CHECK(std::chrono::steady_clock::now() - began < 1s);
    CHECK(h.states.back().state == ConnectionState::Stopped);
    CHECK(h.transport->calls() == 1);
}

TEST_CASE("retryNow during backoff reconnects promptly", "[vthost][reconnect]")
{
    // A wake while reconnecting: the network may be back, so the long backoff wait is cut short.
    auto daemon = Daemon {};
    auto h = Supervised { daemon, { daemon.serving() }, { .first = 10s, .ceiling = 10s } };
    auto const began = std::chrono::steady_clock::now();
    h.drive([](Supervised* h, Daemon* daemon) -> Task<void> {
        auto const connected = co_await h->reached(ConnectionState::Connected);
        REQUIRE(connected);
        daemon->dropConnection();
        auto const reconnecting = co_await h->reached(ConnectionState::Reconnecting);
        REQUIRE(reconnecting);
        co_await h->loop.delay(50ms);
        h->supervisor->retryNow();
        auto const reconnected = co_await h->reached(ConnectionState::Connected, 2);
        CHECK(reconnected); // not REQUIRE: the stop below must run either way, or run() never ends
        h->supervisor->stop();
    }(&h, &daemon));

    CHECK(std::chrono::steady_clock::now() - began < 5s);
    CHECK(h.transport->calls() == 2);
    CHECK(h.states.back().state == ConnectionState::Stopped);
}

TEST_CASE("retryNow from another thread cuts the backoff short", "[vthost][reconnect][threading]")
{
    auto daemon = Daemon {};
    auto h = Supervised { daemon, { daemon.serving() }, { .first = 10s, .ceiling = 10s } };
    auto const began = std::chrono::steady_clock::now();
    h.drive([](Supervised* h, Daemon* daemon) -> Task<void> {
        auto const connected = co_await h->reached(ConnectionState::Connected);
        REQUIRE(connected);
        daemon->dropConnection();
        auto const reconnecting = co_await h->reached(ConnectionState::Reconnecting);
        REQUIRE(reconnecting);
        auto waker = std::async(std::launch::async, [h] { h->supervisor->retryNow(); });
        auto const reconnected = co_await h->reached(ConnectionState::Connected, 2);
        waker.get();
        CHECK(reconnected); // not REQUIRE: the stop below must run either way, or run() never ends
        h->supervisor->stop();
    }(&h, &daemon));

    CHECK(std::chrono::steady_clock::now() - began < 5s);
    CHECK(h.transport->calls() == 2);
}

TEST_CASE("stop during a slow daemon start ends run promptly", "[vthost][reconnect][threading]")
{
    // The start runs on the transport's worker, which stop() does not wait for.
    auto daemon = Daemon {};
    auto started = std::atomic<bool> { false };
    auto release = std::promise<void> {};
    auto transport = std::make_shared<vthost::UnixTransportFactory>(
        "/nonexistent/contour/control", [&started, released = release.get_future().share()] {
            started = true;
            std::ignore = released.wait_for(10s);
            return std::expected<void, TransportError> {};
        });
    auto states = std::vector<ConnectionState> {};
    auto supervisor = ClientSupervisor {
        daemon.loop,
        transport,
        [](std::unique_ptr<Connection>, proto::AttachIntent const&) -> std::unique_ptr<NativeClient> {
            return nullptr;
        },
        { .kind = proto::AttachIntentKind::Default, .workspace = 0 },
        { .first = 1ms, .ceiling = 4ms },
        [&states](ConnectionState state, std::string const&) { states.push_back(state); }
    };
    auto stopAt = std::chrono::steady_clock::time_point {};
    daemon.loop.blockOn(core::net::testing::allOf(
        supervisor.run(),
        [](ClientSupervisor* supervisor,
           core::net::EventLoop* loop,
           std::atomic<bool>* started,
           std::chrono::steady_clock::time_point* stopAt) -> Task<void> {
            auto const running =
                co_await core::net::testing::waitUntil(loop, [started] { return started->load(); });
            REQUIRE(running);
            *stopAt = std::chrono::steady_clock::now();
            supervisor->stop();
        }(&supervisor, &daemon.loop, &started, &stopAt)));
    auto const stoppedIn = std::chrono::steady_clock::now() - stopAt;
    release.set_value();

    CHECK(stoppedIn < 1s);
    CHECK(states == std::vector { ConnectionState::Connecting, ConnectionState::Stopped });
}

TEST_CASE("a permanent transport error during reconnect stops retrying", "[vthost][reconnect]")
{
    auto daemon = Daemon {};
    auto h = Supervised { daemon,
                          { daemon.serving(),
                            failing(TransportErrorKind::AuthenticationRequired, "run `ssh home` once") } };
    h.drive([](Supervised* h, Daemon* daemon) -> Task<void> {
        auto const connected = co_await h->reached(ConnectionState::Connected);
        REQUIRE(connected);
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

namespace
{

/// A daemon that upgrades in place: it closes every connection and is gone until the test says it is back,
/// or a transport starts one. Its first connect, and every one once it is back, reach the test's daemon;
/// every other goes to a real unix factory over a socket nobody serves, which reads as DaemonAbsent and
/// starts a daemon by @ref ensured -- unless the factory holds that back.
class UpgradingTransport final: public vthost::ITransportFactory
{
  public:
    /// @param daemon The daemon its connections reach.
    /// @param control What announces the upgrade to those connections.
    /// @param nobody A unix socket path nobody serves.
    UpgradingTransport(Daemon& daemon, vthost::DaemonControl& control, std::filesystem::path const& nobody):
        _daemon(daemon), _control(control), _absent(nobody, [this] {
            ++ensured;
            return std::expected<void, TransportError> {};
        })
    {
    }

    [[nodiscard]] Task<std::expected<std::unique_ptr<Connection>, TransportError>> connect(
        core::net::EventLoop& loop) override
    {
        return connectOn(&loop);
    }

    [[nodiscard]] std::string describe() const override { return "upgrading:"; }
    [[nodiscard]] vthost::PeerTrust peerTrust() const noexcept override { return vthost::PeerTrust::Local; }
    void holdDaemonStart(core::platform::SteadyTimePoint until) override { _absent.holdDaemonStart(until); }

    std::atomic<int> ensured = 0; ///< How many daemon starts the factory made. Written on its worker.
    bool back = false;            ///< Whether the upgraded daemon serves again.
    std::size_t calls = 0;        ///< How many attempts were made.

  private:
    Task<std::expected<std::unique_ptr<Connection>, TransportError>> connectOn(core::net::EventLoop* loop)
    {
        if (calls++ != 0 && !back)
        {
            auto absent = co_await _absent.connect(*loop);
            // A daemon the factory started is served from now on.
            if (ensured == 0)
                co_return absent;
            back = true;
        }
        co_return _daemon.serving(liveUpgrade())(*loop);
    }

    /// @return What the daemon serves with: LiveUpgrade offered, and the upgrade announced by its control.
    [[nodiscard]] vthost::NativeSessionServices liveUpgrade() const
    {
        auto services = vthost::NativeSessionServices {};
        services.handshake.capabilities.idents.push_back(std::to_underlying(proto::Capability::LiveUpgrade));
        services.daemon = _control;
        return services;
    }

    Daemon& _daemon;
    vthost::DaemonControl& _control;
    vthost::UnixTransportFactory _absent;
};

/// A supervisor on a manual clock over an UpgradingTransport, driven turn by turn from the test.
struct UpgradeRun
{
    UpgradeRun(): transport(std::make_shared<UpgradingTransport>(daemon, control, tmp.path() / "nobody"))
    {
        supervisor.emplace(
            daemon.loop,
            transport,
            [this](std::unique_ptr<Connection> connection, proto::AttachIntent intent) {
                return std::make_unique<NativeClient>(
                    daemon.loop,
                    std::move(connection->socket),
                    NativeClient::HandshakeOptions {
                        .token = connection->token,
                        .capabilities = proto::CapabilitySet::of(
                            { proto::Capability::AttachIntent, proto::Capability::LiveUpgrade }),
                        .attachIntent = intent },
                    vtbackend::CommandBlockStoreLimits {},
                    NativeClient::UpdateHandler {},
                    NativeClient::ImageHandler {},
                    NativeClient::SessionEventHandler {},
                    NativeClient::LayoutHandler {});
            },
            proto::AttachIntent { .kind = proto::AttachIntentKind::Default, .workspace = 0 },
            BackoffSchedule { .first = 1ms, .ceiling = 4ms },
            [this](ConnectionState state, std::string const& reason) {
                states.push_back(Entered { .state = state, .reason = reason });
            });
        daemon.loop.spawn(runToEnd(&*supervisor, &ended));
    }

    UpgradeRun(UpgradeRun const&) = delete;
    UpgradeRun& operator=(UpgradeRun const&) = delete;
    UpgradeRun(UpgradeRun&&) = delete;
    UpgradeRun& operator=(UpgradeRun&&) = delete;

    ~UpgradeRun()
    {
        supervisor->stop();
        std::ignore = turnUntil([this] { return ended; });
    }

    /// Turns the loop until @p ready holds, at most @p turns times, without moving the clock.
    [[nodiscard]] bool turnUntil(std::function<bool()> const& ready, int turns = 5000)
    {
        for ([[maybe_unused]] auto const turn: std::views::iota(0, turns))
        {
            if (ready())
                return true;
            std::ignore = daemon.loop.runOnce(1ms);
        }
        return ready();
    }

    /// Moves the clock on by @p seconds, a second at a time, letting the loop settle after each.
    void letTimePass(int seconds)
    {
        for ([[maybe_unused]] auto const second: std::views::iota(0, seconds))
        {
            clock.advance(1s);
            std::ignore = turnUntil([] { return false; }, 20);
        }
    }

    /// The daemon announces its upgrade to every connection and closes them.
    void upgrade() const
    {
        control.announceUpgrade("9.9.10");
        control.closeObservers();
    }

    /// @return How many times @p state was entered.
    [[nodiscard]] std::size_t count(ConnectionState state) const
    {
        return static_cast<std::size_t>(std::ranges::count(states, state, &Entered::state));
    }

    /// @return The states entered, in order.
    [[nodiscard]] std::vector<ConnectionState> sequence() const
    {
        return std::ranges::to<std::vector>(states | std::views::transform(&Entered::state));
    }

    static Task<void> runToEnd(ClientSupervisor* supervisor, bool* ended)
    {
        co_await supervisor->run();
        *ended = true;
    }

    core::testing::ScopedTempDir tmp { "contour-upgrade-hold" };
    core::platform::ManualClock clock { core::platform::SteadyTimePoint {} + 10s };
    /// The daemon's control, which announces the upgrade to every connection. Before the daemon, whose
    /// connections unsubscribe from it as they go.
    vthost::testing::FakeDaemonControl control;
    Daemon daemon { clock };
    std::shared_ptr<UpgradingTransport> transport;
    std::optional<ClientSupervisor> supervisor;
    std::vector<Entered> states;
    bool ended = false;
};

} // namespace

// [afunix]: the attempts during the hold go to a real unix socket nobody serves.
TEST_CASE("during the upgrade hold DaemonAbsent never runs ensureDaemon",
          "[vthost][reconnect][upgrade][afunix]")
{
    auto h = UpgradeRun {};
    REQUIRE(h.turnUntil([&h] { return h.count(ConnectionState::Connected) == 1; }));

    h.upgrade();
    REQUIRE(h.turnUntil([&h] { return h.count(ConnectionState::Upgrading) == 1; }));
    CHECK(h.states.back().reason == "the daemon is upgrading to 9.9.10");
    auto const before = h.transport->calls;
    h.letTimePass(14);

    // Retried with the normal backoff, each attempt meeting an absent daemon, and none starting one.
    CHECK(h.transport->calls > before + 5);
    CHECK(h.transport->ensured == 0);
    CHECK(h.count(ConnectionState::Reconnecting) == 0);
    CHECK(h.states.back().state == ConnectionState::Upgrading);
}

TEST_CASE("a daemon back within the upgrade hold is reconnected without a start",
          "[vthost][reconnect][upgrade][afunix]")
{
    auto h = UpgradeRun {};
    REQUIRE(h.turnUntil([&h] { return h.count(ConnectionState::Connected) == 1; }));
    h.upgrade();
    REQUIRE(h.turnUntil([&h] { return h.count(ConnectionState::Upgrading) == 1; }));
    h.letTimePass(2);

    h.transport->back = true;
    h.letTimePass(1);
    REQUIRE(h.turnUntil([&h] { return h.count(ConnectionState::Connected) == 2; }));
    CHECK(h.sequence()
          == std::vector { ConnectionState::Connecting,
                           ConnectionState::Connected,
                           ConnectionState::Upgrading,
                           ConnectionState::Connected });
    CHECK(h.transport->ensured == 0);
}

TEST_CASE("after the hold, a still-absent daemon is started as before",
          "[vthost][reconnect][upgrade][afunix]")
{
    auto h = UpgradeRun {};
    REQUIRE(h.turnUntil([&h] { return h.count(ConnectionState::Connected) == 1; }));
    h.upgrade();
    REQUIRE(h.turnUntil([&h] { return h.count(ConnectionState::Upgrading) == 1; }));
    h.letTimePass(14);
    REQUIRE(h.transport->ensured == 0);

    h.letTimePass(2);
    REQUIRE(h.turnUntil([&h] { return h.count(ConnectionState::Connected) == 2; }));
    CHECK(h.transport->ensured == 1);
    CHECK(h.sequence()
          == std::vector { ConnectionState::Connecting,
                           ConnectionState::Connected,
                           ConnectionState::Upgrading,
                           ConnectionState::Reconnecting,
                           ConnectionState::Connected });
    CHECK(h.states[3].reason.contains("did not come back from its upgrade"));
}
